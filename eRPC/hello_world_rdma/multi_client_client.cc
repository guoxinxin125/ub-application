#include "common.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
std::mutex rpc_init_mutex;

struct RequestState {
  erpc::MsgBuffer req;
  erpc::MsgBuffer resp;
  Clock::time_point start;
  int req_id = 0;
  bool in_use = false;
  bool done = false;
};

struct ClientResult {
  double total_lat_us = 0.0;
  uint64_t response_checksum = 0;
  bool success = false;
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

ClientResult run_client(int client_idx, int concurrency, size_t msg_size,
                        int num_requests, int warmup_requests,
                        std::atomic<int> *ready_count,
                        std::atomic<bool> *start_measure) {
  ClientResult result;
  const uint8_t server_rpc_id = 0;
  const uint8_t client_rpc_id = static_cast<uint8_t>(client_idx + 1);
  const uint16_t client_port =
      static_cast<uint16_t>(kClientUDPPort + client_idx);
  std::string client_uri =
      kClientHostname + ":" + std::to_string(client_port);
  std::string server_uri =
      kServerHostname + ":" + std::to_string(kServerUDPPort);

  printf("RDMA MC client %d: uri=%s, rpc_id=%u, server_rpc_id=%u\n",
         client_idx, client_uri.c_str(), static_cast<unsigned>(client_rpc_id),
         static_cast<unsigned>(server_rpc_id));

  erpc::Nexus nexus(client_uri);
  erpc::Rpc<erpc::CTransport> *rpc = nullptr;
  {
    std::lock_guard<std::mutex> lock(rpc_init_mutex);
    rpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, client_rpc_id,
                                          sm_handler, 0);
  }

  int session_num = rpc->create_session(server_uri, server_rpc_id);
  if (session_num < 0) {
    printf("RDMA MC client %d: Failed to create session, error: %d\n",
           client_idx, session_num);
    ready_count->fetch_add(1, std::memory_order_release);
    delete rpc;
    return result;
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
    printf("RDMA MC client %d: Failed to connect after %d attempts\n",
           client_idx, max_attempts);
    ready_count->fetch_add(1, std::memory_order_release);
    delete rpc;
    return result;
  }

  std::vector<RequestState> slots(static_cast<size_t>(concurrency));

  auto run_requests = [&](int total_requests, bool measure_latency) {
    int sent = 0;
    int completed = 0;
    int inflight = 0;
    double total_lat = 0.0;
    const int req_id_base = client_idx * num_requests;

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
        slot->resp = rpc->alloc_msg_buffer_or_die(msg_size);
        slot->req_id = req_id_base + sent;
        slot->done = false;
        slot->in_use = true;

        write_req_id(slot->req, slot->req_id);
        rpc->enqueue_request(session_num, kReqType, &slot->req, &slot->resp,
                             cont_func, slot);

        sent++;
        inflight++;
      }

      rpc->run_event_loop_once();

      for (auto &slot : slots) {
        if (!slot.in_use || !slot.done) continue;

        int resp_id = read_req_id(slot.resp);
        result.response_checksum += static_cast<uint64_t>(resp_id);

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
  ready_count->fetch_add(1, std::memory_order_release);
  while (!start_measure->load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  result.total_lat_us = run_requests(num_requests, true);
  result.success = true;

  printf("RDMA MC client %d: avg latency %.3f us, checksum %llu\n",
         client_idx, result.total_lat_us / num_requests,
         static_cast<unsigned long long>(result.response_checksum));

  delete rpc;
  return result;
}

int main(int argc, char **argv) {
  int concurrency = argc > 1 ? std::atoi(argv[1]) : 1;
  size_t msg_size =
      argc > 2 ? static_cast<size_t>(std::strtoull(argv[2], nullptr, 0))
               : kMsgSize;
  int num_clients = argc > 3 ? std::atoi(argv[3]) : 1;
  const int num_requests = argc > 4 ? std::atoi(argv[4]) : kMeasuredRequests;
  const int warmup_requests = kWarmupRequests;
  const int max_clients = 63;  // server rpc_id 0, clients rpc_id 1..63.

  if (concurrency <= 0) concurrency = 1;
  if (concurrency > static_cast<int>(erpc::kSessionReqWindow)) {
    printf("RDMA MC client: Requested concurrency %d, clamping to "
           "per-session request window %zu\n",
           concurrency, erpc::kSessionReqWindow);
    concurrency = static_cast<int>(erpc::kSessionReqWindow);
  }
  if (msg_size < sizeof(int)) msg_size = sizeof(int);
  if (num_requests <= 0) {
    printf("RDMA MC client: requests/client must be positive\n");
    return -1;
  }
  if (num_clients <= 0) num_clients = 1;
  if (num_clients > max_clients) {
    printf("RDMA MC client: Requested %d clients, clamping to %d\n",
           num_clients, max_clients);
    num_clients = max_clients;
  }

  printf("RDMA MC client: concurrency=%d, msg_size=%zu, clients=%d, "
         "requests/client=%d, warmup/client=%d\n",
         concurrency, msg_size, num_clients, num_requests, warmup_requests);

  std::vector<ClientResult> results(static_cast<size_t>(num_clients));
  std::vector<std::thread> threads;
  std::atomic<int> ready_count(0);
  std::atomic<bool> start_measure(false);
  threads.reserve(static_cast<size_t>(num_clients));

  for (int i = 0; i < num_clients; i++) {
    threads.emplace_back([&, i]() {
      results[static_cast<size_t>(i)] =
          run_client(i, concurrency, msg_size, num_requests, warmup_requests,
                     &ready_count, &start_measure);
    });
  }

  while (ready_count.load(std::memory_order_acquire) < num_clients) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  const auto start = Clock::now();
  start_measure.store(true, std::memory_order_release);

  for (auto &thread : threads) {
    thread.join();
  }

  const auto end = Clock::now();
  const double elapsed_sec =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start)
          .count();

  int successful_clients = 0;
  double total_lat_us = 0.0;
  uint64_t response_checksum = 0;
  for (const auto &result : results) {
    if (!result.success) continue;
    successful_clients++;
    total_lat_us += result.total_lat_us;
    response_checksum += result.response_checksum;
  }

  if (successful_clients != num_clients) {
    printf("RDMA MC client: %d/%d clients completed successfully\n",
           successful_clients, num_clients);
    return -1;
  }

  const int total_requests = num_clients * num_requests;
  const double avg_lat_us = total_lat_us / total_requests;
  const double req_mops = total_requests / elapsed_sec / 1000000.0;
  const double payload_gbps =
      (static_cast<double>(total_requests) * msg_size * 2 * 8) / elapsed_sec /
      1000000000.0;

  printf("RDMA MC client: aggregate avg latency %.3f us\n", avg_lat_us);
  printf("RDMA MC client: measured time %.6f s, throughput %.3f Mops, "
         "payload bandwidth %.3f Gbps\n",
         elapsed_sec, req_mops, payload_gbps);
  printf("RDMA MC client: aggregate response checksum %llu\n",
         static_cast<unsigned long long>(response_checksum));
  return 0;
}
