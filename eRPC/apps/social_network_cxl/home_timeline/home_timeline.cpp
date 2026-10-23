#include <thread>

#include "../utils_mongodb.h"

#include "home_timeline.h"
#include "../ub_breakdown.h"


void connect_sessions(ClientContext *c)
{

    // connect to backward server
    if (!sn_domain::enabled()) {
        if (!sn_measure::rpc_baseline()) {
            c->compose_post_session_number = c->rpc_->create_session(compose_post_addr, get_remote_rpc_id(compose_post_addr));
            my_assert(c->compose_post_session_number >= 0);
        }
        c->nginx_session_number = c->rpc_->create_session(nginx_addr, get_remote_rpc_id(nginx_addr));
        my_assert(c->nginx_session_number >= 0);
    }

    // c->client_session_number = c->rpc_->create_session(client_addr, get_remote_rpc_id(client_addr));
    // my_assert(c->client_session_number >= 0);

    c->post_storage_session_number = c->rpc_->create_session(post_storage_addr, get_remote_rpc_id(post_storage_addr));
    my_assert(c->post_storage_session_number >= 0);

    // while (c->num_sm_resps_ != 4)
    size_t domain_sessions = 0;
    if (sn_domain::enabled()) {
        c->domain_return_sessions.fill(-1);
        for (const auto &client : sn_domain::clients())
            for (uint32_t i = 0; i < FLAGS_server_num; ++i) {
                const uint32_t receiver = client.rpc_id + i;
                const int session = c->rpc_->create_session(client.address, static_cast<uint8_t>(receiver));
                if (session < 0) sn_domain::fail("home_timeline", static_cast<uint32_t>(c->client_id_),
                                                "create-client-session", 0, EHOSTUNREACH);
                c->domain_return_sessions[receiver] = session;
                ++domain_sessions;
            }
    }
    const uint64_t deadline = sn_domain::enabled() ? sn_domain::now_ns() + UINT64_C(30000000000) : 0;
    while (c->num_sm_resps_ != (sn_domain::enabled() ? 1 : (sn_measure::rpc_baseline() ? 2 : 3)) + domain_sessions)
    {
        // // printf("[home_timeline] Waiting for sessions... num_sm_resps_=%zu\n", c->num_sm_resps_);
        c->rpc_->run_event_loop(kAppEvLoopMs);
        if (deadline && sn_domain::now_ns() >= deadline)
            sn_domain::fail("home_timeline", static_cast<uint32_t>(c->client_id_), "session-ready-timeout", 0, ETIMEDOUT);
        if (unlikely(ctrl_c_pressed == 1))
        {
            // // printf("Ctrl-C pressed. Exiting\n");
            return;
        }
    }
    // // printf("[home_timeline] All sessions established! num_sm_resps_=%zu\n", c->num_sm_resps_);
}
void domain_ready_handler(erpc::ReqHandle *handle, void *context) {
    auto *ctx = static_cast<ServerContext *>(context);
    const auto *buffer = handle->get_req_msgbuf();
    invalidate_msgbuf_before_read(ctx->rpc_, *buffer);
    if (buffer->get_data_size() != sizeof(RPCMsgReq<PingRPCReq>))
        sn_domain::fail("home_timeline", static_cast<uint32_t>(ctx->server_id_), "ready-size", 0, EPROTO);
    RPCMsgReq<PingRPCReq> request(RPC_TYPE::RPC_DOMAIN_READY, 0, {});
    std::memcpy(&request, buffer->buf_, sizeof(request));
    const bool ready = ctx->mongodb_init_finished.load(std::memory_order_acquire) &&
        ctx->domain_sender_ready->load(std::memory_order_acquire);
    const int status = request.req_common.type != RPC_TYPE::RPC_DOMAIN_READY || request.req_common.req_number != 0 ||
        request.req_control.timestamp != sn_domain::kWireAbi ? EPROTO : (ready ? 0 : EAGAIN);
    new (handle->pre_resp_msgbuf_.buf_) RPCMsgResp<PingRPCResp>(RPC_TYPE::RPC_DOMAIN_READY, 0, status, {sn_domain::kWireAbi});
    ctx->rpc_->resize_msg_buffer(&handle->pre_resp_msgbuf_, sizeof(RPCMsgResp<PingRPCResp>));
    ctx->rpc_->enqueue_response(handle, &handle->pre_resp_msgbuf_);
}

