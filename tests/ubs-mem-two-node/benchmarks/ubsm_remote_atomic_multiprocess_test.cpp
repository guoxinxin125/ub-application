#include "ubsm_test_common.h"

#include <algorithm>
#include <array>
#include <climits>
#include <new>
#include <vector>

namespace {

constexpr uint64_t kStateMagic = 0x5542534d41544d50ULL; // "UBSMATMP"
constexpr uint32_t kMaxProducers = 64;
constexpr uint64_t kMaxCollectedTickets = 10ULL * 1000ULL * 1000ULL;

struct alignas(64) ProducerStats {
  uint64_t completed;
  uint64_t cas_failures;
  uint8_t padding[48];
};

struct alignas(64) SharedState {
  uint64_t magic;
  uint64_t producer_count;
  uint64_t iterations;
  uint8_t header_padding[40];

  alignas(64) uint64_t start;
  uint8_t start_padding[56];

  alignas(64) uint64_t tail;
  uint8_t tail_padding[56];

  ProducerStats stats[kMaxProducers];
};

static_assert(sizeof(ProducerStats) == 64,
              "producer statistics must occupy one cache line");
static_assert(alignof(SharedState) == 64,
              "shared state must be cache-line aligned");

struct Options {
  std::string role;
  std::string bind_ip;
  std::string owner_ip;
  std::string provider_host;
  std::string name = "ubsm_atomic_mp";
  uint16_t port = 18535;
  uint64_t region_mb = 4;
  uint64_t iterations = 100000;
  uint32_t producer_count = 2;
  uint32_t producer_id = UINT32_MAX;
  int timeout_sec = 120;
  uint32_t provider_socket = UINT32_MAX;
  uint32_t provider_numa = UINT32_MAX;
  uint32_t provider_port = UINT32_MAX;
};

struct HelloMessage {
  uint32_t producer_id;
  uint32_t producer_count;
  uint64_t iterations;
};

struct ResultMessage {
  uint32_t producer_id;
  uint32_t reserved;
  uint64_t operations;
  uint64_t cas_failures;
};

struct ProducerConnection {
  int fd = -1;
  uint32_t producer_id = UINT32_MAX;
};

void usage(const char *program) {
  std::cerr
      << "Usage: " << program
      << " --role owner --bind-ip IP --producer-count N [options]\n"
      << "       " << program
      << " --role remote --owner-ip IP --producer-count N --producer-id N"
         " [options]\n"
      << "Options:\n"
      << "  --name NAME --port N --region-mb N --iterations N\n"
      << "  --timeout-sec N --provider-host HOST --provider-socket N\n"
      << "  --provider-numa N --provider-port N\n";
}

Options parse_options(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      std::exit(0);
    }
    if (i + 1 >= argc)
      ubsm_test::fail("missing value for " + arg);
    const std::string value(argv[++i]);
    if (arg == "--role")
      options.role = value;
    else if (arg == "--bind-ip")
      options.bind_ip = value;
    else if (arg == "--owner-ip")
      options.owner_ip = value;
    else if (arg == "--provider-host")
      options.provider_host = value;
    else if (arg == "--name")
      options.name = value;
    else if (arg == "--port")
      options.port = ubsm_test::parse_port(value);
    else if (arg == "--region-mb")
      options.region_mb = ubsm_test::parse_u64(value, arg);
    else if (arg == "--iterations")
      options.iterations = ubsm_test::parse_u64(value, arg);
    else if (arg == "--producer-count")
      options.producer_count = ubsm_test::parse_u32(value, arg);
    else if (arg == "--producer-id")
      options.producer_id = ubsm_test::parse_u32(value, arg);
    else if (arg == "--timeout-sec")
      options.timeout_sec = ubsm_test::parse_positive_int(value, arg);
    else if (arg == "--provider-socket")
      options.provider_socket = ubsm_test::parse_u32(value, arg);
    else if (arg == "--provider-numa")
      options.provider_numa = ubsm_test::parse_u32(value, arg);
    else if (arg == "--provider-port")
      options.provider_port = ubsm_test::parse_u32(value, arg);
    else
      ubsm_test::fail("unknown option: " + arg);
  }

  if (options.role != "owner" && options.role != "remote")
    ubsm_test::fail("--role must be owner or remote");
  if (options.role == "owner" && options.bind_ip.empty())
    ubsm_test::fail("owner role requires --bind-ip");
  if (options.role == "remote" && options.owner_ip.empty())
    ubsm_test::fail("remote role requires --owner-ip");
  if (options.producer_count < 2 || options.producer_count > kMaxProducers)
    ubsm_test::fail("--producer-count must be in [2, 64]");
  if (options.role == "remote" &&
      options.producer_id >= options.producer_count)
    ubsm_test::fail("remote --producer-id must be smaller than producer count");
  if (options.iterations == 0)
    ubsm_test::fail("--iterations must be greater than zero");
  if (options.iterations >
      std::numeric_limits<uint64_t>::max() / options.producer_count)
    ubsm_test::fail("producer count times iterations overflows uint64_t");
  if (options.iterations * options.producer_count > kMaxCollectedTickets)
    ubsm_test::fail("total ticket count exceeds 10000000; reduce iterations");
  ubsm_test::validate_name(options.name);

  const uint64_t region_bytes =
      ubsm_test::region_bytes_from_mb(options.region_mb);
  if (region_bytes < sizeof(SharedState))
    ubsm_test::fail("shared memory region is too small for atomic test state");

  if (options.role == "owner" && options.provider_host.empty()) {
    char hostname[MAX_HOST_NAME_DESC_LENGTH]{};
    if (gethostname(hostname, sizeof(hostname)) != 0)
      ubsm_test::fail(ubsm_test::errno_message("gethostname"));
    if (hostname[sizeof(hostname) - 1] != '\0')
      ubsm_test::fail("local hostname is too long");
    options.provider_host = hostname;
  }
  return options;
}

