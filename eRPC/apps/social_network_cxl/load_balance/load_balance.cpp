#include <thread>
#include <gflags/gflags.h>
#include "load_balance.h"
#include "../ub_breakdown.h"
#include "../rpc_measure_ready.h"
#include "../domain_proxy.h"

static bool use_multi_client_wire(RPC_TYPE type) {
    return sn_measure::rpc_baseline() && sn_domain::source_count() > 1 &&
           proxy_inline_timeline_type(type);
}

void baseline_ready_handler(erpc::ReqHandle *h, void *c) {
    sn_measure::proxy_ready_request<ServerContext>(h, c, "load_balance");
}

void connect_sessions(ClientContext *c) {
    std::vector<std::string> forward_server_addr = flags_get_balance_servers_index();
    if (sn_measure::rpc_baseline() && sn_domain::proxy_count() == 3) {
        forward_server_addr.clear();
        for (const auto &nginx : sn_domain::nginx_instances())
            forward_server_addr.push_back(nginx.address);
    }

    c->servers_num_ = static_cast<int>(forward_server_addr.size());
    for (const auto& m : forward_server_addr) {
        const auto &registered = sn_domain::nginx_instances();
        const uint8_t remote_id = sn_measure::rpc_baseline() && registered.size() == 3
            ? static_cast<uint8_t>(registered[c->session_num_vec_.size()].rpc_id)
            : get_remote_rpc_id(m);
        int session_num_forward = c->rpc_->create_session(m, remote_id);
        my_assert(session_num_forward >= 0, "Failed to create session");
        c->session_num_vec_.push_back(session_num_forward);
    }

    c->backward_session_num_ = c->rpc_->create_session(client_addr, get_remote_rpc_id(client_addr));
    my_assert(c->backward_session_num_ >= 0, "Failed to create session");
    c->session_num_vec_.push_back(c->backward_session_num_);
    c->backward_sessions_.push_back(c->backward_session_num_);
    if (sn_measure::rpc_baseline() && sn_domain::source_count() > 1) {
        const auto &clients = sn_domain::clients();
        for (size_t i = 1; i < clients.size(); ++i) {
            const int session = c->rpc_->create_session(clients[i].address,
                                                         static_cast<uint8_t>(clients[i].rpc_id));
            if (session < 0) sn_domain::fail("load_balance", static_cast<uint32_t>(c->client_id_),
                                             "create-client-session", 0, EHOSTUNREACH);
            c->backward_sessions_.push_back(session);
        }
    }

    while (c->num_sm_resps_ != forward_server_addr.size() + c->backward_sessions_.size()) {
        c->rpc_->run_event_loop(kAppEvLoopMs);
        if (unlikely(ctrl_c_pressed == 1)) {
            // // printf("Ctrl-C pressed. Exiting\n");
            return;
        }
    }
}
 
