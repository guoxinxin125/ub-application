#include "common.h"

#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ServerContext {
  erpc::Rpc<erpc::CTransport> *rpc = nullptr;
  int server_idx = 0;
  uint8_t rpc_id = 0;
  uint64_t request_count = 0;
  uint64_t request_checksum = 0;
};

volatile sig_atomic_t ctrl_c_pressed = 0;
std::mutex rpc_init_mutex;

void ctrl_c_handler(int) { ctrl_c_pressed = 1; }

void req_handler(erpc::ReqHandle *req_handle, void *context) {
  auto *server_context = reinterpret_cast<ServerContext *>(context);
  server_context->request_count++;

  const erpc::MsgBuffer *req = req_handle->get_req_msgbuf();
  int req_id = 0;
  size_t resp_size = kMsgSize;
  if (req != nullptr && req->buf_ != nullptr) {
    resp_size = req->get_data_size();
    if (resp_size >= sizeof(req_id)) {
      req_id = read_req_id(*req);
      server_context->request_checksum += static_cast<uint64_t>(req_id);
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

  int num_pairs = argc > 1 ? std::atoi(argv[1]) : 1;
  const int max_pairs = 32;
  if (num_pairs <= 0) num_pairs = 1;
  if (num_pairs > max_pairs) {
    printf("RDMA pair server: Requested %d pairs, clamping to %d\n",
           num_pairs, max_pairs);
    num_pairs = max_pairs;
  }

  std::string server_uri =
      kServerHostname + ":" + std::to_string(kServerUDPPort);
  printf("RDMA pair server: Starting %d server RPCs on %s\n", num_pairs,
         server_uri.c_str());

  erpc::Nexus nexus(server_uri);
  nexus.register_req_func(kReqType, req_handler);

  std::vector<ServerContext> contexts(static_cast<size_t>(num_pairs));
  for (int i = 0; i < num_pairs; i++) {
    auto &context = contexts[static_cast<size_t>(i)];
    context.server_idx = i;
    context.rpc_id = static_cast<uint8_t>(i * 2);
  }

  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(num_pairs));
  for (auto &context : contexts) {
    threads.emplace_back([&nexus, &context]() {
      {
        std::lock_guard<std::mutex> lock(rpc_init_mutex);
        context.rpc = new erpc::Rpc<erpc::CTransport>(
            &nexus, &context, context.rpc_id, nullptr, 0);
      }
      printf("RDMA pair server %d: rpc_id=%u ready\n", context.server_idx,
             static_cast<unsigned>(context.rpc_id));
      while (ctrl_c_pressed != 1) {
        context.rpc->run_event_loop(100);
      }
      delete context.rpc;
      context.rpc = nullptr;
    });
  }

  for (auto &thread : threads) {
    thread.join();
  }

  uint64_t total_request_count = 0;
  uint64_t total_request_checksum = 0;
  for (auto &context : contexts) {
    printf("RDMA pair server %d: requests %llu, checksum %llu\n",
           context.server_idx,
           static_cast<unsigned long long>(context.request_count),
           static_cast<unsigned long long>(context.request_checksum));
    total_request_count += context.request_count;
    total_request_checksum += context.request_checksum;
  }

  printf("RDMA pair server: aggregate requests %llu, checksum %llu\n",
         static_cast<unsigned long long>(total_request_count),
         static_cast<unsigned long long>(total_request_checksum));
  return 0;
}