SharedState *shared_state(ubsm_test::SharedMemory &memory) {
  auto *base = const_cast<uint8_t *>(memory.bytes());
  if (reinterpret_cast<uintptr_t>(base) % alignof(SharedState) != 0)
    ubsm_test::fail("mapped shared memory is not 64-byte aligned");
  return reinterpret_cast<SharedState *>(base);
}

void send_all(int fd, const void *buffer, size_t length) {
  const auto *bytes = static_cast<const uint8_t *>(buffer);
  size_t sent_total = 0;
  while (sent_total < length) {
    ssize_t sent = send(fd, bytes + sent_total, length - sent_total,
                        MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent <= 0)
      ubsm_test::fail(sent < 0 ? ubsm_test::errno_message("send")
                               : "connection closed while sending");
    sent_total += static_cast<size_t>(sent);
  }
}

void receive_all(int fd, void *buffer, size_t length) {
  auto *bytes = static_cast<uint8_t *>(buffer);
  size_t received_total = 0;
  while (received_total < length) {
    ssize_t received = recv(fd, bytes + received_total,
                            length - received_total, 0);
    if (received < 0 && errno == EINTR)
      continue;
    if (received <= 0)
      ubsm_test::fail(received < 0 ? ubsm_test::errno_message("recv")
                                   : "connection closed while receiving");
    received_total += static_cast<size_t>(received);
  }
}

int create_listener(const Options &options) {
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0)
    ubsm_test::fail(ubsm_test::errno_message("socket"));
  int reuse = 1;
  (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  const sockaddr_in address =
      ubsm_test::make_address(options.bind_ip, options.port);
  if (bind(listener, reinterpret_cast<const sockaddr *>(&address),
           sizeof(address)) != 0) {
    const std::string message = ubsm_test::errno_message("bind");
    close(listener);
    ubsm_test::fail(message);
  }
  if (listen(listener, static_cast<int>(options.producer_count)) != 0) {
    const std::string message = ubsm_test::errno_message("listen");
    close(listener);
    ubsm_test::fail(message);
  }
  return listener;
}

int accept_producer(int listener, int timeout_sec) {
  int fd;
  do {
    fd = accept(listener, nullptr, nullptr);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0)
    ubsm_test::fail(ubsm_test::errno_message("accept"));
  ubsm_test::configure_socket_timeout(fd, timeout_sec);
  return fd;
}

