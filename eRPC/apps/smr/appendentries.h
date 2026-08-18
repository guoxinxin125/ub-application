/**
 * @file appendentries.h
 * @brief Handlers for appendentries RPC
 */

#pragma once
#include "smr.h"

// With eRPC, there is currently no way for an RPC server to access connection
// data for a request, so the client's Raft node ID is included in the request.
struct app_appendentries_t {
  int node_id;  // Node ID of the sender
  msg_appendentries_t msg_ae;
  // If ae.n_entries > 0, the msg_entry_t structs are serialized here. Each
  // msg_entry_t struct's buf is placed immediately after the struct.

  // Serialize the ingredients of an app_appendentries_t into a network buffer
  static void serialize(erpc::MsgBuffer &req_msgbuf, int node_id,
                        msg_appendentries_t *msg_ae) {
    uint8_t *buf = req_msgbuf.buf_;
    auto *srlz = reinterpret_cast<app_appendentries_t *>(req_msgbuf.buf_);

    // Copy the whole-message header
    srlz->node_id = node_id;
    srlz->msg_ae = *msg_ae;
    srlz->msg_ae.entries = nullptr;  // Was local pointer
    buf += sizeof(app_appendentries_t);

    // Serialize each entry in the message
    for (size_t i = 0; i < static_cast<size_t>(msg_ae->n_entries); i++) {
      // Copy the entry header
      *reinterpret_cast<msg_entry_t *>(buf) = msg_ae->entries[i];
      reinterpret_cast<msg_entry_t *>(buf)->data.buf = nullptr;  // Local ptr
      buf += sizeof(msg_entry_t);

      // Copy the entry data
      assert(msg_ae->entries[i].data.len == sizeof(client_req_t));
      memcpy(buf, msg_ae->entries[i].data.buf, sizeof(client_req_t));
      buf += sizeof(client_req_t);
    }

    assert(buf == req_msgbuf.buf_ + req_msgbuf.get_data_size());
  }

  static constexpr size_t kStaticMsgEntryArrSize = 16;

  // Unpack an appendentries request message received at the server.
  //  * The buffers for entries the unpacked message come from the mempool.
  //  * The entries array for the unpacked message is dynamically allocated
  //    if there are too many entries. Caller must free if so.
  static void unpack(const erpc::MsgBuffer *req_msgbuf,
                     msg_entry_t *static_msg_entry_arr,
                     AppMemPool<client_req_t> &log_entry_appdata_pool) {
    uint8_t *buf = req_msgbuf->buf_;
    auto *ae_req = reinterpret_cast<app_appendentries_t *>(buf);
    msg_appendentries_t &msg_ae = ae_req->msg_ae;
    assert(msg_ae.entries == nullptr);

    size_t n_entries = static_cast<size_t>(msg_ae.n_entries);
    bool is_keepalive = (n_entries == 0);

    if (!is_keepalive) {
      // Non-keepalive appendentries requests contain app-defined log entries
      buf += sizeof(app_appendentries_t);
      msg_ae.entries = n_entries <= kStaticMsgEntryArrSize
                           ? static_msg_entry_arr
                           : new msg_entry_t[n_entries];

      // Invariant: buf points to a msg_entry_t, followed by its buffer
      for (size_t i = 0; i < n_entries; i++) {
        msg_ae.entries[i] = *(reinterpret_cast<msg_entry_t *>(buf));
        buf += sizeof(msg_entry_t);

        assert(msg_ae.entries[i].data.buf == nullptr);
        msg_ae.entries[i].data.buf = log_entry_appdata_pool.alloc();

        // Copy out each SMR command buffer from the request msgbuf since the
        // msgbuf is valid for this function only.
        assert(msg_ae.entries[i].data.len == sizeof(client_req_t));
        memcpy(msg_ae.entries[i].data.buf, buf, sizeof(client_req_t));
        buf += sizeof(client_req_t);
      }

      assert(buf == req_msgbuf->buf_ + req_msgbuf->get_data_size());
    }
  }
};

#ifdef ERPC_CXL

// CXL 零拷贝消息格式：只传递偏移量，不复制数据
struct cxl_entry_header_t {
    msg_entry_t entry;           // entry 元数据
    uint64_t data_offset;        // 数据在共享内存中的偏移量（0 表示无数据）
};

struct app_appendentries_cxl_t {
  int node_id;
  msg_appendentries_t msg_ae;
  
  // 计算零拷贝模式下的请求大小（只包含 header，不包含数据）
  static size_t get_serialized_size(msg_appendentries_t *msg_ae) {
    size_t size = sizeof(app_appendentries_cxl_t);
    size += static_cast<size_t>(msg_ae->n_entries) * sizeof(cxl_entry_header_t);
    return size;
  }
  
