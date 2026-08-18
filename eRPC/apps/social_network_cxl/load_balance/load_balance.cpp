#include <thread>
#include <gflags/gflags.h>
#include "load_balance.h"

void connect_sessions(ClientContext *c) {
    std::vector<std::string> forward_server_addr = flags_get_balance_servers_index();

    c->servers_num_ = static_cast<int>(forward_server_addr.size());
    for (const auto& m : forward_server_addr) {
        int session_num_forward = c->rpc_->create_session(m, get_remote_rpc_id(m));
        my_assert(session_num_forward >= 0, "Failed to create session");
        c->session_num_vec_.push_back(session_num_forward);
    }

    c->backward_session_num_ = c->rpc_->create_session(client_addr, get_remote_rpc_id(client_addr));
    my_assert(c->backward_session_num_ >= 0, "Failed to create session");
    c->session_num_vec_.push_back(c->backward_session_num_);

    while (c->num_sm_resps_ != forward_server_addr.size() + 1) {
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

    if (++send_ping_resp_num[req->req_common.req_number % 10000] == ctx->servers_num_) {
        ctx->backward_spsc_queue->push(pin_msgbuf(ctx->rpc_, *req_msgbuf));
    }

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void common_req_handler(erpc::ReqHandle *req_handle, void *_context) {
    auto *ctx = static_cast<ServerContext *>(_context);
    auto *req_msgbuf = req_handle->get_req_msgbuf();

    const auto req_type = static_cast<RPC_TYPE>(req_msgbuf->get_hdr_req_type());
    const size_t slot = req_msgbuf->get_hdr_req_num() % kAppMaxBuffer;

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

    if (likely(ctx->req_forward_msgbuf_ptr[slot].buf_ != nullptr)) {
        release_msgbuf(ctx->rpc_, ctx->req_forward_msgbuf_ptr[slot]);
        __sync_synchronize();
    }

    ctx->forward_spsc_queue->push(pin_msgbuf(ctx->rpc_, *req_msgbuf));
    __sync_synchronize();

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void common_resp_handler(erpc::ReqHandle *req_handle, void *_context) {
    auto *ctx = static_cast<ServerContext *>(_context);
    auto *req_msgbuf = req_handle->get_req_msgbuf();

    const auto req_type = static_cast<RPC_TYPE>(req_msgbuf->get_hdr_req_type());
    const size_t slot = req_msgbuf->get_hdr_req_num() % kAppMaxBuffer;

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

    if (ctx->req_backward_msgbuf_ptr[slot].buf_ != nullptr) {
        release_msgbuf(ctx->rpc_, ctx->req_backward_msgbuf_ptr[slot]);
        __sync_synchronize();
    }

    ctx->backward_spsc_queue->push(pin_msgbuf(ctx->rpc_, *req_msgbuf));
    __sync_synchronize();

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void callback_ping(void *_context, void *_tag) {
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_forward_msgbuf[req_id];
    my_assert(resp_msgbuf.get_data_size() == sizeof(RPCMsgResp<PingRPCResp>), "data size not match");

    release_msgbuf(ctx->rpc_, ctx->req_forward_msgbuf[req_id]);
}

void handler_ping(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf) {
    auto *req = reinterpret_cast<RPCMsgReq<PingRPCReq> *>(req_msgbuf.buf_);

    for (int i = 0; i < ctx->servers_num_; i++) {
        if (i == ctx->servers_num_ - 1) {
            ctx->req_forward_msgbuf[(req->req_common.req_number + i) % kAppMaxBuffer] = clone_msgbuf(ctx->rpc_, req_msgbuf);
        } else {
            ctx->req_forward_msgbuf[(req->req_common.req_number + i) % kAppMaxBuffer] = clone_msgbuf(ctx->rpc_, req_msgbuf);
        }
        erpc::MsgBuffer &resp_msgbuf = ctx->resp_forward_msgbuf[(req->req_common.req_number + i) % kAppMaxBuffer];

        ctx->rpc_->enqueue_request(i, static_cast<uint8_t>(RPC_TYPE::RPC_PING),
                                   &ctx->req_forward_msgbuf[(req->req_common.req_number + i) % kAppMaxBuffer], &resp_msgbuf,
                                   callback_ping, reinterpret_cast<void *>((req->req_common.req_number + i) % kAppMaxBuffer));
    }
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
    auto *req = reinterpret_cast<RPCMsgReq<PingRPCReq> *>(req_msgbuf.buf_);

    ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer] = req_msgbuf;
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer];

    ctx->rpc_->enqueue_request(ctx->backward_session_num_, static_cast<uint8_t>(RPC_TYPE::RPC_PING_RESP),
                               &ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer], &resp_msgbuf,
                               callback_ping_resp, reinterpret_cast<void *>(req->req_common.req_number % kAppMaxBuffer));
}

void callback_common_req(void *_context, void *_tag) {
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_forward_msgbuf[req_id];
    my_assert(resp_msgbuf.get_data_size() == 0, "data size not match");

    release_msgbuf(ctx->rpc_, ctx->req_forward_msgbuf[req_id]);
    ctx->req_forward_msgbuf[req_id].buf_ = nullptr;
}

void handler_common_req(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf) {
    const uint8_t req_type = req_msgbuf.get_hdr_req_type();
    const size_t slot = req_msgbuf.get_hdr_req_num() % kAppMaxBuffer;

    ctx->req_forward_msgbuf[slot] = req_msgbuf;
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_forward_msgbuf[slot];

    size_t session_num = ctx->session_num_vec_[slot % ctx->servers_num_];

    ctx->rpc_->enqueue_request(session_num, req_type,
                               &ctx->req_forward_msgbuf[slot], &resp_msgbuf,
                               callback_common_req, reinterpret_cast<void *>(slot));
}

void callback_common_resp(void *_context, void *_tag) {
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req_id];
    my_assert(resp_msgbuf.get_data_size() == 0, "data size not match");

    release_msgbuf(ctx->rpc_, ctx->req_backward_msgbuf[req_id]);
    ctx->req_backward_msgbuf[req_id].buf_ = nullptr;
}

