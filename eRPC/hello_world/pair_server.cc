#include "common.h"

#include <signal.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef ERPC_CXL
#include "transport_impl/cxl/cxl_config.h"
#endif

struct ServerContext {
  erpc::Rpc<erpc::CTransport> *rpc = nullptr;
  int server_idx = 0;
  uint8_t rpc_id = 0;
  int request_count = 0;
  uint64_t request_checksum = 0;
};

volatile sig_atomic_t ctrl_c_pressed = 0;
std::mutex rpc_init_mutex;

void ctrl_c_handler(int) { ctrl_c_pressed = 1; }

void configure_cxl_env(int num_pairs) {
#ifdef ERPC_CXL
  const std::string worker_count = std::to_string(num_pairs * 2);
  const std::string reserved_bytes =
      std::to_string(erpc::cxl_config::kReservedQueueBytes);
  const std::string dax_align =
      std::to_string(erpc::cxl_config::kDaxAlignBytes);
  setenv("ERPC_CXL_WORKER_COUNT", worker_count.c_str(), 1);
  setenv("ERPC_CXL_RESERVED_BYTES", reserved_bytes.c_str(), 1);
  setenv("ERPC_CXL_DAX_ALIGN_BYTES", dax_align.c_str(), 1);
#else
  (void)num_pairs;
#endif
}

void req_handler(erpc::ReqHandle *req_handle, void *context) {
  auto *server_context = reinterpret_cast<ServerContext *>(context);
  server_context->request_count++;

  const erpc::MsgBuffer *req = req_handle->get_req_msgbuf();
  int req_id = 0;
  if (req != nullptr && req->buf_ != nullptr &&
      req->get_data_size() >= sizeof(req_id)) {
    req_id = hello_world_read_request_int(*req);
    server_context->request_checksum += static_cast<uint64_t>(req_id);
  }

  const size_t resp_size =
      req != nullptr ? req->get_data_size() : kDefaultMsgSize;
  auto &resp = req_handle->dyn_resp_msgbuf_;
  resp = server_context->rpc->alloc_msg_buffer_or_die(resp_size);
  *reinterpret_cast<int *>(resp.buf_) = req_id;
  hello_world_flush_after_write(resp);
  server_context->rpc->enqueue_response(req_handle, &resp);
}

int main(int argc, char **argv) {
  signal(SIGINT, ctrl_c_handler);

  int num_pairs = argc > 1 ? std::atoi(argv[1]) : 1;
  const int max_pairs = 32;  // CXL queues cover rpc_id 0..63.
  if (num_pairs <= 0) num_pairs = 1;
  if (num_pairs > max_pairs) {
    printf("Pair server: Requested %d pairs, clamping to %d\n", num_pairs,
           max_pairs);
    num_pairs = max_pairs;
  }
  configure_cxl_env(num_pairs);

  std::string server_uri =
      kServerHostname + ":" + std::to_string(kServerUDPPort);
  printf("Pair server: Starting %d server RPCs on %s\n", num_pairs,
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
      printf("Pair server %d: rpc_id=%u ready\n", context.server_idx,
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

  int total_request_count = 0;
  uint64_t total_request_checksum = 0;
  for (auto &context : contexts) {
    printf("Pair server %d: requests %d, checksum %lu\n", context.server_idx,
           context.request_count, context.request_checksum);
    total_request_count += context.request_count;
    total_request_checksum += context.request_checksum;
  }

  printf("Pair server: aggregate requests %d, checksum %lu\n",
         total_request_count, total_request_checksum);
  return 0;
}
