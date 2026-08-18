#include <thread>
#include <atomic>
#include <gflags/gflags.h>
#include "post_storage.h"


#include "../utils_mongodb.h"
#include "../post_data.h"

int mongodb_conns_num = 0;
mongoc_client_pool_t* mongodb_client_pool;
std::atomic<bool> post_storage_clients_stopped{false};

void release_post_storage_map(ServerContext *ctx) {
    std::vector<std::pair<void*, size_t>> buffers_to_release;

    ctx->map_mutex.lock();
    buffers_to_release.reserve(ctx->post_storage_map.size());
    for (auto &entry : ctx->post_storage_map) {
        buffers_to_release.push_back(entry.second);
    }
    ctx->post_storage_map.clear();
    ctx->map_mutex.unlock();

    for (auto &buffer : buffers_to_release) {
        if (buffer.first != nullptr) {
            social_network_cxl::free_cxl_buffer(ctx->rpc_, buffer.first, buffer.second);
        }
    }

    if (!buffers_to_release.empty()) {
        printf("post_storage released %zu stored posts\n", buffers_to_release.size());
        fflush(stdout);
    }
}

void mongodb_init(AppContext *ctx) {
    if (config_json_all["post_storage_mongodb"].contains("connections")) {
        mongodb_conns_num = config_json_all["post_storage_mongodb"]["connections"];
    } else {
        mongodb_conns_num = 100;
    }
    mongodb_client_pool = init_mongodb_client_pool(config_json_all, "post_storage", mongodb_conns_num);
    mongoc_client_t *mongodb_client = mongoc_client_pool_pop(mongodb_client_pool);
    auto collection = mongoc_client_get_collection(mongodb_client, "post", "post");
    my_assert(collection);

    bson_t* query = bson_new();
    mongoc_cursor_t* cursor = mongoc_collection_find_with_opts(collection, query, nullptr, nullptr);
    const bson_t *doc;

    while (ctx->server_contexts_[0]->rpc_ == nullptr) {
        std::this_thread::yield();
    }
    // Give a short delay to ensure transport is fully connected
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    link_worker_cacheable(ctx->server_contexts_[0]->rpc_->get_rpc_id());

    int64_t count = 0;
    while (mongoc_cursor_next(cursor, &doc)) {
        char *str = bson_as_json(doc, nullptr);
        nlohmann::json post_json = nlohmann::json::parse(str);
        bson_free(str);

        PostData post;
        post.init();
        if (post_json.contains("post_id")) post.post_id = post_json["post_id"].get<int64_t>();
        if (post_json.contains("req_id")) post.req_id = post_json["req_id"].get<int64_t>();
        if (post_json.contains("timestamp")) post.timestamp = post_json["timestamp"].get<int64_t>();
        if (post_json.contains("text")) {
            auto t = post_json["text"].get<std::string>();
            size_t l = std::min(t.size(), (size_t)SN_TEXT_LEN - 1);
            memcpy(post.text, t.data(), l);
            post.text[l] = '\0';
        }
        if (post_json.contains("post_type")) post.post_type = post_json["post_type"].get<int>();

        if (post_json.contains("creator")) {
            if (post_json["creator"].contains("user_id")) post.creator_user_id = post_json["creator"]["user_id"].get<int64_t>();
            if (post_json["creator"].contains("username")) {
                auto u = post_json["creator"]["username"].get<std::string>();
                size_t l = std::min(u.size(), (size_t)SN_USERNAME_LEN - 1);
                memcpy(post.creator_username, u.data(), l);
                post.creator_username[l] = '\0';
            }
        }
        if (post_json.contains("user_mentions")) {
            for (auto &item : post_json["user_mentions"]) {
                if (post.mentions_count < SN_MAX_MENTIONS) {
                    if (item.contains("user_id")) post.mentions_ids[post.mentions_count++] = item["user_id"].get<int64_t>();
                }
            }
        }
        if (post_json.contains("media")) {
            for (auto &item : post_json["media"]) {
                if (post.media_count < SN_MAX_MEDIA) {
                    if (item.contains("media_id")) post.media_ids[post.media_count] = item["media_id"].get<int64_t>();
                    if (item.contains("media_type")) {
                        auto mt = item["media_type"].get<std::string>();
                        size_t l = std::min(mt.size(), (size_t)SN_MEDIA_TYPE_LEN - 1);
                        memcpy(post.media_types[post.media_count], mt.data(), l);
                        post.media_types[post.media_count][l] = '\0';
                    }
                    post.media_count++;
                }
            }
        }

        size_t size = sizeof(PostData);

        void *cxl_ptr = social_network_cxl::alloc_cxl_buffer(ctx->server_contexts_[0]->rpc_, size);
        if (!cxl_ptr) {
            printf("CRITICAL ERROR: alloc_cxl_buffer returned nullptr for size %zu\n", size);
            continue;
        }

        memcpy(cxl_ptr, &post, size);
#if defined(USE_NO_CC_QUEUE) && !defined(USE_ONE_SIDE_READ)
        clflush(cxl_ptr, size);
#endif
        // It's the first time placing it in map, the allocator creates it with refcount=1. 
        // We will keep it mapped with that reference count.

        ctx->server_contexts_[0]->map_mutex.lock();
        ctx->server_contexts_[0]->post_storage_map[post.post_id] = {cxl_ptr, size};
        ctx->server_contexts_[0]->map_mutex.unlock();
        count++;
    }
    mongoc_cursor_destroy(cursor);
    bson_destroy(query);
    mongoc_client_pool_push(mongodb_client_pool, mongodb_client);
    
    unlink_worker_cacheable();
    
    printf("post_storage mongodb init finished. loaded %ld posts\n", count); fflush(stdout);
    while (!ctrl_c_pressed) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}


