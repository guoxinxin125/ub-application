#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

extern "C" {
#include "shm/memlink_client.h"
}

namespace {

constexpr std::size_t kMiB = 1024 * 1024;
constexpr std::size_t kCacheLine = 64;
constexpr std::size_t kOwnerReadSlot = 4 * 1024;
constexpr std::size_t kRemoteWriteSlot = 8 * 1024;
constexpr std::size_t kRemoteFetchAddSlot = 12 * 1024;
constexpr std::size_t kRemoteCasSlot = 16 * 1024;
constexpr std::size_t kContendedAtomicSlot = 20 * 1024;
constexpr std::size_t kLocalAtomicSlot = 24 * 1024;
constexpr std::size_t kSizedVisibilitySlot = 128 * 1024;
constexpr std::size_t kStreamRecordSlot = 512 * 1024;
constexpr std::size_t kWorkingSetOffset = 2 * kMiB;

struct Options {
  int id = -1;
  std::string local_ip;
  std::string peer_ip;
  uint16_t port = 19090;
  int owner_node_id = -1;
  std::string prefix;
  std::string test = "basic";
  std::size_t region_mb = 64;
  std::size_t working_set_mb = 4;
  std::size_t iterations = 10000;
  std::size_t size_iterations = 1000;
  std::size_t tcp_iterations = 1000;
  std::size_t atomic_iterations = 10000;
  std::size_t stream_payload_size = 4096;
  std::size_t stream_duration_ms = 3000;
  int startup_timeout_sec = 120;
  bool reuse = false;
  bool test_remote_atomics = false;
};

[[noreturn]] void fail(const std::string &message) {
  throw std::runtime_error(message);
}

std::string errno_message(const std::string &operation) {
  return operation + ": " + std::strerror(errno);
}

std::string take_value(int &index, int argc, char **argv,
                       const std::string &argument) {
  auto equal = argument.find('=');
  if (equal != std::string::npos)
    return argument.substr(equal + 1);
  if (index + 1 >= argc)
    fail("missing value for " + argument);
  return argv[++index];
}

bool parse_bool(const std::string &value) {
  if (value == "1" || value == "true" || value == "on")
    return true;
  if (value == "0" || value == "false" || value == "off")
    return false;
  fail("invalid boolean value: " + value);
}

void print_tests() {
  std::cout
      << "connectivity              TCP reachability and bidirectional RTT\n"
      << "basic                     original latency and 8-byte visibility "
         "test\n"
      << "size_visibility           8B..64KiB bidirectional visibility and "
         "guards\n"
      << "owner_stream_remote_read  owner continuous writes and remote "
         "snapshot polling\n"
      << "remote_atomic             experimental remote CPU atomic capability "
         "test\n";
}

void print_usage(const char *program) {
  std::cout
      << "Usage: " << program << " [options]\n"
      << "  --id 0|1                    local host id\n"
      << "  --local-ip IP               IP bound by this process\n"
      << "  --peer-ip IP                peer IP\n"
      << "  --owner-node-id N           physical Memlink owner node id\n"
      << "  --prefix NAME               unique cluster-wide region prefix\n"
      << "  --test NAME                 one test point to run (default basic)\n"
      << "  --list-tests                print available test points\n"
      << "  --port N                    TCP control port (default 19090)\n"
      << "  --region-mb N               bytes owned by each host in MiB "
         "(default 64)\n"
      << "  --working-set-mb N          local pointer-chase working set "
         "(default 4)\n"
      << "  --iterations N              UB read/write iterations (default "
         "10000)\n"
      << "  --size-iterations N         iterations per data size (default "
         "1000)\n"
      << "  --tcp-iterations N          TCP RTT iterations (default 1000)\n"
      << "  --atomic-iterations N       atomic iterations (default 10000)\n"
      << "  --stream-payload-size N     stream record payload bytes (default "
         "4096)\n"
      << "  --stream-duration-ms N      stream test time per owner (default "
         "3000)\n"
      << "  --startup-timeout-sec N     peer-region/connect timeout (default "
         "120)\n"
      << "  --reuse true|false          allow an existing region (default "
         "false)\n"
      << "  --test-remote-atomics BOOL  probe CPU atomics on remote UB "
         "(default false)\n";
}

Options parse_options(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string argument(argv[i]);
    std::string key = argument.substr(0, argument.find('='));
    if (key == "--help" || key == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (key == "--list-tests") {
      print_tests();
      std::exit(0);
    } else if (key == "--id") {
      options.id = std::stoi(take_value(i, argc, argv, argument));
    } else if (key == "--local-ip") {
      options.local_ip = take_value(i, argc, argv, argument);
    } else if (key == "--peer-ip") {
      options.peer_ip = take_value(i, argc, argv, argument);
    } else if (key == "--port") {
      auto port = std::stoul(take_value(i, argc, argv, argument));
      if (port == 0 || port > std::numeric_limits<uint16_t>::max())
        fail("--port must be between 1 and 65535");
      options.port = static_cast<uint16_t>(port);
    } else if (key == "--owner-node-id") {
      options.owner_node_id = std::stoi(take_value(i, argc, argv, argument));
    } else if (key == "--prefix") {
      options.prefix = take_value(i, argc, argv, argument);
    } else if (key == "--test") {
      options.test = take_value(i, argc, argv, argument);
    } else if (key == "--region-mb") {
      options.region_mb = std::stoull(take_value(i, argc, argv, argument));
    } else if (key == "--working-set-mb") {
      options.working_set_mb = std::stoull(take_value(i, argc, argv, argument));
    } else if (key == "--iterations") {
      options.iterations = std::stoull(take_value(i, argc, argv, argument));
    } else if (key == "--size-iterations") {
      options.size_iterations =
          std::stoull(take_value(i, argc, argv, argument));
    } else if (key == "--tcp-iterations") {
      options.tcp_iterations = std::stoull(take_value(i, argc, argv, argument));
    } else if (key == "--atomic-iterations") {
      options.atomic_iterations =
          std::stoull(take_value(i, argc, argv, argument));
    } else if (key == "--stream-payload-size") {
      options.stream_payload_size =
          std::stoull(take_value(i, argc, argv, argument));
    } else if (key == "--stream-duration-ms") {
      options.stream_duration_ms =
          std::stoull(take_value(i, argc, argv, argument));
    } else if (key == "--startup-timeout-sec") {
      options.startup_timeout_sec =
          std::stoi(take_value(i, argc, argv, argument));
    } else if (key == "--reuse") {
      options.reuse = parse_bool(take_value(i, argc, argv, argument));
    } else if (key == "--test-remote-atomics") {
      options.test_remote_atomics =
          parse_bool(take_value(i, argc, argv, argument));
    } else {
      fail("unknown argument: " + argument);
    }
  }

  if (options.id != 0 && options.id != 1)
    fail("--id must be 0 or 1");
  if (options.local_ip.empty() || options.peer_ip.empty())
    fail("--local-ip and --peer-ip are required");
  const std::vector<std::string> tests = {
      "connectivity", "basic", "size_visibility", "owner_stream_remote_read",
      "remote_atomic"};
  if (std::find(tests.begin(), tests.end(), options.test) == tests.end())
    fail("unknown --test value: " + options.test + "; use --list-tests");
  if (options.test != "connectivity") {
    if (options.owner_node_id < 0)
      fail("--owner-node-id is required for UB tests");
    if (options.prefix.empty())
      fail("--prefix is required and should be unique for every UB run");
  }
  if (options.port == 0)
    fail("--port must be between 1 and 65535");
  if (options.region_mb == 0 || options.working_set_mb == 0)
    fail("region and working-set sizes must be non-zero");
  if (options.iterations == 0 || options.size_iterations == 0 ||
      options.tcp_iterations == 0 || options.atomic_iterations == 0 ||
      options.stream_duration_ms == 0)
    fail("iteration counts must be non-zero");
  if (options.stream_payload_size < kCacheLine ||
      options.stream_payload_size > 64 * 1024)
    fail("--stream-payload-size must be between 64 and 65536 bytes");
  if (options.region_mb * kMiB <
      kWorkingSetOffset + options.working_set_mb * kMiB)
    fail("--region-mb is too small for the requested working set");
  return options;
}

uint64_t now_ns() {
  timespec value{};
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0)
    fail(errno_message("clock_gettime"));
  return static_cast<uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
}