void ping_handler(erpc::ReqHandle *req_handle, void *_context)
{
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_ping_tot++;
    auto *req_msgbuf = req_handle->get_req_msgbuf();
    my_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<PingRPCReq>));

    auto *req = reinterpret_cast<RPCMsgReq<PingRPCReq> *>(req_msgbuf->buf_);

    new (req_handle->pre_resp_msgbuf_.buf_) RPCMsgResp<PingRPCResp>(req->req_common.type, req->req_common.req_number, 0, {req->req_control.timestamp});
    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, sizeof(RPCMsgResp<PingRPCResp>));

    ctx->init_mutex.lock();
    if(ctx->mongodb_init_finished.load(std::memory_order_acquire)){
        ctx->forward_all_mpmc_queue->push(pin_msgbuf(ctx->rpc_, *req_msgbuf));
        
    } else {
        ctx->is_pinged = true;
        ctx->ping_req_number = req->req_common.req_number;
    }
    ctx->init_mutex.unlock();

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void home_timeline_write_req_handler(erpc::ReqHandle *req_handle, void *_context)
{
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_home_timeline_write_req_tot++;

    if (unlikely(!ctx->mongodb_init_finished.load(
            std::memory_order_acquire))) {
        throw std::runtime_error("home_timeline: MongoDB is not initialized");
    }

    auto *req_msgbuf = req_handle->get_req_msgbuf();
    invalidate_msgbuf_before_read(ctx->rpc_, *req_msgbuf);
    auto *req = reinterpret_cast<RPCMsgReq<PostStorageWriteCXLReq> *>(req_msgbuf->buf_);

    my_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<PostStorageWriteCXLReq>));

    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, 0);

//    if (likely(ctx->req_backward_msgbuf_ptr[req->req_common.req_number % kAppMaxBuffer].buf_ != nullptr))
//    {
//        ctx->rpc_->free_msg_buffer(ctx->req_backward_msgbuf_ptr[req->req_common.req_number % kAppMaxBuffer]);
//    }

    ctx->forward_all_mpmc_queue->push(pin_msgbuf(ctx->rpc_, *req_msgbuf));

    

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);

}

void home_timeline_read_req_handler(erpc::ReqHandle *req_handle, void *_context)
{
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_home_timeline_read_req_tot++;

    if (unlikely(!ctx->mongodb_init_finished.load(
            std::memory_order_acquire))) {
        throw std::runtime_error("home_timeline: MongoDB is not initialized");
    }

    auto *req_msgbuf = req_handle->get_req_msgbuf();
    invalidate_msgbuf_before_read(ctx->rpc_, *req_msgbuf);
    auto *req = reinterpret_cast<RPCMsgReq<HomeTimeLineReq> *>(req_msgbuf->buf_);
    const uint32_t req_number = req->req_common.req_number;
    sn_profile::flow_point("home_timeline", req_number, "request_arrival");

    my_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<HomeTimeLineReq>));

    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, 0);

//    if (likely(ctx->req_forward_msgbuf_ptr[req->req_common.req_number % kAppMaxBuffer].buf_ != nullptr))
//    {
//        ctx->rpc_->free_msg_buffer(ctx->req_forward_msgbuf_ptr[req->req_common.req_number % kAppMaxBuffer]);
//    }

    const uint64_t pin_start = sn_profile::start();
    const sn_slow_trace::Stamp copy_trace = sn_slow_trace::start();
#ifdef ERPC_UB
    erpc::MsgBuffer pinned = clone_msgbuf_with_header(ctx->rpc_, *req_msgbuf);
    sn_profile::record(sn_profile::Stage::kTimelineRxCopy, pin_start);
#else
    erpc::MsgBuffer pinned = pin_msgbuf(ctx->rpc_, *req_msgbuf);
    sn_profile::record(sn_profile::Stage::kTimelineRxPin, pin_start);
#endif
    sn_slow_trace::record("timeline_rx_copy", req_number, copy_trace);
    uint64_t queue_start = sn_profile::start();
#ifdef ERPC_UB
    if (queue_start == 0 && sn_slow_trace::enabled())
        queue_start = sn_slow_trace::wall_now_ns();