void ping_handler(erpc::ReqHandle *req_handle, void *_context) {
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_ping_tot++;
    auto *req_msgbuf = req_handle->get_req_msgbuf();
    my_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<PingRPCReq>), "data size not match");

    auto *req = reinterpret_cast<RPCMsgReq<PingRPCReq> *>(req_msgbuf->buf_);

    new (req_handle->pre_resp_msgbuf_.buf_) RPCMsgResp<PingRPCResp>(req->req_common.type, req->req_common.req_number, 0, {req->req_control.timestamp});
    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, sizeof(RPCMsgResp<PingRPCResp>));

    ctx->forward_spsc_queue->push(pin_msgbuf(ctx->rpc_, *req_msgbuf));
    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void ping_resp_handler(erpc::ReqHandle *req_handle, void *_context) {
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_ping_resp_tot++;
    auto *req_msgbuf = req_handle->get_req_msgbuf();
    my_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<PingRPCReq>), "data size not match");

    auto *req = reinterpret_cast<RPCMsgReq<PingRPCReq> *>(req_msgbuf->buf_);

    new (req_handle->pre_resp_msgbuf_.buf_) RPCMsgResp<PingRPCResp>(req->req_common.type, req->req_common.req_number, 0, {req->req_control.timestamp});
    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, sizeof(RPCMsgResp<PingRPCResp>));

    if (++send_ping_resp_num[req->req_common.req_number % 10000] ==
        static_cast<size_t>(ctx->servers_num_)) {
        ctx->backward_spsc_queue->push(pin_msgbuf(ctx->rpc_, *req_msgbuf));
    }

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void common_req_handler(erpc::ReqHandle *req_handle, void *_context) {
    if (sn_measure::rpc_baseline()) sn_measure::proxy_forward().fetch_add(1, std::memory_order_relaxed);
    auto *ctx = static_cast<ServerContext *>(_context);
    auto *req_msgbuf = req_handle->get_req_msgbuf();

    const auto req_type = static_cast<RPC_TYPE>(req_msgbuf->get_hdr_req_type());
    if (req_type == RPC_TYPE::RPC_USER_TIMELINE_READ_REQ ||
        req_type == RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ)
        sn_profile::flow_point("load_balance",
            proxy_request_number(*req_msgbuf, use_multi_client_wire(req_type)), "forward_receive");
    switch (req_type) {
        case RPC_TYPE::RPC_COMPOSE_POST_WRITE_REQ:
            ctx->stat_req_compose_post_tot++;
            break;
        case RPC_TYPE::RPC_USER_TIMELINE_READ_REQ:
            ctx->stat_req_user_timeline_tot++;
            break;
        case RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ:
            ctx->stat_req_home_timeline_tot++;
            break;
        default:
            fprintf(stderr, "error req type %u\n", static_cast<uint>(req_type));
    }

    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, 0);

    const uint64_t pin_start = sn_profile::start();
    const bool inline_copy = proxy_inline_enabled() &&
        proxy_inline_timeline_type(req_type);
    ProxyInlineMessage copied;
    erpc::MsgBuffer pinned;
    if (inline_copy) copied = proxy_inline_copy(*req_msgbuf);
    else pinned = pin_msgbuf(ctx->rpc_, *req_msgbuf);
    sn_profile::record(sn_profile::Stage::kProxyForwardRxPin, pin_start);
    const uint64_t queue_start = sn_handoff::start();
    if (queue_start != 0) {
        const size_t slot = proxy_request_number(*req_msgbuf,
            use_multi_client_wire(req_type)) % kAppMaxBuffer;
        ctx->forward_queue_start_ns[slot].store(queue_start,
                                                 std::memory_order_relaxed);
    }
    if (inline_copy) ctx->forward_inline_queue->push(copied);
    else ctx->forward_spsc_queue->push(pinned);
    sn_profile::record(sn_profile::Stage::kProxyForwardRxQueue, queue_start);

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void common_resp_handler(erpc::ReqHandle *req_handle, void *_context) {
    if (sn_measure::rpc_baseline()) sn_measure::proxy_reverse().fetch_add(1, std::memory_order_relaxed);
    auto *ctx = static_cast<ServerContext *>(_context);
    auto *req_msgbuf = req_handle->get_req_msgbuf();

    const auto req_type = static_cast<RPC_TYPE>(req_msgbuf->get_hdr_req_type());
    if (req_type == RPC_TYPE::RPC_USER_TIMELINE_READ_RESP ||
        req_type == RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP)
        sn_profile::flow_point("load_balance",
            proxy_request_number(*req_msgbuf, use_multi_client_wire(req_type)), "reverse_receive");
    switch (req_type) {
        case RPC_TYPE::RPC_COMPOSE_POST_WRITE_RESP:
            ctx->stat_req_compose_post_resp_tot++;
            break;
        case RPC_TYPE::RPC_USER_TIMELINE_READ_RESP:
            ctx->stat_req_user_timeline_resp_tot++;
            break;
        case RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP:
            ctx->stat_req_home_timeline_resp_tot++;
            break;
        default:
            fprintf(stderr, "error req type %u\n", static_cast<uint>(req_type));
    }

    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, 0);

    const uint64_t pin_start = sn_profile::start();
    const bool inline_copy = proxy_inline_enabled() &&
        proxy_inline_timeline_type(req_type);
    ProxyInlineMessage copied;
    erpc::MsgBuffer pinned;
    if (inline_copy) copied = proxy_inline_copy(*req_msgbuf);
    else pinned = pin_msgbuf(ctx->rpc_, *req_msgbuf);
    sn_profile::record(sn_profile::Stage::kProxyReverseRxPin, pin_start);
    const uint64_t queue_start = sn_handoff::start();
    if (queue_start != 0) {
        const size_t slot = proxy_request_number(*req_msgbuf,
            use_multi_client_wire(req_type)) % kAppMaxBuffer;
        ctx->backward_queue_start_ns[slot].store(queue_start,
                                                  std::memory_order_relaxed);
    }
    if (inline_copy) ctx->backward_inline_queue->push(copied);
    else ctx->backward_spsc_queue->push(pinned);
    sn_profile::record(sn_profile::Stage::kProxyReverseRxQueue, queue_start);

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void callback_ping(void *_context, void *_tag) {
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_forward_msgbuf[req_id];
    my_assert(resp_msgbuf.get_data_size() == sizeof(RPCMsgResp<PingRPCResp>), "data size not match");

    release_msgbuf(ctx->rpc_, ctx->req_forward_msgbuf[req_id]);
    ctx->req_forward_msgbuf[req_id].buf_ = nullptr;
}