void full_fence() {
#if defined(__x86_64__)
  _mm_mfence();
#elif defined(__aarch64__)
  asm volatile("dmb ish" ::: "memory");
#else
#error "cache maintenance is only implemented for x86_64 and aarch64"
#endif
}

void invalidate_range(void *address, std::size_t size) {
  auto begin = reinterpret_cast<uintptr_t>(address) & ~(kCacheLine - 1);
  auto end = reinterpret_cast<uintptr_t>(address) + size;
#if defined(__x86_64__)
  _mm_mfence();
  for (auto current = begin; current < end; current += kCacheLine)
    _mm_clflushopt(reinterpret_cast<void *>(current));
  _mm_mfence();
#elif defined(__aarch64__)
  asm volatile("dmb ish" ::: "memory");
  for (auto current = begin; current < end; current += kCacheLine)
    asm volatile("dc civac, %0" : : "r"(current) : "memory");
  asm volatile("dsb ish" ::: "memory");
#endif
}

void writeback_range(void *address, std::size_t size) {
  auto begin = reinterpret_cast<uintptr_t>(address) & ~(kCacheLine - 1);
  auto end = reinterpret_cast<uintptr_t>(address) + size;
#if defined(__x86_64__)
  _mm_sfence();
  for (auto current = begin; current < end; current += kCacheLine)
    _mm_clwb(reinterpret_cast<void *>(current));
  _mm_sfence();
#elif defined(__aarch64__)
  asm volatile("dmb ish" ::: "memory");
  for (auto current = begin; current < end; current += kCacheLine)
    asm volatile("dc cvac, %0" : : "r"(current) : "memory");
  asm volatile("dsb ish" ::: "memory");
#endif
}

void send_all(int fd, const void *buffer, std::size_t size) {
  auto *bytes = static_cast<const char *>(buffer);
  while (size != 0) {
    ssize_t result = send(fd, bytes, size, MSG_NOSIGNAL);
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0)
      fail(errno_message("send"));
    bytes += result;
    size -= static_cast<std::size_t>(result);
  }
}

void receive_all(int fd, void *buffer, std::size_t size) {
  auto *bytes = static_cast<char *>(buffer);
  while (size != 0) {
    ssize_t result = recv(fd, bytes, size, MSG_WAITALL);
    if (result < 0 && errno == EINTR)
      continue;
    if (result == 0)
      fail("peer closed the TCP connection");
    if (result < 0)
      fail(errno_message("recv"));
    bytes += result;
    size -= static_cast<std::size_t>(result);
  }
}

sockaddr_in make_address(const std::string &ip, uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1)
    fail("invalid IPv4 address: " + ip);
  return address;
}

void configure_socket(int fd) {
  int enabled = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0)
    fail(errno_message("setsockopt(TCP_NODELAY)"));
}

int open_control_connection(const Options &options) {
  if (options.id == 0) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
      fail(errno_message("socket"));
    int enabled = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    auto local = make_address(options.local_ip, options.port);
    if (bind(listen_fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) !=
        0)
      fail(errno_message("bind"));
    if (listen(listen_fd, 1) != 0)
      fail(errno_message("listen"));
    std::cout << "INFO host=0 waiting_for_tcp_peer=" << options.peer_ip << ':'
              << options.port << std::endl;
    sockaddr_in remote{};
    socklen_t remote_size = sizeof(remote);
    int fd =
        accept(listen_fd, reinterpret_cast<sockaddr *>(&remote), &remote_size);
    close(listen_fd);
    if (fd < 0)
      fail(errno_message("accept"));
    configure_socket(fd);
    char peer[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &remote.sin_addr, peer, sizeof(peer));
    std::cout << "INFO host=0 tcp_connected_peer=" << peer << std::endl;
    return fd;
  }

  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::seconds(options.startup_timeout_sec);
  while (std::chrono::steady_clock::now() < deadline) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
      fail(errno_message("socket"));
    auto local = make_address(options.local_ip, 0);
    if (bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
      close(fd);
      fail(errno_message("client bind"));
    }
    auto remote = make_address(options.peer_ip, options.port);
    if (connect(fd, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) ==
        0) {
      configure_socket(fd);
      std::cout << "INFO host=1 tcp_connected_peer=" << options.peer_ip
                << std::endl;
      return fd;
    }
    close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  fail("timed out connecting to TCP peer " + options.peer_ip);
}

