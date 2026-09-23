#include "post_storage.h"
#include "../ub_breakdown.h"

#include <gflags/gflags.h>

#include <atomic>
#include <thread>

#include "../post_data.h"
#include "../utils_mongodb.h"

int mongodb_conns_num = 0;
mongoc_client_pool_t *mongodb_client_pool;
std::atomic<bool> post_storage_clients_stopped{false};

void release_post_storage_map(ServerContext *ctx) {
  std::vector<SharedPostBuffer> buffers_to_release;

  ctx->map_mutex.lock();
  buffers_to_release.reserve(ctx->post_storage_map.size());
  for (auto &entry : ctx->post_storage_map) {
    buffers_to_release.push_back(entry.second);
  }
  ctx->post_storage_map.clear();
  ctx->map_mutex.unlock();

  for (auto &buffer : buffers_to_release) {
    if (buffer.buffer.buf_ != nullptr) {
      release_owned_post(ctx->rpc_, buffer);
    }
  }

  if (!buffers_to_release.empty()) {
    printf("post_storage released %zu stored posts\n",
           buffers_to_release.size());
    fflush(stdout);
  }
}

void mongodb_init(AppContext *ctx) {
  if (config_json_all["post_storage_mongodb"].contains("connections")) {
    mongodb_conns_num = config_json_all["post_storage_mongodb"]["connections"];
  } else {
    mongodb_conns_num = 100;
  }
  mongodb_client_pool = init_mongodb_client_pool(
      config_json_all, "post_storage", mongodb_conns_num);
  mongoc_client_t *mongodb_client = mongoc_client_pool_pop(mongodb_client_pool);
  auto collection =
      mongoc_client_get_collection(mongodb_client, "post", "post");
  my_assert(collection);

  bson_t *query = bson_new();
  mongoc_cursor_t *cursor =
      mongoc_collection_find_with_opts(collection, query, nullptr, nullptr);
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
    if (post_json.contains("post_id"))
      post.post_id = post_json["post_id"].get<int64_t>();
    if (post_json.contains("req_id"))
      post.req_id = post_json["req_id"].get<int64_t>();
    if (post_json.contains("timestamp"))
      post.timestamp = post_json["timestamp"].get<int64_t>();
    if (post_json.contains("text")) {
      auto t = post_json["text"].get<std::string>();
      post.text_length = set_post_string(post.text, t);
    }
    if (post_json.contains("post_type"))
      post.post_type = post_json["post_type"].get<int>();

    if (post_json.contains("creator")) {
      if (post_json["creator"].contains("user_id"))
        post.creator_user_id = post_json["creator"]["user_id"].get<int64_t>();
      if (post_json["creator"].contains("username")) {
        auto u = post_json["creator"]["username"].get<std::string>();
        post.creator_username_length =
            set_post_string(post.creator_username, u);
      }
    }
    if (post_json.contains("user_mentions")) {
      for (auto &item : post_json["user_mentions"]) {
        if (post.mentions_count == SN_MAX_MENTIONS)
          throw std::length_error("PostData: too many mentions");
        const size_t i = post.mentions_count++;
        post.mentions_ids[i] = item.value("user_id", int64_t{0});
        post.mentions_username_lengths[i] = set_post_string(
            post.mentions_usernames[i], item.value("username", std::string{}));
      }
    }
    if (post_json.contains("media")) {
      for (auto &item : post_json["media"]) {
        if (post.media_count == SN_MAX_MEDIA)
          throw std::length_error("PostData: too many media");
        {
          if (item.contains("media_id"))
            post.media_ids[post.media_count] = item["media_id"].get<int64_t>();
          if (item.contains("media_type")) {
            auto mt = item["media_type"].get<std::string>();
            post.media_type_lengths[post.media_count] =
                set_post_string(post.media_types[post.media_count], mt);
          }
          post.media_count++;
        }
      }
    }

    if (post_json.contains("urls")) {
      for (const auto &item : post_json["urls"]) {
        if (post.urls_count == SN_MAX_URLS)
          throw std::length_error("PostData: too many URLs");
        const size_t i = post.urls_count++;
        post.shortened_url_lengths[i] = set_post_string(
            post.shortened_urls[i], item.value("shortened_url", std::string{}));
        post.expanded_url_lengths[i] = set_post_string(
            post.expanded_urls[i], item.value("expanded_url", std::string{}));
      }
    }
    validate_post_data(post);
    size_t size = sizeof(PostData);

    SharedPostBuffer shared_post =
        alloc_shared_post(ctx->server_contexts_[0]->rpc_, size);
    if (shared_post.buffer.buf_ == nullptr) {
      throw std::runtime_error(
          "PostData: shared allocation failed; increase arena/region capacity");
    }

    memcpy(shared_post.buffer.buf_, &post, size);
    publish_shared_post(ctx->server_contexts_[0]->rpc_, shared_post);

    ctx->server_contexts_[0]->map_mutex.lock();
    ctx->server_contexts_[0]->post_storage_map[post.post_id] = shared_post;
    ctx->server_contexts_[0]->map_mutex.unlock();
    count++;
  }
  mongoc_cursor_destroy(cursor);
  bson_destroy(query);
  mongoc_client_pool_push(mongodb_client_pool, mongodb_client);

  unlink_worker_cacheable();

  printf("post_storage mongodb init finished. loaded %ld posts\n", count);
  fflush(stdout);
  while (!ctrl_c_pressed) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void connect_sessions(ClientContext *c) {
  c->client_session_num_ =
      c->rpc_->create_session(client_addr, get_remote_rpc_id(client_addr));
  my_assert(c->client_session_num_ >= 0, "Failed to create session");

  c->user_timeline_session_num_ = c->rpc_->create_session(
      user_timeline_addr, get_remote_rpc_id(user_timeline_addr));
  my_assert(c->user_timeline_session_num_ >= 0, "Failed to create session");

  c->home_timeline_session_num_ = c->rpc_->create_session(
      home_timeline_addr, get_remote_rpc_id(home_timeline_addr));
  my_assert(c->home_timeline_session_num_ >= 0, "Failed to create session");

  c->compose_post_session_num_ = c->rpc_->create_session(
      compose_post_addr, get_remote_rpc_id(compose_post_addr));
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
  my_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<PingRPCReq>),
            "data size not match");

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

  // printf("[post_storage] unsupported_req_handler: type=%u req_number=%u
  // size=%zu\n",
  //    static_cast<uint32_t>(req->type), req->req_number,
  //    req_msgbuf->get_data_size());

  auto &resp = req_handle->pre_resp_msgbuf_;
  resp = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgResp<CommonRPCResp>));
  new (resp.buf_)
      RPCMsgResp<CommonRPCResp>(req->type, req->req_number, -1, {0});
  ctx->rpc_->enqueue_response(req_handle, &resp);
}

