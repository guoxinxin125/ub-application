#include <cstdint>
#include <algorithm>
#include <thread>
#include <unistd.h>

#include "client.h"
#include "../post_consume.h"

namespace {

// Published before receiver threads start; immutable for the campaign.
std::vector<ClientContext *> domain_origins;

ImportedSharedPost consume_timeline_post(
    AppRpc *rpc, const PostStorageReadCXLResp &reference_resp) {
    my_assert(reference_resp.count <= 1);
    if (reference_resp.count == 0) {
        return ImportedSharedPost{};
    }
    const uint64_t import_start = sn_profile::start();
    const sn_slow_trace::Stamp import_trace = sn_slow_trace::start();
    ImportedSharedPost imported =
        import_shared_post(rpc, reference_resp.post);
    sn_slow_trace::record("client_import",
                          sn_slow_trace::current_request_number(),
                          import_trace);
    sn_profile::record(sn_profile::Stage::kClientImport, import_start);
    try {
        sn_consume::retain(sn_consume::native_post(imported.buffer.buf_, imported.size));
    } catch (...) {
        release_imported_post(rpc, imported);
        throw;
    }
    return imported;
}

}  // namespace



void connect_sessions(ClientContext *c) {

    // connect to backward server
    if (!sn_domain::enabled()) {
        c->load_balance_session_num_ = c->rpc_->create_session(load_balance_addr, get_remote_rpc_id(load_balance_addr));
        my_assert(c->load_balance_session_num_ >= 0, "Failed to create session");
    }

    c->post_storage_session_num_ = c->rpc_->create_session(post_storage_addr, get_remote_rpc_id(post_storage_addr));
    my_assert(c->post_storage_session_num_ >= 0, "Failed to create session");

    if (sn_domain::enabled() || sn_measure::rpc_baseline()) {
        const bool home = sn_domain::read_service() == sn_domain::ReadService::Home;
        const char *service = home ? "home_timeline" : "user_timeline";
        const std::string address = config_json_all.at(service).at("server_addr").get<std::string>();
        const uint32_t receiver = config_json_all.at(service).at("rpc_id").get<uint32_t>() +
            static_cast<uint32_t>(c->client_id_) % static_cast<uint32_t>(FLAGS_server_num);
        const int session = c->rpc_->create_session(address, static_cast<uint8_t>(receiver));
        if (session < 0)
            sn_domain::fail("client", static_cast<uint32_t>(c->client_id_), "create-timeline-session", 0, EHOSTUNREACH);
        (home ? c->domain_home_session : c->domain_user_session) = session;
    }
    if (sn_measure::rpc_baseline()) {
        const std::string address = config_json_all.at("nginx").at("server_addr").get<std::string>();
        c->baseline_nginx_session = c->rpc_->create_session(address, get_remote_rpc_id(address));
        if (c->baseline_nginx_session < 0) sn_domain::fail("client", 0, "create-nginx-probe", 0, EHOSTUNREACH);
    }
    const size_t expected = sn_measure::rpc_baseline() ? 4 : 2;
    const uint64_t deadline = (sn_domain::enabled() || sn_measure::rpc_baseline()) ? sn_domain::now_ns() + UINT64_C(30000000000) : 0;
    while (c->num_sm_resps_ != expected) {
        c->rpc_->run_event_loop(kAppEvLoopMs);
        if (deadline && sn_domain::now_ns() >= deadline)
            sn_domain::fail("client", static_cast<uint32_t>(c->client_id_), "session-ready-timeout", 0, ETIMEDOUT);
        if (unlikely(ctrl_c_pressed == 1)) {
            // printf("Ctrl-C pressed. Exiting\n");
            return;
        }
    }
    // // printf("Client session connected successfully!\n");
}

void domain_ready_callback(void *context, void *) {
    auto *ctx = static_cast<ClientContext *>(context);
    invalidate_msgbuf_before_read(ctx->rpc_, ctx->ping_resp_msgbuf);
    if (ctx->ping_resp_msgbuf.get_data_size() != sizeof(RPCMsgResp<PingRPCResp>))
        sn_domain::fail("client", static_cast<uint32_t>(ctx->client_id_), "ready-size", 0, EPROTO);
    RPCMsgResp<PingRPCResp> response(RPC_TYPE::RPC_DOMAIN_READY, 0, 0, {});
    std::memcpy(&response, ctx->ping_resp_msgbuf.buf_, sizeof(response));
    if (response.resp_common.type != RPC_TYPE::RPC_DOMAIN_READY || response.resp_common.req_number != 0 ||
        response.resp_control.timestamp != sn_domain::kWireAbi)
        sn_domain::fail("client", static_cast<uint32_t>(ctx->client_id_), "ready-abi", 0, EPROTO);
    ctx->domain_ready_status = response.resp_common.status;
}
void domain_wait_ready(ClientContext *ctx, int session, const char *peer) {
    const uint64_t deadline = sn_domain::now_ns() + UINT64_C(30000000000);
    do {
        ctx->domain_ready_status = -1;
        new (ctx->ping_msgbuf.buf_) RPCMsgReq<PingRPCReq>(RPC_TYPE::RPC_DOMAIN_READY, 0, {sn_domain::kWireAbi});
        flush_msgbuf_before_send(ctx->rpc_, ctx->ping_msgbuf);
        ctx->rpc_->enqueue_request(session, static_cast<uint8_t>(RPC_TYPE::RPC_DOMAIN_READY),
            &ctx->ping_msgbuf, &ctx->ping_resp_msgbuf, domain_ready_callback, nullptr);
        while (ctx->domain_ready_status == -1) {
            ctx->rpc_->run_event_loop_once();
            if (ctrl_c_pressed || sn_domain::now_ns() >= deadline)
                sn_domain::fail("client", static_cast<uint32_t>(ctx->client_id_), peer, 0, ETIMEDOUT);
        }
        if (ctx->domain_ready_status == 0) break;
        if (ctx->domain_ready_status != EAGAIN)
            sn_domain::fail("client", static_cast<uint32_t>(ctx->client_id_), peer, 0, ctx->domain_ready_status);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (sn_domain::now_ns() >= deadline)
            sn_domain::fail("client", static_cast<uint32_t>(ctx->client_id_), peer, 0, ETIMEDOUT);
    } while (true);
    std::fprintf(stderr, "SN_DOMAIN_INIT source=%u lane=%zu peer=%s ready=1\n", sn_domain::source_id(), ctx->client_id_, peer);
}

