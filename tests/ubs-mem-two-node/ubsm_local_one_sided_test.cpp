#include "ubsm_test_common.h"

namespace {

struct Options {
    std::string name = "ubsm_micro_local";
    std::string provider_host;
    uint64_t region_mb = 4;
    uint64_t test_bytes = 4096;
    uint32_t provider_socket = UINT32_MAX;
    uint32_t provider_numa = UINT32_MAX;
    uint32_t provider_port = UINT32_MAX;
};

void usage(const char *program)
{
    std::cerr << "Usage: " << program
              << " [--name NAME] [--provider-host HOST]"
              << " [--provider-socket N] [--provider-numa N]"
              << " [--provider-port N] [--region-mb N] [--test-bytes N]\n";
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
        else if (arg == "--test-bytes")
            options.test_bytes = ubsm_test::parse_u64(value, arg);
        else
            ubsm_test::fail("unknown option: " + arg);
    }
    if (options.provider_host.empty()) {
        char hostname[MAX_HOST_NAME_DESC_LENGTH]{};
        if (gethostname(hostname, sizeof(hostname)) != 0)
            ubsm_test::fail(ubsm_test::errno_message("gethostname"));
        if (hostname[sizeof(hostname) - 1] != '\0')
            ubsm_test::fail("local hostname is too long");
        options.provider_host = hostname;
    }
    ubsm_test::validate_name(options.name);
    ubsm_test::validate_sizes(ubsm_test::region_bytes_from_mb(options.region_mb),
                              options.test_bytes);
    return options;
}

} // namespace

int main(int argc, char **argv)
{
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

        ubsm_test::write_pattern(memory.bytes(), options.test_bytes, 0x5a);
        ubsm_test::verify_pattern(memory.bytes(), options.test_bytes, 0x5a,
                                  "local owner pattern");
        std::cout << "PASS local owner load/store correctness ("
                  << options.test_bytes << " bytes)\n";

        memory.unmap();
        memory.deallocate();
        session.finalize();
        std::cout << "PASS local UBS Memory one-sided test and cleanup\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
