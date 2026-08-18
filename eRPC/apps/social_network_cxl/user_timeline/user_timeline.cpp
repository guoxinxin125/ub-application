#include <thread>

#include "../utils_mongodb.h"

#include "user_timeline.h"


void connect_sessions(ClientContext *c)
{

    // connect to backward server
    c->compose_post_session_number = c->rpc_->create_session(compose_post_addr, get_remote_rpc_id(compose_post_addr));
    my_assert(c->compose_post_session_number >= 0);

    c->nginx_session_number = c->rpc_->create_session(nginx_addr, get_remote_rpc_id(nginx_addr));
    my_assert(c->nginx_session_number >= 0);

    // c->client_session_number = c->rpc_->create_session(client_addr, get_remote_rpc_id(client_addr));
    // my_assert(c->client_session_number >= 0);

    c->post_storage_session_number = c->rpc_->create_session(post_storage_addr, get_remote_rpc_id(post_storage_addr));
    my_assert(c->post_storage_session_number >= 0);

    // while (c->num_sm_resps_ != 4)
    while (c->num_sm_resps_ != 3)
    {
        // printf("[user_timeline] Waiting for sessions... num_sm_resps_=%zu\n", c->num_sm_resps_);
        c->rpc_->run_event_loop(kAppEvLoopMs);
        if (unlikely(ctrl_c_pressed == 1))
        {
            // printf("Ctrl-C pressed. Exiting\n");
            return;
        }
    }
    // printf("[user_timeline] All sessions established! num_sm_resps_=%zu\n", c->num_sm_resps_);
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
    if(ctx->mongodb_init_finished){
        ctx->forward_all_mpmc_queue->push(pin_msgbuf(ctx->rpc_, *req_msgbuf));
        
    } else {
        ctx->is_pinged = true;
        ctx->ping_req_number = req->req_common.req_number;
    }
    ctx->init_mutex.unlock();

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void user_timeline_write_req_handler(erpc::ReqHandle *req_handle, void *_context)
{
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_user_timeline_write_req_tot++;

    ctx->init_mutex.lock();
    my_assert(ctx->mongodb_init_finished);
    ctx->init_mutex.unlock();

    auto *req_msgbuf = req_handle->get_req_msgbuf();
    invalidate_msgbuf_before_read(ctx->rpc_, *req_msgbuf);
    auto *req = reinterpret_cast<RPCMsgReq<UserTimeLineWriteReq> *>(req_msgbuf->buf_);

    my_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<UserTimeLineWriteReq>));

    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, 0);

//    if (likely(ctx->req_backward_msgbuf_ptr[req->req_common.req_number % kAppMaxBuffer].buf_ != nullptr))
//    {
//        ctx->rpc_->free_msg_buffer(ctx->req_backward_msgbuf_ptr[req->req_common.req_number % kAppMaxBuffer]);
//    }

    ctx->forward_all_mpmc_queue->push(pin_msgbuf(ctx->rpc_, *req_msgbuf));
    __sync_synchronize();
    

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);

}

void user_timeline_read_req_handler(erpc::ReqHandle *req_handle, void *_context)
{
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_user_timeline_read_req_tot++;

    ctx->init_mutex.lock();
    my_assert(ctx->mongodb_init_finished);
    ctx->init_mutex.unlock();

    auto *req_msgbuf = req_handle->get_req_msgbuf();
    invalidate_msgbuf_before_read(ctx->rpc_, *req_msgbuf);
    auto *req = reinterpret_cast<RPCMsgReq<UserTimeLineReq> *>(req_msgbuf->buf_);

    my_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<UserTimeLineReq>));

    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, 0);

//    if (likely(ctx->req_forward_msgbuf_ptr[req->req_common.req_number % kAppMaxBuffer].buf_ != nullptr))
//    {
//        ctx->rpc_->free_msg_buffer(ctx->req_forward_msgbuf_ptr[req->req_common.req_number % kAppMaxBuffer]);
//    }

    ctx->forward_all_mpmc_queue->push(pin_msgbuf(ctx->rpc_, *req_msgbuf));
    __sync_synchronize();
    

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);

}