void domain_timeline_read_resp_handler(erpc::ReqHandle *handle, void *context,
                                       sn_domain::ReadService service) {
    auto *ctx = static_cast<ServerContext *>(context);
    const uint64_t response_arrival = sn_measure::enabled() ? sn_domain::now_ns() : sn_diag::timestamp();
    const uint64_t received = sn_domain::metrics().start();
    const auto *buffer = handle->get_req_msgbuf();
    invalidate_msgbuf_before_read(ctx->rpc_, *buffer);
    if (!sn_domain::enabled() || buffer->get_data_size() != sizeof(RPCMsgReq<DomainUserResponse>))
        sn_domain::fail("client", static_cast<uint32_t>(ctx->server_id_), "response-size-or-mode", 0, EPROTO);
    RPCMsgReq<DomainUserResponse> response(RPC_TYPE::RPC_DOMAIN_USER_TIMELINE_READ_RESP, 0, {});
    std::memcpy(&response, buffer->buf_, sizeof(response));
    const auto &o = response.req_control.origin;
    const uint32_t wire = response.req_common.req_number;
    sn_profile::flow_point_at("client", o.original_request, "response_arrival", response_arrival);
    sn_diag::point_at("client_receive", wire, response_arrival);
    sn_diag::Event *diagnostic_receive = sn_diag::begin("client_consume", wire);
    if (response.req_control.response.count > 1)
        sn_domain::fail("client", static_cast<uint32_t>(ctx->server_id_), "response-count", wire, EPROTO);
    if (response.req_common.type != (service == sn_domain::ReadService::Home ?
            RPC_TYPE::RPC_DOMAIN_HOME_TIMELINE_READ_RESP : RPC_TYPE::RPC_DOMAIN_USER_TIMELINE_READ_RESP) ||
        !sn_domain::origin_valid(o, wire) || o.source != sn_domain::source_id() || o.lane >= domain_origins.size() ||
        o.receiver_rpc != handle->get_server_rpc_id())
        sn_domain::fail("client", static_cast<uint32_t>(ctx->server_id_), "response-origin", wire, EPROTO);
    ClientContext *origin = domain_origins[o.lane];
    const size_t slot = o.original_request % kAppMaxBuffer;
    uint32_t expected = wire;
    if (!origin->domain_inflight[slot].compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
        sn_domain::fail("client", o.lane, "response-inflight", wire, EPROTO);
    ImportedSharedPost imported = consume_timeline_post(ctx->rpc_, response.req_control.response);
    if (sn_domain::boolean_env("ERPC_SN_DOMAIN_FIXTURE")) {
        const PostData *post = imported.buffer.buf_ ? reinterpret_cast<const PostData *>(imported.buffer.buf_) : nullptr;
        if (!sn_fixture::verify(o.original_request, response.req_control.response.count, post)) {
            if (post) release_imported_post(ctx->rpc_, imported);
            sn_domain::fail("client", o.lane, "fixture-result", wire, EPROTO);
        }
        (post ? origin->domain_fixture_nonempty : origin->domain_fixture_empty).fetch_add(1, std::memory_order_relaxed);
    }
    const uint64_t begin = origin->domain_e2e_start[slot].load(std::memory_order_acquire);
    const uint64_t done = sn_domain::now_ns();
    sn_profile::flow_point_at("client", o.original_request, "consume_done", done);
    const uint64_t elapsed_ns = begin ? done - begin : 0;
    sn_diag::point("client_complete", wire);
    sn_diag::finish(diagnostic_receive);
    if (!origin->domain_measurement.record(o.original_request, wire, response.req_control.response.count,
            begin, origin->domain_tx_begin_ns[slot].load(std::memory_order_acquire), response_arrival,
            done, origin->domain_forward_ns[slot].load(std::memory_order_acquire),
            response.req_control.response.count ? response.req_control.response.post.payload_length : 0))
        sn_domain::fail("client", o.lane, "measurement-record", wire, EPROTO);
    if (sn_measure::measured(o.original_request)) {
        if (!sn_measure::enabled()) hdr_record_value_atomic(service == sn_domain::ReadService::Home ?
            latency_home_timeline_hist_ : latency_user_timeline_hist_,
            static_cast<int64_t>(timers[o.lane][slot].toc() * 10));
        if (begin) sn_domain::metrics().add(sn_domain::Stage::kE2e, elapsed_ns);
    }
    if (imported.buffer.buf_ != nullptr) release_imported_post(ctx->rpc_, imported);
    origin->domain_last_done_ns.store(done, std::memory_order_release);
    origin->domain_completed.fetch_add(1, std::memory_order_release);
    if (service == sn_domain::ReadService::Home) ++ctx->stat_req_home_timeline_tot;
    else ++ctx->stat_req_user_timeline_tot;
    ctx->rpc_->resize_msg_buffer(&handle->pre_resp_msgbuf_, 0);
    origin->queue_store->PushNextReq();
    ctx->rpc_->enqueue_response(handle, &handle->pre_resp_msgbuf_);
    if (sn_measure::measured(o.original_request)) sn_domain::metrics().finish(sn_domain::Stage::kReceive, received);
}
void domain_user_timeline_read_resp_handler(erpc::ReqHandle *handle, void *context) {
    domain_timeline_read_resp_handler(handle, context, sn_domain::ReadService::User);
}
void domain_home_timeline_read_resp_handler(erpc::ReqHandle *handle, void *context) {
    domain_timeline_read_resp_handler(handle, context, sn_domain::ReadService::Home);
}

void ping_resp_handler(erpc::ReqHandle *req_handler, void *_context) {
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_ping_tot++;

    auto *req_msgbuf = req_handler->get_req_msgbuf();
    my_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<PingRPCReq>), "data size not match");

    auto *req = reinterpret_cast<RPCMsgReq<PingRPCReq> *>(req_msgbuf->buf_);

    new (req_handler->pre_resp_msgbuf_.buf_) RPCMsgResp<PingRPCResp>(req->req_common.type, req->req_common.req_number, 0, { 0 });
    ctx->rpc_->resize_msg_buffer(&req_handler->pre_resp_msgbuf_, sizeof(RPCMsgResp<PingRPCResp>));

    ctx->rpc_->enqueue_response(req_handler, &req_handler->pre_resp_msgbuf_);

    ctx->queue_store->PushResp(RESP_MSG{ req->req_common.req_number, 0 });

}

void compose_post_write_resp_handler(erpc::ReqHandle *req_handler, void *_context) {
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_compose_post_tot++;
    auto *req_msgbuf = req_handler->get_req_msgbuf();

    auto *req = reinterpret_cast<RPCMsgReq<CommonRPCReq> *>(req_msgbuf->buf_);

    my_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<CommonRPCReq>) + req->req_control.data_length, "data size not match");
    const uint32_t req_number = req->req_common.req_number;
    const size_t elapsed_us =
        timers[ctx->server_id_][req_number % kAppMaxBuffer].toc();
    sn_slow_trace::record_e2e("client_write_e2e", req_number, elapsed_us);
    hdr_record_value_atomic(latency_write_hist_,
                            static_cast<int64_t>(elapsed_us * 10));

    ctx->rpc_->resize_msg_buffer(&req_handler->pre_resp_msgbuf_, 0);

    ctx->queue_store->PushNextReq();

    ctx->rpc_->enqueue_response(req_handler, &req_handler->pre_resp_msgbuf_);

}

