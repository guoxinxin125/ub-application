#include "ubsm_benchmark_common.h"

namespace {

constexpr uint64_t kCachelineSequence = 0x123456789abcdef0ULL;

struct Options {
  std::string name = "ubsm_local_latency";
  std::string provider_host;
  uint64_t region_mb = 4;
  uint64_t iterations = 100000;
  uint32_t provider_socket = UINT32_MAX;
  uint32_t provider_numa = UINT32_MAX;
  uint32_t provider_port = UINT32_MAX;
};

Options parse_options(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      std::cerr << "Usage: " << argv[0]
                << " [--name NAME] [--provider-host HOST]"
                   " [--provider-socket N] [--provider-numa N]"
                   " [--provider-port N] [--region-mb N]"
                   " [--iterations N]\n";
      std::exit(0);
    }
    if (i + 1 >= argc)
      ubsm_test::fail("missing value for " + arg);
    const std::string value(argv[++i]);
    if (arg == "--name")
      options.name = value;
    else if (arg == "--provider-host")
      options.provider_host = value;
    else if (arg == "--provider-socket")
      options.provider_socket = ubsm_test::parse_u32(value, arg);
    else if (arg == "--provider-numa")
      options.provider_numa = ubsm_test::parse_u32(value, arg);
    else if (arg == "--provider-port")
      options.provider_port = ubsm_test::parse_u32(value, arg);
    else if (arg == "--region-mb")
      options.region_mb = ubsm_test::parse_u64(value, arg);
    else if (arg == "--iterations")
      options.iterations = ubsm_test::parse_u64(value, arg);
    else
      ubsm_test::fail("unknown option: " + arg);
  }
  if (options.iterations == 0)
    ubsm_test::fail("--iterations must be greater than zero");
  ubsm_test::validate_name(options.name);
  ubsm_test::validate_sizes(ubsm_test::region_bytes_from_mb(options.region_mb),
                            8);
  if (options.provider_host.empty()) {
    char hostname[MAX_HOST_NAME_DESC_LENGTH]{};
    if (gethostname(hostname, sizeof(hostname)) != 0)
      ubsm_test::fail(ubsm_test::errno_message("gethostname"));
    if (hostname[sizeof(hostname) - 1] != '\0')
      ubsm_test::fail("local hostname is too long");
    options.provider_host = hostname;
  }
  return options;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parse_options(argc, argv);
    const uint64_t region_bytes =
        ubsm_test::region_bytes_from_mb(options.region_mb);
    ubsm_test::SdkSession session;
    ubsm_test::SharedMemory memory(options.name, region_bytes);
    memory.allocate_with_provider(options.provider_host,
                                  options.provider_socket,
                                  options.provider_numa, options.provider_port);
    memory.map();

    volatile uint64_t *latency =
        ubsm_bench::word_at(memory, ubsm_bench::kLatencyOffset);
    *latency = ubsm_bench::kInitialLatencyValue;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    ubsm_test::print_latency(
        "local_load_8b_avg_ns",
        ubsm_bench::benchmark_load_8b(latency, options.iterations));
    ubsm_test::print_latency(
        "local_store_issue_8b_avg_ns",
        ubsm_bench::benchmark_store_issue_8b(latency, options.iterations));
    ubsm_test::print_latency(
        "local_store_fenced_8b_avg_ns",
        ubsm_bench::benchmark_store_fenced_8b(latency, options.iterations));

    volatile uint64_t *cacheline =
        ubsm_bench::word_at(memory, ubsm_bench::kCachelineLatencyOffset);
    ubsm_test::write_cacheline(cacheline, kCachelineSequence);
    ubsm_test::print_latency(
        "local_load_64b_avg_ns",
        ubsm_test::benchmark_checked_cacheline_load(
            cacheline, options.iterations, kCachelineSequence));
    ubsm_test::print_latency(
        "local_store_fenced_64b_avg_ns",
        ubsm_test::benchmark_fenced_cacheline_store(cacheline,
                                                    options.iterations));

    uint64_t *atomic = ubsm_bench::atomic_word(memory);
    ubsm_test::print_latency(
        "local_fetch_add_8b_avg_ns",
        ubsm_bench::benchmark_fetch_add_8b(atomic, options.iterations));
    ubsm_test::print_latency(
        "local_cas_8b_avg_ns",
        ubsm_bench::benchmark_cas_8b(atomic, options.iterations));

    memory.unmap();
    memory.deallocate();
    session.finalize();
    std::cout << "PASS local latency test and cleanup\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
