#include "obmm_test_common.h"

namespace {

struct Options {
    std::string bind_ip;
    uint16_t port = 18515;
    uint32_t local_eid = 0;
    int numa_id = 0;
    uint64_t region_mb = 2;
    uint64_t test_bytes = 4096;
    int timeout_sec = 120;
    bool have_eid = false;
};

void usage(const char *program)
{
    std::cerr << "Usage: " << program
              << " --bind-ip IP --local-eid EID [--port N] [--numa-id N]"
              << " [--region-mb N] [--test-bytes N] [--timeout-sec N]\n";
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
        if (arg == "--bind-ip")
            options.bind_ip = value;
        else if (arg == "--port")
            options.port = obmm_test::parse_port(value);
        else if (arg == "--local-eid") {
            options.local_eid = obmm_test::parse_u32(value, "--local-eid");
            options.have_eid = true;
        } else if (arg == "--numa-id")
            options.numa_id = static_cast<int>(obmm_test::parse_u32(value, "--numa-id"));
        else if (arg == "--region-mb")
            options.region_mb = obmm_test::parse_u64(value, "--region-mb");
        else if (arg == "--test-bytes")
            options.test_bytes = obmm_test::parse_u64(value, "--test-bytes");
        else if (arg == "--timeout-sec")
            options.timeout_sec = static_cast<int>(obmm_test::parse_u32(value, "--timeout-sec"));
        else
            obmm_test::fail("unknown option: " + arg);
    }
    if (options.bind_ip.empty() || !options.have_eid)
        obmm_test::fail("--bind-ip and --local-eid are required");
    if (options.numa_id < 0 || options.numa_id >= OBMM_MAX_LOCAL_NUMA_NODES)
        obmm_test::fail("--numa-id is outside OBMM_MAX_LOCAL_NUMA_NODES");
    const uint64_t region_bytes = obmm_test::mib_to_bytes(options.region_mb);
    if (options.test_bytes == 0 || options.test_bytes > region_bytes)
        obmm_test::fail("--test-bytes must be non-zero and no larger than the region");
    if (options.timeout_sec <= 0)
        obmm_test::fail("--timeout-sec must be non-zero");
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
    int control_fd = -1;
    try {
        if (descriptor.length < options.test_bytes)
            obmm_test::fail("exported region is smaller than --test-bytes");
        mapping = obmm_test::map_nc(export_id, static_cast<size_t>(descriptor.length));
        std::cout << "export created: mem_id="
                  << static_cast<unsigned long long>(export_id)
                  << ", numa_id=" << options.numa_id
                  << ", UBA=0x" << std::hex << descriptor.addr << std::dec
                  << ", length=" << descriptor.length
                  << ", device=" << obmm_test::shmdev_path(export_id)
                  << ", NC_virtual_address=" << mapping.address << std::endl;

        control_fd = obmm_test::accept_importer(options.bind_ip, options.port,
                                                options.timeout_sec);
        obmm_test::ExportDescription wire;
        wire.addr = descriptor.addr;
        wire.length = descriptor.length;
        wire.tokenid = descriptor.tokenid;
        wire.dcna = descriptor.dcna;
        std::memcpy(wire.deid, descriptor.deid, sizeof(wire.deid));
        obmm_test::send_description(control_fd, wire);
        obmm_test::expect_control(control_fd, obmm_test::Control::importer_ready);

        obmm_test::write_pattern(mapping.address,
                                 static_cast<size_t>(options.test_bytes), 0x31);
        obmm_test::send_control(control_fd, obmm_test::Control::exporter_written);
        obmm_test::expect_control(control_fd, obmm_test::Control::importer_read_ok);
        std::cout << "PASS exporter NC store -> importer NC load ("
                  << options.test_bytes << " bytes)" << std::endl;

        obmm_test::expect_control(control_fd, obmm_test::Control::importer_written);
        obmm_test::verify_pattern(mapping.address,
                                  static_cast<size_t>(options.test_bytes), 0xa7,
                                  "importer store -> exporter load");
        obmm_test::send_control(control_fd, obmm_test::Control::exporter_read_ok);
        std::cout << "PASS importer NC store -> exporter NC load ("
                  << options.test_bytes << " bytes)" << std::endl;

        obmm_test::expect_control(control_fd, obmm_test::Control::importer_cleaned);
        close(control_fd);
        control_fd = -1;
        obmm_test::unmap_nc(mapping);
        if (obmm_unexport(export_id, 0) != 0)
            obmm_test::fail(obmm_test::errno_message("obmm_unexport"));
        std::cout << "PASS two-node OBMM NC exporter and cleanup" << std::endl;
        return EXIT_SUCCESS;
    } catch (...) {
        if (control_fd >= 0)
            close(control_fd);
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