  // 零拷贝序列化：只传递偏移量，不复制数据
  static void serialize_zerocopy(erpc::MsgBuffer &req_msgbuf, int node_id,
                                  msg_appendentries_t *msg_ae,
                                  erpc::Rpc<SMR_TRANSPORT>* rpc) {
    uint8_t *buf = req_msgbuf.buf_;
    auto *srlz = reinterpret_cast<app_appendentries_cxl_t *>(buf);

    srlz->node_id = node_id;
    srlz->msg_ae = *msg_ae;
    srlz->msg_ae.entries = nullptr;
    buf += sizeof(app_appendentries_cxl_t);

    for (size_t i = 0; i < static_cast<size_t>(msg_ae->n_entries); i++) {
      auto* header = reinterpret_cast<cxl_entry_header_t*>(buf);
      header->entry = msg_ae->entries[i];
      header->entry.data.buf = nullptr;  // 清除本地指针
      
      //  只传递偏移量，不复制数据
      if (msg_ae->entries[i].data.buf != nullptr && 
          smr_cxl::is_shared_ptr(rpc, msg_ae->entries[i].data.buf)) {
        header->data_offset = smr_cxl::ptr_to_offset(rpc, msg_ae->entries[i].data.buf);
      } else {
        header->data_offset = 0;
      }
      
      buf += sizeof(cxl_entry_header_t);
    }
  }

  // 零拷贝解包：直接使用共享内存指针
  static void unpack_zerocopy(const erpc::MsgBuffer *req_msgbuf,
                               msg_entry_t *static_msg_entry_arr,
                               erpc::Rpc<SMR_TRANSPORT>* rpc) {
    uint8_t *buf = req_msgbuf->buf_;
    auto *ae_req = reinterpret_cast<app_appendentries_cxl_t *>(buf);
    msg_appendentries_t &msg_ae = ae_req->msg_ae;

    size_t n_entries = static_cast<size_t>(msg_ae.n_entries);
    if (n_entries == 0) return;

    buf += sizeof(app_appendentries_cxl_t);
    msg_ae.entries = n_entries <= app_appendentries_t::kStaticMsgEntryArrSize
                         ? static_msg_entry_arr
                         : new msg_entry_t[n_entries];

    for (size_t i = 0; i < n_entries; i++) {
      auto* header = reinterpret_cast<cxl_entry_header_t*>(buf);
      msg_ae.entries[i] = header->entry;
      
      //  根据偏移量直接获取共享内存指针
      if (header->data_offset != 0) {
        msg_ae.entries[i].data.buf = smr_cxl::offset_to_ptr(rpc, header->data_offset);
      } else {
        msg_ae.entries[i].data.buf = nullptr;
      }
      
      buf += sizeof(cxl_entry_header_t);
    }
  }
};

#endif  // ERPC_CXL

