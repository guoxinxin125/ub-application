#include "common.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef ERPC_CXL
#include "transport_impl/cxl/cxl_config.h"
#endif

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

ClientResult run_client_pair(int pair_idx, int concurrency, size_t msg_size,
                             int num_requests, int warmup_requests,
                             std::atomic<int> *ready_count,
                             std::atomic<bool> *start_measure) {
  ClientResult result;
  const uint8_t server_rpc_id = static_cast<uint8_t>(pair_idx * 2);
  const uint8_t client_rpc_id = static_cast<uint8_t>(pair_idx * 2 + 1);
  const uint16_t client_port =
      static_cast<uint16_t>(kClientUDPPort + pair_idx);
  std::string client_uri =
      kClientHostname + ":" + std::to_string(client_port);
  std::string server_uri =
      kServerHostname + ":" + std::to_string(kServerUDPPort);

  printf("Pair client %d: uri=%s, rpc_id=%u, server_rpc_id=%u\n", pair_idx,
         client_uri.c_str(), static_cast<unsigned>(client_rpc_id),
         static_cast<unsigned>(server_rpc_id));

  erpc::Nexus nexus(client_uri);
  erpc::Rpc<erpc::CTransport> *rpc = nullptr;
  {
    std::lock_guard<std::mutex> lock(rpc_init_mutex);
    rpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, client_rpc_id,
                                          sm_handler, 1);
  }

  int session_num = rpc->create_session(server_uri, server_rpc_id);
  if (session_num < 0) {
    printf("Pair client %d: Failed to create session, error: %d\n", pair_idx,
           session_num);
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
    printf("Pair client %d: Failed to connect after %d attempts\n", pair_idx,
           max_attempts);
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
    const int req_id_base = pair_idx * num_requests;

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
        slot->req_id = req_id_base + sent;
        slot->done = false;
        slot->in_use = true;

        *reinterpret_cast<int *>(slot->req.buf_) = slot->req_id;
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

  printf("Pair client %d: avg latency %.3f us, checksum %lu\n", pair_idx,
         result.total_lat_us / num_requests, result.response_checksum);

  delete rpc;
  return result;
}

int main(int argc, char **argv) {
  int concurrency = argc > 1 ? std::atoi(argv[1]) : 1;
  size_t msg_size =
      argc > 2 ? static_cast<size_t>(std::strtoull(argv[2], nullptr, 0))
               : kDefaultMsgSize;
  int num_pairs = argc > 3 ? std::atoi(argv[3]) : 1;
  const int num_requests = argc > 4 ? std::atoi(argv[4]) : 100000;
  const int warmup_requests = 100;
  const int max_pairs = 32;  // CXL queues cover rpc_id 0..63.

  if (concurrency <= 0) concurrency = 1;
  if (concurrency > static_cast<int>(erpc::kSessionReqWindow)) {
    printf("Pair client: Requested concurrency %d, clamping to per-session "
           "request window %zu\n",
           concurrency, erpc::kSessionReqWindow);
    concurrency = static_cast<int>(erpc::kSessionReqWindow);
  }
  if (num_requests <= 0) {
    printf("Pair client: requests/pair must be positive\n");
    return -1;
  }
  if (num_pairs <= 0) num_pairs = 1;
  if (num_pairs > max_pairs) {
    printf("Pair client: Requested %d pairs, clamping to %d\n", num_pairs,
           max_pairs);
    num_pairs = max_pairs;
  }
  configure_cxl_env(num_pairs);
  if (msg_size < sizeof(int)) msg_size = sizeof(int);

  printf("Pair client: concurrency=%d, msg_size=%zu, pairs=%d, "
         "requests/pair=%d, warmup/pair=%d\n",
         concurrency, msg_size, num_pairs, num_requests, warmup_requests);

  std::vector<ClientResult> results(static_cast<size_t>(num_pairs));
  std::vector<std::thread> threads;
  std::atomic<int> ready_count(0);
  std::atomic<bool> start_measure(false);
  threads.reserve(static_cast<size_t>(num_pairs));

  for (int i = 0; i < num_pairs; i++) {
    threads.emplace_back([&, i]() {
      results[static_cast<size_t>(i)] =
          run_client_pair(i, concurrency, msg_size, num_requests,
                          warmup_requests, &ready_count, &start_measure);
    });
  }

  while (ready_count.load(std::memory_order_acquire) < num_pairs) {
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

  int successful_pairs = 0;
  double total_lat_us = 0.0;
  uint64_t response_checksum = 0;
  for (const auto &result : results) {
    if (!result.success) continue;
    successful_pairs++;
    total_lat_us += result.total_lat_us;
    response_checksum += result.response_checksum;
  }

  if (successful_pairs != num_pairs) {
    printf("Pair client: %d/%d pairs completed successfully\n",
           successful_pairs, num_pairs);
    return -1;
  }

  const int total_requests = num_pairs * num_requests;
  const double avg_lat_us = total_lat_us / total_requests;
  const double req_mops = total_requests / elapsed_sec / 1000000.0;
  const double payload_gbps =
      (static_cast<double>(total_requests) * msg_size * 2 * 8) / elapsed_sec /
      1000000000.0;

  printf("Pair client: aggregate avg latency %.3f us\n", avg_lat_us);
  printf("Pair client: measured time %.6f s, throughput %.3f Mops, payload "
         "bandwidth %.3f Gbps\n",
         elapsed_sec, req_mops, payload_gbps);
  printf("Pair client: aggregate response checksum %lu\n", response_checksum);
  return 0;
}
