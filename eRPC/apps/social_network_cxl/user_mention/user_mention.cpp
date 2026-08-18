#include <thread>

#include "../utils_mongodb.h"

#include "user_mention.h"


void connect_sessions(ClientContext *c)
{
    _unused(c);
    // connect to backward server
    c->backward_session_num_ = c->rpc_->create_session(compose_post_addr, get_remote_rpc_id(compose_post_addr));
    my_assert(c->backward_session_num_ >= 0);

    while (c->num_sm_resps_ != 1)
    {
        c->rpc_->run_event_loop(kAppEvLoopMs);
        if (unlikely(ctrl_c_pressed == 1))
    {
            // printf("Ctrl-C pressed. Exiting\n");
            return;
    }
    }
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
//         ctx->forward_spsc_queue->push(clone_msgbuf(ctx->rpc_, *req_msgbuf));
        
    } else {
        ctx->is_pinged = true;
        ctx->ping_req_number = req->req_common.req_number;
    }
    ctx->init_mutex.unlock();

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void user_mention_handler(erpc::ReqHandle *req_handler, void *_context)
{
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_user_mention_tot++;

    ctx->init_mutex.lock();
    my_assert(ctx->mongodb_init_finished);
    ctx->init_mutex.unlock();


    auto *req_msgbuf = req_handler->get_req_msgbuf();
    auto *req = reinterpret_cast<RPCMsgReq<PostStorageWriteCXLReq> *>(req_msgbuf->buf_);

    my_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<PostStorageWriteCXLReq>));

#ifdef ERPC_CXL
    uint64_t cxl_offset = req->req_control.offset;
    void *cxl_ptr = social_network_cxl::get_cxl_allocator(ctx->rpc_)->offset_to_ptr(cxl_offset);
    PostData *cxl_post_ptr = reinterpret_cast<PostData *>(cxl_ptr);
    std::string original_text = cxl_post_ptr->text;
    
    static const std::regex e("@[a-zA-Z0-9-_]+");
    std::sregex_iterator it1(original_text.begin(), original_text.end(), e);
    std::sregex_iterator end;
    while (it1 != end) {
        std::string name = (*it1).str().substr(1);
        if(user_mention_map.find(name) != user_mention_map.end()) {
            social_network::UserMention *target_user_mention = user_mention_map[name];
            if (cxl_post_ptr->mentions_count < SN_MAX_MENTIONS) {
                cxl_post_ptr->mentions_ids[cxl_post_ptr->mentions_count++] = target_user_mention->user_id();
            }
        } else {
            fprintf(stderr, "Warning: don't find user: %s\n", name.c_str());
        }
        it1++;
    }
#endif

    size_t resp_data_length = 0;

    new (req_handler->pre_resp_msgbuf_.buf_) RPCMsgResp<CommonRPCResp>(req->req_common.type, req->req_common.req_number, 0, {resp_data_length});

    ctx->rpc_->resize_msg_buffer(&req_handler->pre_resp_msgbuf_, sizeof(RPCMsgResp<CommonRPCResp>) + resp_data_length);

    ctx->rpc_->enqueue_response(req_handler, &req_handler->pre_resp_msgbuf_);
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

    ctx->rpc_->enqueue_request(ctx->backward_session_num_, static_cast<uint8_t>(RPC_TYPE::RPC_PING_RESP),
                               &ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer], &resp_msgbuf,
                               callback_ping_resp, reinterpret_cast<void *>(req->req_common.req_number % kAppMaxBuffer));
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
        // TODO
        i = rpc.alloc_msg_buffer_or_die(sizeof(RPCMsgResp<CommonRPCResp>));
    }

    connect_sessions(ctx);

    using FUNC_HANDLER = std::function<void(ClientContext *, erpc::MsgBuffer)>;
    FUNC_HANDLER handlers[] = {nullptr, handler_ping_resp};

    while (true)
    {
        unsigned size = ctx->backward_spsc_queue->was_size();
        for (unsigned i = 0; i < size; i++)
        {
            erpc::MsgBuffer req_msg = ctx->backward_spsc_queue->pop();
            auto *req = reinterpret_cast<CommonReq *>(req_msg.buf_);
            if (req->type != RPC_TYPE::RPC_PING_RESP)
                // printf("req->type=%u\n", static_cast<uint32_t>(req->type));
            my_assert(req->type == RPC_TYPE::RPC_PING_RESP);
            handlers[static_cast<uint8_t>(req->type)](ctx, req_msg);
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
        // printf("thread %zu: ping_req : %.2f, user_mention_req : %.2f \n", thread_id, ctx->stat_req_ping_tot / seconds, ctx->stat_req_user_mention_tot / seconds);

        ctx->rpc_->reset_dpath_stats();
        // more handler
        if (ctrl_c_pressed == 1)
        {
            break;
        }
    }
}
void worker_thread_func(size_t thread_id, SPSC_QUEUE *producer, SPSC_QUEUE *consumer, erpc::Rpc<erpc::CXLTransport> *rpc_, erpc::Rpc<erpc::CXLTransport> *server_rpc_)
{
    link_worker_cacheable(server_rpc_->get_rpc_id());
    _unused(thread_id);
    _unused(rpc_);
    _unused(server_rpc_);
    while (true)
    {
        unsigned size = producer->was_size();
        for (unsigned i = 0; i < size; i++)
        {
            erpc::MsgBuffer req_msg = producer->pop();

            auto *req = reinterpret_cast<CommonReq *>(req_msg.buf_);
            my_assert(req->type == RPC_TYPE::RPC_PING);
            req->type = RPC_TYPE::RPC_PING_RESP;
            consumer->push(req_msg);
        }
        if (ctrl_c_pressed == 1)
        {
            break;
        }
    }
    unlink_worker_cacheable();
}