// appendentries request format is like so:
// node ID, msg_appendentries_t, [{size, buf}]
void appendentries_handler(erpc::ReqHandle *req_handle, void *_context) {
  auto *c = static_cast<AppContext *>(_context);
  const erpc::MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();

  if (kAppTimeEnt) c->server.time_ents.emplace_back(TimeEntType::kRecvAeReq);

  msg_entry_t static_msg_entry_arr[app_appendentries_t::kStaticMsgEntryArrSize];

#ifdef ERPC_CXL
  // CXL 零拷贝路径：直接使用共享内存指针
  app_appendentries_cxl_t::unpack_zerocopy(req_msgbuf, static_msg_entry_arr, c->rpc);
#else
  app_appendentries_t::unpack(req_msgbuf, static_msg_entry_arr,
                              c->server.log_entry_appdata_pool);
#endif

  auto *ae_req = reinterpret_cast<app_appendentries_t *>(req_msgbuf->buf_);
  msg_appendentries_t &msg_ae = ae_req->msg_ae;

  if (kAppVerbose) {
    printf("smr: Received appendentries (%s) req from node %s [%s].\n",
           msg_ae.n_entries == 0 ? "keepalive" : "non-keepalive",
           node_id_to_name_map[ae_req->node_id].c_str(),
           erpc::get_formatted_time().c_str());
  }

  erpc::MsgBuffer &resp_msgbuf = req_handle->pre_resp_msgbuf_;
  c->rpc->resize_msg_buffer(&resp_msgbuf, sizeof(msg_appendentries_response_t));

  int e = raft_recv_appendentries(
      c->server.raft, raft_get_node(c->server.raft, ae_req->node_id), &msg_ae,
      reinterpret_cast<msg_appendentries_response_t *>(resp_msgbuf.buf_));
  erpc::rt_assert(e == 0, "raft_recv_appendentries failed");

#ifdef ERPC_CXL
  // CXL 零拷贝：只释放 entries 数组，不释放 entry 数据（它们指向共享内存）
  if (msg_ae.entries != static_msg_entry_arr && msg_ae.entries != nullptr) {
    delete[] msg_ae.entries;
  }
#else
  if (msg_ae.entries != static_msg_entry_arr) delete[] msg_ae.entries;
#endif

  if (kAppTimeEnt) c->server.time_ents.emplace_back(TimeEntType::kSendAeResp);

  c->rpc->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void appendentries_cont(void *, void *);  // Fwd decl

// Raft callback for sending appendentries message
static int smr_raft_send_appendentries_cb(raft_server_t *raft, void *udata,
                                          raft_node_t *node,
                                          msg_appendentries_t *msg_ae) {
  auto *conn = static_cast<connection_t *>(raft_node_get_udata(node));
  AppContext *c = conn->c;

  bool is_keepalive = (msg_ae->n_entries == 0);
  if (kAppVerbose) {
    printf("smr: Sending appendentries (%s) to node %s [%s].\n",
           is_keepalive ? "keepalive" : "non-keepalive",
           node_id_to_name_map[raft_node_get_id(node)].c_str(),
           erpc::get_formatted_time().c_str());
  }

  if (!c->rpc->is_connected(conn->session_num)) {
    if (kAppVerbose) {
      printf("smr: Cannot send ae req on session %d.\n", conn->session_num);
    }
    return 0;
  }

#ifdef ERPC_CXL
  //  CXL 零拷贝：请求大小只包含 header，不包含数据
  size_t req_size = app_appendentries_cxl_t::get_serialized_size(msg_ae);
#else
  // 非 CXL 模式：请求大小包含数据
  size_t req_size = sizeof(app_appendentries_t);
  for (size_t i = 0; i < static_cast<size_t>(msg_ae->n_entries); i++) {
    req_size += sizeof(msg_entry_t) + msg_ae->entries[i].data.len;
  }
#endif

  erpc::rt_assert(req_size <= c->rpc->get_max_msg_size(),
                  "send_appendentries_cb: Message size too large");

  raft_req_tag_t *rrt = c->server.raft_req_tag_pool.alloc();
  rrt->req_msgbuf = c->rpc->alloc_msg_buffer_or_die(req_size);
  rrt->resp_msgbuf =
      c->rpc->alloc_msg_buffer_or_die(sizeof(msg_appendentries_response_t));
  rrt->node = node;

#ifdef ERPC_CXL
  //  CXL 零拷贝序列化：只传递偏移量，增加引用计数
  app_appendentries_cxl_t::serialize_zerocopy(rrt->req_msgbuf, 
                                               c->server.node_id, msg_ae, c->rpc);
#else
  app_appendentries_t::serialize(rrt->req_msgbuf, c->server.node_id, msg_ae);
#endif

  if (kAppTimeEnt) c->server.time_ents.emplace_back(TimeEntType::kSendAeReq);
  c->rpc->enqueue_request(conn->session_num,
                          static_cast<uint8_t>(ReqType::kAppendEntries),
                          &rrt->req_msgbuf, &rrt->resp_msgbuf,
                          appendentries_cont, reinterpret_cast<void *>(rrt));
  return 0;
}

void appendentries_cont(void *_context, void *_tag) {
  auto *c = static_cast<AppContext *>(_context);
  if (kAppTimeEnt) c->server.time_ents.emplace_back(TimeEntType::kRecvAeResp);
  auto *rrt = reinterpret_cast<raft_req_tag_t *>(_tag);

  if (likely(rrt->resp_msgbuf.get_data_size() > 0)) {
    if (kAppVerbose) {
      printf("smr: Received appendentries response from node %s [%s].\n",
             node_id_to_name_map[raft_node_get_id(rrt->node)].c_str(),
             erpc::get_formatted_time().c_str());
    }

    int e = raft_recv_appendentries_response(
        c->server.raft, rrt->node,
        reinterpret_cast<msg_appendentries_response_t *>(
            rrt->resp_msgbuf.buf_));
    erpc::rt_assert(e == 0 || e == RAFT_ERR_NOT_LEADER,
                    "raft_recv_appendentries_response error");
  } else {
    printf("smr: Appendentries RPC to node %s failed to complete [%s].\n",
           node_id_to_name_map[raft_node_get_id(rrt->node)].c_str(),
           erpc::get_formatted_time().c_str());
  }

  c->rpc->free_msg_buffer(rrt->req_msgbuf);
  c->rpc->free_msg_buffer(rrt->resp_msgbuf);
  c->server.raft_req_tag_pool.free(rrt);
}