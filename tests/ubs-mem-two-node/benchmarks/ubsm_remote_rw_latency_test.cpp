#include "ubsm_benchmark_common.h"

namespace {

void run_owner(const ubsm_bench::TwoNodeOptions &options,
               ubsm_test::SharedMemory &memory, int connection) {
  volatile uint64_t *word =
      ubsm_bench::word_at(memory, ubsm_bench::kLatencyOffset);
  *word = ubsm_bench::kInitialLatencyValue;
  ubsm_bench::write_completion_fence();

  ubsm_test::expect_stage(connection, 'H');
  ubsm_test::send_stage(connection, 'R');
  ubsm_test::expect_stage(connection, 'D');
  const uint64_t actual = *word;
  const uint64_t expected = options.iterations - 1;
  if (actual != expected) {
    ubsm_test::fail("remote store final value mismatch: expected=" +
                    std::to_string(expected) +
                    ", actual=" + std::to_string(actual));
  }
  std::cout << "PASS remote store observed by owner without invalidate\n";
  ubsm_test::send_stage(connection, 'V');
  ubsm_bench::owner_cleanup(memory, connection);
}

void run_remote(const ubsm_bench::TwoNodeOptions &options,
                ubsm_test::SharedMemory &memory, int connection) {
  memory.map();
  ubsm_test::send_stage(connection, 'H');
  ubsm_test::expect_stage(connection, 'R');
  volatile uint64_t *word =
      ubsm_bench::word_at(memory, ubsm_bench::kLatencyOffset);
  if (*word != ubsm_bench::kInitialLatencyValue)
    ubsm_test::fail("remote load did not observe owner's initial value");

  ubsm_test::print_latency(
      "remote_nc_load_8b_avg_ns",
      ubsm_bench::benchmark_load_8b(word, options.iterations));
  ubsm_test::print_latency(
      "remote_nc_store_issue_8b_avg_ns",
      ubsm_bench::benchmark_store_issue_8b(word, options.iterations));
  ubsm_test::print_latency(
      "remote_nc_store_fenced_8b_avg_ns",
      ubsm_bench::benchmark_store_fenced_8b(word, options.iterations));
  ubsm_test::send_stage(connection, 'D');
  ubsm_test::expect_stage(connection, 'V');
  ubsm_bench::remote_cleanup(memory, connection);
}

} // namespace

int main(int argc, char **argv) {
  int connection = -1;
  try {
    const auto options = ubsm_bench::parse_two_node_options(
        argc, argv, "ubsm_remote_rw_latency", 18531);
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
    std::cout << "PASS remote read/write latency test " << options.role
              << " and cleanup\n";
    return 0;
  } catch (const std::exception &error) {
    if (connection >= 0)
      close(connection);
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