void connect_sessions(ClientContext *c) {
    c->client_session_num_ = c->rpc_->create_session(client_addr, get_remote_rpc_id(client_addr));
    my_assert(c->client_session_num_ >= 0, "Failed to create session");

    c->user_timeline_session_num_ = c->rpc_->create_session(user_timeline_addr, get_remote_rpc_id(user_timeline_addr));
    my_assert(c->user_timeline_session_num_ >= 0, "Failed to create session");

    c->home_timeline_session_num_ = c->rpc_->create_session(home_timeline_addr, get_remote_rpc_id(home_timeline_addr));
    my_assert(c->home_timeline_session_num_ >= 0, "Failed to create session");

    c->compose_post_session_num_ = c->rpc_->create_session(compose_post_addr, get_remote_rpc_id(compose_post_addr));
    my_assert(c->compose_post_session_num_ >= 0, "Failed to create session");
    
    while (c->num_sm_resps_ != 4) {
        c->rpc_->run_event_loop(kAppEvLoopMs);
        if (unlikely(ctrl_c_pressed == 1)) {
            // printf("Ctrl-C pressed. Exiting\n");
            return;
        }
    }
}

void ping_req_handler(erpc::ReqHandle *req_handle, void *_context) {
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_ping_tot++;

    auto *req_msgbuf = req_handle->get_req_msgbuf();
    my_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<PingRPCReq>), "data size not match");

    auto &resp = req_handle->pre_resp_msgbuf_;
    resp = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgResp<PingRPCResp>));
    new (resp.buf_) RPCMsgResp<PingRPCResp>(RPC_TYPE::RPC_PING_RESP, 0, 0, {});

    ctx->rpc_->enqueue_response(req_handle, &resp);
}

