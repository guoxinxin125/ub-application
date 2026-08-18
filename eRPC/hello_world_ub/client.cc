#include "common.h"

#include <chrono>
#include <exception>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
erpc::Rpc<erpc::CTransport> *g_rpc = nullptr;

struct RequestState {
  erpc::MsgBuffer request;
  erpc::MsgBuffer response;
  Clock::time_point start;
  uint64_t request_id = 0;
  bool in_use = false;
  bool done = false;
};

void continuation(void *, void *tag) {
  static_cast<RequestState *>(tag)->done = true;
}

void sm_handler(int session_num, erpc::SmEventType event,
                erpc::SmErrType error, void *) {
  if (event != erpc::SmEventType::kConnected ||
      error != erpc::SmErrType::kNoError) {
    std::fprintf(stderr,
                 "UB hello client: session=%d event=%s error=%s\n",
                 session_num, erpc::sm_event_type_str(event).c_str(),
                 erpc::sm_err_type_str(error).c_str());
  }
}

struct RunResult {
  uint64_t checksum = 0;
  size_t errors = 0;
  double total_latency_us = 0.0;
};

RunResult run_requests(int session_num, std::vector<RequestState> &slots,
                       size_t total_requests, size_t msg_size,
                       bool measure_latency) {
  RunResult result;
  size_t sent = 0;
  size_t completed = 0;
  size_t inflight = 0;

  while (completed < total_requests) {
    while (sent < total_requests && inflight < slots.size()) {
      RequestState *slot = nullptr;
      for (RequestState &candidate : slots) {
        if (!candidate.in_use) {
          slot = &candidate;
          break;
        }
      }
      if (slot == nullptr) break;

      slot->request = g_rpc->alloc_msg_buffer_or_die(msg_size);
      // Keep response invalid. UB RX will attach the borrowed shared payload.
      slot->response = erpc::MsgBuffer();
      slot->request_id = static_cast<uint64_t>(sent);
      slot->start = Clock::now();
      slot->done = false;
      slot->in_use = true;
      *reinterpret_cast<uint64_t *>(slot->request.buf_) = slot->request_id;
      g_rpc->enqueue_request(session_num, kUBHelloReqType, &slot->request,
                             &slot->response, continuation, slot);
      ++sent;
      ++inflight;
    }

    g_rpc->run_event_loop_once();

    for (RequestState &slot : slots) {
      if (!slot.in_use || !slot.done) continue;
      if (slot.response.buf_ == nullptr ||
          slot.response.get_data_size() < sizeof(uint64_t)) {
        ++result.errors;
      } else {
        const uint64_t response_id =
            *reinterpret_cast<const uint64_t *>(slot.response.buf_);
        result.checksum += response_id;
        if (response_id != slot.request_id) ++result.errors;
      }

      if (measure_latency) {
        result.total_latency_us +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - slot.start).count() / 1000.0;
      }
      g_rpc->free_msg_buffer(slot.request);
      if (slot.response.buf_ != nullptr) g_rpc->free_msg_buffer(slot.response);
      slot = RequestState{};
      ++completed;
      --inflight;
    }
  }
  return result;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3 || argc > 8) {
    std::fprintf(
        stderr,
        "Usage: %s <server-ip> <client-bind-ip> [concurrency] [msg-size] "
        "[requests] [server-port] [client-port]\n",
        argv[0]);
    return 2;
  }

  const std::string server_ip = argv[1];
  const std::string client_ip = argv[2];
  const size_t concurrency = argc >= 4
      ? static_cast<size_t>(ub_hello_parse_u64(argv[3], "concurrency")) : 1;
  size_t msg_size = argc >= 5
      ? static_cast<size_t>(ub_hello_parse_u64(argv[4], "msg-size"))
      : kUBHelloDefaultMsgSize;
  const size_t requests = argc >= 6
      ? static_cast<size_t>(ub_hello_parse_u64(argv[5], "requests"))
      : kUBHelloDefaultRequests;
  const uint16_t server_port = argc >= 7
      ? ub_hello_parse_port(argv[6], "server-port") : kDefaultServerPort;
  const uint16_t client_port = argc >= 8
      ? ub_hello_parse_port(argv[7], "client-port") : kDefaultClientPort;

  if (concurrency == 0 || requests == 0) {
    std::fprintf(stderr, "concurrency and requests must be greater than zero\n");
    return 2;
  }
  if (msg_size < sizeof(uint64_t)) msg_size = sizeof(uint64_t);

  const std::string client_uri =
      client_ip + ":" + std::to_string(client_port);
  const std::string server_uri =
      server_ip + ":" + std::to_string(server_port);
  ub_hello_print_config("client", client_uri, 1);
  std::printf(
      "UB hello client: server=%s concurrency=%zu msg_size=%zu requests=%zu "
      "warmup=%zu\n",
      server_uri.c_str(), concurrency, msg_size, requests,
      kUBHelloWarmupRequests);

  try {
    erpc::Nexus nexus(client_uri);
    g_rpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, 1, sm_handler, 0);
    const int session_num = g_rpc->create_session(server_uri, 0);
    if (session_num < 0) {
      std::fprintf(stderr, "UB hello client: create_session failed: %d\n",
                   session_num);
      delete g_rpc;
      return 1;
    }

    for (size_t attempt = 0;
         !g_rpc->is_connected(session_num) && attempt < 2000; ++attempt) {
      g_rpc->run_event_loop_once();
      if (attempt % 10 == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!g_rpc->is_connected(session_num)) {
      std::fprintf(stderr, "UB hello client: session connection timed out\n");
      delete g_rpc;
      return 1;
    }

    std::vector<RequestState> slots(concurrency);
    const RunResult warmup = run_requests(
        session_num, slots, kUBHelloWarmupRequests, msg_size, false);
    const RunResult measured =
        run_requests(session_num, slots, requests, msg_size, true);
    delete g_rpc;
    g_rpc = nullptr;

    const uint64_t expected = ub_hello_expected_checksum(requests);
    const bool passed = warmup.errors == 0 && measured.errors == 0 &&
                        measured.checksum == expected;
    std::printf(
        "UB hello client: %s checksum=%llu expected=%llu errors=%zu "
        "average_latency=%.3f us\n",
        passed ? "PASS" : "FAIL",
        static_cast<unsigned long long>(measured.checksum),
        static_cast<unsigned long long>(expected), measured.errors,
        measured.total_latency_us / static_cast<double>(requests));
    return passed ? 0 : 1;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "UB hello client failed: %s\n", error.what());
    delete g_rpc;
    return 1;
  }
}