void timeline_read_resp_handler(erpc::ReqHandle *req_handler, void *_context, bool home) {
    auto *ctx = static_cast<ServerContext *>(_context);
    const uint64_t response_arrival = sn_measure::enabled() ? sn_domain::now_ns() : sn_diag::timestamp();
    const uint64_t received = sn_domain::metrics().start();
    if (home) ++ctx->stat_req_home_timeline_tot;
    else ++ctx->stat_req_user_timeline_tot;
    auto *req_msgbuf = req_handler->get_req_msgbuf();
    invalidate_msgbuf_before_read(ctx->rpc_, *req_msgbuf);
    auto *req_common = reinterpret_cast<CommonReq *>(req_msgbuf->buf_);

    my_assert(req_msgbuf->get_data_size() ==
              sizeof(RPCMsgReq<PostStorageReadCXLResp>));

    ctx->rpc_->resize_msg_buffer(&req_handler->pre_resp_msgbuf_, 0);


    auto *req = reinterpret_cast<RPCMsgReq<PostStorageReadCXLResp> *>(req_msgbuf->buf_);
    const PostStorageReadCXLResp &reference_resp = req->req_control;
    const uint32_t wire = req_common->req_number;
    uint32_t req_number = wire;
    if (sn_measure::rpc_baseline() && sn_domain::source_count() > 1) {
        try {
            if (sn_domain::wire_source_index(wire, static_cast<uint32_t>(FLAGS_client_num),
                                             sn_domain::source_count()) != sn_domain::source_index())
                sn_domain::fail("client", static_cast<uint32_t>(ctx->server_id_), "baseline-source", wire, EPROTO);
            req_number = sn_domain::wire_original_request(wire, static_cast<uint32_t>(FLAGS_client_num),
                                                          sn_domain::source_count());
        } catch (const std::exception &) {
            sn_domain::fail("client", static_cast<uint32_t>(ctx->server_id_), "baseline-wire", wire, EPROTO);
        }
    }
    if (sn_measure::rpc_baseline())
        sn_profile::flow_point_at("client", req_number, "response_arrival", response_arrival);
    if ((sn_domain::enabled() || sn_measure::rpc_baseline()) &&
        req_common->type != (home ? RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP : RPC_TYPE::RPC_USER_TIMELINE_READ_RESP))
        sn_domain::fail("client", static_cast<uint32_t>(ctx->server_id_), "timeline-response-type", req_number, EPROTO);
    if (sn_measure::rpc_baseline()) {
        sn_diag::point_at("client_receive", req_number, response_arrival);
        sn_diag::Event *diagnostic_receive = sn_diag::begin("client_consume", req_number);
        if (!req_number || ctx->server_id_ >= domain_origins.size() || reference_resp.count > 1)
            sn_domain::fail("client", 0, "baseline-response", req_number, EPROTO);
        ClientContext *origin = domain_origins[ctx->server_id_];
        const size_t slot = req_number % kAppMaxBuffer;
        uint32_t expected = wire;
        if (!origin->domain_inflight[slot].compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
            sn_domain::fail("client", 0, "baseline-inflight", req_number, EPROTO);
        ImportedSharedPost post = consume_timeline_post(ctx->rpc_, reference_resp);
        const PostData *data = post.buffer.buf_ ? reinterpret_cast<const PostData *>(post.buffer.buf_) : nullptr;
        if (sn_domain::boolean_env("ERPC_SN_DOMAIN_FIXTURE")) {
            if (!sn_fixture::verify(req_number, reference_resp.count, data)) {
                if (data) release_imported_post(ctx->rpc_, post);
                sn_domain::fail("client", 0, "fixture-result", req_number, EPROTO);
            }
            (data ? origin->domain_fixture_nonempty : origin->domain_fixture_empty).fetch_add(1, std::memory_order_relaxed);
        }
        const uint64_t begin = origin->domain_e2e_start[slot].load(std::memory_order_acquire);
        const uint64_t done = sn_domain::now_ns();
        sn_profile::flow_point_at("client", req_number, "consume_done", done);
        const uint64_t elapsed = begin ? done - begin : 0;
        sn_diag::point("client_complete", req_number);
        sn_diag::finish(diagnostic_receive);
        if (!origin->domain_measurement.record(req_number, wire, reference_resp.count,
                begin, origin->domain_tx_begin_ns[slot].load(std::memory_order_acquire), response_arrival,
                done, 0,
                reference_resp.count ? reference_resp.post.payload_length : 0))
            sn_domain::fail("client", 0, "measurement-record", req_number, EPROTO);
        if (sn_measure::measured(req_number)) sn_domain::metrics().add(sn_domain::Stage::kE2e, elapsed);
        if (post.buffer.buf_) release_imported_post(ctx->rpc_, post);
        origin->domain_last_done_ns.store(done, std::memory_order_release);
        origin->domain_completed.fetch_add(1, std::memory_order_release);
        ctx->queue_store->PushNextReq();
        ctx->rpc_->enqueue_response(req_handler, &req_handler->pre_resp_msgbuf_);
        if (sn_measure::measured(req_number)) sn_domain::metrics().finish(sn_domain::Stage::kReceive, received);
        return;
    }
    sn_slow_trace::RequestScope trace_scope(req_number);
    ImportedSharedPost imported =
        consume_timeline_post(ctx->rpc_, reference_resp);

    // All effective fields have been consumed, before reference cleanup.
    const size_t elapsed_us =
        timers[ctx->server_id_][req_number % kAppMaxBuffer].toc();
    sn_slow_trace::record_e2e(home ? "client_home_e2e" : "client_user_e2e", req_number, elapsed_us);
    hdr_record_value_atomic(home ? latency_home_timeline_hist_ : latency_user_timeline_hist_,
                            static_cast<int64_t>(elapsed_us * 10));
    if (sn_domain::metrics().on && ctx->server_id_ < domain_origins.size()) {
        const uint64_t begin = domain_origins[ctx->server_id_]->domain_e2e_start[req_number % kAppMaxBuffer].load(std::memory_order_acquire);
        if (begin) sn_domain::metrics().add(sn_domain::Stage::kE2e, sn_domain::now_ns() - begin);
    }

    if (imported.buffer.buf_ != nullptr) {
        const uint64_t release_start = sn_profile::start();
        release_imported_post(ctx->rpc_, imported);
        sn_profile::record(sn_profile::Stage::kClientRelease, release_start);
    }

    ctx->queue_store->PushNextReq();
    ctx->rpc_->enqueue_response(req_handler, &req_handler->pre_resp_msgbuf_);
    sn_domain::metrics().finish(sn_domain::Stage::kReceive, received);
}
void user_timeline_read_resp_handler(erpc::ReqHandle *req_handler, void *context) {
    timeline_read_resp_handler(req_handler, context, false);
}

void home_timeline_read_resp_handler(erpc::ReqHandle *req_handler, void *_context) {
    timeline_read_resp_handler(req_handler, _context, true);
}

void callback_ping(void *_context, void *_tag) {
    _unused(_tag);
    auto *ctx = static_cast<ClientContext *>(_context);

    auto *resp = reinterpret_cast<RPCMsgResp<PingRPCResp>*>(ctx->ping_resp_msgbuf.buf_);

    // 如果返回值不为0，则认为后续不会有响应，直接将请求号和错误码放入队列
    // 如果返回值为0，则认为后续将有响应，不care
    my_assert(resp->resp_common.status == 0, "Ping callback failed");
}


void handler_ping(ClientContext *ctx, REQ_MSG req_msg) {
    if (sn_domain::enabled()) {
        // Both data services were explicitly probed before the worker loop.
        // Do not use the legacy proxy fanout's single-Client return session.
        ctx->queue_store->PushResp(RESP_MSG{req_msg.req_id, 0});
        return;
    }
    new (ctx->ping_msgbuf.buf_) RPCMsgReq<PingRPCReq>(RPC_TYPE::RPC_PING, req_msg.req_id, { 0 });
    ctx->rpc_->enqueue_request(ctx->load_balance_session_num_, static_cast<uint8_t>(RPC_TYPE::RPC_PING),
        &ctx->ping_msgbuf, &ctx->ping_resp_msgbuf,
        callback_ping, nullptr);
}

void callback_rmem_param(void *_context, void *_tag) {
    _unused(_tag);
    auto *ctx = static_cast<ClientContext *>(_context);

    auto *resp = reinterpret_cast<RPCMsgResp<CommonRPCResp>*>(ctx->rmem_param_resp_msgbuf.buf_);

    // 如果返回值不为0，则认为后续不会有响应，直接将请求号和错误码放入队列
    // 如果返回值为0，则认为后续将有响应，不care
#if defined(ERPC_CXL) || defined(ERPC_UB)
    // CXL path does not require RMEM handshake payload. Some services may reply with non-zero
    // status for RPC_RMEM_PARAM, so we must not abort here.
    if (resp->resp_common.status != 0) {
        rmems_init_number++;
        return;
    }
#else
    my_assert(resp->resp_common.status == 0, "rmem_param callback failed");
#endif

    if (rmems_[ctx->client_id_] != nullptr) {
        return;
    }

    if (resp->resp_control.data_length == 0) {
        // CXL shared memory doesn't need rmem connection
        rmems_init_number++;
    } else {
        social_network::RmemParam rmem_param;
        // CXL shared memory doesn't need rmem connection
        // This callback is not needed for CXL transport
        rmems_init_number++;
    }
}

void handler_rmem_param(ClientContext *ctx, REQ_MSG req_msg) {
#if defined(ERPC_CXL) || defined(ERPC_UB)
    _unused(req_msg);
    // Shared-memory transports do not need RMEM connection negotiation.
    rmems_init_number++;
    return;
#else
    new (ctx->rmem_param_msgbuf.buf_) RPCMsgReq<RmemParamReq>(RPC_TYPE::RPC_RMEM_PARAM, req_msg.req_id, { 0 });
    ctx->rpc_->enqueue_request(ctx->post_storage_session_num_, static_cast<uint8_t>(RPC_TYPE::RPC_RMEM_PARAM),
        &ctx->rmem_param_msgbuf, &ctx->rmem_param_resp_msgbuf,
        callback_rmem_param, nullptr);
#endif
}

void callback_common(void *_context, void *_tag) {
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);

    auto *req = reinterpret_cast<CommonReq *>(
        ctx->req_msgbuf[req_id % kAppMaxBuffer].buf_);
    const auto req_type = req->type;
    erpc::MsgBuffer &resp_msgbuf =
        ctx->resp_msgbuf[req_id % kAppMaxBuffer];

    if (req_type == RPC_TYPE::RPC_DOMAIN_USER_TIMELINE_READ_REQ ||
        req_type == RPC_TYPE::RPC_USER_TIMELINE_READ_REQ ||
        req_type == RPC_TYPE::RPC_DOMAIN_HOME_TIMELINE_READ_REQ ||
        req_type == RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ) {
        const size_t slot = req_id % kAppMaxBuffer;
        if (sn_measure::measured(req_id)) sn_domain::metrics().finish(sn_domain::Stage::kAck, ctx->domain_ack_start[slot]);
        ctx->domain_ack_start[slot] = 0;
        if (resp_msgbuf.get_data_size() != 0)
            sn_domain::fail("client", static_cast<uint32_t>(ctx->client_id_), "request-ack", req_id, EPROTO);
    }

    if (req_type == RPC_TYPE::RPC_POST_STORAGE_WRITE_REQ) {
        my_assert(resp_msgbuf.get_data_size() ==
                  sizeof(RPCMsgResp<CommonRPCResp>),
                  "data size not match");
        const size_t elapsed_us =
            timers[ctx->client_id_][req_id % kAppMaxBuffer].toc();
        sn_slow_trace::record_e2e("client_write_direct_e2e", req_id,
                                  elapsed_us);
        hdr_record_value_atomic(latency_write_hist_,
                                static_cast<int64_t>(elapsed_us * 10));
        ctx->compose_post_req_queue->push(
            ctx->req_msgbuf[req_id % kAppMaxBuffer]);
        ctx->queue_store->PushNextReq();
        return;
    }

    my_assert(resp_msgbuf.get_data_size() == 0, "data size not match");

    switch (req_type) {
    case RPC_TYPE::RPC_COMPOSE_POST_WRITE_REQ:
        ctx->compose_post_req_queue->push(ctx->req_msgbuf[req_id % kAppMaxBuffer]);
        break;
    case RPC_TYPE::RPC_USER_TIMELINE_READ_REQ:
    case RPC_TYPE::RPC_DOMAIN_USER_TIMELINE_READ_REQ:
        ctx->user_timeline_req_queue->push(ctx->req_msgbuf[req_id % kAppMaxBuffer]);
        break;
    case RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ:
    case RPC_TYPE::RPC_DOMAIN_HOME_TIMELINE_READ_REQ:
        ctx->home_timeline_req_queue->push(ctx->req_msgbuf[req_id % kAppMaxBuffer]);
        break;
    case RPC_TYPE::RPC_POST_STORAGE_WRITE_REQ:
        ctx->compose_post_req_queue->push(ctx->req_msgbuf[req_id % kAppMaxBuffer]);
        break;

    default:
        fprintf(stderr, "illegal resp type");
        exit(1);
    }

}