#endif
    const sn_slow_trace::Stamp queue_trace = sn_slow_trace::start();
    if (queue_start != 0) {
        const size_t slot = req_number % kAppMaxBuffer;
        ctx->read_queue_start_ns[slot].store(queue_start,
                                              std::memory_order_relaxed);
    }
    ctx->forward_all_mpmc_queue->push(pinned);
    sn_slow_trace::record("timeline_rx_queue", req_number, queue_trace);
    sn_profile::record(sn_profile::Stage::kTimelineRxQueue, queue_start);

    

    const uint64_t ack_start = sn_profile::start();
    const sn_slow_trace::Stamp ack_trace = sn_slow_trace::start();
    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
    sn_slow_trace::record("timeline_rx_ack", req_number, ack_trace);
    sn_profile::record(sn_profile::Stage::kTimelineRxAck, ack_start);

}
void domain_home_timeline_read_req_handler(erpc::ReqHandle *handle, void *context) {
    auto *ctx = static_cast<ServerContext *>(context);
    const uint64_t received = sn_domain::metrics().start();
    if (!sn_domain::enabled() || !ctx->mongodb_init_finished.load(std::memory_order_acquire))
        sn_domain::fail("home_timeline", static_cast<uint32_t>(ctx->server_id_), "domain-receive-not-ready", 0, EAGAIN);
    const auto *buffer = handle->get_req_msgbuf();
    invalidate_msgbuf_before_read(ctx->rpc_, *buffer);
    if (buffer->get_data_size() != sizeof(RPCMsgReq<DomainHomeRequest>))
        sn_domain::fail("home_timeline", static_cast<uint32_t>(ctx->server_id_), "domain-request-size", 0, EPROTO);
    RPCMsgReq<DomainHomeRequest> incoming(RPC_TYPE::RPC_DOMAIN_HOME_TIMELINE_READ_REQ, 0, {});
    std::memcpy(&incoming, buffer->buf_, sizeof(incoming));
    const uint32_t wire = incoming.req_common.req_number;
    sn_profile::flow_point("home_timeline", wire, "request_arrival");
    if (incoming.req_common.type != RPC_TYPE::RPC_DOMAIN_HOME_TIMELINE_READ_REQ ||
        !sn_domain::origin_valid(incoming.req_control.origin, wire))
        sn_domain::fail("home_timeline", static_cast<uint32_t>(ctx->server_id_), "domain-request-origin", wire, EPROTO);
    const size_t slot = wire % kAppMaxBuffer;
    ctx->domain_pending[slot].store(wire, incoming.req_control.origin);
    erpc::MsgBuffer local = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<HomeTimeLineReq>));
    new (local.buf_) RPCMsgReq<HomeTimeLineReq>(RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ, wire,
                                                   incoming.req_control.request);
    local.set_hdr_req_type(static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ));
    local.set_hdr_req_num(wire);
    ctx->read_queue_start_ns[slot].store(sn_profile::start(), std::memory_order_relaxed);
    ctx->forward_all_mpmc_queue->push(local);
    ++ctx->stat_req_home_timeline_read_req_tot;
    ctx->rpc_->resize_msg_buffer(&handle->pre_resp_msgbuf_, 0);
    ctx->rpc_->enqueue_response(handle, &handle->pre_resp_msgbuf_);
    if (sn_measure::measured(incoming.req_control.origin.original_request))
        sn_domain::metrics().finish(sn_domain::Stage::kReceive, received);
}

void post_storage_read_resp_handler(erpc::ReqHandle *req_handle, void *_context)
{
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_post_storage_read_resp_tot++;

    if (unlikely(!ctx->mongodb_init_finished.load(
            std::memory_order_acquire))) {
        throw std::runtime_error("home_timeline: MongoDB is not initialized");
    }

    auto *req_msgbuf = req_handle->get_req_msgbuf();
    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, 0);

    ctx->forward_all_mpmc_queue->push(pin_msgbuf(ctx->rpc_, *req_msgbuf));

    

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);

}

void callback_ping_resp(void *_context, void *_tag)
{
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);

    // erpc::MsgBuffer &req_msgbuf = ctx->req_backward_msgbuf[req_id];
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req_id];

    my_assert(resp_msgbuf.get_data_size() == sizeof(RPCMsgResp<PingRPCResp>));

    release_msgbuf(ctx->rpc_, ctx->req_backward_msgbuf[req_id]);
    ctx->req_backward_msgbuf[req_id].buf_ = nullptr;

    // PingResp *resp = reinterpret_cast<PingResp *>(resp_msgbuf.buf_);

    // 如果返回值不为0，则认为后续不会有响应，直接将请求号和错误码放入队列
    // 如果返回值为0，则认为后续将有响应，不care
    // if (resp->resp.status != 0)
    // {
    // TODO
    // }

    // TODO check
    // ctx->rpc_->free_msg_buffer(req_msgbuf);
}

void handler_ping_resp(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf)
{

    const size_t slot = req_msgbuf.get_hdr_req_num() % kAppMaxBuffer;

    require_empty_msgbuf_slot(ctx->req_backward_msgbuf[slot],
                              "home_timeline.req_backward_msgbuf", slot);
    ctx->req_backward_msgbuf[slot] = prepare_forward_msgbuf(ctx->rpc_, req_msgbuf);

    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[slot];

    ctx->rpc_->enqueue_request(ctx->nginx_session_number, static_cast<uint8_t>(RPC_TYPE::RPC_PING_RESP),
                               &ctx->req_backward_msgbuf[slot], &resp_msgbuf,
                               callback_ping_resp, reinterpret_cast<void *>(slot));
}

void callback_home_timeline_write_resp(void *_context, void *_tag)
{
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);

    erpc::MsgBuffer &req_msgbuf = ctx->req_backward_msgbuf[req_id];
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req_id];

    my_assert(resp_msgbuf.get_data_size() == 0);

    release_msgbuf(ctx->rpc_, req_msgbuf);
    req_msgbuf.buf_ = nullptr;
}

void handler_home_timeline_write_resp(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf)
{
    const size_t slot = req_msgbuf.get_hdr_req_num() % kAppMaxBuffer;
    require_empty_msgbuf_slot(ctx->req_backward_msgbuf[slot],
                              "home_timeline.req_backward_msgbuf", slot);
    ctx->req_backward_msgbuf[slot] = prepare_forward_msgbuf(ctx->rpc_, req_msgbuf);

    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[slot];

    flush_msgbuf_before_send(ctx->rpc_, ctx->req_backward_msgbuf[slot]);
    ctx->rpc_->enqueue_request(ctx->compose_post_session_number, static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_WRITE_RESP),
                               &ctx->req_backward_msgbuf[slot], &resp_msgbuf,
                               callback_home_timeline_write_resp, reinterpret_cast<void *>(slot));
}

