#ifndef UBSM_BENCHMARK_COMMON_H
#define UBSM_BENCHMARK_COMMON_H

#include "ubsm_test_common.h"

namespace ubsm_bench {

constexpr uint64_t kAtomicOffset = 0;
constexpr uint64_t kLatencyOffset = 64;
constexpr uint64_t kCachelineLatencyOffset = 128;
constexpr uint64_t kRemoteFetchAddOffset = 192;
constexpr uint64_t kRemoteCasOffset = 256;
constexpr uint64_t kPatternOffset = 4096;
constexpr uint64_t kInitialLatencyValue = 0x1020304050607080ULL;

inline volatile uint64_t *word_at(ubsm_test::SharedMemory &memory,
                                  uint64_t offset) {
  return reinterpret_cast<volatile uint64_t *>(memory.bytes() + offset);
}

inline volatile uint8_t *pattern_at(ubsm_test::SharedMemory &memory) {
  return memory.bytes() + kPatternOffset;
}

inline uint64_t *atomic_word(ubsm_test::SharedMemory &memory) {
  return const_cast<uint64_t *>(word_at(memory, kAtomicOffset));
}

inline uint64_t *atomic_word_at(ubsm_test::SharedMemory &memory,
                                uint64_t offset) {
  return const_cast<uint64_t *>(word_at(memory, offset));
}

inline double benchmark_load_8b(volatile uint64_t *word, uint64_t iterations) {
  uint64_t sink = 0;
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < iterations; ++i)
    sink ^= *word;
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (sink == std::numeric_limits<uint64_t>::max())
    std::cerr << "unreachable sink=" << sink << '\n';
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(iterations);
}

inline double benchmark_store_issue_8b(volatile uint64_t *word,
                                       uint64_t iterations) {
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < iterations; ++i)
    *word = i;
  const auto elapsed = std::chrono::steady_clock::now() - start;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(iterations);
}

inline double benchmark_store_fenced_8b(volatile uint64_t *word,
                                        uint64_t iterations) {
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < iterations; ++i) {
    *word = i;
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(iterations);
}

inline uint64_t word_block_value(uint64_t sequence, uint64_t word_index) {
  return sequence ^ (0x9e3779b97f4a7c15ULL * word_index);
}

template <uint64_t WordCount>
inline void write_word_block(volatile uint64_t *words, uint64_t sequence) {
  static_assert(WordCount > 0 && WordCount <= ubsm_test::kCacheLineWords,
                "word block must contain between one and eight words");
  for (uint64_t word = 0; word < WordCount; ++word)
    words[word] = word_block_value(sequence, word);
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

template <uint64_t WordCount>
inline void verify_word_block(volatile const uint64_t *words,
                              uint64_t sequence,
                              const std::string &description) {
  static_assert(WordCount > 0 && WordCount <= ubsm_test::kCacheLineWords,
                "word block must contain between one and eight words");
  std::atomic_thread_fence(std::memory_order_seq_cst);
  for (uint64_t word = 0; word < WordCount; ++word) {
    const uint64_t expected = word_block_value(sequence, word);
    const uint64_t actual = words[word];
    if (actual != expected) {
      ubsm_test::fail(description + " mismatch at word " +
                      std::to_string(word) + ": expected=" +
                      std::to_string(expected) + ", actual=" +
                      std::to_string(actual));
    }
  }
}

template <uint64_t WordCount>
inline double benchmark_word_block_load(volatile uint64_t *words,
                                        uint64_t iterations,
                                        uint64_t sequence) {
  verify_word_block<WordCount>(words, sequence,
                               "word-block load before benchmark");
  uint64_t sinks[WordCount]{};
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < iterations; ++i) {
    for (uint64_t word = 0; word < WordCount; ++word)
      sinks[word] ^= words[word];
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  verify_word_block<WordCount>(words, sequence,
                               "word-block load after benchmark");
  uint64_t sink = 0;
  for (uint64_t word = 0; word < WordCount; ++word)
    sink ^= sinks[word];
  if (sink == std::numeric_limits<uint64_t>::max())
    std::cerr << "unreachable sink=" << sink << '\n';
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(iterations);
}

template <uint64_t WordCount>
inline double benchmark_word_block_store_issue(volatile uint64_t *words,
                                               uint64_t iterations) {
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < iterations; ++i) {
    for (uint64_t word = 0; word < WordCount; ++word)
      words[word] = word_block_value(i, word);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  std::atomic_thread_fence(std::memory_order_seq_cst);
  verify_word_block<WordCount>(words, iterations - 1,
                               "word-block store-issue benchmark");
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(iterations);
}

template <uint64_t WordCount>
inline double benchmark_word_block_store_fenced(volatile uint64_t *words,
                                                uint64_t iterations) {
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < iterations; ++i) {
    for (uint64_t word = 0; word < WordCount; ++word)
      words[word] = word_block_value(i, word);
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  verify_word_block<WordCount>(words, iterations - 1,
                               "word-block fenced-store benchmark");
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(iterations);
}

inline double benchmark_fetch_add_8b(uint64_t *word, uint64_t iterations) {
  __atomic_store_n(word, 0, __ATOMIC_SEQ_CST);
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < iterations; ++i)
    (void)__atomic_fetch_add(word, 1, __ATOMIC_SEQ_CST);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != iterations)
    ubsm_test::fail("fetch-add final value mismatch");
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(iterations);
}