void post_storage_read_req_handler(erpc::ReqHandle *req_handle,
                                   void *_context) {
  auto *ctx = static_cast<ServerContext *>(_context);
  ctx->stat_req_post_storage_read_tot++;

  auto *req_msgbuf = req_handle->get_req_msgbuf();
  invalidate_msgbuf_before_read(ctx->rpc_, *req_msgbuf);
  auto *req =
      reinterpret_cast<RPCMsgReq<PostStorageReadCXLReq> *>(req_msgbuf->buf_);
  my_assert(req_msgbuf->get_data_size() ==
            sizeof(RPCMsgReq<PostStorageReadCXLReq>));
  PostStorageReadCXLResp response{};
  const uint64_t lock_start = sn_profile::start();
  ctx->map_mutex.lock();
  sn_profile::record(sn_profile::Stage::kStorageReadLock, lock_start);
  const uint64_t lookup_start = sn_profile::start();
  auto it = ctx->post_storage_map.find(req->req_control.post_id);
  sn_profile::record(sn_profile::Stage::kStorageReadLookup, lookup_start);
  if (it != ctx->post_storage_map.end()) {
    // This reference is transferred through the response handle to the
    // final client. Intermediate services only forward the handle.
    const uint64_t retain_start = sn_profile::start();
    retain_shared_post(ctx->rpc_, it->second);
    sn_profile::record(sn_profile::Stage::kStorageReadRetain, retain_start);
    response.count = 1;
    response.post = it->second.handle;
  }
  ctx->map_mutex.unlock();

  const uint64_t response_start = sn_profile::start();
  new (req_handle->pre_resp_msgbuf_.buf_) RPCMsgResp<PostStorageReadCXLResp>(
      RPC_TYPE::RPC_POST_STORAGE_READ_RESP, req->req_common.req_number, 0,
      response);
  ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_,
                               sizeof(RPCMsgResp<PostStorageReadCXLResp>));
  ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
  sn_profile::record(sn_profile::Stage::kStorageReadResponse, response_start);
}