void callback_home_timeline_read_resp(void *_context, void *_tag)
{
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);

    if (sn_domain::enabled()) {
        const auto *response = reinterpret_cast<const RPCMsgReq<DomainHomeResponse> *>(ctx->req_backward_msgbuf[req_id].buf_);
        const uint32_t wire = response->req_common.req_number;
        if (sn_measure::measured(response->req_control.origin.original_request))
            sn_domain::metrics().finish(sn_domain::Stage::kAck, ctx->domain_ack_start[req_id]);
        ctx->domain_ack_start[req_id] = 0;
        ctx->domain_pending[req_id].clear(wire);
    }

    // erpc::MsgBuffer &req_msgbuf = ctx->req_backward_msgbuf[req_id];
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req_id];

    my_assert(resp_msgbuf.get_data_size() == 0);

    release_msgbuf(ctx->rpc_, ctx->req_backward_msgbuf[req_id]);
    ctx->req_backward_msgbuf[req_id].buf_ = nullptr;

}

void handler_home_timeline_read_resp(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf)
{
    sn_profile::flow_point("home_timeline", req_msgbuf.get_hdr_req_num(), "storage_dispatch");
    if (sn_domain::enabled()) {
        if (req_msgbuf.get_data_size() != sizeof(RPCMsgReq<PostStorageReadCXLResp>))
            sn_domain::fail("home_timeline", static_cast<uint32_t>(ctx->client_id_), "result-size", 0, EPROTO);
        invalidate_msgbuf_before_read(ctx->rpc_, req_msgbuf);
        RPCMsgReq<PostStorageReadCXLResp> result(RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP, 0, {});
        std::memcpy(&result, req_msgbuf.buf_, sizeof(result));
        const uint32_t wire = result.req_common.req_number;
        const size_t slot = wire % kAppMaxBuffer;
        require_empty_msgbuf_slot(ctx->req_backward_msgbuf[slot], "home_timeline.domain_response", slot);
        const sn_domain::Origin origin = ctx->domain_pending[slot].get(wire);
        if (result.req_common.type != RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP ||
            result.req_control.count > 1 || !sn_domain::origin_valid(origin, wire))
            sn_domain::fail("home_timeline", static_cast<uint32_t>(ctx->client_id_), "result-identity", wire, EPROTO);
        const auto &post = result.req_control.post;
        sn_profile::flow_point("home_timeline", wire, "reverse_begin");
        ctx->domain_runtime.reverse(wire, origin, post.machine_id, post.block_offset,
                                    post.payload_offset, post.payload_length, sn_domain::ReadService::Home);
        sn_profile::flow_point("home_timeline", wire, "reverse_end");
        const int session = ctx->domain_return_sessions[origin.receiver_rpc];
        if (session < 0 || !ctx->rpc_->is_connected(session))
            sn_domain::fail("home_timeline", static_cast<uint32_t>(ctx->client_id_), "return-session", wire, ENOTCONN);
        erpc::MsgBuffer outgoing = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<DomainHomeResponse>));
        DomainHomeResponse response{}; response.origin = origin; response.response = result.req_control;
        new (outgoing.buf_) RPCMsgReq<DomainHomeResponse>(RPC_TYPE::RPC_DOMAIN_HOME_TIMELINE_READ_RESP, wire, response);
        outgoing.set_hdr_req_type(static_cast<uint8_t>(RPC_TYPE::RPC_DOMAIN_HOME_TIMELINE_READ_RESP));
        outgoing.set_hdr_req_num(wire);
        release_msgbuf(ctx->rpc_, req_msgbuf);
        ctx->req_backward_msgbuf[slot] = outgoing;
        flush_msgbuf_before_send(ctx->rpc_, outgoing);
        ctx->domain_ack_start[slot] = sn_measure::measured(origin.original_request) ? sn_domain::metrics().start() : 0;
        const uint64_t send = ctx->domain_ack_start[slot];
        ctx->rpc_->enqueue_request(session, static_cast<uint8_t>(RPC_TYPE::RPC_DOMAIN_HOME_TIMELINE_READ_RESP),
            &ctx->req_backward_msgbuf[slot], &ctx->resp_backward_msgbuf[slot],
            callback_home_timeline_read_resp, reinterpret_cast<void *>(slot));
        sn_profile::flow_point("home_timeline", wire, "response_enqueue");
        sn_domain::metrics().finish(sn_domain::Stage::kSend, send);
        return;
    }
    const size_t slot = req_msgbuf.get_hdr_req_num() % kAppMaxBuffer;
    require_empty_msgbuf_slot(ctx->req_backward_msgbuf[slot],
                              "home_timeline.req_backward_msgbuf", slot);
    ctx->req_backward_msgbuf[slot] = prepare_forward_msgbuf(ctx->rpc_, req_msgbuf);

    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[slot];

    // ctx->rpc_->enqueue_request(ctx->client_session_number, static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP),
    //                            &ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer], &resp_msgbuf,
    //                            callback_home_timeline_read_resp, reinterpret_cast<void *>(req->req_common.req_number % kAppMaxBuffer));