void handler_ping(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf) {
    auto *req = reinterpret_cast<RPCMsgReq<PingRPCReq> *>(req_msgbuf.buf_);

    for (int i = 0; i < ctx->servers_num_; i++) {
        const size_t slot =
            (req->req_common.req_number + static_cast<size_t>(i)) %
            kAppMaxBuffer;
        require_empty_msgbuf_slot(ctx->req_forward_msgbuf[slot],
                                  "load_balance.req_forward_msgbuf", slot);
        ctx->req_forward_msgbuf[slot] = clone_msgbuf(ctx->rpc_, req_msgbuf);
        erpc::MsgBuffer &resp_msgbuf = ctx->resp_forward_msgbuf[slot];

        ctx->rpc_->enqueue_request(i, static_cast<uint8_t>(RPC_TYPE::RPC_PING),
                                   &ctx->req_forward_msgbuf[slot], &resp_msgbuf,
                                   callback_ping, reinterpret_cast<void *>(slot));
    }
    release_msgbuf(ctx->rpc_, req_msgbuf);
}

void callback_ping_resp(void *_context, void *_tag) {
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req_id];
    my_assert(resp_msgbuf.get_data_size() == sizeof(RPCMsgResp<PingRPCResp>), "data size not match");

    release_msgbuf(ctx->rpc_, ctx->req_backward_msgbuf[req_id]);
    ctx->req_backward_msgbuf[req_id].buf_ = nullptr;
}

void handler_ping_resp(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf) {
    const size_t slot = req_msgbuf.get_hdr_req_num() % kAppMaxBuffer;

    require_empty_msgbuf_slot(ctx->req_backward_msgbuf[slot],
                              "load_balance.req_backward_msgbuf", slot);
    ctx->req_backward_msgbuf[slot] = prepare_forward_msgbuf(ctx->rpc_, req_msgbuf);
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[slot];

    ctx->rpc_->enqueue_request(ctx->backward_session_num_, static_cast<uint8_t>(RPC_TYPE::RPC_PING_RESP),
                               &ctx->req_backward_msgbuf[slot], &resp_msgbuf,
                               callback_ping_resp, reinterpret_cast<void *>(slot));
}

void callback_common_req(void *_context, void *_tag) {
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_forward_msgbuf[req_id];
    my_assert(resp_msgbuf.get_data_size() == 0, "data size not match");

    const uint64_t release_start = sn_profile::start();
    release_msgbuf(ctx->rpc_, ctx->req_forward_msgbuf[req_id]);
    sn_profile::record(sn_profile::Stage::kProxyForwardCallbackRelease,
                       release_start);
    ctx->req_forward_msgbuf[req_id].buf_ = nullptr;
}