void post_storage_write_req_handler(erpc::ReqHandle *req_handle,
                                    void *_context) {
  auto *ctx = static_cast<ServerContext *>(_context);
  ctx->stat_req_post_storage_write_tot++;

  auto *req_msgbuf = req_handle->get_req_msgbuf();
  invalidate_msgbuf_before_read(ctx->rpc_, *req_msgbuf);
  auto *req =
      reinterpret_cast<RPCMsgReq<PostStorageWriteCXLReq> *>(req_msgbuf->buf_);
  my_assert(req_msgbuf->get_data_size() ==
            sizeof(RPCMsgReq<PostStorageWriteCXLReq>));
#ifdef ERPC_UB
  const uint64_t alloc_start = sn_profile::start();
  SharedPostBuffer stored_post = alloc_shared_post(ctx->rpc_, sizeof(PostData));
  sn_profile::record(sn_profile::Stage::kStorageWriteAlloc, alloc_start);
  my_assert(stored_post.buffer.buf_ != nullptr);
  auto *stored = new (stored_post.buffer.buf_) PostData;
  try {
    const uint64_t copy_start = sn_profile::start();
    std::memcpy(stored, &req->req_control.post, sizeof(PostData));
    sn_profile::record(sn_profile::Stage::kStorageWriteCopy, copy_start);
    const uint64_t validate_start = sn_profile::start();
    validate_post_data(*stored);
    sn_profile::record(sn_profile::Stage::kStorageWriteValidate,
                       validate_start);
  } catch (...) {
    release_owned_post(ctx->rpc_, stored_post);
    throw;
  }
  const int64_t final_post_id = stored->post_id;
#else
  PostData post;
  std::memcpy(&post, &req->req_control.post, sizeof(post));
  validate_post_data(post);
  const int64_t final_post_id = post.post_id;

  SharedPostBuffer stored_post = alloc_shared_post(ctx->rpc_, sizeof(PostData));
  my_assert(stored_post.buffer.buf_ != nullptr);
  std::memcpy(stored_post.buffer.buf_, &post, sizeof(PostData));
#endif
  publish_shared_post(ctx->rpc_, stored_post);

  SharedPostBuffer old_buffer;
  const uint64_t map_start = sn_profile::start();
  ctx->map_mutex.lock();
  auto old_it = ctx->post_storage_map.find(final_post_id);
  if (old_it != ctx->post_storage_map.end()) {
    old_buffer = old_it->second;
  }
  ctx->post_storage_map[final_post_id] = stored_post;
  ctx->map_mutex.unlock();
  sn_profile::record(sn_profile::Stage::kStorageWriteMap, map_start);

  if (old_buffer.buffer.buf_ != nullptr) {
    const uint64_t release_start = sn_profile::start();
    release_owned_post(ctx->rpc_, old_buffer);
    sn_profile::record(sn_profile::Stage::kStorageWriteOldRelease,
                       release_start);
  }

  const uint64_t response_start = sn_profile::start();
  new (req_handle->pre_resp_msgbuf_.buf_)
      RPCMsgResp<CommonRPCResp>(RPC_TYPE::RPC_POST_STORAGE_WRITE_RESP,
                                req->req_common.req_number, 0, {0});
  ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_,
                               sizeof(RPCMsgResp<CommonRPCResp>));
  ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
  sn_profile::record(sn_profile::Stage::kStorageWriteResponse, response_start);
}