std::vector<uint64_t> claim_tickets(SharedState *state,
                                    uint64_t iterations,
                                    uint64_t &cas_failures) {
  std::vector<uint64_t> tickets;
  tickets.reserve(static_cast<size_t>(iterations));

  while (__atomic_load_n(&state->start, __ATOMIC_ACQUIRE) == 0) {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
  }

  cas_failures = 0;
  for (uint64_t i = 0; i < iterations; ++i) {
    uint64_t expected = __atomic_load_n(&state->tail, __ATOMIC_RELAXED);
    while (true) {
      const uint64_t desired = expected + 1;
      if (__atomic_compare_exchange_n(&state->tail, &expected, desired, true,
                                      __ATOMIC_RELEASE,
                                      __ATOMIC_RELAXED)) {
        tickets.push_back(expected);
        break;
      }
      ++cas_failures;
#if defined(__aarch64__)
      asm volatile("yield" ::: "memory");
#else
      std::this_thread::yield();
#endif
    }
  }
  return tickets;
}

void initialize_owner_state(SharedState *state, const Options &options) {
  std::memset(state, 0, sizeof(*state));
  __atomic_store_n(&state->producer_count, options.producer_count,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&state->iterations, options.iterations, __ATOMIC_RELAXED);
  __atomic_store_n(&state->start, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&state->tail, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&state->magic, kStateMagic, __ATOMIC_RELEASE);
}

void validate_remote_state(SharedState *state, const Options &options) {
  if (__atomic_load_n(&state->magic, __ATOMIC_ACQUIRE) != kStateMagic)
    ubsm_test::fail("shared atomic test state is not initialized");
  if (__atomic_load_n(&state->producer_count, __ATOMIC_ACQUIRE) !=
      options.producer_count)
    ubsm_test::fail("producer count differs from owner configuration");
  if (__atomic_load_n(&state->iterations, __ATOMIC_ACQUIRE) !=
      options.iterations)
    ubsm_test::fail("iteration count differs from owner configuration");
}

void verify_results(SharedState *state, const Options &options,
                    std::vector<uint64_t> &tickets,
                    uint64_t reported_failures) {
  const uint64_t expected_total =
      options.iterations * options.producer_count;
  const uint64_t actual_tail =
      __atomic_load_n(&state->tail, __ATOMIC_ACQUIRE);
  if (actual_tail != expected_total) {
    ubsm_test::fail("contended CAS tail mismatch: expected=" +
                    std::to_string(expected_total) + ", actual=" +
                    std::to_string(actual_tail));
  }
  if (tickets.size() != expected_total)
    ubsm_test::fail("collected ticket count mismatch");

  std::sort(tickets.begin(), tickets.end());
  for (uint64_t i = 0; i < expected_total; ++i) {
    if (tickets[static_cast<size_t>(i)] != i) {
      ubsm_test::fail("ticket mismatch at index " + std::to_string(i) +
                      ": expected=" + std::to_string(i) + ", actual=" +
                      std::to_string(tickets[static_cast<size_t>(i)]));
    }
  }

  uint64_t shared_failures = 0;
  for (uint32_t id = 0; id < options.producer_count; ++id) {
    const uint64_t completed =
        __atomic_load_n(&state->stats[id].completed, __ATOMIC_ACQUIRE);
    if (completed != options.iterations)
      ubsm_test::fail("producer " + std::to_string(id) +
                      " completion count mismatch");
    shared_failures += __atomic_load_n(&state->stats[id].cas_failures,
                                       __ATOMIC_ACQUIRE);
  }
  if (shared_failures != reported_failures)
    ubsm_test::fail("CAS failure statistics differ between shared memory and TCP results");
  if (reported_failures == 0)
    ubsm_test::fail("no CAS conflict was observed; increase --iterations");

  std::cout << "PASS " << options.producer_count
            << " remote processes claimed " << expected_total
            << " unique CAS tickets; cas_failures=" << reported_failures
            << '\n';
}

