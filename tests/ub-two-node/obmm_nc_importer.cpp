#include "obmm_test_common.h"

namespace {

struct Options {
    std::string owner_ip;
    uint16_t port = 18515;
    uint32_t local_eid = 0;
    uint32_t local_scna = 0;
    uint64_t test_bytes = 4096;
    int timeout_sec = 120;
    bool have_eid = false;
    bool have_scna = false;
};

void usage(const char *program)
{
    std::cerr << "Usage: " << program
              << " --owner-ip IP --local-eid EID --local-scna SCNA"
              << " [--port N] [--test-bytes N] [--timeout-sec N]\n";
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
        if (arg == "--owner-ip")
            options.owner_ip = value;
        else if (arg == "--port")
            options.port = obmm_test::parse_port(value);
        else if (arg == "--local-eid") {
            options.local_eid = obmm_test::parse_u32(value, "--local-eid");
            options.have_eid = true;
        } else if (arg == "--local-scna") {
            options.local_scna = obmm_test::parse_u32(value, "--local-scna");
            options.have_scna = true;
        } else if (arg == "--test-bytes")
            options.test_bytes = obmm_test::parse_u64(value, "--test-bytes");
        else if (arg == "--timeout-sec")
            options.timeout_sec = static_cast<int>(obmm_test::parse_u32(value, "--timeout-sec"));
        else
            obmm_test::fail("unknown option: " + arg);
    }
    if (options.owner_ip.empty() || !options.have_eid || !options.have_scna)
        obmm_test::fail("--owner-ip, --local-eid, and --local-scna are required");
    if (options.test_bytes == 0)
        obmm_test::fail("--test-bytes must be non-zero");
    if (options.timeout_sec <= 0)
        obmm_test::fail("--timeout-sec must be non-zero");
    return options;
}

int run(const Options &options)
{
    int control_fd = obmm_test::connect_to_exporter(options.owner_ip, options.port,
                                                    options.timeout_sec);
    const obmm_test::ExportDescription wire =
        obmm_test::receive_description(control_fd);
    if (wire.length < options.test_bytes) {
        close(control_fd);
        obmm_test::fail("exported region is smaller than --test-bytes");
    }

    obmm_mem_desc descriptor{};
    descriptor.addr = wire.addr;
    descriptor.length = wire.length;
    descriptor.tokenid = wire.tokenid;
    descriptor.dcna = wire.dcna;
    std::memcpy(descriptor.deid, wire.deid, sizeof(descriptor.deid));
    obmm_test::encode_local_eid(descriptor.seid, options.local_eid);
    descriptor.scna = options.local_scna;
    descriptor.priv_len = 0;

    const mem_id import_id =
        obmm_import(&descriptor, OBMM_IMPORT_FLAG_ALLOW_MMAP, 0, nullptr);
    if (import_id == OBMM_INVALID_MEMID) {
        close(control_fd);
        obmm_test::fail(obmm_test::errno_message("obmm_import"));
    }

    obmm_test::NcMapping mapping;
    try {
        mapping = obmm_test::map_nc(import_id, static_cast<size_t>(wire.length));
        std::cout << "import created: local_mem_id="
                  << static_cast<unsigned long long>(import_id)
                  << ", remote_UBA=0x" << std::hex << wire.addr << std::dec
                  << ", length=" << wire.length
                  << ", device=" << obmm_test::shmdev_path(import_id)
                  << ", NC_virtual_address=" << mapping.address << std::endl;
        obmm_test::send_control(control_fd, obmm_test::Control::importer_ready);

        obmm_test::expect_control(control_fd, obmm_test::Control::exporter_written);
        obmm_test::verify_pattern(mapping.address,
                                  static_cast<size_t>(options.test_bytes), 0x31,
                                  "exporter store -> importer load");
        obmm_test::send_control(control_fd, obmm_test::Control::importer_read_ok);
        std::cout << "PASS importer observed exporter pattern" << std::endl;

        obmm_test::write_pattern(mapping.address,
                                 static_cast<size_t>(options.test_bytes), 0xa7);
        obmm_test::send_control(control_fd, obmm_test::Control::importer_written);
        obmm_test::expect_control(control_fd, obmm_test::Control::exporter_read_ok);
        std::cout << "PASS importer NC store observed by exporter" << std::endl;

        obmm_test::unmap_nc(mapping);
        if (obmm_unimport(import_id, 0) != 0)
            obmm_test::fail(obmm_test::errno_message("obmm_unimport"));
        obmm_test::send_control(control_fd, obmm_test::Control::importer_cleaned);
        close(control_fd);
        std::cout << "PASS two-node OBMM NC importer and cleanup" << std::endl;
        return EXIT_SUCCESS;
    } catch (...) {
        obmm_test::best_effort_unmap(mapping);
        obmm_unimport(import_id, 0);
        close(control_fd);
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
