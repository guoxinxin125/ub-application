#include "ubsm_benchmark_common.h"

namespace {

constexpr uint8_t kOldSeed = 0x28;
constexpr uint8_t kNewSeed = 0xc7;
constexpr uint64_t kLatencySeed = 0x6a09e667f3bcc909ULL;

uint64_t latency_value(uint64_t sequence) { return kLatencySeed ^ sequence; }

double timed_load_fenced(volatile uint64_t *word, uint64_t expected) {
  const auto start = std::chrono::steady_clock::now();
  const uint64_t actual = *word;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (actual != expected) {
    ubsm_test::fail(
        "owner timed load mismatch: expected=" + std::to_string(expected) +
        ", actual=" + std::to_string(actual));
  }
  return std::chrono::duration<double, std::nano>(elapsed).count();
}

void run_owner(const ubsm_bench::TwoNodeOptions &options,
               ubsm_test::SharedMemory &memory, int connection) {
  volatile uint64_t *latency_word =
      ubsm_bench::word_at(memory, ubsm_bench::kLatencyOffset);
  ubsm_test::write_pattern(ubsm_bench::pattern_at(memory), options.test_bytes,
                           kOldSeed);
  ubsm_test::verify_pattern(ubsm_bench::pattern_at(memory), options.test_bytes,
                            kOldSeed, "owner initial cached pattern");
  *latency_word = latency_value(0);
  std::atomic_thread_fence(std::memory_order_seq_cst);

  ubsm_test::expect_stage(connection, 'H');
  ubsm_test::send_stage(connection, 'R');
  ubsm_test::expect_stage(connection, 'D');

  // Owner deliberately performs no invalidate before reading.
  ubsm_test::verify_pattern(ubsm_bench::pattern_at(memory), options.test_bytes,
                            kNewSeed, "remote NC store observed by owner");
  std::cout << "PASS remote NC store -> owner load without invalidate\n";

  double load_total_ns = 0.0;
  for (uint64_t i = 0; i < options.iterations; ++i) {
    const uint64_t sequence = i + 1;
    ubsm_test::send_stage(connection, 'L');
    ubsm_test::expect_stage(connection, 'D');
    load_total_ns += timed_load_fenced(latency_word, latency_value(sequence));
  }
  ubsm_test::print_latency("owner_cc_load_after_remote_store_fenced_8b_avg_ns",
                           load_total_ns /
                               static_cast<double>(options.iterations));

  ubsm_test::send_stage(connection, 'V');
  ubsm_bench::owner_cleanup(memory, connection);
}

void run_remote(const ubsm_bench::TwoNodeOptions &options,
                ubsm_test::SharedMemory &memory, int connection) {
  memory.map();
  volatile uint64_t *latency_word =
      ubsm_bench::word_at(memory, ubsm_bench::kLatencyOffset);
  ubsm_test::send_stage(connection, 'H');
  ubsm_test::expect_stage(connection, 'R');

  // NC mapping has no cache to write back. DSB waits for store completion.
  ubsm_test::write_pattern(ubsm_bench::pattern_at(memory), options.test_bytes,
                           kNewSeed);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  ubsm_test::send_stage(connection, 'D');

  for (uint64_t i = 0; i < options.iterations; ++i) {
    const uint64_t sequence = i + 1;
    ubsm_test::expect_stage(connection, 'L');
    *latency_word = latency_value(sequence);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ubsm_test::send_stage(connection, 'D');
  }

  ubsm_test::expect_stage(connection, 'V');
  std::cout << "PASS remote NC write and completion fence\n";
  ubsm_bench::remote_cleanup(memory, connection);
}

} // namespace

int main(int argc, char **argv) {
  int connection = -1;
  try {
    const auto options = ubsm_bench::parse_two_node_options(
        argc, argv, "ubsm_remote_to_owner_cc", 18534);
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
    std::cout << "PASS remote-to-owner CC test " << options.role
              << " and cleanup\n";
    return 0;
  } catch (const std::exception &error) {
    if (connection >= 0)
      close(connection);
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