#if defined(USE_NO_CC_QUEUE) && !defined(USE_ONE_SIDE_READ)
    if (msgbuf_owner_rpc_id(ctx->rpc_, ctx->req_backward_msgbuf[slot]) ==
        ctx->rpc_->get_rpc_id()) {
        flush_msgbuf_before_send(ctx->rpc_, ctx->req_backward_msgbuf[slot]);
    }
#endif
    ctx->rpc_->enqueue_request(ctx->nginx_session_number, static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP),
                               &ctx->req_backward_msgbuf[slot], &resp_msgbuf,
                               callback_home_timeline_read_resp, reinterpret_cast<void *>(slot));
    sn_profile::flow_point("home_timeline", req_msgbuf.get_hdr_req_num(), "response_enqueue");

}

void callback_post_storage_read_req(void *_context, void *_tag)
{
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);

    erpc::MsgBuffer &req_msgbuf = ctx->req_forward_msgbuf[req_id];
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_forward_msgbuf[req_id];
    const auto *storage_req =
        reinterpret_cast<const CommonReq *>(req_msgbuf.buf_);
    sn_profile::flow_point("home_timeline", storage_req->req_number, "storage_callback");
    sn_handoff::record(sn_profile::Stage::kTimelineStorageRtt,
                       "timeline_storage_rtt", storage_req->req_number,
                       ctx->post_storage_start_ns[req_id]);
    ctx->post_storage_start_ns[req_id] = 0;

    my_assert(resp_msgbuf.get_data_size() ==
              sizeof(RPCMsgResp<PostStorageReadCXLResp>));
    auto *resp = reinterpret_cast<RPCMsgResp<PostStorageReadCXLResp> *>(
        resp_msgbuf.buf_);

    const uint64_t build_start = sn_profile::start();
    erpc::MsgBuffer result = ctx->rpc_->alloc_msg_buffer_or_die(
        sizeof(RPCMsgReq<PostStorageReadCXLResp>));
    new (result.buf_) RPCMsgReq<PostStorageReadCXLResp>(
        RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP,
        resp->resp_common.req_number, resp->resp_control);
    result.set_hdr_req_type(
        static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP));
    result.set_hdr_req_num(resp->resp_common.req_number);
    sn_profile::record(sn_profile::Stage::kTimelineCallbackBuild, build_start);

    const uint64_t release_start = sn_profile::start();
    release_msgbuf(ctx->rpc_, req_msgbuf);
    req_msgbuf.buf_ = nullptr;
    sn_profile::record(sn_profile::Stage::kTimelineCallbackRelease,
                       release_start);
#ifdef ERPC_UB
    // The PostStorage continuation runs on this client RPC thread. Forward
    // directly instead of handing the result back to the same thread.
    const uint64_t forward_start = sn_profile::start();
    handler_home_timeline_read_resp(ctx, result);
    sn_profile::record(sn_profile::Stage::kTimelineCallbackForward,
                       forward_start);
#else
    ctx->backward_mpmc_queue->push(result);
#endif
}

void handler_post_storage_read_req(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf)
{
    const size_t slot = req_msgbuf.get_hdr_req_num() % kAppMaxBuffer;
    require_empty_msgbuf_slot(ctx->req_forward_msgbuf[slot],
                              "home_timeline.req_forward_msgbuf", slot);
    const uint64_t prepare_start = sn_profile::start();
    ctx->req_forward_msgbuf[slot] = prepare_forward_msgbuf(ctx->rpc_, req_msgbuf);
    sn_profile::record(sn_profile::Stage::kTimelineTxPrepare, prepare_start);

    erpc::MsgBuffer &resp_msgbuf = ctx->resp_forward_msgbuf[slot];

    flush_msgbuf_before_send(ctx->rpc_, ctx->req_forward_msgbuf[slot]);
    const uint64_t enqueue_start = sn_handoff::start();
    ctx->post_storage_start_ns[slot] = enqueue_start;
    ctx->rpc_->enqueue_request(ctx->post_storage_session_number, static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_READ_REQ),
                               &ctx->req_forward_msgbuf[slot], &resp_msgbuf,
                               callback_post_storage_read_req, reinterpret_cast<void *>(slot));
    sn_profile::flow_point("home_timeline", req_msgbuf.get_hdr_req_num(), "storage_enqueue");
    sn_profile::record(sn_profile::Stage::kTimelineTxEnqueue, enqueue_start);

}