void post_storage_read_resp_handler(erpc::ReqHandle *req_handle, void *_context)
{
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_post_storage_read_resp_tot++;

    ctx->init_mutex.lock();
    my_assert(ctx->mongodb_init_finished);
    ctx->init_mutex.unlock();

    auto *req_msgbuf = req_handle->get_req_msgbuf();
    invalidate_msgbuf_before_read(ctx->rpc_, *req_msgbuf);
    auto *req_common = reinterpret_cast<CommonReq *>(req_msgbuf->buf_);
    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, 0);

 
    if (likely(ctx->req_backward_msgbuf_ptr[req_common->req_number % kAppMaxBuffer].buf_ != nullptr))
    {
        release_msgbuf(ctx->rpc_, ctx->req_backward_msgbuf_ptr[req_common->req_number % kAppMaxBuffer]);
        __sync_synchronize();
    }

    ctx->forward_all_mpmc_queue->push(pin_msgbuf(ctx->rpc_, *req_msgbuf));
    __sync_synchronize();
    

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

    auto *req = reinterpret_cast<RPCMsgReq<PingRPCReq> *>(req_msgbuf.buf_);

    ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer] = req_msgbuf;

    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer];

    ctx->rpc_->enqueue_request(ctx->nginx_session_number, static_cast<uint8_t>(RPC_TYPE::RPC_PING_RESP),
                               &ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer], &resp_msgbuf,
                               callback_ping_resp, reinterpret_cast<void *>(req->req_common.req_number % kAppMaxBuffer));
}

void callback_user_timeline_write_resp(void *_context, void *_tag)
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

void handler_user_timeline_write_resp(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf)
{
    auto *req = reinterpret_cast<RPCMsgReq<UserTimeLineWriteReq> *>(req_msgbuf.buf_);
    ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer] = req_msgbuf;

    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer];

    flush_msgbuf_before_send(ctx->rpc_, ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer]);
    ctx->rpc_->enqueue_request(ctx->compose_post_session_number, static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_WRITE_RESP),
                               &ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer], &resp_msgbuf,
                               callback_user_timeline_write_resp, reinterpret_cast<void *>(req->req_common.req_number % kAppMaxBuffer));
}

void callback_user_timeline_read_resp(void *_context, void *_tag)
{
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);

    // erpc::MsgBuffer &req_msgbuf = ctx->req_backward_msgbuf[req_id];
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req_id];

    my_assert(resp_msgbuf.get_data_size() == 0);

    release_msgbuf(ctx->rpc_, ctx->req_backward_msgbuf[req_id]);
    ctx->req_backward_msgbuf[req_id].buf_ = nullptr;

}

void handler_user_timeline_read_resp(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf)
{
    auto *req = reinterpret_cast<RPCMsgReq<CommonRPCReq> *>(req_msgbuf.buf_);
    ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer] = req_msgbuf;

    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer];

    // ctx->rpc_->enqueue_request(ctx->client_session_number, static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_READ_RESP),
    //                            &ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer], &resp_msgbuf,
    //                            callback_user_timeline_read_resp, reinterpret_cast<void *>(req->req_common.req_number % kAppMaxBuffer));
    flush_msgbuf_before_send(ctx->rpc_, ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer]);
    ctx->rpc_->enqueue_request(ctx->nginx_session_number, static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_READ_RESP),
                               &ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer], &resp_msgbuf,
                               callback_user_timeline_read_resp, reinterpret_cast<void *>(req->req_common.req_number % kAppMaxBuffer));

}