void handler_compose_post_write_req(ClientContext *ctx, REQ_MSG req_msg) {
    erpc::MsgBuffer &req_msgbuf = ctx->req_msgbuf[req_msg.req_id % kAppMaxBuffer];
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_msgbuf[req_msg.req_id % kAppMaxBuffer];

    req_msgbuf = ctx->compose_post_req_queue->pop();
    //    new (req_msgbuf.buf_) CommonReq{RPC_TYPE::RPC_COMPOSE_POST_WRITE_REQ, req_msg.req_id};

    new (req_msgbuf.buf_) CommonReq{RPC_TYPE::RPC_POST_STORAGE_WRITE_REQ, req_msg.req_id};

    timers[ctx->client_id_][req_msg.req_id % kAppMaxBuffer].tic();
    flush_msgbuf_before_send(ctx->rpc_, req_msgbuf);
    ctx->rpc_->enqueue_request(ctx->post_storage_session_num_, static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_WRITE_REQ),
        &req_msgbuf, &resp_msgbuf,
        callback_common, reinterpret_cast<void *>(req_msg.req_id));
}

void handler_timeline_read_req(ClientContext *ctx, REQ_MSG req_msg, bool home) {
    erpc::MsgBuffer &req_msgbuf = ctx->req_msgbuf[req_msg.req_id % kAppMaxBuffer];
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_msgbuf[req_msg.req_id % kAppMaxBuffer];

    req_msgbuf = home ? ctx->home_timeline_req_queue->pop() : ctx->user_timeline_req_queue->pop();
    // The comparison starts at the same application boundary for both routes.
    const uint64_t call_begin = sn_measure::enabled() ? sn_domain::now_ns() : 0;
    sn_profile::flow_point_at("client", req_msg.req_id, "after_pop", call_begin);
    if (sn_domain::enabled()) {
        const uint32_t lane = static_cast<uint32_t>(ctx->client_id_);
        const uint32_t wire = sn_domain::client_wire(req_msg.req_id, lane);
        sn_profile::flow_point_at("client", wire, "wire_identity", call_begin, req_msg.req_id);
        const size_t slot = req_msg.req_id % kAppMaxBuffer;
        if (ctx->domain_inflight[slot].load(std::memory_order_acquire))
            sn_domain::fail("client", lane, "request-slot-busy", wire, EBUSY);
        if (!sn_measure::enabled()) timers[lane][slot].tic();
        const uint64_t start = sn_measure::enabled() ? call_begin : sn_domain::metrics().start();
        ctx->domain_e2e_start[slot].store(start, std::memory_order_release);
        sn_diag::point("client_call_begin", wire);
        const uint64_t forward_begin = sn_measure::enabled() ? sn_domain::now_ns() : 0;
        sn_profile::flow_point_at("client", req_msg.req_id, "forward_begin", forward_begin);
        const uint32_t nginx = ctx->domain_runtime.forward(wire, sn_domain::route_key(req_msg.req_id),
            sn_domain::source_id(), home ? sn_domain::ReadService::Home : sn_domain::ReadService::User);
        const uint64_t forward_end = sn_measure::enabled() ? sn_domain::now_ns() : 0;
        sn_profile::flow_point_at("client", req_msg.req_id, "forward_end", forward_end);
        sn_diag::point("client_forward_done", wire);
        if (sn_measure::enabled()) ctx->domain_forward_ns[slot].store(forward_end-forward_begin, std::memory_order_release);
        const sn_domain::Origin origin{sn_domain::kWireAbi, sn_domain::source_id(), lane,
            sn_domain::receiver_base() + lane % static_cast<uint32_t>(FLAGS_server_num),
            req_msg.req_id, nginx};
        if (home) {
            auto *request = reinterpret_cast<RPCMsgReq<DomainHomeRequest> *>(req_msgbuf.buf_);
            request->req_common = {RPC_TYPE::RPC_DOMAIN_HOME_TIMELINE_READ_REQ, wire};
            request->req_control.origin = origin;
        } else {
            auto *request = reinterpret_cast<RPCMsgReq<DomainUserRequest> *>(req_msgbuf.buf_);
            request->req_common = {RPC_TYPE::RPC_DOMAIN_USER_TIMELINE_READ_REQ, wire};
            request->req_control.origin = origin;
        }
        ctx->domain_inflight[slot].store(wire, std::memory_order_release);
        sn_profile::flow_point("client", req_msg.req_id, "request_ready");
        if (sn_measure::enabled()) ctx->domain_tx_begin_ns[slot].store(sn_domain::now_ns(), std::memory_order_release);
        sn_profile::flow_point("client", req_msg.req_id, "send_begin");
        flush_msgbuf_before_send(ctx->rpc_, req_msgbuf);
        ctx->domain_ack_start[slot] = sn_measure::measured(req_msg.req_id) ? sn_domain::metrics().start() : 0;
        const uint64_t send = ctx->domain_ack_start[slot];
        sn_diag::point("client_send_begin", wire);
        ctx->rpc_->enqueue_request(home ? ctx->domain_home_session : ctx->domain_user_session,
            static_cast<uint8_t>(home ? RPC_TYPE::RPC_DOMAIN_HOME_TIMELINE_READ_REQ :
                                 RPC_TYPE::RPC_DOMAIN_USER_TIMELINE_READ_REQ), &req_msgbuf, &resp_msgbuf,
            callback_common, reinterpret_cast<void *>(req_msg.req_id));
        sn_profile::flow_point("client", req_msg.req_id, "enqueue_done");
        sn_diag::point("client_send_done", wire);
        sn_domain::metrics().finish(sn_domain::Stage::kSend, send);
        return;
    }
    const uint32_t baseline_wire = sn_measure::rpc_baseline() && sn_domain::source_count() > 1 ?
        sn_domain::client_wire(req_msg.req_id, static_cast<uint32_t>(ctx->client_id_)) : req_msg.req_id;
    new (req_msgbuf.buf_) CommonReq{ home ? RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ :
        RPC_TYPE::RPC_USER_TIMELINE_READ_REQ, baseline_wire };
    sn_profile::flow_point("client", req_msg.req_id, "request_ready");

    if (sn_measure::rpc_baseline()) {
        const size_t slot = req_msg.req_id % kAppMaxBuffer;
        if (ctx->domain_inflight[slot].exchange(baseline_wire, std::memory_order_acq_rel))
            sn_domain::fail("client", 0, "baseline-slot-busy", req_msg.req_id, EBUSY);
        ctx->domain_e2e_start[slot].store(call_begin, std::memory_order_release);
        sn_diag::point("client_call_begin", req_msg.req_id);
    } else {
        timers[ctx->client_id_][req_msg.req_id % kAppMaxBuffer].tic();
        if (sn_domain::metrics().on) ctx->domain_e2e_start[req_msg.req_id % kAppMaxBuffer].store(sn_domain::metrics().start(), std::memory_order_release);
    }
    if (sn_measure::rpc_baseline())
        ctx->domain_tx_begin_ns[req_msg.req_id % kAppMaxBuffer].store(sn_domain::now_ns(), std::memory_order_release);
    sn_profile::flow_point("client", req_msg.req_id, "send_begin");
    flush_msgbuf_before_send(ctx->rpc_, req_msgbuf);
    ctx->domain_ack_start[req_msg.req_id % kAppMaxBuffer] = sn_measure::measured(req_msg.req_id) ? sn_domain::metrics().start() : 0;
    const uint64_t send = ctx->domain_ack_start[req_msg.req_id % kAppMaxBuffer];
    if (sn_measure::rpc_baseline()) sn_diag::point("client_send_begin", req_msg.req_id);
    ctx->rpc_->enqueue_request(ctx->load_balance_session_num_, static_cast<uint8_t>(home ?
        RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ : RPC_TYPE::RPC_USER_TIMELINE_READ_REQ),
        &req_msgbuf, &resp_msgbuf,
        callback_common, reinterpret_cast<void *>(req_msg.req_id));
    sn_profile::flow_point("client", req_msg.req_id, "enqueue_done");
    if (sn_measure::rpc_baseline()) sn_diag::point("client_send_done", req_msg.req_id);
    sn_domain::metrics().finish(sn_domain::Stage::kSend, send);
}
void handler_user_timeline_read_req(ClientContext *ctx, REQ_MSG req_msg) {
    handler_timeline_read_req(ctx, req_msg, false);
}