void client_thread_func(size_t thread_id, ClientContext *ctx, erpc::Nexus *nexus)
{
    ctx->client_id_ = thread_id;
    sn_domain::metrics().configure("home_timeline-send", static_cast<uint32_t>(thread_id), 0);
    sn_domain::pin_worker("home_timeline", static_cast<uint32_t>(thread_id));
    std::vector<size_t> port_vec = flags_get_cxl_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

    uint8_t rpc_id = FLAGS_rpc_id + 20 + thread_id;

    AppRpc rpc(nexus, static_cast<void *>(ctx),
                                    rpc_id,
                                    basic_sm_handler_client, phy_port);
    rpc.retry_connect_on_invalid_rpc_id_ = true;
    ctx->rpc_ = &rpc;
    for (auto & i : ctx->resp_backward_msgbuf)
    {
        i = rpc.alloc_msg_buffer_or_die(sizeof(RPCMsgResp<CommonRPCResp>));
    }

    for(auto & i : ctx->resp_forward_msgbuf)
    {
        i = rpc.alloc_msg_buffer_or_die(
            sizeof(RPCMsgResp<PostStorageReadCXLResp>));
    }

    connect_sessions(ctx);
    if (sn_domain::enabled()) {
        ctx->domain_runtime.open("home_timeline", static_cast<uint32_t>(thread_id), sn_domain::proxy_count());
        ctx->domain_sender_ready.store(true, std::memory_order_release);
    }
    if (sn_measure::rpc_baseline()) ctx->domain_sender_ready.store(true, std::memory_order_release);

    while (true)
    {
        erpc::MsgBuffer req_msg;
        for (size_t i = 0;
             i < kAppMaxBuffer &&
             ctx->forward_mpmc_queue->try_pop(req_msg);
             i++) {
            if (sn_profile::enabled()) {
                const size_t slot = req_msg.get_hdr_req_num() % kAppMaxBuffer;
                sn_profile::record(
                    sn_profile::Stage::kTimelineForwardQueueHandoff,
                    ctx->forward_queue_start_ns[slot].exchange(
                        0, std::memory_order_relaxed));
            }
            const auto req_type = static_cast<RPC_TYPE>(req_msg.get_hdr_req_type());
            my_assert(req_type == RPC_TYPE::RPC_POST_STORAGE_READ_REQ);
            handler_post_storage_read_req(ctx, req_msg);
        }

        for (size_t i = 0;
             i < kAppMaxBuffer &&
             ctx->backward_mpmc_queue->try_pop(req_msg);
             i++) {
            const auto req_type = static_cast<RPC_TYPE>(req_msg.get_hdr_req_type());

            my_assert(req_type == RPC_TYPE::RPC_PING_RESP || req_type == RPC_TYPE::RPC_HOME_TIMELINE_WRITE_RESP ||
                            req_type == RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP
                    );
            switch (req_type) {
                case RPC_TYPE::RPC_PING_RESP:
                    handler_ping_resp(ctx, req_msg);
                    break;
                case RPC_TYPE::RPC_HOME_TIMELINE_WRITE_RESP:
                    handler_home_timeline_write_resp(ctx, req_msg);
                    break;
                case RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP:
                    handler_home_timeline_read_resp(ctx, req_msg);
                    break;
                default:
                    my_assert(false);
            }
        }
        ctx->rpc_->run_event_loop_once();
        if (unlikely(ctrl_c_pressed))
        {
            break;
        }
    }
    ctx->domain_runtime.close();
    sn_domain::metrics().report();
}