void barrier(int fd, int id, uint64_t phase) {
  uint64_t value = 0x5542424152520000ULL ^ phase;
  uint64_t received = 0;
  if (id == 0) {
    send_all(fd, &value, sizeof(value));
    receive_all(fd, &received, sizeof(received));
  } else {
    receive_all(fd, &received, sizeof(received));
    send_all(fd, &received, sizeof(received));
  }
  if (received != value)
    fail("TCP barrier protocol mismatch");
}

double percentile(const std::vector<uint64_t> &sorted, double fraction) {
  if (sorted.empty())
    return 0;
  std::size_t index = static_cast<std::size_t>(fraction * (sorted.size() - 1));
  return static_cast<double>(sorted[index]);
}

void print_samples(int host, const std::string &name,
                   std::vector<uint64_t> samples) {
  std::sort(samples.begin(), samples.end());
  long double total = 0;
  for (auto sample : samples)
    total += sample;
  std::cout << std::fixed << std::setprecision(2) << "METRIC host=" << host
            << " name=" << name << " unit=ns count=" << samples.size()
            << " avg=" << static_cast<double>(total / samples.size())
            << " p50=" << percentile(samples, 0.50)
            << " p95=" << percentile(samples, 0.95)
            << " p99=" << percentile(samples, 0.99)
            << " min=" << samples.front() << " max=" << samples.back()
            << std::endl;
}

void print_average(int host, const std::string &name, uint64_t elapsed,
                   std::size_t operations) {
  std::cout << std::fixed << std::setprecision(2) << "METRIC host=" << host
            << " name=" << name << " unit=ns/op count=" << operations
            << " avg=" << static_cast<double>(elapsed) / operations
            << std::endl;
}

std::string region_name(const Options &options, int owner) {
  return options.prefix + "_owner_" + std::to_string(owner);
}