void handler_home_timeline_read_req(ClientContext *ctx, REQ_MSG req_msg) {
    handler_timeline_read_req(ctx, req_msg, true);
}


void client_thread_func(size_t thread_id, ClientContext *ctx, erpc::Nexus *nexus) {
    ctx->client_id_ = thread_id;
    if (sn_domain::enabled() || sn_measure::rpc_baseline()) ctx->domain_measurement.configure(static_cast<uint64_t>(generate_data_num));
    sn_domain::metrics().configure("client-send", static_cast<uint32_t>(thread_id), sn_domain::enabled() ? sn_domain::source_id() : 0);
    sn_domain::pin_worker("client", static_cast<uint32_t>(thread_id));
    std::vector<size_t> port_vec = flags_get_cxl_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

    uint8_t rpc_id = FLAGS_rpc_id + 20 + thread_id;

    AppRpc rpc(nexus, static_cast<void *>(ctx),
        rpc_id,
        basic_sm_handler_client, phy_port);
    rpc.retry_connect_on_invalid_rpc_id_ = true;
    ctx->rpc_ = &rpc;

    for (size_t i = 0; i < kAppMaxBuffer; i++) {
        ctx->resp_msgbuf[i] = rpc.alloc_msg_buffer_or_die(sizeof(RPCMsgResp<CommonRPCResp>));
    }
    generate_compose_post_req_msgbuf(ctx->rpc_, ctx->compose_post_req_queue);
    generate_user_timeline_req_msgbuf(ctx->rpc_, ctx->user_timeline_req_queue);
    generate_home_timeline_req_msgbuf(ctx->rpc_, ctx->home_timeline_req_queue);
    ctx->ping_msgbuf = rpc.alloc_msg_buffer_or_die(sizeof(RPCMsgReq<PingRPCReq>));
    ctx->ping_resp_msgbuf = rpc.alloc_msg_buffer_or_die(sizeof(RPCMsgResp<PingRPCResp>));
    ctx->rmem_param_msgbuf = rpc.alloc_msg_buffer_or_die(sizeof(RPCMsgReq<RmemParamReq>));
    ctx->rmem_param_resp_msgbuf = rpc.alloc_msg_buffer_or_die(KB(1));


    connect_sessions(ctx);
    if (sn_domain::enabled()) {
        ctx->domain_runtime.open("client", static_cast<uint32_t>(thread_id), sn_domain::proxy_count(), sn_domain::source_id());
    }
    if (sn_domain::enabled() || sn_measure::rpc_baseline()) {
        domain_wait_ready(ctx, sn_domain::read_service() == sn_domain::ReadService::Home ?
            ctx->domain_home_session : ctx->domain_user_session, "timeline-ready");
        domain_wait_ready(ctx, ctx->post_storage_session_num_, "post-storage-ready");
        if (sn_measure::rpc_baseline()) {
            domain_wait_ready(ctx, ctx->load_balance_session_num_, "load-balance-ready");
            domain_wait_ready(ctx, ctx->baseline_nginx_session, "nginx-ready");
        }
    }

    while (true) {
        REQ_MSG req_msg{};
        for (size_t i = 0;
             i < kAppMaxBuffer && ctx->queue_store->TryPopReq(req_msg);
             i++) {
            switch (req_msg.req_type) {
                case RPC_TYPE::RPC_PING:
                    handler_ping(ctx, req_msg);
                    break;
                case RPC_TYPE::RPC_RMEM_PARAM:
                    handler_rmem_param(ctx, req_msg);
                    break;
                case RPC_TYPE::RPC_COMPOSE_POST_WRITE_REQ:
                    handler_compose_post_write_req(ctx, req_msg);
                    break;
                case RPC_TYPE::RPC_USER_TIMELINE_READ_REQ:
                    handler_user_timeline_read_req(ctx, req_msg);
                    break;
                case RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ:
                    handler_home_timeline_read_req(ctx, req_msg);
                    break;
                default:
                    my_assert(false);
            }
        }
        ctx->rpc_->run_event_loop_once();
        if (unlikely(ctrl_c_pressed)) {
            break;
        }
    }
    ctx->domain_runtime.close();
    sn_domain::metrics().report();
    if (sn_domain::enabled()) std::fprintf(stderr, "SN_DOMAIN_CLIENT_DONE source=%u lane=%zu completed=%llu\n",
        sn_domain::source_id(), thread_id, static_cast<unsigned long long>(ctx->domain_completed.load(std::memory_order_relaxed)));
}

