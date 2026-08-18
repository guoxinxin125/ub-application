#ifdef ERPC_UB

#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>

#include "transport_impl/ub/ub_machine.h"
#include "transport_impl/ub/ub_manager_protocol.h"

namespace {

volatile sig_atomic_t g_stop = 0;
static constexpr time_t kClientTimeoutSeconds = 5;

void request_stop(int) { g_stop = 1; }

bool receive_all(int fd, void *data, size_t length) {
  auto *cursor = static_cast<uint8_t *>(data);
  while (length > 0) {
    const ssize_t received = recv(fd, cursor, length, 0);
    if (received < 0 && errno == EINTR) {
      if (g_stop != 0) return false;
      continue;
    }
    if (received <= 0) return false;
    cursor += received;
    length -= static_cast<size_t>(received);
  }
  return true;
}

bool send_all(int fd, const void *data, size_t length) {
  const auto *cursor = static_cast<const uint8_t *>(data);
  while (length > 0) {
    const ssize_t sent = send(fd, cursor, length, MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR) {
      if (g_stop != 0) return false;
      continue;
    }
    if (sent <= 0) return false;
    cursor += sent;
    length -= static_cast<size_t>(sent);
  }
  return true;
}

erpc::UBManagerResponse handle_request(erpc::UBMachineRegionOwner &owner,
                                       const erpc::UBManagerRequest &request) {
  erpc::UBManagerResponse response{};
  response.machine_id = owner.machine_id();
  response.region_bytes = owner.size();
  response.allocation_flags = erpc::ub_config::allocation_flags();

  if (request.magic != erpc::kUBManagerProtocolMagic ||
      request.version != erpc::kUBManagerProtocolVersion) {
    response.status = -2;
    return response;
  }

  try {
    const auto operation =
        static_cast<erpc::UBManagerOperation>(request.operation);
    if (operation == erpc::UBManagerOperation::kRegisterEndpoint) {
      response.endpoint = owner.register_endpoint(
          request.process_id, request.sm_udp_port, request.rpc_id);
      response.status = 0;
    } else if (operation == erpc::UBManagerOperation::kUnregisterEndpoint) {
      const erpc::UBEndpointHandle endpoint(
          request.slot, request.rpc_id, request.generation,
          request.inbox_offset, request.arena_offset, request.arena_size);
      response.status =
          owner.unregister_endpoint(endpoint, request.process_id) ? 0 : -3;
    } else {
      response.status = -4;
    }
  } catch (const std::exception &error) {
    std::fprintf(stderr, "ub_manager: request failed: %s\n", error.what());
    response.status = -5;
  }
  return response;
}

}  // namespace

int main() {
  using namespace erpc;
  try {
    if (ub_config::process_mode() != ub_config::ProcessMode::kMulti) {
      std::fprintf(stderr, "ub_manager requires ERPC_UB_PROCESS_MODE=multi\n");
      return 2;
    }
  } catch (const std::exception &error) {
    std::fprintf(stderr, "ub_manager: %s\n", error.what());
    return 2;
  }

  const std::string socket_path = ub_config::manager_socket_path();
  int listen_fd = -1;
  bool sdk_initialized = false;
  bool socket_bound = false;
  try {
    sockaddr_un address{};
    if (socket_path.empty() || socket_path.size() >= sizeof(address.sun_path)) {
      throw std::runtime_error("invalid ERPC_UB_MANAGER_SOCKET path");
    }

    ub_sdk_initialize();
    sdk_initialized = true;
    UBMachineRegionOwner owner(ub_config::ProcessMode::kMulti, 0);

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) throw std::runtime_error("socket() failed");
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    (void)unlink(socket_path.c_str());
    if (bind(listen_fd, reinterpret_cast<const sockaddr *>(&address),
             static_cast<socklen_t>(sizeof(address))) != 0) {
      throw std::runtime_error("bind() failed for " + socket_path);
    }
    socket_bound = true;
    if (listen(listen_fd, 64) != 0) {
      throw std::runtime_error("listen() failed");
    }

    struct sigaction stop_action {};
    stop_action.sa_handler = request_stop;
    sigemptyset(&stop_action.sa_mask);
    stop_action.sa_flags = 0;
    if (sigaction(SIGINT, &stop_action, nullptr) != 0 ||
        sigaction(SIGTERM, &stop_action, nullptr) != 0) {
      throw std::runtime_error("sigaction() failed");
    }
    std::printf("ub_manager: machine=%llu region=%s bytes=%zu socket=%s\n",
                static_cast<unsigned long long>(owner.machine_id()),
                owner.name().c_str(), owner.size(), socket_path.c_str());

    while (!g_stop) {
      const int client_fd = accept(listen_fd, nullptr, nullptr);
      if (client_fd < 0) {
        if (errno == EINTR) {
          if (g_stop != 0) break;
          continue;
        }
        throw std::runtime_error("accept() failed");
      }
      const timeval timeout{kClientTimeoutSeconds, 0};
      if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) != 0 ||
          setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout)) != 0) {
        std::fprintf(stderr,
                     "ub_manager: failed to configure client timeout: %s\n",
                     std::strerror(errno));
        close(client_fd);
        continue;
      }
      UBManagerRequest request{};
      if (receive_all(client_fd, &request, sizeof(request))) {
        const UBManagerResponse response = handle_request(owner, request);
        (void)send_all(client_fd, &response, sizeof(response));
      }
      close(client_fd);
    }

    close(listen_fd);
    listen_fd = -1;
    (void)unlink(socket_path.c_str());
    socket_bound = false;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "ub_manager: %s\n", error.what());
    if (listen_fd >= 0) close(listen_fd);
    if (socket_bound) (void)unlink(socket_path.c_str());
    if (sdk_initialized) ub_sdk_finalize_noexcept();
    return 1;
  }

  if (sdk_initialized) ub_sdk_finalize_noexcept();
  return 0;
}

#endif  // ERPC_UB
