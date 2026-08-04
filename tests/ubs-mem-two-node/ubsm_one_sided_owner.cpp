#include "ubsm_test_common.h"

namespace {

struct Options {
    std::string bind_ip;
    std::string provider_host;
    std::string name = "ubsm_micro_two_node";
    uint16_t port = 18525;
    uint64_t region_mb = 4;
    uint64_t test_bytes = 4096;
    uint64_t iterations = 1000;
    int timeout_sec = 60;
    uint32_t provider_socket = UINT32_MAX;
    uint32_t provider_numa = UINT32_MAX;
    uint32_t provider_port = UINT32_MAX;
};

void usage(const char *program)
{
    std::cerr << "Usage: " << program
              << " --bind-ip IP [--provider-host HOST] [--provider-socket N]"
              << " [--provider-numa N] [--provider-port N]"
              << " [--port N] [--name NAME] [--region-mb N]"
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
        if (arg == "--bind-ip")
            options.bind_ip = value;
        else if (arg == "--provider-host")
            options.provider_host = value;
        else if (arg == "--provider-socket")
            options.provider_socket = ubsm_test::parse_u32(value, arg);
        else if (arg == "--provider-numa")
            options.provider_numa = ubsm_test::parse_u32(value, arg);
        else if (arg == "--provider-port")
            options.provider_port = ubsm_test::parse_u32(value, arg);
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
    if (options.bind_ip.empty())
        ubsm_test::fail("--bind-ip is required");
    if (options.provider_host.empty()) {
        char hostname[MAX_HOST_NAME_DESC_LENGTH]{};
        if (gethostname(hostname, sizeof(hostname)) != 0)
            ubsm_test::fail(ubsm_test::errno_message("gethostname"));
        if (hostname[sizeof(hostname) - 1] != '\0')
            ubsm_test::fail("local hostname is too long");
        options.provider_host = hostname;
    }
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
        memory.allocate_with_provider(options.provider_host,
                                      options.provider_socket,
                                      options.provider_numa,
                                      options.provider_port);
        memory.map();

        ubsm_test::print_latency(
            "owner_local_load_avg_ns",
            ubsm_test::benchmark_load(memory.word(), options.iterations));
        ubsm_test::print_latency(
            "owner_local_fenced_store_avg_ns",
            ubsm_test::benchmark_fenced_store(memory.word(), options.iterations));

        connection = ubsm_test::accept_remote(options.bind_ip, options.port,
                                              options.timeout_sec);
        ubsm_test::expect_stage(connection, 'H');

        ubsm_test::write_pattern(memory.bytes(), options.test_bytes, 0x39);
        ubsm_test::send_stage(connection, 'A');
        ubsm_test::expect_stage(connection, 'B');
        ubsm_test::verify_pattern(memory.bytes(), options.test_bytes, 0xc7,
                                  "remote NC store observed by owner");
        std::cout << "PASS remote NC store -> owner cacheable load ("
                  << options.test_bytes << " bytes)\n";

        ubsm_test::send_stage(connection, 'C');
        ubsm_test::expect_stage(connection, 'D');
        memory.unmap();
        memory.deallocate();
        ubsm_test::send_stage(connection, 'E');
        close(connection);
        connection = -1;
        session.finalize();
        std::cout << "PASS two-node UBS Memory owner and cleanup\n";
        return 0;
    } catch (const std::exception &error) {
        if (connection >= 0)
            close(connection);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
