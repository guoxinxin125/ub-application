#include "ubsm_benchmark_common.h"

#include <algorithm>
#include <vector>

namespace {

constexpr uint16_t kDefaultPort = 18536;
constexpr uint32_t kDefaultMaxThreads = 8;
constexpr uint32_t kMaxThreads = 256;
constexpr uint64_t kLoadSeed = 0x243f6a8885a308d3ULL;

struct Options {
  ubsm_bench::TwoNodeOptions common;
  uint32_t max_threads = kDefaultMaxThreads;
};

struct ParallelMetrics {
  double mean_thread_op_ns;
  double aggregate_mops;
};

uint64_t load_value(uint32_t thread_count) {
  return kLoadSeed ^ (static_cast<uint64_t>(thread_count) << 32U);
}

std::vector<uint32_t> make_thread_counts(uint32_t max_threads) {
  std::vector<uint32_t> counts;
  for (uint32_t count = 1; count < max_threads;) {
    counts.push_back(count);
    if (count > max_threads / 2)
      break;
    count *= 2;
  }
  if (counts.empty() || counts.back() != max_threads)
    counts.push_back(max_threads);
  return counts;
}

Options parse_options(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      ubsm_bench::two_node_usage(argv[0], kDefaultPort);
      std::cerr << "         --max-threads N(default " << kDefaultMaxThreads
                << ", maximum " << kMaxThreads << ")\n";
      std::exit(0);
    }
  }

  Options options;
  std::vector<char *> common_argv;
  common_argv.reserve(static_cast<size_t>(argc));
  common_argv.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--max-threads") {
      if (i + 1 >= argc)
        ubsm_test::fail("missing value for --max-threads");
      options.max_threads = ubsm_test::parse_u32(argv[++i], arg);
    } else {
      common_argv.push_back(argv[i]);
    }
  }
  options.common = ubsm_bench::parse_two_node_options(
      static_cast<int>(common_argv.size()), common_argv.data(),
      "ubsm_remote_multithread_load", kDefaultPort);

  if (options.max_threads == 0 || options.max_threads > kMaxThreads)
    ubsm_test::fail("--max-threads must be in [1, 256]");
  return options;
}

ParallelMetrics benchmark_shared_word_load(volatile uint64_t *word,
                                           uint32_t thread_count,
                                           uint64_t iterations,
                                           uint64_t expected) {
  using Clock = std::chrono::steady_clock;
  std::atomic<uint32_t> ready_threads{0};
  std::atomic<bool> start{false};
  std::vector<Clock::time_point> starts(thread_count);
  std::vector<Clock::time_point> ends(thread_count);
  std::vector<uint64_t> sums(thread_count, 0);
  std::vector<std::thread> workers;
  workers.reserve(thread_count);

  for (uint32_t thread_id = 0; thread_id < thread_count; ++thread_id) {
    workers.emplace_back([&, thread_id]() {
      ready_threads.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();

      uint64_t sum = 0;
      starts[thread_id] = Clock::now();
      for (uint64_t i = 0; i < iterations; ++i) {
        sum += *word;
        std::atomic_thread_fence(std::memory_order_seq_cst);
      }
      ends[thread_id] = Clock::now();
      sums[thread_id] = sum;
    });
  }

  while (ready_threads.load(std::memory_order_acquire) != thread_count)
    std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (std::thread &worker : workers)
    worker.join();

  const uint64_t expected_sum = expected * iterations;
  for (uint32_t thread_id = 0; thread_id < thread_count; ++thread_id) {
    if (sums[thread_id] != expected_sum) {
      ubsm_test::fail("thread " + std::to_string(thread_id) +
                      " observed an unexpected shared load value");
    }
  }

  Clock::time_point earliest = starts[0];
  Clock::time_point latest = ends[0];
  double total_thread_ns = 0.0;
  for (uint32_t thread_id = 0; thread_id < thread_count; ++thread_id) {
    earliest = std::min(earliest, starts[thread_id]);
    latest = std::max(latest, ends[thread_id]);
    total_thread_ns += std::chrono::duration<double, std::nano>(
                           ends[thread_id] - starts[thread_id])
                           .count();
  }
  const double wall_ns =
      std::chrono::duration<double, std::nano>(latest - earliest).count();
  const double total_operations =
      static_cast<double>(thread_count) * static_cast<double>(iterations);
  return {total_thread_ns / total_operations,
          total_operations * 1000.0 / wall_ns};
}

void print_metrics(uint32_t thread_count, const ParallelMetrics &metrics) {
  std::cout << std::fixed << std::setprecision(2) << "threads=" << thread_count
            << " operation=remote_nc_shared_load_fenced_8b"
            << " mean_thread_op_ns=" << metrics.mean_thread_op_ns
            << " aggregate_mops=" << metrics.aggregate_mops << '\n';
}

void verify_word(volatile uint64_t *word, uint64_t expected,
                 const std::string &description) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  const uint64_t actual = *word;
  if (actual != expected) {
    ubsm_test::fail(description + ": expected=" + std::to_string(expected) +
                    ", actual=" + std::to_string(actual));
  }
}

void run_owner(const Options &options, ubsm_test::SharedMemory &memory,
               int connection) {
  volatile uint64_t *word =
      ubsm_bench::word_at(memory, ubsm_bench::kLatencyOffset);
  ubsm_test::expect_stage(connection, 'H');
  for (uint32_t thread_count : make_thread_counts(options.max_threads)) {
    const uint64_t expected = load_value(thread_count);
    *word = expected;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ubsm_test::send_stage(connection, 'R');
    ubsm_test::expect_stage(connection, 'D');
    verify_word(word, expected, "owner shared word after remote loads");
  }
  ubsm_test::send_stage(connection, 'V');
  ubsm_bench::owner_cleanup(memory, connection);
}

void run_remote(const Options &options, ubsm_test::SharedMemory &memory,
                int connection) {
  memory.map();
  volatile uint64_t *word =
      ubsm_bench::word_at(memory, ubsm_bench::kLatencyOffset);
  ubsm_test::send_stage(connection, 'H');
  for (uint32_t thread_count : make_thread_counts(options.max_threads)) {
    ubsm_test::expect_stage(connection, 'R');
    const uint64_t expected = load_value(thread_count);
    verify_word(word, expected, "remote shared word before benchmark");
    print_metrics(thread_count,
                  benchmark_shared_word_load(
                      word, thread_count, options.common.iterations, expected));
    verify_word(word, expected, "remote shared word after benchmark");
    ubsm_test::send_stage(connection, 'D');
  }
  ubsm_test::expect_stage(connection, 'V');
  ubsm_bench::remote_cleanup(memory, connection);
}

} // namespace

int main(int argc, char **argv) {
  int connection = -1;
  try {
    const Options options = parse_options(argc, argv);
    const uint64_t region_bytes =
        ubsm_test::region_bytes_from_mb(options.common.region_mb);
    ubsm_test::SdkSession session;
    ubsm_test::SharedMemory memory(options.common.name, region_bytes);
    if (options.common.role == "owner")
      ubsm_bench::allocate_owner(memory, options.common);
    connection = ubsm_bench::open_connection(options.common);

    if (options.common.role == "owner")
      run_owner(options, memory, connection);
    else
      run_remote(options, memory, connection);

    close(connection);
    connection = -1;
    session.finalize();
    std::cout << "PASS remote multithread shared-load test "
              << options.common.role << " and cleanup\n";
    return 0;
  } catch (const std::exception &error) {
    if (connection >= 0)
      close(connection);
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