void callback_post_storage_read_req(void *_context, void *_tag)
{
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);

    erpc::MsgBuffer &req_msgbuf = ctx->req_forward_msgbuf[req_id];
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_forward_msgbuf[req_id];

    my_assert(resp_msgbuf.get_data_size() == 0);

    release_msgbuf(ctx->rpc_, req_msgbuf);
    req_msgbuf.buf_ = nullptr;
}

void handler_post_storage_read_req(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf)
{
    auto *req = reinterpret_cast<RPCMsgReq<CommonRPCReq> *>(req_msgbuf.buf_);
    ctx->req_forward_msgbuf[req->req_common.req_number % kAppMaxBuffer] = req_msgbuf;

    erpc::MsgBuffer &resp_msgbuf = ctx->resp_forward_msgbuf[req->req_common.req_number % kAppMaxBuffer];

    flush_msgbuf_before_send(ctx->rpc_, ctx->req_forward_msgbuf[req->req_common.req_number % kAppMaxBuffer]);
    ctx->rpc_->enqueue_request(ctx->post_storage_session_number, static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_READ_REQ),
                               &ctx->req_forward_msgbuf[req->req_common.req_number % kAppMaxBuffer], &resp_msgbuf,
                               callback_post_storage_read_req, reinterpret_cast<void *>(req->req_common.req_number % kAppMaxBuffer));

}

void client_thread_func(size_t thread_id, ClientContext *ctx, erpc::Nexus *nexus)
{
    ctx->client_id_ = thread_id;
    std::vector<size_t> port_vec = flags_get_cxl_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

    uint8_t rpc_id = FLAGS_rpc_id + 20 + thread_id;

    erpc::Rpc<erpc::CXLTransport> rpc(nexus, static_cast<void *>(ctx),
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
        i = rpc.alloc_msg_buffer_or_die(sizeof(RPCMsgResp<CommonRPCResp>));
    }

    connect_sessions(ctx);

    using FUNC_HANDLER = std::function<void(ClientContext *, erpc::MsgBuffer)>;

    std::map<RPC_TYPE ,FUNC_HANDLER > handlers{
            {RPC_TYPE::RPC_PING_RESP, handler_ping_resp},
            {RPC_TYPE::RPC_USER_TIMELINE_WRITE_RESP, handler_user_timeline_write_resp},
            {RPC_TYPE::RPC_USER_TIMELINE_READ_RESP, handler_user_timeline_read_resp},
            {RPC_TYPE::RPC_POST_STORAGE_READ_REQ, handler_post_storage_read_req}
    };

    while (true)
    {
        unsigned size = ctx->forward_mpmc_queue->was_size();

        for (unsigned i = 0; i < size; i++)
        {
            erpc::MsgBuffer req_msg = ctx->forward_mpmc_queue->pop();
            __sync_synchronize();
            auto *req = reinterpret_cast<CommonReq *>(req_msg.buf_);
            my_assert(req->type == RPC_TYPE::RPC_POST_STORAGE_READ_REQ);
            handlers[req->type](ctx, req_msg);
        }

        size = ctx->backward_mpmc_queue->was_size();
        for (unsigned i = 0; i < size; i++)
        {
            erpc::MsgBuffer req_msg = ctx->backward_mpmc_queue->pop();
            __sync_synchronize();
            auto *req = reinterpret_cast<CommonReq *>(req_msg.buf_);

            my_assert(req->type == RPC_TYPE::RPC_PING_RESP || req->type == RPC_TYPE::RPC_USER_TIMELINE_WRITE_RESP ||
                            req->type == RPC_TYPE::RPC_USER_TIMELINE_READ_RESP
                            );
            handlers[req->type](ctx, req_msg);
        }
        ctx->rpc_->run_event_loop_once();
        if (unlikely(ctrl_c_pressed))
        {
            break;
        }
    }
}