std::vector<void *> initialize_regions(const Options &options) {
  auto local_name = region_name(options, options.id);
  bool exists = false;
  int result = MemlinkMemExist(local_name.c_str(), &exists);
  if (result != 0)
    fail("MemlinkMemExist failed for " + local_name +
         ", result=" + std::to_string(result));
  if (exists && !options.reuse)
    fail("region already exists: " + local_name +
         "; use a unique --prefix or --reuse true");
  if (!exists) {
    result = MemlinkMemShmCreate(local_name.c_str(), options.region_mb,
                                 options.owner_node_id);
    if (result != 0)
      fail("MemlinkMemShmCreate failed for " + local_name +
           ", result=" + std::to_string(result));
    std::cout << "INFO host=" << options.id << " created_region=" << local_name
              << " owner_node_id=" << options.owner_node_id
              << " size_mb=" << options.region_mb << std::endl;
  }

  std::vector<void *> bases(2, nullptr);
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::seconds(options.startup_timeout_sec);
  for (int owner = 0; owner < 2; ++owner) {
    auto name = region_name(options, owner);
    while (true) {
      exists = false;
      result = MemlinkMemExist(name.c_str(), &exists);
      if (result != 0)
        fail("MemlinkMemExist failed for " + name +
             ", result=" + std::to_string(result));
      if (exists)
        break;
      if (std::chrono::steady_clock::now() >= deadline)
        fail("timed out waiting for Memlink region " + name);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    int mmap_result = 0;
    void *base = MemlinkMemShmMmap(nullptr, options.region_mb * kMiB,
                                   PROT_READ | PROT_WRITE, MAP_SHARED,
                                   name.c_str(), 0, &mmap_result);
    if (mmap_result != 0 || base == nullptr || base == MAP_FAILED)
      fail("MemlinkMemShmMmap failed for " + name +
           ", result=" + std::to_string(mmap_result));
    bases[owner] = base;
    std::cout << "INFO host=" << options.id << " mapped_owner=" << owner
              << " base=" << base << std::endl;
  }
  return bases;
}

void benchmark_tcp_rtt(int fd, int local_id, int initiator,
                       std::size_t iterations) {
  uint64_t value = 0;
  std::vector<uint64_t> samples;
  if (local_id == initiator)
    samples.reserve(iterations);
  for (std::size_t i = 0; i < iterations; ++i) {
    if (local_id == initiator) {
      value = 0x5450430000000000ULL ^ i;
      uint64_t begin = now_ns();
      send_all(fd, &value, sizeof(value));
      uint64_t echo = 0;
      receive_all(fd, &echo, sizeof(echo));
      uint64_t end = now_ns();
      if (echo != value)
        fail("TCP echo mismatch");
      samples.push_back(end - begin);
    } else {
      receive_all(fd, &value, sizeof(value));
      send_all(fd, &value, sizeof(value));
    }
  }
  if (local_id == initiator)
    print_samples(local_id, "tcp_rtt_initiator_" + std::to_string(initiator),
                  std::move(samples));
}

void initialize_pointer_cycle(char *base, std::size_t bytes, int host) {
  std::size_t lines = bytes / kCacheLine;
  std::vector<std::size_t> order(lines);
  for (std::size_t i = 0; i < lines; ++i)
    order[i] = i;
  std::mt19937_64 random(0x55420000ULL + host);
  std::shuffle(order.begin(), order.end(), random);
  for (std::size_t i = 0; i < lines; ++i) {
    auto *slot = reinterpret_cast<uint64_t *>(base + order[i] * kCacheLine);
    *slot = order[(i + 1) % lines];
  }
}

void benchmark_local_memory(const Options &options, void *local_base) {
  auto *working_set = static_cast<char *>(local_base) + kWorkingSetOffset;
  std::size_t bytes = options.working_set_mb * kMiB;
  std::size_t lines = bytes / kCacheLine;
  initialize_pointer_cycle(working_set, bytes, options.id);
  full_fence();

  volatile uint64_t *hot = reinterpret_cast<volatile uint64_t *>(working_set);
  uint64_t sink = 0;
  uint64_t begin = now_ns();
  for (std::size_t i = 0; i < options.iterations; ++i)
    sink ^= *hot;
  uint64_t end = now_ns();
  asm volatile("" : "+r"(sink) : : "memory");
  print_average(options.id, "local_owner_hot_read", end - begin,
                options.iterations);

  std::size_t index = 0;
  begin = now_ns();
  for (std::size_t i = 0; i < options.iterations; ++i) {
    auto *slot =
        reinterpret_cast<volatile uint64_t *>(working_set + index * kCacheLine);
    index = *slot;
  }
  end = now_ns();
  asm volatile("" : "+r"(index) : : "memory");
  print_average(options.id, "local_owner_pointer_chase_read", end - begin,
                options.iterations);

  begin = now_ns();
  for (std::size_t i = 0; i < options.iterations; ++i) {
    auto *slot = reinterpret_cast<volatile uint64_t *>(
        working_set + (i % lines) * kCacheLine + 8);
    *slot = i;
  }
  full_fence();
  end = now_ns();
  print_average(options.id, "local_owner_cached_write", end - begin,
                options.iterations);

  begin = now_ns();
  for (std::size_t i = 0; i < options.iterations; ++i) {
    auto *slot = reinterpret_cast<volatile uint64_t *>(
        working_set + (i % lines) * kCacheLine + 16);
    *slot = i;
    full_fence();
  }
  end = now_ns();
  print_average(options.id, "local_owner_ordered_write", end - begin,
                options.iterations);
}

void benchmark_local_atomics(const Options &options, void *local_base) {
  auto *target = reinterpret_cast<uint64_t *>(static_cast<char *>(local_base) +
                                              kLocalAtomicSlot);
  __atomic_store_n(target, 0, __ATOMIC_RELAXED);
  uint64_t sink = 0;

  uint64_t begin = now_ns();
  for (std::size_t i = 0; i < options.atomic_iterations; ++i)
    sink ^= __atomic_load_n(target, __ATOMIC_RELAXED);
  uint64_t end = now_ns();
  asm volatile("" : "+r"(sink) : : "memory");
  print_average(options.id, "local_owner_atomic_load_relaxed", end - begin,
                options.atomic_iterations);

  begin = now_ns();
  for (std::size_t i = 0; i < options.atomic_iterations; ++i)
    __atomic_store_n(target, i, __ATOMIC_RELAXED);
  end = now_ns();
  print_average(options.id, "local_owner_atomic_store_relaxed", end - begin,
                options.atomic_iterations);

  __atomic_store_n(target, 0, __ATOMIC_RELAXED);
  begin = now_ns();
  for (std::size_t i = 0; i < options.atomic_iterations; ++i)
    __atomic_fetch_add(target, 1, __ATOMIC_RELAXED);
  end = now_ns();
  print_average(options.id, "local_owner_atomic_fetch_add", end - begin,
                options.atomic_iterations);

  __atomic_store_n(target, 0, __ATOMIC_RELAXED);
  std::size_t failures = 0;
  begin = now_ns();
  for (std::size_t i = 0; i < options.atomic_iterations; ++i) {
    uint64_t expected = i;
    if (!__atomic_compare_exchange_n(target, &expected, i + 1, false,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED))
      ++failures;
  }
  end = now_ns();
  print_average(options.id, "local_owner_atomic_cas_success", end - begin,
                options.atomic_iterations);
  std::cout << "CHECK host=" << options.id
            << " name=local_atomic_cas failures=" << failures
            << " status=" << (failures == 0 ? "PASS" : "FAIL") << std::endl;
}

uint64_t test_value(int actor, std::size_t iteration) {
  return 0x5542000000000000ULL | (static_cast<uint64_t>(actor) << 48) |
         iteration;
}

void benchmark_owner_write_remote_read(const Options &options,
                                       const std::vector<void *> &bases, int fd,
                                       int owner) {
  int reader = 1 - owner;
  auto *target = reinterpret_cast<volatile uint64_t *>(
      static_cast<char *>(bases[owner]) + kOwnerReadSlot);
  std::vector<uint64_t> samples;
  if (options.id == reader)
    samples.reserve(options.iterations);

  for (std::size_t i = 0; i < options.iterations; ++i) {
    if (options.id == owner) {
      uint64_t value = test_value(owner, i);
      *target = value;
      full_fence(); // ordering only; the owner never flushes or invalidates
      send_all(fd, &value, sizeof(value));
      uint64_t ack = 0;
      receive_all(fd, &ack, sizeof(ack));
      if (ack != value)
        fail("remote-read acknowledgement mismatch");
    } else {
      uint64_t expected = 0;
      receive_all(fd, &expected, sizeof(expected));
      uint64_t begin = now_ns();
      invalidate_range(const_cast<uint64_t *>(target), sizeof(*target));
      uint64_t observed = *target;
      uint64_t end = now_ns();
      if (observed != expected)
        fail("owner-write/remote-read visibility failure: expected=" +
             std::to_string(expected) +
             " observed=" + std::to_string(observed));
      samples.push_back(end - begin);
      send_all(fd, &observed, sizeof(observed));
    }
  }
  if (options.id == reader) {
    print_samples(options.id,
                  "remote_read_from_owner_" + std::to_string(owner) +
                      "_invalidate_load",
                  std::move(samples));
    std::cout << "CHECK host=" << options.id
              << " name=owner_write_remote_read owner=" << owner
              << " status=PASS" << std::endl;
  }
}

void benchmark_remote_write_owner_read(const Options &options,
                                       const std::vector<void *> &bases, int fd,
                                       int owner) {
  int writer = 1 - owner;
  auto *target = reinterpret_cast<volatile uint64_t *>(
      static_cast<char *>(bases[owner]) + kRemoteWriteSlot);
  std::vector<uint64_t> samples;
  if (options.id == owner) {
    *target = 0;
    full_fence();
    volatile uint64_t preload = *target;
    (void)preload;
  } else {
    samples.reserve(options.iterations);
  }
  barrier(fd, options.id, 0x100 + owner);

  for (std::size_t i = 0; i < options.iterations; ++i) {
    if (options.id == writer) {
      uint64_t value = test_value(writer, i);
      uint64_t begin = now_ns();
      invalidate_range(const_cast<uint64_t *>(target), sizeof(*target));
      *target = value;
      writeback_range(const_cast<uint64_t *>(target), sizeof(*target));
      uint64_t end = now_ns();
      samples.push_back(end - begin);
      send_all(fd, &value, sizeof(value));
      uint64_t ack = 0;
      receive_all(fd, &ack, sizeof(ack));
      if (ack != value)
        fail("remote-write acknowledgement mismatch");
    } else {
      uint64_t expected = 0;
      receive_all(fd, &expected, sizeof(expected));
      uint64_t observed = *target; // owner access: no flush or invalidate
      if (observed != expected)
        fail("remote-write/owner-read visibility failure: expected=" +
             std::to_string(expected) +
             " observed=" + std::to_string(observed));
      send_all(fd, &observed, sizeof(observed));
    }
  }
  if (options.id == writer) {
    print_samples(options.id,
                  "remote_write_to_owner_" + std::to_string(owner) +
                      "_invalidate_store_writeback",
                  std::move(samples));
    std::cout << "CHECK host=" << options.id
              << " name=remote_write_owner_read owner=" << owner
              << " status=PASS" << std::endl;
  }
}

uint8_t payload_byte(int actor, uint64_t sequence, std::size_t offset) {
  uint64_t value = sequence * 0x9e3779b97f4a7c15ULL;
  value ^= static_cast<uint64_t>(actor + 1) * 0xd6e8feb86659fd93ULL;
  value ^= offset * 0xa0761d6478bd642fULL;
  value ^= value >> 29;
  return static_cast<uint8_t>(value);
}

std::vector<uint8_t> make_payload(int actor, uint64_t sequence,
                                  std::size_t size) {
  std::vector<uint8_t> payload(size);
  for (std::size_t i = 0; i < size; ++i)
    payload[i] = payload_byte(actor, sequence, i);
  return payload;
}

bool check_bytes(const volatile uint8_t *address,
                 const std::vector<uint8_t> &expected,
                 std::size_t *bad_offset = nullptr) {
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (address[i] != expected[i]) {
      if (bad_offset != nullptr)
        *bad_offset = i;
      return false;
    }
  }
  return true;
}

