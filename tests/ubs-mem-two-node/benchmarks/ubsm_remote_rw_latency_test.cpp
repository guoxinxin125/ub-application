#include "ubsm_benchmark_common.h"

#include <algorithm>
#include <array>
#include <vector>

namespace {

constexpr uint8_t kSweepReadSeed = 0x39;
constexpr uint8_t kSweepWriteSeed = 0xa6;
constexpr uint64_t kSweepTargetBytes = 32ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaximumSweepBytes = 2ULL * 1024ULL * 1024ULL;

std::vector<uint64_t> sweep_sizes(uint64_t test_bytes) {
  static constexpr std::array<uint64_t, 19> kSizes = {
      8,    16,    32,    64,    128,    256,    512,    1024,    2048,   4096,
      8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576, 2097152};
  std::vector<uint64_t> sizes;
  const uint64_t maximum = std::min(test_bytes, kMaximumSweepBytes);
  for (const uint64_t size : kSizes) {
    if (size <= maximum)
      sizes.push_back(size);
  }
  if (sizes.empty() || sizes.back() != maximum)
    sizes.push_back(maximum);
  return sizes;
}

uint64_t sweep_iterations(uint64_t requested, uint64_t bytes) {
  return std::min(requested, std::max<uint64_t>(1, kSweepTargetBytes / bytes));
}

uint8_t *aligned_local_buffer(std::vector<uint8_t> &storage) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(storage.data());
  return reinterpret_cast<uint8_t *>((address + 63U) & ~uintptr_t{63U});
}

void compiler_observe(const void *buffer, size_t bytes) {
  asm volatile("" : : "r"(buffer), "r"(bytes) : "memory");
}

double benchmark_memcpy_read(const uint8_t *remote, uint8_t *local,
                             uint64_t bytes, uint64_t iterations) {
  std::memcpy(local, remote, static_cast<size_t>(bytes));
  compiler_observe(local, static_cast<size_t>(bytes));
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < iterations; ++i) {
    std::memcpy(local, remote, static_cast<size_t>(bytes));
    compiler_observe(local, static_cast<size_t>(bytes));
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(iterations);
}

double benchmark_memcpy_write(uint8_t *remote, const uint8_t *local,
                              uint64_t bytes, uint64_t iterations) {
  std::memcpy(remote, local, static_cast<size_t>(bytes));
  std::atomic_thread_fence(std::memory_order_seq_cst);
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < iterations; ++i) {
    std::memcpy(remote, local, static_cast<size_t>(bytes));
    compiler_observe(remote, static_cast<size_t>(bytes));
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(iterations);
}

double benchmark_scalar_load_fenced(volatile const uint64_t *remote,
                                    uint64_t bytes, uint64_t iterations) {
  const uint64_t word_count = bytes / sizeof(uint64_t);
  uint64_t sink0 = 0;
  uint64_t sink1 = 0;
  uint64_t sink2 = 0;
  uint64_t sink3 = 0;
  uint64_t sink4 = 0;
  uint64_t sink5 = 0;
  uint64_t sink6 = 0;
  uint64_t sink7 = 0;
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < iterations; ++i) {
    uint64_t word = 0;
    for (; word + 8 <= word_count; word += 8) {
      sink0 ^= remote[word];
      sink1 ^= remote[word + 1];
      sink2 ^= remote[word + 2];
      sink3 ^= remote[word + 3];
      sink4 ^= remote[word + 4];
      sink5 ^= remote[word + 5];
      sink6 ^= remote[word + 6];
      sink7 ^= remote[word + 7];
    }
    for (; word < word_count; ++word)
      sink0 ^= remote[word];
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const uint64_t sink =
      sink0 ^ sink1 ^ sink2 ^ sink3 ^ sink4 ^ sink5 ^ sink6 ^ sink7;
  if (sink == std::numeric_limits<uint64_t>::max())
    std::cerr << "unreachable scalar-load sink=" << sink << '\n';
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(iterations);
}

double benchmark_scalar_store_fenced(volatile uint64_t *remote, uint64_t bytes,
                                     uint64_t iterations) {
  const uint64_t word_count = bytes / sizeof(uint64_t);
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < iterations; ++i) {
    for (uint64_t word = 0; word < word_count; ++word)
      remote[word] = i;
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(iterations);
}

void verify_scalar_store(volatile const uint64_t *remote, uint64_t bytes,
                         uint64_t sequence) {
  const uint64_t word_count = bytes / sizeof(uint64_t);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  for (uint64_t word = 0; word < word_count; ++word) {
    const uint64_t expected = sequence;
    const uint64_t actual = remote[word];
    if (actual != expected) {
      ubsm_test::fail("remote scalar-store sweep mismatch at word " +
                      std::to_string(word) +
                      ": expected=" + std::to_string(expected) +
                      ", actual=" + std::to_string(actual));
    }
  }
}

void print_sweep_result(const char *operation, uint64_t bytes,
                        uint64_t iterations, double average_ns) {
  const double gib_per_sec = (static_cast<double>(bytes) / average_ns) * 1e9 /
                             static_cast<double>(1024ULL * 1024ULL * 1024ULL);
  std::cout << std::fixed << std::setprecision(2) << "operation=" << operation
            << " bytes=" << bytes << " iterations=" << iterations
            << " avg_ns=" << average_ns << " gib_per_sec=" << gib_per_sec
            << '\n';
}