void server_thread_func(size_t thread_id, ServerContext *ctx, erpc::Nexus *nexus)
{
    ctx->server_id_ = thread_id;
    std::vector<size_t> port_vec = flags_get_cxl_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

    uint8_t rpc_id = FLAGS_rpc_id + thread_id;

    erpc::Rpc<erpc::CXLTransport> rpc(nexus, static_cast<void *>(ctx),
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
        // printf("thread %zu: ping_req : %.2f, user_timeline write: %.2f read: %.2f, post_storage read: %.2f \n", thread_id, ctx->stat_req_ping_tot / seconds, ctx->stat_req_user_timeline_write_req_tot / seconds, ctx->stat_req_user_timeline_read_req_tot/ seconds, ctx->stat_req_post_storage_read_resp_tot / seconds);

        ctx->rpc_->reset_dpath_stats();
        // more handler
        if (ctrl_c_pressed == 1)
        {
            break;
        }
    }
}
void worker_thread_func(size_t thread_id, MPMC_QUEUE *producer, MPMC_QUEUE *consumer_back, MPMC_QUEUE *consumer_fwd, erpc::Rpc<erpc::CXLTransport> *rpc_, erpc::Rpc<erpc::CXLTransport> *server_rpc_)
{
    link_worker_cacheable(server_rpc_->get_rpc_id());
    // printf("[user_timeline] worker_thread_func %zu: STARTED, producer=%p\n", thread_id, (void*)producer);
    _unused(thread_id);
    _unused(rpc_);
    while (true)
    {
        unsigned size = producer->was_size();
        if (size > 0) {
            // printf("[user_timeline] worker_thread_func: processing %u messages\n", size);
        }

        for (unsigned i = 0; i < size; i++)
        {
            erpc::MsgBuffer req_msg = producer->pop();
            __sync_synchronize();

            auto *req = reinterpret_cast<CommonReq *>(req_msg.buf_);
            // printf("[user_timeline] worker_thread_func: got msg type=%u\n", static_cast<uint32_t>(req->type));
            // printf("read req %u, type %u\n", req->req_number, static_cast<uint32_t>(req->type));
            my_assert(req->type == RPC_TYPE::RPC_PING || req->type == RPC_TYPE::RPC_USER_TIMELINE_WRITE_REQ ||
                            req->type == RPC_TYPE::RPC_USER_TIMELINE_READ_REQ || req->type == RPC_TYPE::RPC_POST_STORAGE_READ_RESP);

            if(req->type == RPC_TYPE::RPC_USER_TIMELINE_READ_REQ){
                read_post_details(req_msg.buf_,rpc_,consumer_fwd,consumer_back);
                release_msgbuf(server_rpc_, req_msg);
            } else if(req->type == RPC_TYPE::RPC_USER_TIMELINE_WRITE_REQ){
                write_post_ids_and_return(req_msg.buf_,rpc_,consumer_back);
                release_msgbuf(server_rpc_, req_msg);
            } else if(req->type == RPC_TYPE::RPC_POST_STORAGE_READ_RESP){
                req->type = RPC_TYPE::RPC_USER_TIMELINE_READ_RESP;
                __sync_synchronize();
                consumer_back->push(req_msg);
            } else {
                req->type = RPC_TYPE::RPC_PING_RESP;
                __sync_synchronize();
                consumer_back->push(req_msg);
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

    auto collection = mongoc_client_get_collection(mongodb_client, "user-timeline", "user-timeline");

    my_assert(collection);

    bson_t* query = bson_new();
    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(collection, query, nullptr, nullptr);

    const bson_t *doc;

    while(mongoc_cursor_next(cursor,&doc)) {
        bson_iter_t iter;

        int64_t user_id;
        std::set<int64_t> post_ids;
        if (bson_iter_init_find(&iter, doc, "user_id")) {
            user_id = bson_iter_value(&iter)->value.v_int64;
        } else {
            fprintf(stderr, "can't find user_id in mongodb");
            exit(1);
        }

        if (bson_iter_init_find(&iter, doc, "posts")) {
            // get the values from array posts, and push them into post_ids
            bson_iter_t array_iter;
            bson_iter_recurse(&iter, &array_iter);
            while (bson_iter_next(&array_iter)) {
                bson_iter_t  post_iter;
                bson_iter_recurse(&array_iter, &post_iter);
                while(bson_iter_next(&post_iter)) {
                    if(BSON_ITER_IS_KEY(&post_iter, "post_id") && BSON_ITER_HOLDS_INT64(&post_iter)){
                        post_ids.insert(bson_iter_value(&post_iter)->value.v_int64);
                    }
                }
            }
        }
        user_timeline_map[user_id] = std::move(post_ids);
        if(ctrl_c_pressed){
            return;
        }
    }

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


        item->mongodb_init_finished = true;
        item->init_mutex.unlock();
    }

    printf("[user_timeline] mongodb init finished! Total init %zu users\n", user_timeline_map.size());
}

void leader_thread_func()
{
    erpc::Nexus nexus(FLAGS_server_addr, FLAGS_numa_server_node, 0);

    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_PING), ping_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_WRITE_REQ), user_timeline_write_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_READ_REQ), user_timeline_read_req_handler);

    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_READ_RESP), post_storage_read_resp_handler);

    std::vector<std::thread> clients(FLAGS_client_num);
    std::vector<std::thread> servers(FLAGS_server_num);
    std::vector<std::thread> workers(FLAGS_client_num);

    auto *context = new AppContext();

    std::thread mongodb_init_thread(mongodb_init, context);
    erpc::bind_to_core(mongodb_init_thread, 1, get_bind_core(1));


    clients[0] = std::thread(client_thread_func, 0, context->client_contexts_[0], &nexus);
    sleep(2);
    erpc::bind_to_core(clients[0], FLAGS_numa_client_node, get_bind_core(FLAGS_numa_client_node) + FLAGS_bind_core_offset);

    for (size_t i = 1; i < FLAGS_client_num; i++)
    {
        clients[i] = std::thread(client_thread_func, i, context->client_contexts_[i], &nexus);

        erpc::bind_to_core(clients[i], FLAGS_numa_client_node, get_bind_core(FLAGS_numa_client_node) + FLAGS_bind_core_offset);
    }

    for (size_t i = 0; i < FLAGS_server_num; i++)
    {
        servers[i] = std::thread(server_thread_func, i, context->server_contexts_[i], &nexus);

        erpc::bind_to_core(servers[i], FLAGS_numa_server_node, get_bind_core(FLAGS_numa_server_node) + FLAGS_bind_core_offset);
    }
    sleep(3);

    for (size_t i = 0; i < FLAGS_client_num; i++)
    {
        // printf("[user_timeline] Creating worker thread %zu, server_context->rpc_=%p\n",
        //        i, (void*)context->server_contexts_[i]->rpc_);
        my_assert(context->server_contexts_[i]->rpc_ != nullptr);
        workers[i] = std::thread(worker_thread_func, i, context->client_contexts_[i]->forward_all_mpmc_queue,
                                 context->client_contexts_[i]->backward_mpmc_queue, context->client_contexts_[i]->forward_mpmc_queue,
                                 context->client_contexts_[i]->rpc_, context->server_contexts_[i]->rpc_);
//        uint64_t worker_offset = FLAGS_worker_bind_core_offset == UINT64_MAX ? FLAGS_bind_core_offset : FLAGS_worker_bind_core_offset;
//        erpc::bind_to_core(workers[i], FLAGS_numa_worker_node, get_bind_core(FLAGS_numa_worker_node) + worker_offset);
        erpc::bind_to_core(workers[i], FLAGS_numa_client_node, get_bind_core(FLAGS_numa_client_node) + FLAGS_bind_core_offset);

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

    init_service_config(FLAGS_config_file,"user_timeline");
    init_specific_config();

    std::thread leader_thread(leader_thread_func);
    erpc::bind_to_core(leader_thread, 1, get_bind_core(1));
    leader_thread.join();
}