void server_thread_func(size_t thread_id, ServerContext *ctx, erpc::Nexus *nexus)
{
    ctx->server_id_ = thread_id;
    sn_domain::metrics().configure("home_timeline-receive", static_cast<uint32_t>(thread_id), 0);
    std::vector<size_t> port_vec = flags_get_cxl_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

    uint8_t rpc_id = FLAGS_rpc_id + thread_id;

    AppRpc rpc(nexus, static_cast<void *>(ctx),
                                    rpc_id,
                                    basic_sm_handler_server, phy_port);
    rpc.retry_connect_on_invalid_rpc_id_ = true;
    ctx->rpc_ = &rpc;

    while (true)
    {
        ctx->reset_stat();
        erpc::ChronoTimer start;
        start.reset();
        rpc.run_event_loop(kAppEvLoopMs);
        const double seconds = start.get_sec();
        // printf("thread %zu: ping_req : %.2f, home_timeline write: %.2f read: %.2f, post_storage read: %.2f \n", thread_id, ctx->stat_req_ping_tot / seconds, ctx->stat_req_home_timeline_write_req_tot / seconds, ctx->stat_req_home_timeline_read_req_tot/ seconds, ctx->stat_req_post_storage_read_resp_tot / seconds);

        ctx->rpc_->reset_dpath_stats();
        // more handler
        if (ctrl_c_pressed == 1)
        {
            break;
        }
    }
    sn_domain::metrics().report();
}
void worker_thread_func(size_t thread_id, MPMC_QUEUE *producer, MPMC_QUEUE *consumer_back, MPMC_QUEUE *consumer_fwd, AppRpc *rpc_, AppRpc *server_rpc_, std::atomic<uint64_t> *read_queue_start_ns, std::atomic<uint64_t> *forward_queue_start_ns)
{
    link_worker_cacheable(server_rpc_->get_rpc_id());
    // // printf("[home_timeline] worker_thread_func %zu: STARTED, producer=%p\n", thread_id, (void*)producer);
    _unused(thread_id);
    while (true)
    {
        erpc::MsgBuffer req_msg;
        for (size_t i = 0;
             i < kAppMaxBuffer && producer->try_pop(req_msg);
             i++) {
            uint8_t *req_data = req_msg.buf_;
            const auto req_type = static_cast<RPC_TYPE>(req_msg.get_hdr_req_type());
            // // printf("[home_timeline] worker_thread_func: got msg type=%u\n", static_cast<uint32_t>(req->type));
            my_assert(req_type == RPC_TYPE::RPC_PING || req_type == RPC_TYPE::RPC_HOME_TIMELINE_WRITE_REQ ||
                            req_type == RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ || req_type == RPC_TYPE::RPC_POST_STORAGE_READ_RESP);

            if(req_type == RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ){
                sn_profile::flow_point("home_timeline", req_msg.get_hdr_req_num(), "worker_begin");
                if (sn_profile::enabled() || sn_slow_trace::enabled()) {
                    const auto *read_req =
                        reinterpret_cast<const RPCMsgReq<HomeTimeLineReq> *>(
                            req_data);
                    const size_t slot =
                        read_req->req_common.req_number % kAppMaxBuffer;
                    const uint64_t started_ns =
                        read_queue_start_ns[slot].exchange(
                            0, std::memory_order_relaxed);
                    sn_slow_trace::record_handoff("timeline_queue_handoff",
                                                  read_req->req_common.req_number,
                                                  started_ns);
                    sn_profile::record(sn_profile::Stage::kTimelineQueueHandoff,
                                       started_ns);
                }
                const uint64_t lookup_start = sn_profile::start();
                const sn_slow_trace::Stamp lookup_trace = sn_slow_trace::start();
                read_home_time_line_post_details(req_data,rpc_,consumer_fwd,consumer_back,forward_queue_start_ns);
                sn_profile::flow_point("home_timeline", req_msg.get_hdr_req_num(), "worker_done");
                const auto *read_req =
                    reinterpret_cast<const RPCMsgReq<HomeTimeLineReq> *>(
                        req_data);
                sn_slow_trace::record("timeline_worker_lookup_queue",
                                      read_req->req_common.req_number,
                                      lookup_trace);
                sn_profile::record(sn_profile::Stage::kTimelineWorkerLookupQueue,
                                   lookup_start);
                const uint64_t release_start = sn_profile::start();
                release_msgbuf(server_rpc_, req_msg);
                sn_profile::record(sn_profile::Stage::kTimelineWorkerRelease,
                                   release_start);
            } else if(req_type == RPC_TYPE::RPC_HOME_TIMELINE_WRITE_REQ){
                write_home_timeline_and_return(req_msg.buf_,rpc_,consumer_back);
                release_msgbuf(server_rpc_, req_msg);
            } else if(req_type == RPC_TYPE::RPC_POST_STORAGE_READ_RESP){
                erpc::MsgBuffer local = prepare_forward_msgbuf(rpc_, req_msg);
                local.set_hdr_req_type(static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP));
                consumer_back->push(local);
            } else {
                erpc::MsgBuffer local = prepare_forward_msgbuf(rpc_, req_msg);
                auto *req = reinterpret_cast<CommonReq *>(local.buf_);
                req->type = RPC_TYPE::RPC_PING_RESP;
                local.set_hdr_req_type(static_cast<uint8_t>(RPC_TYPE::RPC_PING_RESP));
                consumer_back->push(local);
            }
        }
        if (ctrl_c_pressed == 1)
        {
            break;
        }
    }
    unlink_worker_cacheable();
}

void mongodb_init(AppContext *ctx){
    mongodb_client_pool = init_mongodb_client_pool(config_json_all, "user_timeline", mongodb_conns_num);
    mongoc_client_t *mongodb_client =  mongoc_client_pool_pop(mongodb_client_pool);
    if (!mongodb_client) mongodb_load_failure("home_timeline", "client-create");

    auto collection = mongoc_client_get_collection(mongodb_client, "user-timeline", "user-timeline");

    if (!collection) mongodb_load_failure("home_timeline", "collection-create");

    bson_t* query = bson_new();
    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(collection, query, nullptr, nullptr);
    if (!cursor) mongodb_load_failure("home_timeline", "cursor-create");

    const bson_t *doc;

    while(mongoc_cursor_next(cursor,&doc)) {
        bson_iter_t iter;

        if (bson_iter_init_find(&iter, doc, "posts")) {
            // get the values from array posts, and push them into post_ids
            bson_iter_t array_iter;
            bson_iter_recurse(&iter, &array_iter);
            while (bson_iter_next(&array_iter)) {
                bson_iter_t  post_iter;
                bson_iter_recurse(&array_iter, &post_iter);
                while(bson_iter_next(&post_iter)) {
                    if(BSON_ITER_IS_KEY(&post_iter, "post_id") && BSON_ITER_HOLDS_INT64(&post_iter)){
                        post_ids_queue.push(bson_iter_value(&post_iter)->value.v_int64);
                    }
                }
            }
        }

        if(ctrl_c_pressed){
            return;
        }
    }

    require_mongodb_query_success(cursor, "home_timeline");
    bson_destroy(query);
    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(collection);
    mongoc_client_pool_push(mongodb_client_pool, mongodb_client);

    for(auto item : ctx->server_contexts_){
        item->init_mutex.lock();
        if(item->is_pinged){
            auto _buf = item->rpc_->alloc_msg_buffer(sizeof(RPCMsgReq<PingRPCReq>));
            auto *req = reinterpret_cast<RPCMsgReq<PingRPCReq> *>(_buf.buf_);
            req->req_common.type = RPC_TYPE::RPC_PING;
            req->req_common.req_number = item->ping_req_number;
            req->req_control.timestamp = 0;

            item->forward_all_mpmc_queue->push(_buf);
        }


        item->mongodb_init_finished.store(true, std::memory_order_release);
        item->init_mutex.unlock();
    }

    printf("[home_timeline] mongodb init finished! Total init %zu posts\n", post_ids_queue.size());
}


