#include "ubsm_benchmark_common.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace {

constexpr uint64_t kMaxIterationsPerProcess = 5ULL * 1000ULL * 1000ULL;

struct PhaseResult {
  uint64_t operations;
  uint64_t cas_failures;
};

void send_all(int fd, const void *buffer, size_t length) {
  const auto *bytes = static_cast<const uint8_t *>(buffer);
  size_t sent_total = 0;
  while (sent_total < length) {
    const ssize_t sent =
        send(fd, bytes + sent_total, length - sent_total, MSG_NOSIGNAL);
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
    const ssize_t received =
        recv(fd, bytes + received_total, length - received_total, 0);
    if (received < 0 && errno == EINTR)
      continue;
    if (received <= 0)
      ubsm_test::fail(received < 0 ? ubsm_test::errno_message("recv")
                                   : "connection closed while receiving");
    received_total += static_cast<size_t>(received);
  }
}

std::vector<uint64_t> run_fetch_add(uint64_t *word, uint64_t iterations) {
  std::vector<uint64_t> tickets;
  tickets.reserve(static_cast<size_t>(iterations));
  for (uint64_t i = 0; i < iterations; ++i)
    tickets.push_back(__atomic_fetch_add(word, uint64_t{1}, __ATOMIC_SEQ_CST));
  return tickets;
}