void unsupported_req_handler(erpc::ReqHandle *req_handle, void *_context) {
    auto *ctx = static_cast<ServerContext *>(_context);
    auto *req_msgbuf = req_handle->get_req_msgbuf();
    invalidate_msgbuf_before_read(ctx->rpc_, *req_msgbuf);
    auto *req = reinterpret_cast<CommonReq *>(req_msgbuf->buf_);

    // printf("[post_storage] unsupported_req_handler: type=%u req_number=%u size=%zu\n",
        //    static_cast<uint32_t>(req->type), req->req_number, req_msgbuf->get_data_size());

    auto &resp = req_handle->pre_resp_msgbuf_;
    resp = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgResp<CommonRPCResp>));
    new (resp.buf_) RPCMsgResp<CommonRPCResp>(req->type, req->req_number, -1, {0});
    ctx->rpc_->enqueue_response(req_handle, &resp);
}

void post_storage_read_req_handler(erpc::ReqHandle *req_handle, void *_context) {
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_post_storage_read_tot++;

    auto *req_msgbuf = req_handle->get_req_msgbuf();
    invalidate_msgbuf_before_read(ctx->rpc_, *req_msgbuf);
    auto *req = reinterpret_cast<RPCMsgReq<PostStorageReadCXLReq> *>(req_msgbuf->buf_);

    auto *storage_handler = new StorageHandler();
    storage_handler->req_number = req->req_common.req_number;
    storage_handler->rpc_type = req->req_control.rpc_type;
    storage_handler->is_read = true;

    for (size_t i = 0; i < req->req_control.count; i++) {
        storage_handler->post_ids.push_back(req->req_control.post_ids[i]);
    }

    ctx->map_mutex.lock();
    for (auto post_id : storage_handler->post_ids) {
        auto it = ctx->post_storage_map.find(post_id);
        if (it != ctx->post_storage_map.end()) {
            storage_handler->buffers.push_back(it->second);
        }
    }
    ctx->map_mutex.unlock();

    storage_queues[ctx->server_id_]->push(storage_handler);

    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, 0);
    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void post_storage_write_req_handler(erpc::ReqHandle *req_handle, void *_context) {
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_post_storage_write_tot++;

    auto *req_msgbuf = req_handle->get_req_msgbuf();
    invalidate_msgbuf_before_read(ctx->rpc_, *req_msgbuf);
    auto *req = reinterpret_cast<RPCMsgReq<PostStorageWriteCXLReq> *>(req_msgbuf->buf_);
    int64_t final_post_id = req->req_control.post_id;
    erpc::MsgBuffer mb;
    
    // cxl_ptr points to the very base allocated by compose_post
    void *cxl_ptr = social_network_cxl::get_cxl_allocator(ctx->rpc_)->offset_to_ptr(req->req_control.offset);
    size_t data_size = req->req_control.size;
    
    std::pair<void*, size_t> old_buffer = {nullptr, 0};
    ctx->map_mutex.lock();
    auto old_it = ctx->post_storage_map.find(final_post_id);
    if (old_it == ctx->post_storage_map.end() || old_it->second.first != cxl_ptr) {
        // Keep one post_storage-owned reference so sender-side release cannot free it.
        social_network_cxl::add_ref_cxl_buffer(ctx->rpc_, cxl_ptr);
        if (old_it != ctx->post_storage_map.end()) {
            old_buffer = old_it->second;
        }
    }
    ctx->post_storage_map[final_post_id] = {cxl_ptr, data_size};
    ctx->map_mutex.unlock();

    if (old_buffer.first != nullptr) {
        social_network_cxl::free_cxl_buffer(ctx->rpc_, old_buffer.first, old_buffer.second);
    }
    
    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, 0);
    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
    
    auto *storage_handler = new StorageHandler();
    storage_handler->req_number = req->req_common.req_number;
    storage_handler->rpc_type = static_cast<uint32_t>(req->req_common.type);
    storage_handler->is_read = false;
    storage_queues[ctx->server_id_]->push(storage_handler);
}

void callback_post_storage_resp(void *_context, void *_tag) {
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req_id];
    my_assert(resp_msgbuf.get_data_size() == 0, "data size not match");
    release_msgbuf(ctx->rpc_, ctx->req_backward_msgbuf[req_id]);
    ctx->req_backward_msgbuf[req_id].buf_ = nullptr;
}