void leader_thread_func()
{
    erpc::Nexus nexus(FLAGS_server_addr, FLAGS_numa_server_node, 0);

    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_PING), ping_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_WRITE_REQ), home_timeline_write_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ), home_timeline_read_req_handler);
    if (sn_domain::enabled()) nexus.register_req_func(
        static_cast<uint8_t>(RPC_TYPE::RPC_DOMAIN_HOME_TIMELINE_READ_REQ), domain_home_timeline_read_req_handler);
    if (sn_domain::enabled() || sn_measure::rpc_baseline()) nexus.register_req_func(
        static_cast<uint8_t>(RPC_TYPE::RPC_DOMAIN_READY), domain_ready_handler);

    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_READ_RESP), post_storage_read_resp_handler);

    std::vector<std::thread> clients(FLAGS_client_num);
    std::vector<std::thread> servers(FLAGS_server_num);
    std::vector<std::thread> workers(FLAGS_client_num);

    auto *context = new AppContext();

    std::thread mongodb_init_thread(mongodb_init, context);
    erpc::bind_to_core(mongodb_init_thread, FLAGS_numa_server_node,
                       get_bind_core(FLAGS_numa_server_node));


    clients[0] = std::thread(client_thread_func, 0, context->client_contexts_[0], &nexus);
    sleep(2);
    erpc::bind_to_core(clients[0], FLAGS_numa_client_node,
                       app_client_bind_core(0));

    for (size_t i = 1; i < FLAGS_client_num; i++)
    {
        clients[i] = std::thread(client_thread_func, i, context->client_contexts_[i], &nexus);

        erpc::bind_to_core(clients[i], FLAGS_numa_client_node,
                           app_client_bind_core(i));
    }

    for (size_t i = 0; i < FLAGS_server_num; i++)
    {
        servers[i] = std::thread(server_thread_func, i, context->server_contexts_[i], &nexus);

        erpc::bind_to_core(servers[i], FLAGS_numa_server_node,
                           app_server_bind_core(i));
    }
    sleep(3);

    for (size_t i = 0; i < FLAGS_client_num; i++)
    {
        // // printf("[home_timeline] Creating worker thread %zu, server_context->rpc_=%p\n",
            //    i, (void*)context->server_contexts_[i]->rpc_);
        my_assert(context->server_contexts_[i]->rpc_ != nullptr);
        workers[i] = std::thread(worker_thread_func, i, context->client_contexts_[i]->forward_all_mpmc_queue,
                                 context->client_contexts_[i]->backward_mpmc_queue, context->client_contexts_[i]->forward_mpmc_queue,
                                  context->client_contexts_[i]->rpc_, context->server_contexts_[i]->rpc_,
                                  context->client_contexts_[i]->read_queue_start_ns,
                                  context->client_contexts_[i]->forward_queue_start_ns);
//        uint64_t worker_offset = FLAGS_worker_bind_core_offset == UINT64_MAX ? FLAGS_bind_core_offset : FLAGS_worker_bind_core_offset;
//        erpc::bind_to_core(workers[i], FLAGS_numa_worker_node, get_bind_core(FLAGS_numa_worker_node) + worker_offset);
        erpc::bind_to_core(workers[i], FLAGS_numa_client_node,
                           app_worker_bind_core(i));

    }

    sleep(2);
    if (FLAGS_timeout_second != UINT64_MAX)
    {
        sleep(FLAGS_timeout_second);
        ctrl_c_pressed = true;
    }

    mongodb_init_thread.join();
    for (size_t i = 0; i < FLAGS_client_num; i++)
    {
        workers[i].join();
    }
    for (size_t i = 0; i < FLAGS_client_num; i++)
    {
        clients[i].join();
    }
    for (size_t i = 0; i < FLAGS_server_num; i++)
    {
        servers[i].join();
    }
}

int main(int argc, char **argv)
{

    signal(SIGINT, ctrl_c_handler);
    signal(SIGTERM, ctrl_c_handler);
    // only config_file is required!!!
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    init_service_config(FLAGS_config_file,"home_timeline");
    init_specific_config();

    std::thread leader_thread(leader_thread_func);
    erpc::bind_to_core(leader_thread, FLAGS_numa_server_node,
                       get_bind_core(FLAGS_numa_server_node));
    leader_thread.join();

}