bool check_guard(const volatile uint8_t *address, std::size_t size,
                 uint8_t expected, std::size_t *bad_offset = nullptr) {
  for (std::size_t i = 0; i < size; ++i) {
    if (address[i] != expected) {
      if (bad_offset != nullptr)
        *bad_offset = i;
      return false;
    }
  }
  return true;
}

void initialize_sized_guards(volatile uint8_t *left, volatile uint8_t *right) {
  for (std::size_t i = 0; i < kCacheLine; ++i) {
    left[i] = 0xa5;
    right[i] = 0x5a;
  }
}

void validate_sized_record(volatile uint8_t *left, volatile uint8_t *payload,
                           volatile uint8_t *right,
                           const std::vector<uint8_t> &expected,
                           const std::string &operation) {
  std::size_t bad_offset = 0;
  if (!check_guard(left, kCacheLine, 0xa5, &bad_offset))
    fail(operation + " left guard corrupted at byte " +
         std::to_string(bad_offset));
  if (!check_bytes(payload, expected, &bad_offset))
    fail(operation + " payload mismatch at byte " + std::to_string(bad_offset));
  if (!check_guard(right, kCacheLine, 0x5a, &bad_offset))
    fail(operation + " right guard corrupted at byte " +
         std::to_string(bad_offset));
}

void sized_owner_write_remote_read(const Options &options,
                                   const std::vector<void *> &bases, int fd,
                                   int owner, std::size_t size) {
  int reader = 1 - owner;
  auto *left = reinterpret_cast<volatile uint8_t *>(
      static_cast<char *>(bases[owner]) + kSizedVisibilitySlot);
  auto *payload = left + kCacheLine;
  auto *right = payload + size;
  std::vector<uint64_t> samples;
  if (options.id == reader)
    samples.reserve(options.size_iterations);

  for (std::size_t i = 0; i < options.size_iterations; ++i) {
    auto expected = make_payload(owner, i + 1, size);
    uint64_t signal = test_value(owner, i) ^ size;
    if (options.id == owner) {
      initialize_sized_guards(left, right);
      std::memcpy(const_cast<uint8_t *>(payload), expected.data(), size);
      full_fence(); // owner ordering only; no flush or invalidate
      send_all(fd, &signal, sizeof(signal));
      uint64_t ack = 0;
      receive_all(fd, &ack, sizeof(ack));
      if (ack != signal)
        fail("sized remote-read acknowledgement mismatch");
    } else {
      uint64_t received = 0;
      receive_all(fd, &received, sizeof(received));
      if (received != signal)
        fail("sized remote-read control message mismatch");
      uint64_t begin = now_ns();
      invalidate_range(const_cast<uint8_t *>(left), 2 * kCacheLine + size);
      validate_sized_record(left, payload, right, expected,
                            "owner-write/remote-read size=" +
                                std::to_string(size));
      uint64_t end = now_ns();
      samples.push_back(end - begin);
      send_all(fd, &received, sizeof(received));
    }
  }

  if (options.id == reader) {
    print_samples(options.id,
                  "size_visibility_remote_read_owner_" + std::to_string(owner) +
                      "_bytes_" + std::to_string(size),
                  std::move(samples));
    std::cout << "CHECK host=" << options.id
              << " name=size_owner_write_remote_read owner=" << owner
              << " size=" << size << " status=PASS" << std::endl;
  }
}

void sized_remote_write_owner_read(const Options &options,
                                   const std::vector<void *> &bases, int fd,
                                   int owner, std::size_t size) {
  int writer = 1 - owner;
  auto *left = reinterpret_cast<volatile uint8_t *>(
      static_cast<char *>(bases[owner]) + kSizedVisibilitySlot);
  auto *payload = left + kCacheLine;
  auto *right = payload + size;
  if (options.id == owner) {
    initialize_sized_guards(left, right);
    std::memset(const_cast<uint8_t *>(payload), 0, size);
    full_fence();
    for (std::size_t i = 0; i < 2 * kCacheLine + size; i += kCacheLine) {
      volatile uint8_t preload = left[i];
      (void)preload;
    }
  }
  barrier(fd, options.id, 0x5000 + owner * 0x100 + size);

  std::vector<uint64_t> samples;
  if (options.id == writer)
    samples.reserve(options.size_iterations);
  for (std::size_t i = 0; i < options.size_iterations; ++i) {
    auto expected = make_payload(writer, i + 1, size);
    uint64_t signal = test_value(writer, i) ^ size;
    if (options.id == writer) {
      uint64_t begin = now_ns();
      invalidate_range(const_cast<uint8_t *>(left), 2 * kCacheLine + size);
      std::memcpy(const_cast<uint8_t *>(payload), expected.data(), size);
      writeback_range(const_cast<uint8_t *>(payload), size);
      uint64_t end = now_ns();
      samples.push_back(end - begin);
      send_all(fd, &signal, sizeof(signal));
      uint64_t ack = 0;
      receive_all(fd, &ack, sizeof(ack));
      if (ack != signal)
        fail("sized remote-write acknowledgement mismatch");
    } else {
      uint64_t received = 0;
      receive_all(fd, &received, sizeof(received));
      if (received != signal)
        fail("sized remote-write control message mismatch");
      validate_sized_record(left, payload, right, expected,
                            "remote-write/owner-read size=" +
                                std::to_string(size));
      send_all(fd, &received, sizeof(received));
    }
  }

  if (options.id == writer) {
    print_samples(options.id,
                  "size_visibility_remote_write_owner_" +
                      std::to_string(owner) + "_bytes_" + std::to_string(size),
                  std::move(samples));
    std::cout << "CHECK host=" << options.id
              << " name=size_remote_write_owner_read owner=" << owner
              << " size=" << size << " status=PASS" << std::endl;
  }
}