void client_thread_func(size_t thread_id, ClientContext *ctx, erpc::Nexus *nexus) {
    ctx->client_id_ = thread_id;
    std::vector<size_t> port_vec = flags_get_cxl_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());
    uint8_t rpc_id = FLAGS_rpc_id + 20 + thread_id;

    erpc::Rpc<erpc::CXLTransport> rpc(nexus, static_cast<void *>(ctx),
                                    rpc_id,
                                    basic_sm_handler_client, phy_port);
    rpc.retry_connect_on_invalid_rpc_id_ = true;
    ctx->rpc_ = &rpc;

    for (auto & i : ctx->resp_backward_msgbuf) {
        i = rpc.alloc_msg_buffer_or_die(sizeof(RPCMsgResp<CommonRPCResp>));
    }

    connect_sessions(ctx);

    while (!ctrl_c_pressed) {
        StorageHandler *storage_handler;
        if (storage_queues[thread_id % FLAGS_server_num]->try_pop(storage_handler)) {
            if (storage_handler->is_read) {
                auto dest_session = (storage_handler->rpc_type == static_cast<uint32_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ)) 
                                        ? ctx->home_timeline_session_num_ 
                                        : ctx->user_timeline_session_num_;

                auto &req_msgbuf = ctx->req_backward_msgbuf[storage_handler->req_number % kAppMaxBuffer];
                req_msgbuf = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<PostStorageReadCXLResp>));

                auto *cxl_resp_ptr = reinterpret_cast<PostStorageReadCXLResp*>((req_msgbuf.buf_) + sizeof(CommonReq));
                cxl_resp_ptr->count = 0;

                for (auto &buffer : storage_handler->buffers) {
                    if (cxl_resp_ptr->count < 64) {
                        void* original_ptr = buffer.first;
                        uint64_t offset = social_network_cxl::get_cxl_allocator(ctx->rpc_)->ptr_to_offset(original_ptr);
                        cxl_resp_ptr->posts[cxl_resp_ptr->count].offset = offset;
                        cxl_resp_ptr->posts[cxl_resp_ptr->count].size = buffer.second;
                        cxl_resp_ptr->posts[cxl_resp_ptr->count].ref_count = 1;
                        cxl_resp_ptr->count++;
                        
                        social_network_cxl::add_ref_cxl_buffer(ctx->rpc_, original_ptr);
                    }
                }
                
                new (req_msgbuf.buf_) RPCMsgReq<PostStorageReadCXLResp>(RPC_TYPE::RPC_POST_STORAGE_READ_RESP, storage_handler->req_number, *cxl_resp_ptr);

                auto &resp_msgbuf = ctx->resp_backward_msgbuf[storage_handler->req_number % kAppMaxBuffer];
                flush_msgbuf_before_send(ctx->rpc_, req_msgbuf);
                ctx->rpc_->enqueue_request(dest_session,
                                           static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_READ_RESP),
                                           &req_msgbuf,
                                           &resp_msgbuf,
                                           callback_post_storage_resp,
                                           reinterpret_cast<void *>(static_cast<std::uintptr_t>(storage_handler->req_number % kAppMaxBuffer)));
            } else {
                auto &req_msgbuf = ctx->req_backward_msgbuf[storage_handler->req_number % kAppMaxBuffer];
                req_msgbuf = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<CommonRPCReq>));
                new (req_msgbuf.buf_) RPCMsgReq<CommonRPCReq>(RPC_TYPE::RPC_POST_STORAGE_WRITE_RESP, storage_handler->req_number, {0});

                auto &resp_msgbuf = ctx->resp_backward_msgbuf[storage_handler->req_number % kAppMaxBuffer];
                flush_msgbuf_before_send(ctx->rpc_, req_msgbuf);
                ctx->rpc_->enqueue_request(ctx->client_session_num_,
                                           static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_WRITE_RESP),
                                           &req_msgbuf,
                                           &resp_msgbuf,
                                           callback_post_storage_resp,
                                           reinterpret_cast<void *>(static_cast<std::uintptr_t>(storage_handler->req_number % kAppMaxBuffer)));
            }

            delete storage_handler;
        }

        rpc.run_event_loop_once();
    }
}

