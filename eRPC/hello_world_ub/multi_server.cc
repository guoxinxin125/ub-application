#include <array>
#include <csignal>
#include <exception>

#include "common.h"

namespace {

struct ClientStats {
  size_t requests = 0;
  uint64_t checksum = 0;
  size_t errors = 0;
};

erpc::Rpc<erpc::CTransport> *g_rpc = nullptr;
volatile sig_atomic_t g_stop = 0;
std::array<ClientStats, erpc::ub_config::kMaxRpcEndpoints> g_client_stats{};
size_t g_expected_clients = 0;
size_t g_expected_requests_per_client = 0;
size_t g_total_requests = 0;
size_t g_request_errors = 0;

void signal_handler(int) { g_stop = 1; }

void request_handler(erpc::ReqHandle *req_handle, void *) {
  const erpc::MsgBuffer *request = req_handle->get_req_msgbuf();
  if (request == nullptr || request->buf_ == nullptr ||
      request->get_data_size() < kUBHelloPayloadHeaderSize) {
    std::fprintf(stderr, "UB multi server: received an invalid request\n");
    ++g_request_errors;
    g_stop = 1;
    return;
  }

  erpc::MsgBuffer &response = req_handle->dyn_resp_msgbuf_;
  response = g_rpc->alloc_msg_buffer_or_die(request->get_data_size());
  UBHelloPayloadIdentity identity{};
  const bool payload_valid = ub_hello_make_response(
      request->buf_, response.buf_, request->get_data_size(), &identity);

  ++g_total_requests;
  const bool client_valid = identity.client_id > 0 &&
                            identity.client_id <= g_expected_clients &&
                            identity.client_id < g_client_stats.size();
  if (!client_valid) {
    ++g_request_errors;
  } else {
    ClientStats &stats = g_client_stats[identity.client_id];
    ++stats.requests;
    stats.checksum += identity.request_id;
    if (!payload_valid) ++stats.errors;
  }
  if (!payload_valid) ++g_request_errors;

  g_rpc->enqueue_response(req_handle, &response);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 4 || argc > 5) {
    std::fprintf(stderr,
                 "Usage: %s <bind-ip> <clients> <requests-per-client> "
                 "[server-port]\n",
                 argv[0]);
    return 2;
  }

  const std::string bind_ip = argv[1];
  g_expected_clients =
      static_cast<size_t>(ub_hello_parse_u64(argv[2], "clients"));
  const size_t measured_requests =
      static_cast<size_t>(ub_hello_parse_u64(argv[3], "requests-per-client"));
  const uint16_t port = argc >= 5 ? ub_hello_parse_port(argv[4], "server-port")
                                  : kDefaultServerPort;
  if (g_expected_clients == 0 ||
      g_expected_clients >= erpc::ub_config::kMaxRpcEndpoints) {
    std::fprintf(stderr, "clients must be in [1, 63]\n");
    return 2;
  }
  if (measured_requests == 0) {
    std::fprintf(stderr, "requests-per-client must be greater than zero\n");
    return 2;
  }
  g_expected_requests_per_client = kUBHelloWarmupRequests + measured_requests;
  const size_t expected_total =
      g_expected_clients * g_expected_requests_per_client;
  const uint64_t expected_checksum =
      ub_hello_expected_checksum(kUBHelloWarmupRequests) +
      ub_hello_expected_checksum(measured_requests);
  const std::string server_uri = bind_ip + ":" + std::to_string(port);

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
  ub_hello_print_config("multi-server", server_uri, 0);
  std::printf(
      "UB multi server: clients=%zu requests_per_client=%zu warmup=%zu "
      "expected_total=%zu\n",
      g_expected_clients, measured_requests, kUBHelloWarmupRequests,
      expected_total);

  try {
    erpc::Nexus nexus(server_uri, ub_hello_numa_node());
    nexus.register_req_func(kUBHelloReqType, request_handler);
    g_rpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, 0, nullptr, 0);
    while (g_stop == 0) {
      g_rpc->run_event_loop(100);
      if (g_total_requests >= expected_total &&
          g_rpc->num_active_sessions() == 0) {
        break;
      }
    }
    delete g_rpc;
    g_rpc = nullptr;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "UB multi server failed: %s\n", error.what());
    delete g_rpc;
    return 1;
  }

  bool passed = g_request_errors == 0 && g_total_requests == expected_total;
  for (size_t client_id = 1; client_id <= g_expected_clients; ++client_id) {
    const ClientStats &stats = g_client_stats[client_id];
    const bool client_passed =
        stats.requests == g_expected_requests_per_client &&
        stats.checksum == expected_checksum && stats.errors == 0;
    std::printf(
        "UB multi server: client=%zu %s requests=%zu expected=%zu "
        "checksum=%llu expected_checksum=%llu errors=%zu\n",
        client_id, client_passed ? "PASS" : "FAIL", stats.requests,
        g_expected_requests_per_client,
        static_cast<unsigned long long>(stats.checksum),
        static_cast<unsigned long long>(expected_checksum), stats.errors);
    passed = passed && client_passed;
  }
  std::printf("UB multi server: %s total_requests=%zu errors=%zu\n",
              passed ? "PASS" : "FAIL", g_total_requests, g_request_errors);
  return passed ? 0 : 1;
}