inline double benchmark_cas_8b(uint64_t *word, uint64_t iterations) {
  __atomic_store_n(word, 0, __ATOMIC_SEQ_CST);
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < iterations; ++i) {
    uint64_t expected = i;
    if (!__atomic_compare_exchange_n(word, &expected, i + 1, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      ubsm_test::fail("CAS unexpectedly failed at iteration " +
                      std::to_string(i));
    }
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != iterations)
    ubsm_test::fail("CAS final value mismatch");
  return std::chrono::duration<double, std::nano>(elapsed).count() /
         static_cast<double>(iterations);
}

inline void run_atomic_validation(ubsm_test::SharedMemory &memory,
                                  uint64_t base, uint64_t iterations,
                                  uint64_t magic) {
  uint64_t *word = atomic_word(memory);
  for (uint64_t i = 0; i < iterations; ++i) {
    const uint64_t old = __atomic_fetch_add(word, 1, __ATOMIC_SEQ_CST);
    if (old != base + i) {
      ubsm_test::fail("fetch-add returned unexpected old value at iteration " +
                      std::to_string(i));
    }
  }

  uint64_t expected = base + iterations;
  if (!__atomic_compare_exchange_n(word, &expected, magic, false,
                                   __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
    ubsm_test::fail("successful CAS validation failed, observed=" +
                    std::to_string(expected));
  }
  expected = base;
  if (__atomic_compare_exchange_n(word, &expected, base + 1, false,
                                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
    ubsm_test::fail("failed-CAS validation unexpectedly succeeded");
  }
  if (expected != magic)
    ubsm_test::fail("failed CAS did not return the current value");
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

struct TwoNodeOptions {
  std::string role;
  std::string bind_ip;
  std::string owner_ip;
  std::string provider_host;
  std::string name;
  uint16_t port = 0;
  uint64_t region_mb = 4;
  uint64_t test_bytes = 4096;
  uint64_t iterations = 100000;
  uint64_t atomic_iterations = 1000;
  int timeout_sec = 120;
  uint32_t provider_socket = UINT32_MAX;
  uint32_t provider_numa = UINT32_MAX;
  uint32_t provider_port = UINT32_MAX;
};

inline void two_node_usage(const char *program, uint16_t default_port) {
  std::cerr << "Usage: " << program << " --role owner --bind-ip IP [options]\n"
            << "       " << program
            << " --role remote --owner-ip IP [options]\n"
            << "Options: --name NAME --port N(default " << default_port
            << ") --region-mb N --test-bytes N --iterations N\n"
            << "         --atomic-iterations N --timeout-sec N\n"
            << "         --provider-host HOST --provider-socket N"
               " --provider-numa N --provider-port N\n";
}

inline TwoNodeOptions parse_two_node_options(int argc, char **argv,
                                             const std::string &default_name,
                                             uint16_t default_port) {
  TwoNodeOptions options;
  options.name = default_name;
  options.port = default_port;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      two_node_usage(argv[0], default_port);
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
    else if (arg == "--provider-socket")
      options.provider_socket = ubsm_test::parse_u32(value, arg);
    else if (arg == "--provider-numa")
      options.provider_numa = ubsm_test::parse_u32(value, arg);
    else if (arg == "--provider-port")
      options.provider_port = ubsm_test::parse_u32(value, arg);
    else if (arg == "--name")
      options.name = value;
    else if (arg == "--port")
      options.port = ubsm_test::parse_port(value);
    else if (arg == "--region-mb")
      options.region_mb = ubsm_test::parse_u64(value, arg);
    else if (arg == "--test-bytes")
      options.test_bytes = ubsm_test::parse_u64(value, arg);
    else if (arg == "--iterations")
      options.iterations = ubsm_test::parse_u64(value, arg);
    else if (arg == "--atomic-iterations")
      options.atomic_iterations = ubsm_test::parse_u64(value, arg);
    else if (arg == "--timeout-sec")
      options.timeout_sec = ubsm_test::parse_positive_int(value, arg);
    else
      ubsm_test::fail("unknown option: " + arg);
  }

  if (options.role != "owner" && options.role != "remote")
    ubsm_test::fail("--role must be owner or remote");
  if (options.role == "owner" && options.bind_ip.empty())
    ubsm_test::fail("owner role requires --bind-ip");
  if (options.role == "remote" && options.owner_ip.empty())
    ubsm_test::fail("remote role requires --owner-ip");
  if (options.iterations == 0 || options.atomic_iterations == 0)
    ubsm_test::fail("iteration counts must be greater than zero");
  ubsm_test::validate_name(options.name);
  const uint64_t region_bytes =
      ubsm_test::region_bytes_from_mb(options.region_mb);
  ubsm_test::validate_sizes(region_bytes, options.test_bytes);
  if (options.test_bytes > region_bytes - kPatternOffset)
    ubsm_test::fail("--test-bytes plus control area exceeds region size");

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

inline void allocate_owner(ubsm_test::SharedMemory &memory,
                           const TwoNodeOptions &options) {
  memory.allocate_with_provider(options.provider_host, options.provider_socket,
                                options.provider_numa, options.provider_port);
  memory.map();
}

inline int open_connection(const TwoNodeOptions &options) {
  if (options.role == "owner")
    return ubsm_test::accept_remote(options.bind_ip, options.port,
                                    options.timeout_sec);
  return ubsm_test::connect_to_owner(options.owner_ip, options.port,
                                     options.timeout_sec);
}

inline void owner_cleanup(ubsm_test::SharedMemory &memory, int connection) {
  ubsm_test::expect_stage(connection, 'U');
  memory.unmap();
  memory.deallocate();
  ubsm_test::send_stage(connection, 'E');
}

inline void remote_cleanup(ubsm_test::SharedMemory &memory, int connection) {
  memory.unmap();
  ubsm_test::send_stage(connection, 'U');
  ubsm_test::expect_stage(connection, 'E');
}

} // namespace ubsm_bench

#endif