void client_thread_func(size_t thread_id, ClientContext *ctx,
                        erpc::Nexus *nexus) {
  ctx->client_id_ = thread_id;
  std::vector<size_t> port_vec = flags_get_cxl_ports(0);
  uint8_t phy_port = port_vec.at(thread_id % port_vec.size());
  uint8_t rpc_id = FLAGS_rpc_id + 20 + thread_id;

  AppRpc rpc(nexus, static_cast<void *>(ctx), rpc_id, basic_sm_handler_client,
             phy_port);
  rpc.retry_connect_on_invalid_rpc_id_ = true;
  ctx->rpc_ = &rpc;

  connect_sessions(ctx);

  while (!ctrl_c_pressed) {
    rpc.run_event_loop_once();
  }
}

void server_thread_func(size_t thread_id, ServerContext *ctx,
                        erpc::Nexus *nexus) {
  ctx->server_id_ = thread_id;
  std::vector<size_t> port_vec = flags_get_cxl_ports(0);

  AppRpc rpc(nexus, static_cast<void *>(ctx), FLAGS_rpc_id + thread_id,
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
    servers[i] =
        std::thread(server_thread_func, i, context->server_contexts_[i], nexus);
    erpc::bind_to_core(
        servers[i], FLAGS_numa_server_node,
        get_bind_core(FLAGS_numa_server_node) + FLAGS_bind_core_offset);
  }
  for (size_t i = 0; i < FLAGS_client_num; i++) {
    clients[i] =
        std::thread(client_thread_func, i, context->client_contexts_[i], nexus);
    erpc::bind_to_core(
        clients[i], FLAGS_numa_client_node,
        get_bind_core(FLAGS_numa_client_node) + FLAGS_bind_core_offset);
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
  // Register fallback handlers first to prevent null handler dispatch when
  // wrong request types arrive.
  nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_PING_RESP),
                          unsupported_req_handler);
  nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_UNIQUE_ID),
                          unsupported_req_handler);
  nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_URL_SHORTEN),
                          unsupported_req_handler);
  nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_USER_MENTION),
                          unsupported_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_COMPOSE_POST_WRITE_REQ),
      unsupported_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_COMPOSE_POST_WRITE_RESP),
      unsupported_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_WRITE_REQ),
      unsupported_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_WRITE_RESP),
      unsupported_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_READ_REQ),
      unsupported_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_USER_TIMELINE_READ_RESP),
      unsupported_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_READ_RESP),
      unsupported_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_WRITE_RESP),
      unsupported_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_WRITE_REQ),
      unsupported_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_WRITE_RESP),
      unsupported_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ),
      unsupported_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_RESP),
      unsupported_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_COMPOSE_CREATOR_WITH_USER_ID),
      unsupported_req_handler);
  nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_RMEM_PARAM),
                          unsupported_req_handler);

  nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_PING),
                          ping_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_READ_REQ),
      post_storage_read_req_handler);
  nexus.register_req_func(
      static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_WRITE_REQ),
      post_storage_write_req_handler);

  AppContext context;
  std::thread mongodb_init_thread(mongodb_init, &context);
  erpc::bind_to_core(mongodb_init_thread, FLAGS_numa_server_node,
                     get_bind_core(FLAGS_numa_server_node));

  std::thread leader_thread(leader_thread_func, &nexus, &context);
  erpc::bind_to_core(leader_thread, FLAGS_numa_server_node,
                     get_bind_core(FLAGS_numa_server_node));
  leader_thread.join();
  mongodb_init_thread.join();
}
