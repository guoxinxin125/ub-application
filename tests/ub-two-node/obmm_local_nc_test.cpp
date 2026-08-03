#include "obmm_test_common.h"

namespace {

struct Options {
    uint32_t local_eid = 0;
    int numa_id = 0;
    uint64_t region_mb = 2;
    uint64_t test_bytes = 4096;
    uint64_t iterations = 100000;
    bool have_eid = false;
};

void usage(const char *program)
{
    std::cerr << "Usage: " << program
              << " --local-eid EID [--numa-id N] [--region-mb N]"
              << " [--test-bytes N] [--iterations N]\n";
}

Options parse_options(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (i + 1 >= argc)
            obmm_test::fail("missing value after " + arg);
        const char *value = argv[++i];
        if (arg == "--local-eid") {
            options.local_eid = obmm_test::parse_u32(value, "--local-eid");
            options.have_eid = true;
        } else if (arg == "--numa-id") {
            options.numa_id = static_cast<int>(obmm_test::parse_u32(value, "--numa-id"));
        } else if (arg == "--region-mb") {
            options.region_mb = obmm_test::parse_u64(value, "--region-mb");
        } else if (arg == "--test-bytes") {
            options.test_bytes = obmm_test::parse_u64(value, "--test-bytes");
        } else if (arg == "--iterations") {
            options.iterations = obmm_test::parse_u64(value, "--iterations");
        } else {
            obmm_test::fail("unknown option: " + arg);
        }
    }
    if (!options.have_eid)
        obmm_test::fail("--local-eid is required");
    if (options.numa_id < 0 || options.numa_id >= OBMM_MAX_LOCAL_NUMA_NODES)
        obmm_test::fail("--numa-id is outside OBMM_MAX_LOCAL_NUMA_NODES");
    const uint64_t region_bytes = obmm_test::mib_to_bytes(options.region_mb);
    if (options.test_bytes < sizeof(uint64_t) || options.test_bytes > region_bytes)
        obmm_test::fail("--test-bytes must be at least 8 and no larger than the region");
    if (options.iterations == 0)
        obmm_test::fail("--iterations must be non-zero");
    return options;
}

int run(const Options &options)
{
    const uint64_t requested_length = obmm_test::mib_to_bytes(options.region_mb);
    size_t lengths[OBMM_MAX_LOCAL_NUMA_NODES]{};
    lengths[options.numa_id] = static_cast<size_t>(requested_length);

    obmm_mem_desc descriptor{};
    obmm_test::encode_local_eid(descriptor.deid, options.local_eid);
    descriptor.priv_len = 0;
    const mem_id export_id =
        obmm_export(lengths, OBMM_EXPORT_FLAG_ALLOW_MMAP, &descriptor);
    if (export_id == OBMM_INVALID_MEMID)
        obmm_test::fail(obmm_test::errno_message("obmm_export"));

    obmm_test::NcMapping mapping;
    try {
        if (descriptor.length < options.test_bytes)
            obmm_test::fail("exported region is smaller than --test-bytes");
        mapping = obmm_test::map_nc(export_id, static_cast<size_t>(descriptor.length));
        std::cout << "local export: mem_id="
                  << static_cast<unsigned long long>(export_id)
                  << ", numa_id=" << options.numa_id
                  << ", UBA=0x" << std::hex << descriptor.addr << std::dec
                  << ", length=" << descriptor.length
                  << ", device=" << obmm_test::shmdev_path(export_id)
                  << ", NC_virtual_address=" << mapping.address << std::endl;

        obmm_test::write_pattern(mapping.address,
                                 static_cast<size_t>(options.test_bytes), 0x53);
        obmm_test::verify_pattern(mapping.address,
                                  static_cast<size_t>(options.test_bytes), 0x53,
                                  "local NC load/store correctness");
        std::cout << "PASS local NC load/store correctness ("
                  << options.test_bytes << " bytes)" << std::endl;

        auto *word = static_cast<volatile uint64_t *>(mapping.address);
        *word = 1;
        __sync_synchronize();
        uint64_t sink = 0;
        auto start = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < options.iterations; ++i)
            sink ^= *word;
        const double load_ns = obmm_test::elapsed_ns(start, options.iterations);

        start = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < options.iterations; ++i) {
            *word = i;
            __sync_synchronize();
        }
        const double store_ns = obmm_test::elapsed_ns(start, options.iterations);
        if (*word != options.iterations - 1)
            obmm_test::fail("local fenced store final-value check failed");

        std::cout << "local_nc_load_avg_ns=" << load_ns
                  << ", local_nc_fenced_store_avg_ns=" << store_ns
                  << ", iterations=" << options.iterations
                  << ", load_sink=" << sink << std::endl;

        obmm_test::unmap_nc(mapping);
        if (obmm_unexport(export_id, 0) != 0)
            obmm_test::fail(obmm_test::errno_message("obmm_unexport"));
        std::cout << "PASS local OBMM NC test and cleanup" << std::endl;
        return EXIT_SUCCESS;
    } catch (...) {
        obmm_test::best_effort_unmap(mapping);
        obmm_unexport(export_id, 0);
        throw;
    }
}

} // namespace

int main(int argc, char **argv)
{
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
}