void handler_common_req(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf,
                        const ProxyInlineMessage *inline_msg = nullptr) {
    const uint8_t req_type = inline_msg == nullptr ? req_msgbuf.get_hdr_req_type()
                                                    : inline_msg->req_type;
    const bool application_identity =
        use_multi_client_wire(static_cast<RPC_TYPE>(req_type));
    const uint32_t request = inline_msg == nullptr
        ? proxy_request_number(req_msgbuf, application_identity)
        : proxy_request_number(*inline_msg, application_identity);
    const size_t slot = request % kAppMaxBuffer;

    require_empty_msgbuf_slot(ctx->req_forward_msgbuf[slot],
                              "load_balance.req_forward_msgbuf", slot);
    const uint64_t prepare_start = sn_profile::start();
    ctx->req_forward_msgbuf[slot] = inline_msg == nullptr
        ? prepare_forward_msgbuf(ctx->rpc_, req_msgbuf)
        : proxy_inline_materialize(ctx->rpc_, *inline_msg);
    sn_profile::record(sn_profile::Stage::kProxyForwardTxPrepare,
                       prepare_start);
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_forward_msgbuf[slot];

    size_t target_index = slot % ctx->servers_num_;
    if (sn_measure::rpc_baseline() && sn_domain::proxy_count() == 3 &&
        proxy_inline_timeline_type(static_cast<RPC_TYPE>(req_type))) {
        const uint32_t wire = inline_msg == nullptr ? proxy_read_wire_number(req_msgbuf)
                                                    : proxy_read_wire_number(*inline_msg);
        target_index = sn_domain::rpc_nginx_index(wire);
    }
    size_t session_num = ctx->session_num_vec_.at(target_index);

    const uint64_t enqueue_start = sn_profile::start();
    ctx->rpc_->enqueue_request(session_num, req_type,
                               &ctx->req_forward_msgbuf[slot], &resp_msgbuf,
                               callback_common_req, reinterpret_cast<void *>(slot));
    if (req_type == static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_READ_REQ) ||
        req_type == static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ))
        sn_profile::flow_point("load_balance", request, "forward_enqueue");
    sn_profile::record(sn_profile::Stage::kProxyForwardTxEnqueue,
                       enqueue_start);
}

void callback_common_resp(void *_context, void *_tag) {
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req_id];
    my_assert(resp_msgbuf.get_data_size() == 0, "data size not match");

    const uint64_t release_start = sn_profile::start();
    release_msgbuf(ctx->rpc_, ctx->req_backward_msgbuf[req_id]);
    sn_profile::record(sn_profile::Stage::kProxyReverseCallbackRelease,
                       release_start);
    ctx->req_backward_msgbuf[req_id].buf_ = nullptr;
}

void handler_common_resp(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf,
                         const ProxyInlineMessage *inline_msg = nullptr) {
    const uint8_t req_type = inline_msg == nullptr ? req_msgbuf.get_hdr_req_type()
                                                    : inline_msg->req_type;
    const bool application_identity =
        use_multi_client_wire(static_cast<RPC_TYPE>(req_type));
    const uint32_t request = inline_msg == nullptr
        ? proxy_request_number(req_msgbuf, application_identity)
        : proxy_request_number(*inline_msg, application_identity);
    const size_t slot = request % kAppMaxBuffer;

    require_empty_msgbuf_slot(ctx->req_backward_msgbuf[slot],
                              "load_balance.req_backward_msgbuf", slot);
    const uint64_t prepare_start = sn_profile::start();
    ctx->req_backward_msgbuf[slot] = inline_msg == nullptr
        ? prepare_forward_msgbuf(ctx->rpc_, req_msgbuf)
        : proxy_inline_materialize(ctx->rpc_, *inline_msg);
    sn_profile::record(sn_profile::Stage::kProxyReverseTxPrepare,
                       prepare_start);
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[slot];

    const uint64_t enqueue_start = sn_profile::start();
    size_t source = 0;
    if (sn_measure::rpc_baseline() && sn_domain::source_count() > 1) {
        try {
            source = sn_domain::wire_source_index(request, static_cast<uint32_t>(FLAGS_client_num),
                                                   sn_domain::source_count());
        } catch (const std::exception &) {
            sn_domain::fail("load_balance", static_cast<uint32_t>(ctx->client_id_),
                            "reverse-source", request, EPROTO);
        }
    }
    ctx->rpc_->enqueue_request(ctx->backward_sessions_.at(source), req_type,
                               &ctx->req_backward_msgbuf[slot], &resp_msgbuf,
                               callback_common_resp, reinterpret_cast<void *>(slot));
    if (req_type == static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_READ_RESP) ||
        req_type == static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP))
        sn_profile::flow_point("load_balance", request, "reverse_enqueue");
    sn_profile::record(sn_profile::Stage::kProxyReverseTxEnqueue,
                       enqueue_start);
}

