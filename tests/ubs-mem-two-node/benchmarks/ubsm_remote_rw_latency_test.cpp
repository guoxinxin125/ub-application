#include "ubsm_benchmark_common.h"

namespace {

void run_owner(const ubsm_bench::TwoNodeOptions &options,
               ubsm_test::SharedMemory &memory, int connection) {
  volatile uint64_t *word =
      ubsm_bench::word_at(memory, ubsm_bench::kLatencyOffset);
  volatile uint64_t *cacheline =
      ubsm_bench::word_at(memory, ubsm_bench::kCachelineLatencyOffset);
  constexpr uint64_t kCachelineSequence = 0x123456789abcdef0ULL;
  ubsm_bench::write_word_block<1>(word, ubsm_bench::kInitialLatencyValue);
  ubsm_bench::write_word_block<8>(cacheline, kCachelineSequence);

  ubsm_test::expect_stage(connection, 'H');
  ubsm_test::send_stage(connection, 'R');
  ubsm_test::expect_stage(connection, 'D');
  ubsm_bench::verify_word_block<1>(
      word, options.iterations - 1,
      "remote 8-byte fenced store observed by owner");
  ubsm_bench::verify_word_block<8>(
      cacheline, options.iterations - 1,
      "remote 64-byte fenced store observed by owner");
  std::cout << "PASS remote final 8-byte and 64-byte stores observed by owner "
               "without invalidate\n";
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
  volatile uint64_t *cacheline =
      ubsm_bench::word_at(memory, ubsm_bench::kCachelineLatencyOffset);
  constexpr uint64_t kCachelineSequence = 0x123456789abcdef0ULL;
  ubsm_test::print_latency(
      "remote_nc_load_8b_avg_ns",
      ubsm_bench::benchmark_word_block_load<1>(
          word, options.iterations, ubsm_bench::kInitialLatencyValue));
  ubsm_test::print_latency(
      "remote_nc_load_16b_avg_ns",
      ubsm_bench::benchmark_word_block_load<2>(
          cacheline, options.iterations, kCachelineSequence));
  ubsm_test::print_latency(
      "remote_nc_load_32b_avg_ns",
      ubsm_bench::benchmark_word_block_load<4>(
          cacheline, options.iterations, kCachelineSequence));
  ubsm_test::print_latency(
      "remote_nc_load_64b_avg_ns",
      ubsm_bench::benchmark_word_block_load<8>(
          cacheline, options.iterations, kCachelineSequence));

  ubsm_test::print_latency(
      "remote_nc_store_issue_8b_avg_ns",
      ubsm_bench::benchmark_word_block_store_issue<1>(word,
                                                       options.iterations));
  ubsm_test::print_latency(
      "remote_nc_store_issue_16b_avg_ns",
      ubsm_bench::benchmark_word_block_store_issue<2>(cacheline,
                                                       options.iterations));
  ubsm_test::print_latency(
      "remote_nc_store_issue_32b_avg_ns",
      ubsm_bench::benchmark_word_block_store_issue<4>(cacheline,
                                                       options.iterations));
  ubsm_test::print_latency(
      "remote_nc_store_issue_64b_avg_ns",
      ubsm_bench::benchmark_word_block_store_issue<8>(cacheline,
                                                       options.iterations));

  ubsm_test::print_latency(
      "remote_nc_store_fenced_8b_avg_ns",
      ubsm_bench::benchmark_word_block_store_fenced<1>(word,
                                                        options.iterations));
  ubsm_test::print_latency(
      "remote_nc_store_fenced_16b_avg_ns",
      ubsm_bench::benchmark_word_block_store_fenced<2>(cacheline,
                                                        options.iterations));
  ubsm_test::print_latency(
      "remote_nc_store_fenced_32b_avg_ns",
      ubsm_bench::benchmark_word_block_store_fenced<4>(cacheline,
                                                        options.iterations));
  ubsm_test::print_latency(
      "remote_nc_store_fenced_64b_avg_ns",
      ubsm_bench::benchmark_word_block_store_fenced<8>(cacheline,
                                                        options.iterations));
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