void server_thread_func(size_t thread_id, ServerContext *ctx, erpc::Nexus *nexus) {
    ctx->server_id_ = thread_id;
    sn_domain::metrics().configure("client-receive", static_cast<uint32_t>(thread_id), sn_domain::enabled() ? sn_domain::source_id() : 0);
    std::vector<size_t> port_vec = flags_get_cxl_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

    uint8_t rpc_id = FLAGS_rpc_id + thread_id;

    AppRpc rpc(nexus, static_cast<void *>(ctx),
        rpc_id,
        basic_sm_handler_server, phy_port);
    rpc.retry_connect_on_invalid_rpc_id_ = true;
    ctx->rpc_ = &rpc;

    while (true) {
        ctx->reset_stat();
        erpc::ChronoTimer start;
        start.reset();
        rpc.run_event_loop(kAppEvLoopMs);
        const double seconds = start.get_sec();
        // // printf("thread %zu: ping: %.2f, param: %.2f, compose: %.2f, user: %.2f, home: %.2f \n", thread_id,
        //     ctx->stat_req_ping_tot / seconds, ctx->stat_rmem_param_tot / seconds, ctx->stat_req_compose_post_tot / seconds,
        //     ctx->stat_req_user_timeline_tot / seconds, ctx->stat_req_home_timeline_tot / seconds);

        ctx->rpc_->reset_dpath_stats();
        // more handler
        if (ctrl_c_pressed == 1) {
            break;
        }
    }
    sn_domain::metrics().report();
}
#if defined(RMEM_PROGRAM)
void reader_thread_func(size_t thread_id, QueueStore *store) {
    std::queue<ReaderHandler *> queues;

    size_t reading_posts = 0;
    ReaderHandler *tmp = nullptr;
    const int max_batch = 9;
    int *batch_buffer = new int[max_batch];

    while (rmems_init_number != FLAGS_client_num) {
        usleep(1000);
    }
    while (true) {

        if (reading_posts < kAppMaxBuffer) {
            if (reader_queues[thread_id]->try_pop(tmp)) {
                queues.push(tmp);
                for (size_t i = 0;i < tmp->rmem_bufs.size();i++) {
                    rmems_[thread_id]->rmem_read_async(tmp->rmem_bufs[i], tmp->addrs_size[i].first + rmem_base_addr[thread_id], tmp->addrs_size[i].second);
                }
                reading_posts += tmp->rmem_bufs.size();
            }
        }

        int fetch_num = rmems_[thread_id]->rmem_poll(batch_buffer, max_batch);

        reading_posts -= fetch_num;
        for (int i = 0;i < fetch_num;i++) {
            my_assert(batch_buffer[i] == 0, "poll post error!");
        }

        while (fetch_num) {
            tmp = queues.front();
            size_t now_finish_num = tmp->finished_num;
            int parse_num = 0;
            for (int i = 0;i < fetch_num && now_finish_num + i < tmp->rmem_bufs.size();i++) {
                parse_num++;
                tmp->finished_num++;
            }

            if (tmp->finished_num == tmp->rmem_bufs.size()) {
                hdr_record_value_atomic(tmp->hist,
                    static_cast<int64_t>(timers[thread_id][tmp->req_num % kAppMaxBuffer].toc() * 10));

                store->PushNextReq();
                for (auto read_buf : tmp->rmem_bufs) {
                    free(read_buf);
                }
                delete tmp;
                queues.pop();
            }
            fetch_num -= parse_num;
        }
        if (unlikely(ctrl_c_pressed == 1)) {
            break;
        }
    }
}
#endif