void run_size_visibility(const Options &options,
                         const std::vector<void *> &bases, int fd) {
  const std::vector<std::size_t> sizes = {8, 64, 256, 4096, 64 * 1024};
  uint64_t phase = 0x6000;
  for (auto size : sizes) {
    for (int owner = 0; owner < 2; ++owner) {
      sized_owner_write_remote_read(options, bases, fd, owner, size);
      barrier(fd, options.id, phase++);
      sized_remote_write_owner_read(options, bases, fd, owner, size);
      barrier(fd, options.id, phase++);
    }
  }
}

struct StreamRecord {
  volatile uint64_t *begin_version;
  volatile uint8_t *payload;
  volatile uint64_t *end_version;
  std::size_t payload_size;
};

std::size_t align_up(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

StreamRecord stream_record(void *base, std::size_t payload_size) {
  auto *record = static_cast<char *>(base) + kStreamRecordSlot;
  auto footer_offset = align_up(kCacheLine + payload_size, kCacheLine);
  return {reinterpret_cast<volatile uint64_t *>(record),
          reinterpret_cast<volatile uint8_t *>(record + kCacheLine),
          reinterpret_cast<volatile uint64_t *>(record + footer_offset),
          payload_size};
}

void initialize_stream_record(const StreamRecord &record, int owner) {
  auto payload = make_payload(owner, 0, record.payload_size);
  *record.begin_version = 0;
  std::memcpy(const_cast<uint8_t *>(record.payload), payload.data(),
              record.payload_size);
  *record.end_version = 0;
  full_fence();
}

void publish_stream_record(const StreamRecord &record, int owner,
                           uint64_t sequence, std::vector<uint8_t> &payload) {
  uint64_t odd = sequence * 2 - 1;
  uint64_t even = sequence * 2;
  *record.begin_version = odd;
  *record.end_version = odd;
  full_fence();
  for (std::size_t i = 0; i < record.payload_size; ++i)
    payload[i] = payload_byte(owner, sequence, i);
  std::memcpy(const_cast<uint8_t *>(record.payload), payload.data(),
              record.payload_size);
  full_fence();
  *record.end_version = even;
  full_fence();
  *record.begin_version = even;
  full_fence(); // owner never performs cache-line maintenance
}

enum class SnapshotResult { VALID, RETRY, CORRUPT };

SnapshotResult read_remote_stream_snapshot(const StreamRecord &record,
                                           int owner,
                                           std::vector<uint8_t> &snapshot,
                                           uint64_t &version) {
  invalidate_range(const_cast<uint64_t *>(record.begin_version),
                   sizeof(uint64_t));
  uint64_t first = *record.begin_version;
  if ((first & 1) != 0)
    return SnapshotResult::RETRY;

  auto payload_span = reinterpret_cast<uintptr_t>(record.end_version) +
                      sizeof(uint64_t) -
                      reinterpret_cast<uintptr_t>(record.payload);
  invalidate_range(const_cast<uint8_t *>(record.payload), payload_span);
  for (std::size_t i = 0; i < record.payload_size; ++i)
    snapshot[i] = record.payload[i];
  uint64_t footer = *record.end_version;

  invalidate_range(const_cast<uint64_t *>(record.begin_version),
                   sizeof(uint64_t));
  uint64_t second = *record.begin_version;
  if (first != second || first != footer || (second & 1) != 0)
    return SnapshotResult::RETRY;

  version = second;
  uint64_t sequence = version / 2;
  for (std::size_t i = 0; i < record.payload_size; ++i) {
    if (snapshot[i] != payload_byte(owner, sequence, i))
      return SnapshotResult::CORRUPT;
  }
  return SnapshotResult::VALID;
}

bool run_stream_direction(const Options &options,
                          const std::vector<void *> &bases, int fd, int owner) {
  auto record = stream_record(bases[owner], options.stream_payload_size);
  if (options.id == owner)
    initialize_stream_record(record, owner);
  barrier(fd, options.id, 0x7000 + owner);

  auto duration = std::chrono::milliseconds(options.stream_duration_ms);
  if (options.id == owner) {
    std::vector<uint8_t> payload(options.stream_payload_size);
    uint64_t updates = 0;
    uint64_t begin = now_ns();
    auto deadline = std::chrono::steady_clock::now() + duration;
    do {
      publish_stream_record(record, owner, ++updates, payload);
    } while (std::chrono::steady_clock::now() < deadline);
    uint64_t end = now_ns();
    send_all(fd, &updates, sizeof(updates));
    uint64_t stats[6]{};
    receive_all(fd, stats, sizeof(stats));
    print_average(options.id,
                  "owner_stream_publish_owner_" + std::to_string(owner),
                  end - begin, updates);
    bool pass =
        stats[0] != 0 && stats[2] == 0 && stats[3] == 0 && stats[5] == 1;
    std::cout << "CHECK host=" << options.id
              << " name=owner_stream_remote_read owner=" << owner
              << " updates=" << updates << " accepted=" << stats[0]
              << " retries=" << stats[1] << " corrupt=" << stats[2]
              << " regressions=" << stats[3] << " last_version=" << stats[4]
              << " final_seen=" << stats[5]
              << " status=" << (pass ? "PASS" : "FAIL") << std::endl;
    return pass;
  }

  std::vector<uint8_t> snapshot(options.stream_payload_size);
  uint64_t accepted = 0;
  uint64_t retries = 0;
  uint64_t corrupt = 0;
  uint64_t regressions = 0;
  uint64_t last_version = 0;
  uint64_t attempts = 0;
  uint64_t begin = now_ns();
  auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    uint64_t version = 0;
    auto result = read_remote_stream_snapshot(record, owner, snapshot, version);
    ++attempts;
    if (result == SnapshotResult::RETRY) {
      ++retries;
    } else if (result == SnapshotResult::CORRUPT) {
      ++corrupt;
    } else {
      if (version < last_version)
        ++regressions;
      last_version = std::max(last_version, version);
      ++accepted;
    }
  }
  uint64_t end = now_ns();

  uint64_t updates = 0;
  receive_all(fd, &updates, sizeof(updates));
  uint64_t final_version = updates * 2;
  uint64_t final_seen = 0;
  for (std::size_t i = 0; i < 10000 && final_seen == 0; ++i) {
    uint64_t version = 0;
    auto result = read_remote_stream_snapshot(record, owner, snapshot, version);
    if (result == SnapshotResult::CORRUPT)
      ++corrupt;
    if (result == SnapshotResult::VALID && version == final_version) {
      final_seen = 1;
      last_version = std::max(last_version, version);
    }
  }

  uint64_t stats[6] = {accepted,    retries,      corrupt,
                       regressions, last_version, final_seen};
  send_all(fd, stats, sizeof(stats));
  if (attempts != 0)
    print_average(options.id,
                  "remote_stream_poll_owner_" + std::to_string(owner),
                  end - begin, attempts);
  bool pass =
      accepted != 0 && corrupt == 0 && regressions == 0 && final_seen == 1;
  std::cout << "CHECK host=" << options.id
            << " name=remote_stream_snapshot owner=" << owner
            << " attempts=" << attempts << " accepted=" << accepted
            << " retries=" << retries << " corrupt=" << corrupt
            << " regressions=" << regressions
            << " last_version=" << last_version << " final_seen=" << final_seen
            << " status=" << (pass ? "PASS" : "FAIL") << std::endl;
  return pass;
}