void client_thread_func(size_t thread_id, ClientContext *ctx, erpc::Nexus *nexus) {
    ctx->client_id_ = thread_id;
    std::vector<size_t> port_vec = flags_get_cxl_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());
    uint8_t rpc_id = FLAGS_rpc_id + 20 + thread_id;

    AppRpc rpc(nexus, static_cast<void *>(ctx), rpc_id,
                                    basic_sm_handler_client, phy_port);
    rpc.retry_connect_on_invalid_rpc_id_ = true;
    ctx->rpc_ = &rpc;

    for (size_t i = 0; i < kAppMaxBuffer; i++) {
        ctx->resp_forward_msgbuf[i] = rpc.alloc_msg_buffer_or_die(sizeof(RPCMsgResp<CommonRPCResp>));
        ctx->resp_backward_msgbuf[i] = rpc.alloc_msg_buffer_or_die(sizeof(RPCMsgResp<CommonRPCResp>));
    }

    connect_sessions(ctx);
    if (sn_measure::rpc_baseline()) sn_measure::proxy_ready().store(true, std::memory_order_release);

    // The borrowed-buffer baseline retains its historical polling batch.
    const size_t queue_batch = proxy_poll_batch();
    while (true) {
        erpc::MsgBuffer req_msg;
        for (size_t i = 0;
             i < queue_batch &&
             ctx->forward_spsc_queue->try_pop(req_msg);
             i++) {
            const auto req_type =
                static_cast<RPC_TYPE>(req_msg.get_hdr_req_type());
            if ((sn_profile::enabled() || sn_slow_trace::enabled()) &&
                req_type != RPC_TYPE::RPC_PING) {
                const uint32_t request = proxy_request_number(req_msg,
                    use_multi_client_wire(req_type));
                const size_t slot = request % kAppMaxBuffer;
                sn_handoff::record(
                    sn_profile::Stage::kProxyForwardQueueHandoff,
                    "lb_forward_queue_handoff", request,
                    ctx->forward_queue_start_ns[slot].exchange(
                        0, std::memory_order_relaxed));
            }
            if (req_type == RPC_TYPE::RPC_PING) {
                handler_ping(ctx, req_msg);
            } else {
                handler_common_req(ctx, req_msg);
            }
        }

        for (size_t i = 0;
             i < queue_batch &&
             ctx->backward_spsc_queue->try_pop(req_msg);
             i++) {
            const auto req_type =
                static_cast<RPC_TYPE>(req_msg.get_hdr_req_type());
            if ((sn_profile::enabled() || sn_slow_trace::enabled()) &&
                req_type != RPC_TYPE::RPC_PING_RESP) {
                const uint32_t request = proxy_request_number(req_msg,
                    use_multi_client_wire(req_type));
                const size_t slot = request % kAppMaxBuffer;
                sn_handoff::record(
                    sn_profile::Stage::kProxyReverseQueueHandoff,
                    "lb_reverse_queue_handoff", request,
                    ctx->backward_queue_start_ns[slot].exchange(
                        0, std::memory_order_relaxed));
            }
            if (req_type == RPC_TYPE::RPC_PING_RESP) {
                handler_ping_resp(ctx, req_msg);
            } else {
                handler_common_resp(ctx, req_msg);
            }
        }

        if (ctx->forward_inline_queue != nullptr) {
            ProxyInlineMessage msg;
            for (size_t i = 0;
                 i < queue_batch && ctx->forward_inline_queue->try_pop(msg);
                 ++i) {
                if (sn_profile::enabled() || sn_slow_trace::enabled()) {
                    const uint32_t request = proxy_request_number(msg,
                        use_multi_client_wire(static_cast<RPC_TYPE>(msg.req_type)));
                    const size_t slot = request % kAppMaxBuffer;
                    sn_handoff::record(
                        sn_profile::Stage::kProxyForwardQueueHandoff,
                        "lb_forward_queue_handoff", request,
                        ctx->forward_queue_start_ns[slot].exchange(
                            0, std::memory_order_relaxed));
                }
                handler_common_req(ctx, erpc::MsgBuffer{}, &msg);
            }
        }
        if (ctx->backward_inline_queue != nullptr) {
            ProxyInlineMessage msg;
            for (size_t i = 0;
                 i < queue_batch && ctx->backward_inline_queue->try_pop(msg);
                 ++i) {
                if (sn_profile::enabled() || sn_slow_trace::enabled()) {
                    const uint32_t request = proxy_request_number(msg,
                        use_multi_client_wire(static_cast<RPC_TYPE>(msg.req_type)));
                    const size_t slot = request % kAppMaxBuffer;
                    sn_handoff::record(
                        sn_profile::Stage::kProxyReverseQueueHandoff,
                        "lb_reverse_queue_handoff", request,
                        ctx->backward_queue_start_ns[slot].exchange(
                            0, std::memory_order_relaxed));
                }
                handler_common_resp(ctx, erpc::MsgBuffer{}, &msg);
            }
        }

        ctx->rpc_->run_event_loop_once();
        if (unlikely(ctrl_c_pressed)) {
            break;
        }
    }
}