void mongodb_init(AppContext *ctx){
    mongodb_client_pool = init_mongodb_client_pool(config_json_all, "user", mongodb_conns_num);
    mongoc_client_t *mongodb_client =  mongoc_client_pool_pop(mongodb_client_pool);

    auto collection = mongoc_client_get_collection(mongodb_client, "user", "user");

    my_assert(collection);
 
    bson_t* query = bson_new();
    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(collection, query, nullptr, nullptr);

    const bson_t *doc;

    while(mongoc_cursor_next(cursor,&doc)) {
        bson_iter_t iter;
        auto *new_user_mention = new social_network::UserMention();

        if (bson_iter_init_find(&iter, doc, "user_id")) {
            new_user_mention->set_user_id(bson_iter_value(&iter)->value.v_int64);
        } else {
            fprintf(stderr, "can't find user_id in mongodb");
            exit(1);
        }

        if (bson_iter_init_find(&iter, doc, "username")) {
            new_user_mention->set_username(bson_iter_value(&iter)->value.v_utf8.str);
        } else {
            fprintf(stderr, "can't find username in mongodb");
            exit(1);
        }
        user_mention_map[new_user_mention->username()] = new_user_mention;
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

            item->forward_spsc_queue->push(_buf);
        }


        item->mongodb_init_finished = true;
        item->init_mutex.unlock();
    }

    printf("[user_mention] mongodb init finished! Total user num: %zu\n", user_mention_map.size());
}

void leader_thread_func()
{
    erpc::Nexus nexus(FLAGS_server_addr, FLAGS_numa_server_node, 0);

    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_PING), ping_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_USER_MENTION), user_mention_handler);

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

    servers[0] = std::thread(server_thread_func, 0, context->server_contexts_[0], &nexus);
    sleep(2);
    erpc::bind_to_core(servers[0], FLAGS_numa_server_node, get_bind_core(FLAGS_numa_server_node) + FLAGS_bind_core_offset);

    for (size_t i = 1; i < FLAGS_server_num; i++)
    {
        servers[i] = std::thread(server_thread_func, i, context->server_contexts_[i], &nexus);

        erpc::bind_to_core(servers[i], FLAGS_numa_server_node, get_bind_core(FLAGS_numa_server_node) + FLAGS_bind_core_offset);
    }
    sleep(3);

    for (size_t i = 0; i < FLAGS_client_num; i++)
    {
    my_assert(context->server_contexts_[i]->rpc_ != nullptr);
    workers[i] = std::thread(worker_thread_func, i, context->client_contexts_[i]->forward_spsc_queue, context->client_contexts_[i]->backward_spsc_queue, context->client_contexts_[i]->rpc_, context->server_contexts_[i]->rpc_);
////    uint64_t worker_offset = FLAGS_worker_bind_core_offset == UINT64_MAX ? FLAGS_bind_core_offset : FLAGS_worker_bind_core_offset;
////    erpc::bind_to_core(workers[i], FLAGS_numa_worker_node, get_bind_core(FLAGS_numa_worker_node) + worker_offset);
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
    clients[i].join();
    }
    for (size_t i = 0; i < FLAGS_server_num; i++)
    {
        servers[i].join();
    }
    for (size_t i = 0; i < FLAGS_client_num; i++)
    {
    workers[i].join();
    }
}

int main(int argc, char **argv)
{

    signal(SIGINT, ctrl_c_handler);
    signal(SIGTERM, ctrl_c_handler);
    // only config_file is required!!!
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    init_service_config(FLAGS_config_file,"user_mention");
    init_specific_config();

    std::thread leader_thread(leader_thread_func);
    erpc::bind_to_core(leader_thread, 1, get_bind_core(1));
    leader_thread.join();
}
