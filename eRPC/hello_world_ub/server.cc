#include "common.h"

#include <csignal>
#include <exception>

namespace {

erpc::Rpc<erpc::CTransport> *g_rpc = nullptr;
volatile sig_atomic_t g_stop = 0;
size_t g_request_count = 0;
uint64_t g_request_checksum = 0;
size_t g_max_requests = 0;

void signal_handler(int) { g_stop = 1; }

void request_handler(erpc::ReqHandle *req_handle, void *) {
  const erpc::MsgBuffer *request = req_handle->get_req_msgbuf();
  if (request == nullptr || request->buf_ == nullptr ||
      request->get_data_size() < sizeof(uint64_t)) {
    std::fprintf(stderr, "UB hello server: received an invalid request\n");
    g_stop = 1;
    return;
  }

  const uint64_t request_id =
      *reinterpret_cast<const uint64_t *>(request->buf_);
  ++g_request_count;
  g_request_checksum += request_id;

  erpc::MsgBuffer &response = req_handle->dyn_resp_msgbuf_;
  response = g_rpc->alloc_msg_buffer_or_die(request->get_data_size());
  *reinterpret_cast<uint64_t *>(response.buf_) = request_id;
  g_rpc->enqueue_response(req_handle, &response);

  if (g_max_requests != 0 && g_request_count >= g_max_requests) g_stop = 1;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2 || argc > 4) {
    std::fprintf(stderr,
                 "Usage: %s <bind-ip> [server-port] [max-requests]\n",
                 argv[0]);
    return 2;
  }

  const std::string bind_ip = argv[1];
  const uint16_t port =
      argc >= 3 ? ub_hello_parse_port(argv[2], "server-port")
                : kDefaultServerPort;
  g_max_requests = argc >= 4
                       ? static_cast<size_t>(
                             ub_hello_parse_u64(argv[3], "max-requests"))
                       : 0;
  const std::string server_uri = bind_ip + ":" + std::to_string(port);

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
  ub_hello_print_config("server", server_uri, 0);

  try {
    erpc::Nexus nexus(server_uri);
    nexus.register_req_func(kUBHelloReqType, request_handler);
    g_rpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, 0, nullptr, 0);
    while (g_stop == 0) g_rpc->run_event_loop(100);
    delete g_rpc;
    g_rpc = nullptr;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "UB hello server failed: %s\n", error.what());
    delete g_rpc;
    return 1;
  }

  std::printf("UB hello server: requests=%zu checksum=%llu\n",
              g_request_count,
              static_cast<unsigned long long>(g_request_checksum));
  return 0;
}