bool write_latency_and_reset(const std::string &filename, hdr_histogram *hdr,
                             bool reset = true) {
    FILE *fp = fopen(filename.c_str(), "w");
    if (fp == nullptr) {
        fprintf(stderr, "failed to open latency file %s\n", filename.c_str());
        fflush(stderr);
        return false;
    }
    printf("writing latency file %s, count=%ld\n", filename.c_str(), hdr->total_count);
    fflush(stdout);
    hdr_percentiles_print(hdr, fp, 5, 10, CLASSIC);
    fclose(fp);
    if (reset) {
        hdr_reset(hdr);
    }
    return true;
}

void write_overall_latency(bool reset = true) {
    write_latency_and_reset(FLAGS_latency_file + std::to_string(FLAGS_concurrency) + "-write",
                            latency_write_hist_, reset);
    write_latency_and_reset(FLAGS_latency_file + std::to_string(FLAGS_concurrency) + "-user",
                            latency_user_timeline_hist_, reset);
    write_latency_and_reset(FLAGS_latency_file + std::to_string(FLAGS_concurrency) + "-home",
                            latency_home_timeline_hist_, reset);
}

void leader_thread_func() {
    erpc::Nexus nexus(FLAGS_server_addr, FLAGS_numa_server_node, 0);

    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_PING_RESP), ping_resp_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_COMPOSE_POST_WRITE_RESP), compose_post_write_resp_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_WRITE_RESP), compose_post_write_resp_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_READ_RESP), user_timeline_read_resp_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP), home_timeline_read_resp_handler);
    if (sn_domain::enabled()) nexus.register_req_func(
        static_cast<uint8_t>(RPC_TYPE::RPC_DOMAIN_USER_TIMELINE_READ_RESP), domain_user_timeline_read_resp_handler);
    if (sn_domain::enabled()) nexus.register_req_func(
        static_cast<uint8_t>(RPC_TYPE::RPC_DOMAIN_HOME_TIMELINE_READ_RESP), domain_home_timeline_read_resp_handler);


    std::vector<std::thread> clients;
    std::vector<std::thread> servers;

    auto *context = new AppContext();
    domain_origins = context->client_contexts_;

    clients.emplace_back(client_thread_func, 0, context->client_contexts_[0], &nexus);
    sleep(2);
    erpc::bind_to_core(clients[0], FLAGS_numa_client_node,
                       app_client_bind_core(0));

    for (size_t i = 1; i < FLAGS_client_num; i++) {
        clients.emplace_back(client_thread_func, i, context->client_contexts_[i], &nexus);

        erpc::bind_to_core(clients[i], FLAGS_numa_client_node,
                           app_client_bind_core(i));
    }

    for (size_t i = 0; i < FLAGS_server_num; i++) {
        servers.emplace_back(server_thread_func, i, context->server_contexts_[i], &nexus);

        erpc::bind_to_core(servers[i], FLAGS_numa_server_node,
                           app_server_bind_core(i));
    }
#if defined(RMEM_PROGRAM)
    std::vector<std::thread> readers;
    for (size_t i = 0; i < FLAGS_server_num; i++) {
        readers.emplace_back(reader_thread_func, i, context->client_contexts_[i]->queue_store);

        erpc::bind_to_core(readers[i], FLAGS_numa_server_node,
                           app_worker_bind_core(i));
    }
