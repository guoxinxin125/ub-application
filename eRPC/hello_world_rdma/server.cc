#include "common.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdint>

#include "util/rdma_breakdown.h"

erpc::Rpc<erpc::CTransport> *rpc;
volatile sig_atomic_t ctrl_c_pressed = 0;
int request_count = 0;
uint64_t request_checksum = 0;
using Clock = std::chrono::steady_clock;

struct ServerBreakdown {
  uint64_t count = 0;
  uint64_t read_req_ns = 0;
  uint64_t prepare_resp_buf_ns = 0;
  uint64_t write_resp_ns = 0;
  uint64_t enqueue_resp_ns = 0;
  uint64_t total_handler_ns = 0;
  uint64_t dyn_resp_allocs = 0;

  void print() const {
    if (count == 0) return;
    const double n = static_cast<double>(count);
    printf("RDMA Server breakdown (avg us/request):\n");
    printf("  read request:          %.3f\n", read_req_ns / n / 1000.0);
    printf("  prepare resp buffer:   %.3f\n",
           prepare_resp_buf_ns / n / 1000.0);
    printf("  write response:        %.3f\n", write_resp_ns / n / 1000.0);
    printf("  enqueue_response send: %.3f\n", enqueue_resp_ns / n / 1000.0);
    printf("  total handler:         %.3f\n",
           total_handler_ns / n / 1000.0);
    printf("  dynamic resp allocs:   %lu\n", dyn_resp_allocs);
  }
};

ServerBreakdown server_breakdown;

static inline uint64_t ns_between(Clock::time_point start,
                                  Clock::time_point end) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
          .count());
}

void ctrl_c_handler(int) { ctrl_c_pressed = 1; }

void req_handler(erpc::ReqHandle *req_handle, void *) {
  if (request_count == kWarmupRequests) {
    server_breakdown = ServerBreakdown();
#ifdef ERPC_RDMA_BREAKDOWN
    erpc::rdma_breakdown::reset();
#endif
  }

  auto start = Clock::now();
  request_count++;

  const erpc::MsgBuffer *req = req_handle->get_req_msgbuf();
  int req_id = 0;
  if (req != nullptr && req->buf_ != nullptr &&
      req->get_data_size() >= sizeof(req_id)) {
    req_id = read_req_id(*req);
    request_checksum += static_cast<uint64_t>(req_id);
  }
  auto after_read = Clock::now();

  erpc::MsgBuffer *resp = &req_handle->pre_resp_msgbuf_;
  if (kMsgSize > erpc::CTransport::kMaxDataPerPkt) {
    req_handle->dyn_resp_msgbuf_ = rpc->alloc_msg_buffer_or_die(kMsgSize);
    resp = &req_handle->dyn_resp_msgbuf_;
    server_breakdown.dyn_resp_allocs++;
  }

  rpc->resize_msg_buffer(resp, kMsgSize);
  auto after_prepare_resp = Clock::now();

  write_req_id(*resp, req_id);
  auto after_write = Clock::now();

  rpc->enqueue_response(req_handle, resp);
  auto after_enqueue = Clock::now();

  server_breakdown.count++;
  server_breakdown.read_req_ns += ns_between(start, after_read);
  server_breakdown.prepare_resp_buf_ns +=
      ns_between(after_read, after_prepare_resp);
  server_breakdown.write_resp_ns += ns_between(after_prepare_resp, after_write);
  server_breakdown.enqueue_resp_ns += ns_between(after_write, after_enqueue);
  server_breakdown.total_handler_ns += ns_between(start, after_enqueue);
}

int main() {
  signal(SIGINT, ctrl_c_handler);

  std::string server_uri = kServerHostname + ":" + std::to_string(kServerUDPPort);
  printf("RDMA Server: Starting on %s\n", server_uri.c_str());

  erpc::Nexus nexus(server_uri);
  nexus.register_req_func(kReqType, req_handler);

  rpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, 0, nullptr, 0);
  printf("RDMA Server: RPC created with RPC ID 0\n");

  int loop_count = 0;
  auto start_time = std::chrono::steady_clock::now();

  while (ctrl_c_pressed != 1) {
    rpc->run_event_loop(100);
    loop_count++;

    if (loop_count % 50 == 0) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed =
          std::chrono::duration_cast<std::chrono::seconds>(now - start_time)
              .count();
      printf("RDMA Server: running for %ld seconds, processed %d requests\n",
             elapsed, request_count);
    }
  }

  printf("RDMA Server: Shutting down after %d requests, checksum %lu\n",
         request_count, request_checksum);
  server_breakdown.print();
#ifdef ERPC_RDMA_BREAKDOWN
  erpc::rdma_breakdown::print("RDMA Server");
#endif
  delete rpc;
  return 0;
}