bool run_owner_stream_remote_read(const Options &options,
                                  const std::vector<void *> &bases, int fd) {
  bool pass = run_stream_direction(options, bases, fd, 0);
  barrier(fd, options.id, 0x7100);
  pass &= run_stream_direction(options, bases, fd, 1);
  barrier(fd, options.id, 0x7101);
  return pass;
}

bool benchmark_remote_fetch_add(const Options &options,
                                const std::vector<void *> &bases, int fd,
                                int owner) {
  int remote = 1 - owner;
  auto *target = reinterpret_cast<uint64_t *>(
      static_cast<char *>(bases[owner]) + kRemoteFetchAddSlot);
  if (options.id == owner) {
    __atomic_store_n(target, 0, __ATOMIC_RELAXED);
    full_fence();
  }
  barrier(fd, options.id, 0x200 + owner);

  std::vector<uint64_t> samples;
  if (options.id == remote) {
    samples.reserve(options.atomic_iterations);
    for (std::size_t i = 0; i < options.atomic_iterations; ++i) {
      uint64_t begin = now_ns();
      invalidate_range(target, sizeof(*target));
      __atomic_fetch_add(target, 1, __ATOMIC_RELAXED);
      writeback_range(target, sizeof(*target));
      uint64_t end = now_ns();
      samples.push_back(end - begin);
    }
    uint64_t done = options.atomic_iterations;
    send_all(fd, &done, sizeof(done));
    uint64_t observed = 0;
    receive_all(fd, &observed, sizeof(observed));
    print_samples(options.id,
                  "remote_atomic_fetch_add_owner_" + std::to_string(owner) +
                      "_with_cache_maintenance",
                  std::move(samples));
    std::cout << "CHECK host=" << options.id
              << " name=sequential_remote_fetch_add owner=" << owner
              << " expected=" << options.atomic_iterations
              << " observed=" << observed << " status="
              << (observed == options.atomic_iterations ? "PASS" : "FAIL")
              << std::endl;
    return observed == options.atomic_iterations;
  } else {
    uint64_t done = 0;
    receive_all(fd, &done, sizeof(done));
    uint64_t observed = __atomic_load_n(target, __ATOMIC_RELAXED);
    send_all(fd, &observed, sizeof(observed));
    return observed == options.atomic_iterations;
  }
}

bool benchmark_remote_cas(const Options &options,
                          const std::vector<void *> &bases, int fd, int owner) {
  int remote = 1 - owner;
  auto *target = reinterpret_cast<uint64_t *>(
      static_cast<char *>(bases[owner]) + kRemoteCasSlot);
  if (options.id == owner) {
    __atomic_store_n(target, 0, __ATOMIC_RELAXED);
    full_fence();
  }
  barrier(fd, options.id, 0x300 + owner);

  if (options.id == remote) {
    std::vector<uint64_t> samples;
    samples.reserve(options.atomic_iterations);
    std::size_t failures = 0;
    for (std::size_t i = 0; i < options.atomic_iterations; ++i) {
      uint64_t expected = i;
      uint64_t begin = now_ns();
      invalidate_range(target, sizeof(*target));
      bool success = __atomic_compare_exchange_n(
          target, &expected, i + 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
      writeback_range(target, sizeof(*target));
      uint64_t end = now_ns();
      samples.push_back(end - begin);
      if (!success)
        ++failures;
    }
    uint64_t message[2] = {options.atomic_iterations, failures};
    send_all(fd, message, sizeof(message));
    uint64_t observed = 0;
    receive_all(fd, &observed, sizeof(observed));
    print_samples(options.id,
                  "remote_atomic_cas_owner_" + std::to_string(owner) +
                      "_with_cache_maintenance",
                  std::move(samples));
    bool pass = failures == 0 && observed == options.atomic_iterations;
    std::cout << "CHECK host=" << options.id
              << " name=sequential_remote_cas owner=" << owner
              << " failures=" << failures << " observed=" << observed
              << " status=" << (pass ? "PASS" : "FAIL") << std::endl;
    return pass;
  } else {
    uint64_t message[2]{};
    receive_all(fd, message, sizeof(message));
    uint64_t observed = __atomic_load_n(target, __ATOMIC_RELAXED);
    send_all(fd, &observed, sizeof(observed));
    return message[1] == 0 && observed == options.atomic_iterations;
  }
}

bool test_contended_cross_host_atomic(const Options &options,
                                      const std::vector<void *> &bases,
                                      int fd) {
  auto *target = reinterpret_cast<uint64_t *>(static_cast<char *>(bases[0]) +
                                              kContendedAtomicSlot);
  if (options.id == 0) {
    __atomic_store_n(target, 0, __ATOMIC_RELAXED);
    full_fence();
    volatile uint64_t preload = *target;
    (void)preload;
  }
  barrier(fd, options.id, 0x400);

  if (options.id == 0) {
    std::atomic<bool> stop(false);
    std::atomic<uint64_t> local_operations(0);
    std::thread owner_worker([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        __atomic_fetch_add(target, 1, __ATOMIC_RELAXED);
        local_operations.fetch_add(1, std::memory_order_relaxed);
      }
    });

    uint64_t remote_operations = 0;
    receive_all(fd, &remote_operations, sizeof(remote_operations));
    stop.store(true, std::memory_order_relaxed);
    owner_worker.join();
    full_fence();
    uint64_t local_count = local_operations.load(std::memory_order_relaxed);
    uint64_t observed = __atomic_load_n(target, __ATOMIC_RELAXED);
    uint64_t expected = local_count + remote_operations;
    uint64_t response[3] = {expected, observed, local_count};
    send_all(fd, response, sizeof(response));
    bool pass = observed == expected;
    std::cout << "CHECK host=0 name=contended_cross_host_fetch_add expected="
              << expected << " observed=" << observed
              << " local_ops=" << local_count
              << " remote_ops=" << remote_operations
              << " status=" << (pass ? "PASS" : "FAIL") << std::endl;
    return pass;
  }

  std::vector<uint64_t> samples;
  samples.reserve(options.atomic_iterations);
  for (std::size_t i = 0; i < options.atomic_iterations; ++i) {
    uint64_t begin = now_ns();
    invalidate_range(target, sizeof(*target));
    __atomic_fetch_add(target, 1, __ATOMIC_RELAXED);
    writeback_range(target, sizeof(*target));
    uint64_t end = now_ns();
    samples.push_back(end - begin);
  }
  uint64_t remote_operations = options.atomic_iterations;
  send_all(fd, &remote_operations, sizeof(remote_operations));
  uint64_t response[3]{};
  receive_all(fd, response, sizeof(response));
  print_samples(options.id, "contended_remote_atomic_fetch_add_owner_0",
                std::move(samples));
  bool pass = response[0] == response[1];
  std::cout << "CHECK host=1 name=contended_cross_host_fetch_add expected="
            << response[0] << " observed=" << response[1]
            << " owner_local_ops=" << response[2]
            << " remote_ops=" << remote_operations
            << " status=" << (pass ? "PASS" : "FAIL") << std::endl;
  return pass;
}