#endif
    sleep(3);


    if (!sn_measure::enabled()) {
    for (size_t i = 0; i < FLAGS_client_num; i++) {
        context->client_contexts_[i]->queue_store->PushPingReq();
    }

    for (size_t i = 0; i < FLAGS_server_num; i++) {
        // connect success
        RESP_MSG msg [[maybe_unused]] = context->server_contexts_[i]->queue_store->PopResp();
        // // printf("server %zu: status %d, req_id %u\n", i, msg.status, msg.req_id);
        my_assert(msg.status == 0 && msg.req_id == 0, "server connect failed");
    }

    // // printf("ping ready, begin to generate param");


    while (rmems_init_number != FLAGS_client_num) {
        for (size_t i = 0; i < FLAGS_client_num; i++) {
            context->client_contexts_[i]->queue_store->PushParamReq();
        }
        sleep(3);
    }

    // // printf("param ready, sleep 2 and begin to generate workload");
    sleep(2);
    }
    if (const char *start_file = std::getenv("ERPC_SN_CLIENT_START_FILE")) {
        if (!*start_file) sn_domain::fail("client", 0, "start-file-empty", 0, EINVAL);
        std::fprintf(stderr, "SN_CLIENT_ARMED source=%u\n", sn_domain::source_id());
        const uint64_t deadline = sn_domain::now_ns() + UINT64_C(120000000000);
        while (access(start_file, F_OK) != 0) {
            if (ctrl_c_pressed || sn_domain::now_ns() >= deadline)
                sn_domain::fail("client", 0, "start-barrier-timeout", 0, ETIMEDOUT);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::fprintf(stderr, "SN_CLIENT_STARTED source=%u at_ns=%llu\n", sn_domain::source_id(),
                     static_cast<unsigned long long>(sn_domain::now_ns()));
    }
    const uint64_t campaign_begin_ns = sn_domain::now_ns();
    uint64_t campaign_end_ns = 0;
    for (size_t i = 0; i < FLAGS_client_num; i++) {
        size_t tmp = FLAGS_concurrency;
        while (tmp--) {
            context->client_contexts_[i]->queue_store->PushNextReq();
        }
    }

    if ((sn_domain::enabled() || sn_measure::rpc_baseline()) && sn_domain::boolean_env("ERPC_SN_DOMAIN_FINISH")) {
        const uint64_t seconds = FLAGS_timeout_second == UINT64_MAX ? 120 : FLAGS_timeout_second;
        if (!seconds || seconds > 3600) sn_domain::fail("client", 0, "finish-timeout-range", 0, EINVAL);
        const uint64_t deadline = sn_domain::now_ns() + seconds * UINT64_C(1000000000);
        for (;;) {
            bool finished = true;
            for (auto *client : context->client_contexts_)
                if (client->domain_completed.load(std::memory_order_acquire) != static_cast<uint64_t>(generate_data_num)) finished = false;
            if (finished) {
                for (auto *client : context->client_contexts_)
                    campaign_end_ns = std::max(campaign_end_ns,
                        client->domain_last_done_ns.load(std::memory_order_acquire));
                break;
            }
            if (ctrl_c_pressed || sn_domain::now_ns() >= deadline)
                sn_domain::fail("client", 0, "finish-incomplete", 0, ETIMEDOUT);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Keep receiver event loops alive to publish the last response ACKs.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        for (auto *client : context->client_contexts_)
            for (const auto &slot : client->domain_inflight)
                if (slot.load(std::memory_order_acquire)) sn_domain::fail("client", 0, "finish-inflight", 0, EBUSY);
        ctrl_c_pressed = true;
    } else if (FLAGS_timeout_second != UINT64_MAX) {
        sleep(FLAGS_timeout_second);
        ctrl_c_pressed = true;
        write_overall_latency(false);
    }

    for (size_t i = 0; i < FLAGS_client_num; i++) {
        clients[i].join();
    }
    for (size_t i = 0; i < FLAGS_server_num; i++) {
        servers[i].join();
    }
    if ((sn_domain::enabled() || sn_measure::rpc_baseline()) && sn_domain::boolean_env("ERPC_SN_DOMAIN_FINISH")) {
        uint64_t completed = 0, nonempty = 0, empty = 0;
        for (auto *client : context->client_contexts_) {
            completed += client->domain_completed.load(); nonempty += client->domain_fixture_nonempty.load();
            empty += client->domain_fixture_empty.load();
        }
        const uint64_t expected = static_cast<uint64_t>(generate_data_num) * FLAGS_client_num;
        const uint64_t measured = expected - static_cast<uint64_t>(sn_measure::warmup()) * FLAGS_client_num;
        const hdr_histogram *read_hist = sn_domain::read_service() == sn_domain::ReadService::Home ?
            latency_home_timeline_hist_ : latency_user_timeline_hist_;
        if (completed != expected || (!sn_measure::enabled() && static_cast<uint64_t>(read_hist->total_count) != measured))
            sn_domain::fail("client", 0, "finish-count", 0, EPROTO);
        for (auto *client : context->client_contexts_)
            if (!client->domain_measurement.report(sn_domain::source_id(), client->client_id_))
                sn_domain::fail("client", 0, "measurement-incomplete", 0, EPROTO);
        const sn_measure::Statistics stats = context->client_contexts_[0]->domain_measurement.statistics(true);
        const bool single_clock = sn_measure::enabled();
        if (single_clock && stats.values.size() != measured) sn_domain::fail("client", 0, "measurement-count", 0, EPROTO);
        std::fprintf(stderr, "SN_DOMAIN_E2E source=%u completed=%llu expected=%llu nonempty=%llu empty=%llu fixture=%u p50_ns=%.1f p99_ns=%.1f avg_ns=%.1f scope=call-to-consume fixture_validation_included=%u clock=%s cold_included=%u warmup=%u measured=%llu campaign_begin_ns=%llu campaign_end_ns=%llu\n",
            sn_domain::source_id(), static_cast<unsigned long long>(completed), static_cast<unsigned long long>(expected),
            static_cast<unsigned long long>(nonempty), static_cast<unsigned long long>(empty),
            sn_domain::boolean_env("ERPC_SN_DOMAIN_FIXTURE") ? 1U : 0U,
            single_clock ? static_cast<double>(stats.percentile(50)) : static_cast<double>(hdr_value_at_percentile(latency_user_timeline_hist_, 50)) * 100.0,
            single_clock ? static_cast<double>(stats.percentile(99)) : static_cast<double>(hdr_value_at_percentile(latency_user_timeline_hist_, 99)) * 100.0,
            single_clock ? static_cast<double>(stats.total)/static_cast<double>(measured) : hdr_mean(latency_user_timeline_hist_) * 100.0,
            sn_domain::boolean_env("ERPC_SN_DOMAIN_FIXTURE") ? 1U : 0U,
            single_clock ? "steady_ns" : "high_resolution_us",
            sn_measure::warmup() ? 0U : 1U, sn_measure::warmup(), static_cast<unsigned long long>(measured),
            static_cast<unsigned long long>(campaign_begin_ns),
            static_cast<unsigned long long>(campaign_end_ns));
    }
    if (!sn_measure::enabled()) write_overall_latency();

#if defined(RMEM_PROGRAM)
    for (size_t i = 0;i < FLAGS_server_num; i++) {
        readers[i].join();
    }
#endif
}

int main(int argc, char **argv) {
    signal(SIGINT, ctrl_c_handler);
    signal(SIGTERM, ctrl_c_handler);
    // only config_file is required!!!
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    init_service_config(FLAGS_config_file, "client");
    init_specific_config();

    int ret [[maybe_unused]] = hdr_init(1, 1000 * 1000 * 10, 3,
        &latency_write_hist_);
    my_assert(ret == 0, "hdr_init failed");
    ret = hdr_init(1, 1000 * 1000 * 10, 3,
        &latency_user_timeline_hist_);
    my_assert(ret == 0, "hdr_init failed");
    ret = hdr_init(1, 1000 * 1000 * 10, 3,
        &latency_home_timeline_hist_);
    my_assert(ret == 0, "hdr_init failed");

    // CXL shared memory is initialized by eRPC automatically

    std::thread leader_thread(leader_thread_func);
    erpc::bind_to_core(leader_thread, FLAGS_numa_server_node,
                       get_bind_core(FLAGS_numa_server_node));
    leader_thread.join();
    if (!sn_diag::report("client")) sn_domain::fail("client", 0, "diagnostic-overflow", 0, ENOSPC);


    hdr_close(latency_write_hist_);
    hdr_close(latency_user_timeline_hist_);
    hdr_close(latency_home_timeline_hist_);
}