void handler_common_resp(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf) {
    const uint8_t req_type = req_msgbuf.get_hdr_req_type();
    const size_t slot = req_msgbuf.get_hdr_req_num() % kAppMaxBuffer;

    ctx->req_backward_msgbuf[slot] = req_msgbuf;
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[slot];

    ctx->rpc_->enqueue_request(ctx->backward_session_num_, req_type,
                               &ctx->req_backward_msgbuf[slot], &resp_msgbuf,
                               callback_common_resp, reinterpret_cast<void *>(slot));
}

void client_thread_func(size_t thread_id, ClientContext *ctx, erpc::Nexus *nexus) {
    ctx->client_id_ = thread_id;
    std::vector<size_t> port_vec = flags_get_cxl_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());
    uint8_t rpc_id = FLAGS_rpc_id + 20 + thread_id;

    erpc::Rpc<erpc::CXLTransport> rpc(nexus, static_cast<void *>(ctx), rpc_id,
                                    basic_sm_handler_client, phy_port);
    rpc.retry_connect_on_invalid_rpc_id_ = true;
    ctx->rpc_ = &rpc;

    for (size_t i = 0; i < kAppMaxBuffer; i++) {
        ctx->resp_forward_msgbuf[i] = rpc.alloc_msg_buffer_or_die(sizeof(RPCMsgResp<CommonRPCResp>));
        ctx->resp_backward_msgbuf[i] = rpc.alloc_msg_buffer_or_die(sizeof(RPCMsgResp<CommonRPCResp>));
    }

    connect_sessions(ctx);

    using FUNC_HANDLER = std::function<void(ClientContext *, erpc::MsgBuffer)>;
    std::map<RPC_TYPE, FUNC_HANDLER> handlers{
        {RPC_TYPE::RPC_PING, handler_ping},
        {RPC_TYPE::RPC_PING_RESP, handler_ping_resp},
        {RPC_TYPE::RPC_COMPOSE_POST_WRITE_REQ, handler_common_req},
        {RPC_TYPE::RPC_COMPOSE_POST_WRITE_RESP, handler_common_resp},
        {RPC_TYPE::RPC_USER_TIMELINE_READ_REQ, handler_common_req},
        {RPC_TYPE::RPC_USER_TIMELINE_READ_RESP, handler_common_resp},
        {RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ, handler_common_req},
        {RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP, handler_common_resp},
    };

    while (true) {
        unsigned size = ctx->forward_spsc_queue->was_size();
        for (unsigned i = 0; i < size; i++) {
            erpc::MsgBuffer req_msg = ctx->forward_spsc_queue->pop();
            __sync_synchronize();

            handlers[static_cast<RPC_TYPE>(req_msg.get_hdr_req_type())](ctx, req_msg);
        }

        size = ctx->backward_spsc_queue->was_size();
        for (unsigned i = 0; i < size; i++) {
            erpc::MsgBuffer req_msg = ctx->backward_spsc_queue->pop();
            __sync_synchronize();

            handlers[static_cast<RPC_TYPE>(req_msg.get_hdr_req_type())](ctx, req_msg);
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

    erpc::Rpc<erpc::CXLTransport> rpc(nexus, static_cast<void *>(ctx), rpc_id,
                                    basic_sm_handler_server, phy_port);
    rpc.retry_connect_on_invalid_rpc_id_ = true;
    ctx->rpc_ = &rpc;
    ctx->servers_num_ = flags_get_balance_servers_index().size();

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
    erpc::bind_to_core(clients[0], FLAGS_numa_client_node, get_bind_core(FLAGS_numa_client_node) + FLAGS_bind_core_offset);

    for (size_t i = 1; i < FLAGS_client_num; i++) {
        clients[i] = std::thread(client_thread_func, i, context->client_contexts_[i], &nexus);
        erpc::bind_to_core(clients[i], FLAGS_numa_client_node, get_bind_core(FLAGS_numa_client_node) + FLAGS_bind_core_offset);
    }

    for (size_t i = 0; i < FLAGS_server_num; i++) {
        servers[i] = std::thread(server_thread_func, i, context->server_contexts_[i], &nexus);
        erpc::bind_to_core(servers[i], FLAGS_numa_server_node, get_bind_core(FLAGS_numa_server_node) + FLAGS_bind_core_offset);
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
    erpc::bind_to_core(leader_thread, 1, get_bind_core(1));
    leader_thread.join();
}
