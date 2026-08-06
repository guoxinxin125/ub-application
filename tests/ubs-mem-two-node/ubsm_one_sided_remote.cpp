#include "ubsm_test_common.h"

namespace {

struct Options {
    std::string owner_ip;
    std::string name = "ubsm_micro_two_node";
    uint16_t port = 18525;
    uint64_t region_mb = 4;
    uint64_t test_bytes = 4096;
    uint64_t iterations = 1000;
    int timeout_sec = 60;
};

void usage(const char *program)
{
    std::cerr << "Usage: " << program
              << " --owner-ip IP [--port N] [--name NAME] [--region-mb N]"
              << " [--test-bytes N] [--iterations N] [--timeout-sec N]\n";
}

Options parse_options(int argc, char **argv)
{
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
        if (arg == "--owner-ip")
            options.owner_ip = value;
        else if (arg == "--port")
            options.port = ubsm_test::parse_port(value);
        else if (arg == "--name")
            options.name = value;
        else if (arg == "--region-mb")
            options.region_mb = ubsm_test::parse_u64(value, arg);
        else if (arg == "--test-bytes")
            options.test_bytes = ubsm_test::parse_u64(value, arg);
        else if (arg == "--iterations")
            options.iterations = ubsm_test::parse_u64(value, arg);
        else if (arg == "--timeout-sec")
            options.timeout_sec = ubsm_test::parse_positive_int(value, arg);
        else
            ubsm_test::fail("unknown option: " + arg);
    }
    if (options.owner_ip.empty())
        ubsm_test::fail("--owner-ip is required");
    if (options.iterations == 0)
        ubsm_test::fail("--iterations must be greater than zero");
    ubsm_test::validate_name(options.name);
    ubsm_test::validate_sizes(ubsm_test::region_bytes_from_mb(options.region_mb),
                              options.test_bytes);
    return options;
}

} // namespace

int main(int argc, char **argv)
{
    int connection = -1;
    try {
        const Options options = parse_options(argc, argv);
        const uint64_t region_bytes =
            ubsm_test::region_bytes_from_mb(options.region_mb);
        ubsm_test::SdkSession session;
        ubsm_test::SharedMemory memory(options.name, region_bytes);

        connection = ubsm_test::connect_to_owner(options.owner_ip, options.port,
                                                 options.timeout_sec);
        memory.map();
        ubsm_test::send_stage(connection, 'H');
        ubsm_test::expect_stage(connection, 'A');
        ubsm_test::verify_pattern(memory.bytes(), options.test_bytes, 0x39,
                                  "owner cacheable store observed by remote");
        std::cout << "PASS owner cacheable store -> remote NC load ("
                  << options.test_bytes << " bytes)\n";

        constexpr uint64_t kLoadSequence = 0x123456789abcdef0ULL;
        ubsm_test::write_cacheline(memory.word(), kLoadSequence);
        ubsm_test::print_latency(
            "remote_nc_checked_fenced_load_64b_avg_ns",
            ubsm_test::benchmark_checked_fenced_cacheline_load(
                memory.word(), options.iterations, kLoadSequence));
        ubsm_test::print_latency(
            "remote_nc_fenced_store_64b_avg_ns",
            ubsm_test::benchmark_fenced_cacheline_store(
                memory.word(), options.iterations));

        ubsm_test::write_pattern(memory.bytes(), options.test_bytes, 0xc7);
        ubsm_test::send_stage(connection, 'B');
        ubsm_test::expect_stage(connection, 'C');
        memory.unmap();
        ubsm_test::send_stage(connection, 'D');
        ubsm_test::expect_stage(connection, 'E');
        close(connection);
        connection = -1;
        session.finalize();
        std::cout << "PASS two-node UBS Memory remote and cleanup\n";
        return 0;
    } catch (const std::exception &error) {
        if (connection >= 0)
            close(connection);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
