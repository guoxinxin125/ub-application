#include "common.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>

struct ServerContext {
  erpc::Rpc<erpc::CTransport> *rpc = nullptr;
  std::atomic<uint64_t> request_count{0};
  std::atomic<uint64_t> request_checksum{0};
};

volatile sig_atomic_t ctrl_c_pressed = 0;

void ctrl_c_handler(int) { ctrl_c_pressed = 1; }

void req_handler(erpc::ReqHandle *req_handle, void *context) {
  auto *server_context = reinterpret_cast<ServerContext *>(context);
  server_context->request_count.fetch_add(1, std::memory_order_relaxed);

  const erpc::MsgBuffer *req = req_handle->get_req_msgbuf();
  int req_id = 0;
  size_t resp_size = kMsgSize;
  if (req != nullptr && req->buf_ != nullptr) {
    resp_size = req->get_data_size();
    if (resp_size >= sizeof(req_id)) {
      req_id = read_req_id(*req);
      server_context->request_checksum.fetch_add(
          static_cast<uint64_t>(req_id), std::memory_order_relaxed);
    }
  }

  erpc::MsgBuffer *resp = &req_handle->pre_resp_msgbuf_;
  if (resp_size > erpc::CTransport::kMaxDataPerPkt) {
    req_handle->dyn_resp_msgbuf_ =
        server_context->rpc->alloc_msg_buffer_or_die(resp_size);
    resp = &req_handle->dyn_resp_msgbuf_;
  }

  server_context->rpc->resize_msg_buffer(resp, resp_size);
  write_req_id(*resp, req_id);
  server_context->rpc->enqueue_response(req_handle, resp);
}

int main(int argc, char **argv) {
  signal(SIGINT, ctrl_c_handler);

  int num_clients = argc > 1 ? std::atoi(argv[1]) : 1;
  if (num_clients <= 0) num_clients = 1;

  std::string server_uri =
      kServerHostname + ":" + std::to_string(kServerUDPPort);
  printf("RDMA MC server: rpc_id=0, expecting up to %d clients on %s\n",
         num_clients, server_uri.c_str());

  erpc::Nexus nexus(server_uri);
  nexus.register_req_func(kReqType, req_handler);

  ServerContext context;
  context.rpc =
      new erpc::Rpc<erpc::CTransport>(&nexus, &context, 0, nullptr, 0);

  while (ctrl_c_pressed != 1) {
    context.rpc->run_event_loop(100);
  }

  delete context.rpc;
  context.rpc = nullptr;

  printf("RDMA MC server: requests %llu, checksum %llu\n",
         static_cast<unsigned long long>(
             context.request_count.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(
             context.request_checksum.load(std::memory_order_relaxed)));
  return 0;
}