void run_owner(const Options &options, ubsm_test::SharedMemory &memory) {
  SharedState *state = shared_state(memory);
  initialize_owner_state(state, options);

  int listener = create_listener(options);
  std::vector<ProducerConnection> producers;
  producers.reserve(options.producer_count);
  std::array<bool, kMaxProducers> seen{};

  try {
    std::cout << "waiting for " << options.producer_count
              << " remote producer processes on " << options.bind_ip << ':'
              << options.port << std::endl;
    for (uint32_t i = 0; i < options.producer_count; ++i) {
      const int fd = accept_producer(listener, options.timeout_sec);
      HelloMessage hello{};
      receive_all(fd, &hello, sizeof(hello));
      if (hello.producer_count != options.producer_count ||
          hello.iterations != options.iterations ||
          hello.producer_id >= options.producer_count ||
          seen[hello.producer_id]) {
        close(fd);
        ubsm_test::fail("invalid or duplicate remote producer registration");
      }
      seen[hello.producer_id] = true;
      ProducerConnection producer;
      producer.fd = fd;
      producer.producer_id = hello.producer_id;
      producers.push_back(producer);
    }
    close(listener);
    listener = -1;

    // One shared release starts every independently mapped remote process.
    __atomic_store_n(&state->start, 1, __ATOMIC_RELEASE);

    const uint64_t expected_total =
        options.iterations * options.producer_count;
    std::vector<uint64_t> all_tickets;
    all_tickets.reserve(static_cast<size_t>(expected_total));
    uint64_t total_failures = 0;

    for (const ProducerConnection &producer : producers) {
      ResultMessage result{};
      receive_all(producer.fd, &result, sizeof(result));
      if (result.producer_id != producer.producer_id ||
          result.operations != options.iterations)
        ubsm_test::fail("invalid remote producer result header");
      std::vector<uint64_t> tickets(static_cast<size_t>(result.operations));
      receive_all(producer.fd, tickets.data(),
                  tickets.size() * sizeof(uint64_t));
      all_tickets.insert(all_tickets.end(), tickets.begin(), tickets.end());
      total_failures += result.cas_failures;
    }

    verify_results(state, options, all_tickets, total_failures);

    for (const ProducerConnection &producer : producers)
      ubsm_test::send_stage(producer.fd, 'V');
    for (const ProducerConnection &producer : producers)
      ubsm_test::expect_stage(producer.fd, 'U');
    for (const ProducerConnection &producer : producers)
      close(producer.fd);

    memory.unmap();
    memory.deallocate();
  } catch (...) {
    if (listener >= 0)
      close(listener);
    for (const ProducerConnection &producer : producers) {
      if (producer.fd >= 0)
        close(producer.fd);
    }
    throw;
  }
}

void run_remote(const Options &options, ubsm_test::SharedMemory &memory) {
  memory.map();
  SharedState *state = shared_state(memory);

  const int connection = ubsm_test::connect_to_owner(
      options.owner_ip, options.port, options.timeout_sec);
  try {
    // The owner starts listening only after publishing the initialized state.
    validate_remote_state(state, options);
    const HelloMessage hello{options.producer_id, options.producer_count,
                             options.iterations};
    send_all(connection, &hello, sizeof(hello));

    uint64_t cas_failures = 0;
    std::vector<uint64_t> tickets =
        claim_tickets(state, options.iterations, cas_failures);

    __atomic_store_n(&state->stats[options.producer_id].cas_failures,
                     cas_failures, __ATOMIC_RELAXED);
    __atomic_store_n(&state->stats[options.producer_id].completed,
                     options.iterations, __ATOMIC_RELEASE);

    const ResultMessage result{options.producer_id, 0, options.iterations,
                               cas_failures};
    send_all(connection, &result, sizeof(result));
    send_all(connection, tickets.data(), tickets.size() * sizeof(uint64_t));

    ubsm_test::expect_stage(connection, 'V');
    memory.unmap();
    ubsm_test::send_stage(connection, 'U');
    close(connection);
    std::cout << "PASS remote producer " << options.producer_id
              << " operations=" << options.iterations
              << " cas_failures=" << cas_failures << '\n';
  } catch (...) {
    close(connection);
    throw;
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parse_options(argc, argv);
    const uint64_t region_bytes =
        ubsm_test::region_bytes_from_mb(options.region_mb);
    ubsm_test::SdkSession session;
    ubsm_test::SharedMemory memory(options.name, region_bytes);

    if (options.role == "owner") {
      memory.allocate_with_provider(
          options.provider_host, options.provider_socket,
          options.provider_numa, options.provider_port);
      memory.map();
      run_owner(options, memory);
    } else {
      run_remote(options, memory);
    }

    session.finalize();
    std::cout << "PASS remote atomic multiprocess test " << options.role
              << " and cleanup\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