void server_thread_func(size_t thread_id, ServerContext *ctx, erpc::Nexus *nexus) {
    ctx->server_id_ = thread_id;
    std::vector<size_t> port_vec = flags_get_cxl_ports(0);

    erpc::Rpc<erpc::CXLTransport> rpc(nexus, static_cast<void *>(ctx),
                                        FLAGS_rpc_id + thread_id,
                                        basic_sm_handler_server, port_vec[0]);
    ctx->rpc_ = &rpc;

    // printf("Server thread %zu: rpc_id %d, listening on port %zu\n",
        //    thread_id, rpc.get_rpc_id(), port_vec[0]);



    while (!ctrl_c_pressed) {
        rpc.run_event_loop(kAppEvLoopMs);
        if (ctrl_c_pressed) break;
    }

    while (!post_storage_clients_stopped.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    release_post_storage_map(ctx);

    // printf("Server thread %zu exiting\n", thread_id);
}

void leader_thread_func(erpc::Nexus *nexus, AppContext *context) {
    std::vector<std::thread> servers(FLAGS_server_num);
    std::vector<std::thread> clients(FLAGS_client_num);
    for (size_t i = 0; i < FLAGS_server_num; i++) {
        servers[i] = std::thread(server_thread_func, i, context->server_contexts_[i], nexus);
        erpc::bind_to_core(servers[i], FLAGS_numa_server_node, get_bind_core(FLAGS_numa_server_node) + FLAGS_bind_core_offset);
    }
    for (size_t i = 0; i < FLAGS_client_num; i++) {
        clients[i] = std::thread(client_thread_func, i, context->client_contexts_[i], nexus);
        erpc::bind_to_core(clients[i], FLAGS_numa_client_node, get_bind_core(FLAGS_numa_client_node) + FLAGS_bind_core_offset);
    }
    for (size_t i = 0; i < FLAGS_client_num; i++) {
        clients[i].join();
    }
    post_storage_clients_stopped.store(true, std::memory_order_release);
    for (size_t i = 0; i < FLAGS_server_num; i++) {
        servers[i].join();
    }
}

int main(int argc, char **argv) {
    signal(SIGINT, ctrl_c_handler);
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    init_service_config(FLAGS_config_file, "post_storage");
    init_specific_config();

    erpc::Nexus nexus(FLAGS_server_addr, FLAGS_numa_server_node, 0);
    // Register fallback handlers first to prevent null handler dispatch when wrong request types arrive.
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_PING_RESP), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_UNIQUE_ID), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_URL_SHORTEN), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_USER_MENTION), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_COMPOSE_POST_WRITE_REQ), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_COMPOSE_POST_WRITE_RESP), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_WRITE_REQ), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_WRITE_RESP), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_READ_REQ), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_READ_RESP), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_READ_RESP), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_WRITE_RESP), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_WRITE_REQ), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_WRITE_RESP), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_COMPOSE_CREATOR_WITH_USER_ID), unsupported_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_RMEM_PARAM), unsupported_req_handler);

    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_PING), ping_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_READ_REQ), post_storage_read_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_WRITE_REQ), post_storage_write_req_handler);

    AppContext context;
    std::thread mongodb_init_thread(mongodb_init, &context);
    erpc::bind_to_core(mongodb_init_thread, 1, get_bind_core(1));

    std::thread leader_thread(leader_thread_func, &nexus, &context);
    erpc::bind_to_core(leader_thread, 1, get_bind_core(1));
    leader_thread.join();
    mongodb_init_thread.join();

}
