#include "common.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

erpc::Rpc<erpc::CTransport> *rpc;
using Clock = std::chrono::steady_clock;
uint64_t response_checksum = 0;

struct RequestState {
  erpc::MsgBuffer req;
  erpc::MsgBuffer resp;
  Clock::time_point start;
  int req_id = 0;
  bool in_use = false;
  bool done = false;
};

void cont_func(void *, void *tag) {
  auto *state = reinterpret_cast<RequestState *>(tag);
  state->done = true;
}

void sm_handler(int session_num, erpc::SmEventType sm_event_type,
                erpc::SmErrType sm_err_type, void *) {
  (void)session_num;
  (void)sm_event_type;
  (void)sm_err_type;
}

int main(int argc, char **argv) {
  int concurrency = argc > 1 ? std::atoi(argv[1]) : 1;
  size_t msg_size =
      argc > 2 ? static_cast<size_t>(std::strtoull(argv[2], nullptr, 0))
               : kDefaultMsgSize;
  const int num_requests = 100000;
  const int warmup_requests = 100;
  if (concurrency <= 0) concurrency = 1;
  if (msg_size < sizeof(int)) msg_size = sizeof(int);

  std::string client_uri =
      kClientHostname + ":" + std::to_string(kClientUDPPort);
  printf("Client: Starting on %s\n", client_uri.c_str());
  printf("Client: concurrency=%d, msg_size=%zu, measured_requests=%d, "
         "warmup_requests=%d\n",
         concurrency, msg_size, num_requests, warmup_requests);

  erpc::Nexus nexus(client_uri);

  rpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, 1, sm_handler, 1);
  std::string server_uri =
      kServerHostname + ":" + std::to_string(kServerUDPPort);

  int session_num = rpc->create_session(server_uri, 0);
  if (session_num < 0) {
    printf("Client: Failed to create session, error: %d\n", session_num);
    delete rpc;
    return -1;
  }
  int connection_attempts = 0;
  const int max_attempts = 2000;

  while (!rpc->is_connected(session_num) && connection_attempts < max_attempts) {
    rpc->run_event_loop_once();
    connection_attempts++;

    if (connection_attempts % 10 == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  if (!rpc->is_connected(session_num)) {
    printf("Client: Failed to connect after %d attempts\n", max_attempts);
    delete rpc;
    return -1;
  }

  printf("Client: Connected successfully!\n");

  std::vector<RequestState> slots(static_cast<size_t>(concurrency));

  auto run_requests = [&](int total_requests, bool measure_latency) {
    int sent = 0;
    int completed = 0;
    int inflight = 0;
    double total_lat = 0.0;

    while (completed < total_requests) {
      while (sent < total_requests && inflight < concurrency) {
        RequestState *slot = nullptr;
        for (auto &candidate : slots) {
          if (!candidate.in_use) {
            slot = &candidate;
            break;
          }
        }
        if (slot == nullptr) break;

        slot->start = Clock::now();
        slot->req = rpc->alloc_msg_buffer_or_die(msg_size);
        slot->resp = erpc::MsgBuffer();
        slot->req_id = sent;
        slot->done = false;
        slot->in_use = true;

        *reinterpret_cast<int *>(slot->req.buf_) = sent;
        hello_world_flush_after_write(slot->req);
        rpc->enqueue_request(session_num, kReqType, &slot->req, &slot->resp,
                             cont_func, slot);

        sent++;
        inflight++;
      }

      rpc->run_event_loop_once();

      for (auto &slot : slots) {
        if (!slot.in_use || !slot.done) continue;

        int resp_id = hello_world_read_response_int(slot.resp);
        response_checksum += static_cast<uint64_t>(resp_id);

        rpc->free_msg_buffer(slot.req);
        rpc->free_msg_buffer(slot.resp);

        if (measure_latency) {
          auto end = Clock::now();
          total_lat += std::chrono::duration_cast<std::chrono::nanoseconds>(
                           end - slot.start)
                           .count() /
                       1000.0;
        }

        slot.in_use = false;
        slot.done = false;
        completed++;
        inflight--;
      }
    }

    return total_lat;
  };

  run_requests(warmup_requests, false);

  double total_lat = run_requests(num_requests, true);

  printf("Client: Average latency for %d requests at concurrency %d: %.3f us\n",
         num_requests, concurrency, total_lat / num_requests);
  printf("Client: Response checksum: %lu\n", response_checksum);

  delete rpc;
  return 0;
}