void run_owner(const ubsm_bench::TwoNodeOptions &options,
               ubsm_test::SharedMemory &memory, int connection) {
  volatile uint64_t *word =
      ubsm_bench::word_at(memory, ubsm_bench::kLatencyOffset);
  volatile uint64_t *cacheline =
      ubsm_bench::word_at(memory, ubsm_bench::kCachelineLatencyOffset);
  constexpr uint64_t kCachelineSequence = 0x123456789abcdef0ULL;
  ubsm_bench::write_word_block<1>(word, ubsm_bench::kInitialLatencyValue);
  ubsm_bench::write_word_block<8>(cacheline, kCachelineSequence);
  ubsm_test::write_pattern(ubsm_bench::pattern_at(memory), options.test_bytes,
                           kSweepReadSeed);

  ubsm_test::expect_stage(connection, 'H');
  ubsm_test::send_stage(connection, 'R');
  ubsm_test::expect_stage(connection, 'M');
  const uint64_t largest_sweep = sweep_sizes(options.test_bytes).back();
  ubsm_test::verify_pattern(ubsm_bench::pattern_at(memory), largest_sweep,
                            kSweepWriteSeed,
                            "remote memcpy sweep write observed by owner");
  ubsm_test::send_stage(connection, 'N');
  ubsm_test::expect_stage(connection, 'D');
  ubsm_bench::verify_word_block<1>(
      word, options.iterations - 1,
      "remote 8-byte fenced store observed by owner");
  ubsm_bench::verify_word_block<8>(
      cacheline, options.iterations - 1,
      "remote 64-byte fenced store observed by owner");
  const uint64_t largest_iterations =
      sweep_iterations(options.iterations, largest_sweep);
  verify_scalar_store(reinterpret_cast<volatile const uint64_t *>(
                          ubsm_bench::pattern_at(memory)),
                      largest_sweep, largest_iterations - 1);
  std::cout << "PASS remote final 8-byte and 64-byte stores observed by owner "
               "without invalidate; memcpy and scalar sweep writes verified\n";
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
      "remote_nc_load_fenced_8b_avg_ns",
      ubsm_bench::benchmark_word_block_load_fenced<1>(
          word, options.iterations, ubsm_bench::kInitialLatencyValue));
  ubsm_test::print_latency(
      "remote_nc_load_fenced_64b_avg_ns",
      ubsm_bench::benchmark_word_block_load_fenced<8>(
          cacheline, options.iterations, kCachelineSequence));

  ubsm_test::print_latency("remote_nc_store_fenced_8b_avg_ns",
                           ubsm_bench::benchmark_word_block_store_fenced<1>(
                               word, options.iterations));
  ubsm_test::print_latency("remote_nc_store_fenced_64b_avg_ns",
                           ubsm_bench::benchmark_word_block_store_fenced<8>(
                               cacheline, options.iterations));

  const std::vector<uint64_t> sizes = sweep_sizes(options.test_bytes);
  std::vector<uint8_t> local_storage(static_cast<size_t>(sizes.back() + 63U));
  uint8_t *const local = aligned_local_buffer(local_storage);
  const uint8_t *const remote_read =
      const_cast<const uint8_t *>(ubsm_bench::pattern_at(memory));
  uint8_t *const remote_write = const_cast<uint8_t *>(remote_read);
  std::cout << "memcpy_sweep_max_bytes=" << sizes.back()
            << " local_alignment_mod_64="
            << reinterpret_cast<uintptr_t>(local) % 64
            << " remote_alignment_mod_64="
            << reinterpret_cast<uintptr_t>(remote_read) % 64 << '\n';

  for (const uint64_t bytes : sizes) {
    const uint64_t iterations = sweep_iterations(options.iterations, bytes);
    const double average_ns =
        benchmark_memcpy_read(remote_read, local, bytes, iterations);
    print_sweep_result("remote_nc_memcpy_read", bytes, iterations, average_ns);
  }
  ubsm_test::verify_pattern(local, sizes.back(), kSweepReadSeed,
                            "remote memcpy sweep read");

  ubsm_test::write_pattern(local, sizes.back(), kSweepWriteSeed);
  for (const uint64_t bytes : sizes) {
    const uint64_t iterations = sweep_iterations(options.iterations, bytes);
    const double average_ns =
        benchmark_memcpy_write(remote_write, local, bytes, iterations);
    print_sweep_result("remote_nc_memcpy_write", bytes, iterations, average_ns);
  }
  ubsm_test::send_stage(connection, 'M');
  ubsm_test::expect_stage(connection, 'N');

  volatile uint64_t *const remote_words =
      reinterpret_cast<volatile uint64_t *>(remote_write);
  for (const uint64_t bytes : sizes) {
    const uint64_t iterations = sweep_iterations(options.iterations, bytes);
    const double average_ns =
        benchmark_scalar_load_fenced(remote_words, bytes, iterations);
    print_sweep_result("remote_nc_scalar_load_fenced", bytes, iterations,
                       average_ns);
  }
  for (const uint64_t bytes : sizes) {
    const uint64_t iterations = sweep_iterations(options.iterations, bytes);
    const double average_ns =
        benchmark_scalar_store_fenced(remote_words, bytes, iterations);
    print_sweep_result("remote_nc_scalar_store_fenced", bytes, iterations,
                       average_ns);
  }
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
    if (options.test_bytes % sizeof(uint64_t) != 0)
      ubsm_test::fail("--test-bytes must be a multiple of 8");
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