std::vector<uint64_t> run_cas_increment(uint64_t *word, uint64_t iterations,
                                        uint64_t &cas_failures) {
  std::vector<uint64_t> tickets;
  tickets.reserve(static_cast<size_t>(iterations));
  cas_failures = 0;
  for (uint64_t i = 0; i < iterations; ++i) {
    uint64_t expected = __atomic_load_n(word, __ATOMIC_RELAXED);
    while (true) {
      const uint64_t desired = expected + 1;
      if (__atomic_compare_exchange_n(word, &expected, desired, false,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        tickets.push_back(desired - 1);
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

void send_phase_result(int connection, const std::vector<uint64_t> &tickets,
                       uint64_t cas_failures) {
  const PhaseResult result{static_cast<uint64_t>(tickets.size()), cas_failures};
  send_all(connection, &result, sizeof(result));
  send_all(connection, tickets.data(), tickets.size() * sizeof(uint64_t));
}

std::vector<uint64_t> receive_phase_result(int connection,
                                           uint64_t expected_operations,
                                           uint64_t &cas_failures) {
  PhaseResult result{};
  receive_all(connection, &result, sizeof(result));
  if (result.operations != expected_operations)
    ubsm_test::fail("remote phase operation count mismatch");
  std::vector<uint64_t> tickets(static_cast<size_t>(result.operations));
  receive_all(connection, tickets.data(), tickets.size() * sizeof(uint64_t));
  cas_failures = result.cas_failures;
  return tickets;
}

void verify_tickets(const std::string &phase, uint64_t *word,
                    uint64_t iterations,
                    const std::vector<uint64_t> &owner_tickets,
                    const std::vector<uint64_t> &remote_tickets) {
  const uint64_t expected_total = iterations * 2;
  const uint64_t actual = __atomic_load_n(word, __ATOMIC_SEQ_CST);
  if (actual != expected_total) {
    ubsm_test::fail(phase + " final value mismatch: expected=" +
                    std::to_string(expected_total) +
                    ", actual=" + std::to_string(actual));
  }

  std::vector<uint64_t> all_tickets;
  all_tickets.reserve(static_cast<size_t>(expected_total));
  all_tickets.insert(all_tickets.end(), owner_tickets.begin(),
                     owner_tickets.end());
  all_tickets.insert(all_tickets.end(), remote_tickets.begin(),
                     remote_tickets.end());
  if (all_tickets.size() != expected_total)
    ubsm_test::fail(phase + " collected ticket count mismatch");

  std::sort(all_tickets.begin(), all_tickets.end());
  for (uint64_t expected = 0; expected < expected_total; ++expected) {
    if (all_tickets[static_cast<size_t>(expected)] != expected) {
      ubsm_test::fail(
          phase + " ticket mismatch at index " + std::to_string(expected) +
          ": expected=" + std::to_string(expected) + ", actual=" +
          std::to_string(all_tickets[static_cast<size_t>(expected)]));
    }
  }
}

void prepare_owner_word(uint64_t *word) {
  __atomic_store_n(word, uint64_t{0}, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != 0)
    ubsm_test::fail("owner failed to initialize atomic word");
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

void start_owner_phase(int connection, char phase) {
  ubsm_test::send_stage(connection, phase);
  ubsm_test::expect_stage(connection, 'B');
}

void start_remote_phase(int connection, char phase) {
  ubsm_test::expect_stage(connection, phase);
  ubsm_test::send_stage(connection, 'B');
}

void run_owner(const ubsm_bench::TwoNodeOptions &options,
               ubsm_test::SharedMemory &memory, int connection) {
  uint64_t *fetch_add_word =
      ubsm_bench::atomic_word_at(memory, ubsm_bench::kRemoteFetchAddOffset);
  uint64_t *cas_word =
      ubsm_bench::atomic_word_at(memory, ubsm_bench::kRemoteCasOffset);
  prepare_owner_word(fetch_add_word);
  prepare_owner_word(cas_word);
  ubsm_test::expect_stage(connection, 'H');

  start_owner_phase(connection, 'F');
  std::vector<uint64_t> owner_fetch_tickets =
      run_fetch_add(fetch_add_word, options.atomic_iterations);
  uint64_t unused_failures = 0;
  std::vector<uint64_t> remote_fetch_tickets = receive_phase_result(
      connection, options.atomic_iterations, unused_failures);
  if (unused_failures != 0)
    ubsm_test::fail("remote reported CAS failures during fetch-add phase");
  verify_tickets("contended fetch-add", fetch_add_word,
                 options.atomic_iterations, owner_fetch_tickets,
                 remote_fetch_tickets);
  std::cout << "PASS owner cacheable and remote NC fetch-add claimed "
            << options.atomic_iterations * 2 << " unique tickets\n";
  ubsm_test::send_stage(connection, 'f');

  prepare_owner_word(cas_word);
  start_owner_phase(connection, 'C');
  uint64_t owner_cas_failures = 0;
  std::vector<uint64_t> owner_cas_tickets = run_cas_increment(
      cas_word, options.atomic_iterations, owner_cas_failures);
  uint64_t remote_cas_failures = 0;
  std::vector<uint64_t> remote_cas_tickets = receive_phase_result(
      connection, options.atomic_iterations, remote_cas_failures);
  verify_tickets("contended CAS", cas_word, options.atomic_iterations,
                 owner_cas_tickets, remote_cas_tickets);
  const uint64_t total_failures = owner_cas_failures + remote_cas_failures;
  if (total_failures == 0) {
    ubsm_test::fail(
        "no CAS conflict was observed; increase --atomic-iterations");
  }
  std::cout << "PASS owner cacheable and remote NC CAS claimed "
            << options.atomic_iterations * 2
            << " unique tickets; owner_cas_failures=" << owner_cas_failures
            << " remote_cas_failures=" << remote_cas_failures << '\n';
  ubsm_test::send_stage(connection, 'c');

  ubsm_test::send_stage(connection, 'V');
  ubsm_bench::owner_cleanup(memory, connection);
}

void run_remote(const ubsm_bench::TwoNodeOptions &options,
                ubsm_test::SharedMemory &memory, int connection) {
  memory.map();
  uint64_t *fetch_add_word =
      ubsm_bench::atomic_word_at(memory, ubsm_bench::kRemoteFetchAddOffset);
  uint64_t *cas_word =
      ubsm_bench::atomic_word_at(memory, ubsm_bench::kRemoteCasOffset);
  ubsm_test::send_stage(connection, 'H');

  start_remote_phase(connection, 'F');
  const std::vector<uint64_t> fetch_tickets =
      run_fetch_add(fetch_add_word, options.atomic_iterations);
  send_phase_result(connection, fetch_tickets, 0);
  ubsm_test::expect_stage(connection, 'f');

  start_remote_phase(connection, 'C');
  uint64_t cas_failures = 0;
  const std::vector<uint64_t> cas_tickets =
      run_cas_increment(cas_word, options.atomic_iterations, cas_failures);
  send_phase_result(connection, cas_tickets, cas_failures);
  ubsm_test::expect_stage(connection, 'c');

  ubsm_test::expect_stage(connection, 'V');
  ubsm_bench::remote_cleanup(memory, connection);
  std::cout << "PASS remote NC atomic contention phases completed\n";
}

} // namespace

int main(int argc, char **argv) {
  int connection = -1;
  try {
    const auto options = ubsm_bench::parse_two_node_options(
        argc, argv, "ubsm_owner_remote_atomic", 18537);
    if (options.atomic_iterations > kMaxIterationsPerProcess)
      ubsm_test::fail("--atomic-iterations must not exceed 5000000");

    const uint64_t region_bytes =
        ubsm_test::region_bytes_from_mb(options.region_mb);
    ubsm_test::SdkSession session;
    ubsm_test::SharedMemory memory(options.name, region_bytes);
    if (options.role == "owner")
      ubsm_bench::allocate_owner(memory, options);
    connection = ubsm_bench::open_connection(options);

    if (options.role == "owner")
      run_owner(options, memory, connection);
    else
      run_remote(options, memory, connection);

    close(connection);
    connection = -1;
    session.finalize();
    std::cout << "PASS owner/remote atomic contention test " << options.role
              << " and cleanup\n";
    return 0;
  } catch (const std::exception &error) {
    if (connection >= 0)
      close(connection);
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
