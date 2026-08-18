#include "common.h"

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <thread>

#include "util/rdma_breakdown.h"

erpc::Rpc<erpc::CTransport> *rpc;
bool response_received = false;
uint64_t response_checksum = 0;
using Clock = std::chrono::steady_clock;

struct ClientBreakdown {
  uint64_t count = 0;
  uint64_t alloc_ns = 0;
  uint64_t prepare_ns = 0;
  uint64_t enqueue_ns = 0;
  uint64_t wait_resp_ns = 0;
  uint64_t read_resp_ns = 0;
  uint64_t free_ns = 0;
  uint64_t total_ns = 0;
  uint64_t event_loop_iters = 0;

  void print() const {
    if (count == 0) return;
    const double n = static_cast<double>(count);
    printf("RDMA Client breakdown (avg us/request):\n");
    printf("  alloc req+resp:        %.3f\n", alloc_ns / n / 1000.0);
    printf("  prepare request:       %.3f\n", prepare_ns / n / 1000.0);
    printf("  enqueue_request send:  %.3f\n", enqueue_ns / n / 1000.0);
    printf("  wait response receive: %.3f\n", wait_resp_ns / n / 1000.0);
    printf("  read response:         %.3f\n", read_resp_ns / n / 1000.0);
    printf("  free req+resp:         %.3f\n", free_ns / n / 1000.0);
    printf("  total measured:        %.3f\n", total_ns / n / 1000.0);
    printf("  event_loop_once calls: %.3f / request\n",
           event_loop_iters / n);
  }
};

static inline uint64_t ns_between(Clock::time_point start,
                                  Clock::time_point end) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
          .count());
}

void cont_func(void *, void *) { response_received = true; }

void sm_handler(int session_num, erpc::SmEventType sm_event_type,
                erpc::SmErrType sm_err_type, void *) {
  printf("RDMA Client: SM event - session %d, event %d, error %d\n",
         session_num, static_cast<int>(sm_event_type),
         static_cast<int>(sm_err_type));
}

int main() {
  std::string client_uri = kClientHostname + ":" + std::to_string(kClientUDPPort);
  printf("RDMA Client: Starting on %s\n", client_uri.c_str());

  erpc::Nexus nexus(client_uri);
  rpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, 1, sm_handler, 0);
  printf("RDMA Client: RPC created with RPC ID 1\n");

  std::string server_uri = kServerHostname + ":" + std::to_string(kServerUDPPort);
  int session_num = rpc->create_session(server_uri, 0);
  if (session_num < 0) {
    printf("RDMA Client: Failed to create session, error %d\n", session_num);
    delete rpc;
    return -1;
  }

  const int max_attempts = 2000;
  int connection_attempts = 0;
  while (!rpc->is_connected(session_num) && connection_attempts < max_attempts) {
    rpc->run_event_loop_once();
    connection_attempts++;
    if (connection_attempts % 10 == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  if (!rpc->is_connected(session_num)) {
    printf("RDMA Client: Failed to connect after %d attempts\n", max_attempts);
    delete rpc;
    return -1;
  }
  printf("RDMA Client: Connected, session_num = %d\n", session_num);

  const int warmup_requests = kWarmupRequests;
  printf("RDMA Client: Sending %d warm-up requests\n", warmup_requests);
  for (int i = 0; i < warmup_requests; i++) {
    erpc::MsgBuffer req = rpc->alloc_msg_buffer_or_die(kMsgSize);
    erpc::MsgBuffer resp = rpc->alloc_msg_buffer_or_die(kMsgSize);
    write_req_id(req, i);

    response_received = false;
    rpc->enqueue_request(session_num, kReqType, &req, &resp, cont_func, nullptr);
    while (!response_received) {
      rpc->run_event_loop_once();
    }

    response_checksum += static_cast<uint64_t>(read_req_id(resp));
    rpc->free_msg_buffer(req);
    rpc->free_msg_buffer(resp);
  }
#ifdef ERPC_RDMA_BREAKDOWN
  erpc::rdma_breakdown::reset();
#endif

  const int num_requests = kMeasuredRequests;
  double total_lat = 0;
  ClientBreakdown breakdown;
  printf("RDMA Client: Sending %d measured requests\n", num_requests);

  for (int i = 0; i < num_requests; i++) {
    auto start = Clock::now();

    erpc::MsgBuffer req = rpc->alloc_msg_buffer_or_die(kMsgSize);
    erpc::MsgBuffer resp = rpc->alloc_msg_buffer_or_die(kMsgSize);
    auto after_alloc = Clock::now();

    write_req_id(req, i);
    auto after_prepare = Clock::now();

    response_received = false;
    rpc->enqueue_request(session_num, kReqType, &req, &resp, cont_func, nullptr);
    auto after_enqueue = Clock::now();

    uint64_t event_loop_iters = 0;
    while (!response_received) {
      rpc->run_event_loop_once();
      event_loop_iters++;
    }
    auto after_wait = Clock::now();

    response_checksum += static_cast<uint64_t>(read_req_id(resp));
    auto after_read = Clock::now();

    rpc->free_msg_buffer(req);
    rpc->free_msg_buffer(resp);
    auto end = Clock::now();

    breakdown.count++;
    breakdown.alloc_ns += ns_between(start, after_alloc);
    breakdown.prepare_ns += ns_between(after_alloc, after_prepare);
    breakdown.enqueue_ns += ns_between(after_prepare, after_enqueue);
    breakdown.wait_resp_ns += ns_between(after_enqueue, after_wait);
    breakdown.read_resp_ns += ns_between(after_wait, after_read);
    breakdown.free_ns += ns_between(after_read, end);
    breakdown.total_ns += ns_between(start, end);
    breakdown.event_loop_iters += event_loop_iters;

    total_lat +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count() /
        1000.0;
  }

  printf("RDMA Client: Average latency for %d requests: %.3f us\n",
         num_requests, total_lat / num_requests);
  printf("RDMA Client: Response checksum: %lu\n", response_checksum);
  breakdown.print();
#ifdef ERPC_RDMA_BREAKDOWN
  erpc::rdma_breakdown::print("RDMA Client");
#endif

  delete rpc;
  return 0;
}
