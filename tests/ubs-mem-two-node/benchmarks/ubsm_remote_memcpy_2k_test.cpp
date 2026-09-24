#include "ubsm_benchmark_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

#include <sched.h>

namespace {

constexpr uint64_t kCopyBytes = 2048;
constexpr uint64_t kWarmupCopies = 100;

using CopyBuffer = std::array<uint8_t, kCopyBytes>;

struct LatencyStats {
  double average_ns = 0.0;
  uint64_t p50_ns = 0;
  uint64_t p99_ns = 0;
  uint64_t max_ns = 0;
};

uint8_t stable_seed(uint64_t object_index) {
  return static_cast<uint8_t>(0x31U + (object_index * 17U) % 193U);
}

uint8_t fresh_seed(uint64_t iteration) {
  return static_cast<uint8_t>(0x80U + iteration % 113U);
}

void consume_copy(const CopyBuffer &buffer) {
  asm volatile("" : : "m"(buffer) : "memory");
}

uint64_t elapsed_ns(std::chrono::steady_clock::time_point start,
                    std::chrono::steady_clock::time_point end) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
          .count());
}

uint64_t measure_timestamp_overhead_ns() {
  uint64_t minimum = std::numeric_limits<uint64_t>::max();
  for (uint64_t i = 0; i < 10000; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const auto end = std::chrono::steady_clock::now();
    minimum = std::min(minimum, elapsed_ns(start, end));
  }
  return minimum;
}

uint64_t percentile(const std::vector<uint64_t> &sorted, double fraction) {
  const size_t rank = static_cast<size_t>(
      std::ceil(fraction * static_cast<double>(sorted.size())));
  return sorted[std::max<size_t>(1, rank) - 1];
}

LatencyStats summarize(std::vector<uint64_t> samples) {
  if (samples.empty())
    ubsm_test::fail("cannot summarize an empty latency sample set");
  const uint64_t total =
      std::accumulate(samples.begin(), samples.end(), uint64_t{0});
  std::sort(samples.begin(), samples.end());
  LatencyStats stats;
  stats.average_ns =
      static_cast<double>(total) / static_cast<double>(samples.size());
  stats.p50_ns = percentile(samples, 0.50);
  stats.p99_ns = percentile(samples, 0.99);
  stats.max_ns = samples.back();
  return stats;
}

std::vector<uint64_t> subtract_overhead(const std::vector<uint64_t> &samples,
                                        uint64_t overhead_ns) {
  std::vector<uint64_t> adjusted;
  adjusted.reserve(samples.size());
  for (const uint64_t sample : samples)
    adjusted.push_back(sample > overhead_ns ? sample - overhead_ns : 0);
  return adjusted;
}

void print_stats(const std::string &mode, const std::vector<uint64_t> &samples,
                 uint64_t timestamp_overhead_ns) {
  const LatencyStats raw = summarize(samples);
  const LatencyStats adjusted =
      summarize(subtract_overhead(samples, timestamp_overhead_ns));
  std::cout << std::fixed << std::setprecision(1) << "mode=" << mode
            << " calls=" << samples.size() << " avg_ns=" << raw.average_ns
            << " adjusted_avg_ns=" << adjusted.average_ns
            << " p50_ns=" << raw.p50_ns
            << " adjusted_p50_ns=" << adjusted.p50_ns
            << " p99_ns=" << raw.p99_ns
            << " adjusted_p99_ns=" << adjusted.p99_ns
            << " max_ns=" << raw.max_ns << '\n';
}

void verify_local_copy(const CopyBuffer &buffer, uint8_t seed,
                       const std::string &description) {
  ubsm_test::verify_pattern(buffer.data(), buffer.size(), seed, description);
}

template <class SourceAt>
std::vector<uint64_t> measure_copies(uint64_t iterations,
                                     uint64_t warmup_copies, SourceAt source_at,
                                     CopyBuffer &destination) {
  for (uint64_t i = 0; i < warmup_copies; ++i) {
    std::memcpy(destination.data(), source_at(i), destination.size());
    consume_copy(destination);
  }

  std::vector<uint64_t> samples;
  samples.reserve(static_cast<size_t>(iterations));
  for (uint64_t i = 0; i < iterations; ++i) {
    const auto start = std::chrono::steady_clock::now();
    std::memcpy(destination.data(), source_at(warmup_copies + i),
                destination.size());
    consume_copy(destination);
    const auto end = std::chrono::steady_clock::now();
    samples.push_back(elapsed_ns(start, end));
  }
  return samples;
}

void initialize_source_pool(ubsm_test::SharedMemory &memory,
                            uint64_t object_count) {
  volatile uint8_t *const source = ubsm_bench::pattern_at(memory);
  for (uint64_t object = 0; object < object_count; ++object) {
    ubsm_test::write_pattern(source + object * kCopyBytes, kCopyBytes,
                             stable_seed(object));
  }
}

void run_owner(const ubsm_bench::TwoNodeOptions &options,
               ubsm_test::SharedMemory &memory, int connection,
               uint64_t object_count, uint64_t warmup_copies) {
  initialize_source_pool(memory, object_count);

  ubsm_test::expect_stage(connection, 'H');
  ubsm_test::send_stage(connection, 'R');

  // The remote first runs local, same-address and rotating-address copies.
  ubsm_test::expect_stage(connection, 'S');

  volatile uint8_t *const source = ubsm_bench::pattern_at(memory);
  const uint64_t total_fresh_copies = warmup_copies + options.iterations;
  for (uint64_t i = 0; i < total_fresh_copies; ++i) {
    ubsm_test::expect_stage(connection, 'W');
    ubsm_test::write_pattern(source, kCopyBytes, fresh_seed(i));
    ubsm_test::send_stage(connection, 'R');
  }

  ubsm_test::expect_stage(connection, 'D');
  ubsm_bench::owner_cleanup(memory, connection);
}