void unmap_regions(const Options &options, const std::vector<void *> &bases) {
  for (void *base : bases) {
    int result = 0;
    int call_result =
        MemlinkMemShmUnmmap(base, options.region_mb * kMiB, &result);
    if (call_result != 0 || result != 0)
      std::cerr << "WARN host=" << options.id
                << " MemlinkMemShmUnmmap failed call_result=" << call_result
                << " result=" << result << std::endl;
  }
}

} // namespace

int main(int argc, char **argv) {
  int fd = -1;
  try {
    Options options = parse_options(argc, argv);
    std::cout << "INFO host=" << options.id << " local_ip=" << options.local_ip
              << " peer_ip=" << options.peer_ip << " prefix=" << options.prefix
              << " test=" << options.test << " remote_atomics="
              << (options.test_remote_atomics ? "enabled" : "disabled")
              << std::endl;

    fd = open_control_connection(options);
    barrier(fd, options.id, 1);

    if (options.test == "connectivity" || options.test == "basic") {
      benchmark_tcp_rtt(fd, options.id, 0, options.tcp_iterations);
      benchmark_tcp_rtt(fd, options.id, 1, options.tcp_iterations);
      barrier(fd, options.id, 2);
    }

    if (options.test == "connectivity") {
      std::cout << "SUMMARY host=" << options.id
                << " test=connectivity status=PASS" << std::endl;
      close(fd);
      return 0;
    }

    // Establish and measure the ordinary network path first.  A
    // later Memlink failure can then be distinguished from an IP
    // connectivity problem.
    auto bases = initialize_regions(options);
    barrier(fd, options.id, 3);

    bool pass = true;
    int correctness_failure_exit_code = 1;
    if (options.test == "basic") {
      std::memset(static_cast<char *>(bases[options.id]) + kWorkingSetOffset, 0,
                  options.working_set_mb * kMiB);
      full_fence();
      benchmark_local_memory(options, bases[options.id]);
      benchmark_local_atomics(options, bases[options.id]);
      barrier(fd, options.id, 4);

      benchmark_owner_write_remote_read(options, bases, fd, 0);
      barrier(fd, options.id, 5);
      benchmark_owner_write_remote_read(options, bases, fd, 1);
      barrier(fd, options.id, 6);

      benchmark_remote_write_owner_read(options, bases, fd, 0);
      barrier(fd, options.id, 7);
      benchmark_remote_write_owner_read(options, bases, fd, 1);
      barrier(fd, options.id, 8);
    } else if (options.test == "size_visibility") {
      run_size_visibility(options, bases, fd);
    } else if (options.test == "owner_stream_remote_read") {
      pass &= run_owner_stream_remote_read(options, bases, fd);
    }

    if (options.test == "remote_atomic" ||
        (options.test == "basic" && options.test_remote_atomics)) {
      correctness_failure_exit_code = 2;
      std::cout << "WARN host=" << options.id
                << " remote CPU atomics are an experimental capability probe, "
                   "not an assumed UB guarantee"
                << std::endl;
      if (options.test == "remote_atomic")
        benchmark_local_atomics(options, bases[options.id]);
      pass &= benchmark_remote_fetch_add(options, bases, fd, 0);
      barrier(fd, options.id, 9);
      pass &= benchmark_remote_fetch_add(options, bases, fd, 1);
      barrier(fd, options.id, 10);
      pass &= benchmark_remote_cas(options, bases, fd, 0);
      barrier(fd, options.id, 11);
      pass &= benchmark_remote_cas(options, bases, fd, 1);
      barrier(fd, options.id, 12);
      pass &= test_contended_cross_host_atomic(options, bases, fd);
      barrier(fd, options.id, 13);
    }

    barrier(fd, options.id, 0x7fff);
    std::cout << "SUMMARY host=" << options.id << " test=" << options.test
              << " status=" << (pass ? "PASS" : "FAIL") << std::endl;
    close(fd);
    fd = -1;
    unmap_regions(options, bases);
    return pass ? 0 : correctness_failure_exit_code;
  } catch (const std::exception &error) {
    if (fd >= 0)
      close(fd);
    std::cerr << "ERROR " << error.what() << std::endl;
    return 1;
  }
}