void server_thread_func(size_t thread_id, ServerContext *ctx, erpc::Nexus *nexus) {
    ctx->server_id_ = thread_id;
    std::vector<size_t> port_vec = flags_get_cxl_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());
    uint8_t rpc_id = FLAGS_rpc_id + thread_id;

    AppRpc rpc(nexus, static_cast<void *>(ctx), rpc_id,
                                    basic_sm_handler_server, phy_port);
    rpc.retry_connect_on_invalid_rpc_id_ = true;
    ctx->rpc_ = &rpc;
    ctx->servers_num_ = flags_get_balance_servers_index().size();
    if (sn_measure::rpc_baseline() && sn_domain::proxy_count() == 3)
        ctx->servers_num_ = 3;

    while (true) {
        ctx->reset_stat();
        erpc::ChronoTimer start;
        start.reset();
        rpc.run_event_loop(kAppEvLoopMs);
        const double seconds = start.get_sec();
        // printf("thread %zu: ping_req: %.2f, ping_resp: %.2f, compose_post: %.2f, user_timeline: %.2f, home_timeline: %.2f\n", thread_id, ctx->stat_req_ping_tot / seconds, ctx->stat_req_ping_resp_tot / seconds, ctx->stat_req_compose_post_tot / seconds, ctx->stat_req_user_timeline_tot / seconds, ctx->stat_req_home_timeline_tot / seconds);

        ctx->rpc_->reset_dpath_stats();
        if (ctrl_c_pressed == 1) {
            break;
        }
    }
}

void leader_thread_func() {
    erpc::Nexus nexus(FLAGS_server_addr, FLAGS_numa_server_node, 0);
    if (sn_measure::rpc_baseline()) nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_DOMAIN_READY), baseline_ready_handler);

    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_PING), ping_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_PING_RESP), ping_resp_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_COMPOSE_POST_WRITE_REQ), common_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_COMPOSE_POST_WRITE_RESP), common_resp_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_READ_REQ), common_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_READ_RESP), common_resp_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ), common_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP), common_resp_handler);

    std::vector<std::thread> clients(FLAGS_client_num);
    std::vector<std::thread> servers(FLAGS_server_num);

    auto *context = new AppContext();

    clients[0] = std::thread(client_thread_func, 0, context->client_contexts_[0], &nexus);
    sleep(2);
    erpc::bind_to_core(clients[0], FLAGS_numa_client_node,
                       app_client_bind_core(0));

    for (size_t i = 1; i < FLAGS_client_num; i++) {
        clients[i] = std::thread(client_thread_func, i, context->client_contexts_[i], &nexus);
        erpc::bind_to_core(clients[i], FLAGS_numa_client_node,
                           app_client_bind_core(i));
    }

    for (size_t i = 0; i < FLAGS_server_num; i++) {
        servers[i] = std::thread(server_thread_func, i, context->server_contexts_[i], &nexus);
        erpc::bind_to_core(servers[i], FLAGS_numa_server_node,
                           app_server_bind_core(i));
    }
    sleep(3);

    if (FLAGS_timeout_second != UINT64_MAX) {
        sleep(FLAGS_timeout_second);
        ctrl_c_pressed = true;
    }

    for (size_t i = 0; i < FLAGS_client_num; i++) {
        clients[i].join();
    }
    for (size_t i = 0; i < FLAGS_server_num; i++) {
        servers[i].join();
    }
}

int main(int argc, char **argv) {
    signal(SIGINT, ctrl_c_handler);
    signal(SIGTERM, ctrl_c_handler);
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    init_service_config(FLAGS_config_file, "load_balance");
    init_specific_config();

    std::thread leader_thread(leader_thread_func);
    erpc::bind_to_core(leader_thread, FLAGS_numa_server_node,
                       get_bind_core(FLAGS_numa_server_node));
    leader_thread.join();
    sn_measure::proxy_report("load_balance");
}