void run_remote(const ubsm_bench::TwoNodeOptions &options,
                ubsm_test::SharedMemory &memory, int connection,
                uint64_t object_count, uint64_t warmup_copies) {
  memory.map();
  ubsm_test::send_stage(connection, 'H');
  ubsm_test::expect_stage(connection, 'R');

  volatile uint8_t *const volatile_source = ubsm_bench::pattern_at(memory);
  const uint8_t *const source = const_cast<const uint8_t *>(volatile_source);
  alignas(64) CopyBuffer local_source{};
  alignas(64) CopyBuffer destination{};
  ubsm_test::write_pattern(local_source.data(), local_source.size(),
                           stable_seed(0));

  const uint64_t timestamp_overhead_ns = measure_timestamp_overhead_ns();
  std::cout << "copy_bytes=" << kCopyBytes
            << " logical_64b_chunks=" << kCopyBytes / 64
            << " source_objects=" << object_count
            << " warmup_copies=" << warmup_copies
            << " iterations=" << options.iterations
            << " timestamp_overhead_ns=" << timestamp_overhead_ns
            << " cpu=" << sched_getcpu() << " remote_source_alignment_mod_64="
            << reinterpret_cast<uintptr_t>(source) % 64
            << " local_destination_alignment_mod_64="
            << reinterpret_cast<uintptr_t>(destination.data()) % 64 << '\n';

  auto local_samples = measure_copies(
      options.iterations, warmup_copies,
      [&](uint64_t) { return local_source.data(); }, destination);
  verify_local_copy(destination, stable_seed(0), "local 2 KiB memcpy");
  print_stats("local_same_address_2k", local_samples, timestamp_overhead_ns);

  auto same_address_samples = measure_copies(
      options.iterations, warmup_copies, [&](uint64_t) { return source; },
      destination);
  verify_local_copy(destination, stable_seed(0),
                    "remote same-address 2 KiB memcpy");
  print_stats("remote_same_address_2k", same_address_samples,
              timestamp_overhead_ns);

  auto rotating_samples = measure_copies(
      options.iterations, warmup_copies,
      [&](uint64_t iteration) {
        return source + (iteration % object_count) * kCopyBytes;
      },
      destination);
  const uint64_t final_rotating_object =
      (warmup_copies + options.iterations - 1) % object_count;
  verify_local_copy(destination, stable_seed(final_rotating_object),
                    "remote rotating-address 2 KiB memcpy");
  print_stats("remote_rotating_address_2k", rotating_samples,
              timestamp_overhead_ns);

  ubsm_test::send_stage(connection, 'S');

  std::vector<uint64_t> fresh_samples;
  fresh_samples.reserve(static_cast<size_t>(options.iterations));
  const uint64_t total_fresh_copies = warmup_copies + options.iterations;
  for (uint64_t i = 0; i < total_fresh_copies; ++i) {
    ubsm_test::send_stage(connection, 'W');
    ubsm_test::expect_stage(connection, 'R');
    const auto start = std::chrono::steady_clock::now();
    std::memcpy(destination.data(), source, destination.size());
    consume_copy(destination);
    const auto end = std::chrono::steady_clock::now();
    if (i >= warmup_copies)
      fresh_samples.push_back(elapsed_ns(start, end));
  }
  verify_local_copy(destination, fresh_seed(total_fresh_copies - 1),
                    "remote freshly-owner-written 2 KiB memcpy");
  print_stats("remote_after_owner_write_2k", fresh_samples,
              timestamp_overhead_ns);

  ubsm_test::send_stage(connection, 'D');
  ubsm_bench::remote_cleanup(memory, connection);
}

} // namespace

int main(int argc, char **argv) {
  int connection = -1;
  try {
    const auto options = ubsm_bench::parse_two_node_options(
        argc, argv, "ubsm_remote_memcpy_2k", 18538);
    if (options.test_bytes < kCopyBytes ||
        options.test_bytes % kCopyBytes != 0) {
      ubsm_test::fail("--test-bytes must be a positive multiple of 2048");
    }
    const uint64_t object_count = options.test_bytes / kCopyBytes;
    const uint64_t warmup_copies =
        std::min<uint64_t>(kWarmupCopies, options.iterations);
    const uint64_t region_bytes =
        ubsm_test::region_bytes_from_mb(options.region_mb);
    ubsm_test::SdkSession session;
    ubsm_test::SharedMemory memory(options.name, region_bytes);
    if (options.role == "owner")
      ubsm_bench::allocate_owner(memory, options);
    connection = ubsm_bench::open_connection(options);

    if (options.role == "owner") {
      run_owner(options, memory, connection, object_count, warmup_copies);
    } else {
      run_remote(options, memory, connection, object_count, warmup_copies);
    }

    close(connection);
    connection = -1;
    session.finalize();
    std::cout << "PASS remote 2 KiB memcpy test " << options.role
              << " and cleanup\n";
    return 0;
  } catch (const std::exception &error) {
    if (connection >= 0)
      close(connection);
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
