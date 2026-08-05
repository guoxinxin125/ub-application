#include "ubsm_test_common.h"

namespace {

struct Options {
  char role = '\0';
  std::string bind_ip;
  std::string peer_ip;
  std::string provider_host;
  std::string name_prefix = "ubsm_bidir";
  uint16_t port = 18526;
  uint64_t region_mb = 4;
  uint64_t test_bytes = 4096;
  int timeout_sec = 60;
  uint32_t provider_socket = UINT32_MAX;
  uint32_t provider_numa = UINT32_MAX;
  uint32_t provider_port = UINT32_MAX;
};

void usage(const char *program) {
  std::cerr
      << "Usage: " << program << " --role a --bind-ip IP [options]\n"
      << "       " << program << " --role b --peer-ip IP [options]\n"
      << "Options:\n"
      << "  --provider-host HOST  --provider-socket N  --provider-numa N\n"
      << "  --provider-port N     --name-prefix PREFIX --port N\n"
      << "  --region-mb N         --test-bytes N        --timeout-sec N\n";
}

Options parse_options(int argc, char **argv) {
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
    if (arg == "--role") {
      if (value != "a" && value != "b")
        ubsm_test::fail("--role must be a or b");
      options.role = value[0];
    } else if (arg == "--bind-ip") {
      options.bind_ip = value;
    } else if (arg == "--peer-ip") {
      options.peer_ip = value;
    } else if (arg == "--provider-host") {
      options.provider_host = value;
    } else if (arg == "--provider-socket") {
      options.provider_socket = ubsm_test::parse_u32(value, arg);
    } else if (arg == "--provider-numa") {
      options.provider_numa = ubsm_test::parse_u32(value, arg);
    } else if (arg == "--provider-port") {
      options.provider_port = ubsm_test::parse_u32(value, arg);
    } else if (arg == "--name-prefix") {
      options.name_prefix = value;
    } else if (arg == "--port") {
      options.port = ubsm_test::parse_port(value);
    } else if (arg == "--region-mb") {
      options.region_mb = ubsm_test::parse_u64(value, arg);
    } else if (arg == "--test-bytes") {
      options.test_bytes = ubsm_test::parse_u64(value, arg);
    } else if (arg == "--timeout-sec") {
      options.timeout_sec = ubsm_test::parse_positive_int(value, arg);
    } else {
      ubsm_test::fail("unknown option: " + arg);
    }
  }

  if (options.role == '\0')
    ubsm_test::fail("--role is required");
  if (options.role == 'a' && options.bind_ip.empty())
    ubsm_test::fail("role a requires --bind-ip");
  if (options.role == 'b' && options.peer_ip.empty())
    ubsm_test::fail("role b requires --peer-ip");

  if (options.provider_host.empty()) {
    char hostname[MAX_HOST_NAME_DESC_LENGTH]{};
    if (gethostname(hostname, sizeof(hostname)) != 0)
      ubsm_test::fail(ubsm_test::errno_message("gethostname"));
    if (hostname[sizeof(hostname) - 1] != '\0')
      ubsm_test::fail("local hostname is too long");
    options.provider_host = hostname;
  }

  ubsm_test::validate_name(options.name_prefix + "_a_inbox");
  ubsm_test::validate_name(options.name_prefix + "_b_inbox");
  ubsm_test::validate_sizes(ubsm_test::region_bytes_from_mb(options.region_mb),
                            options.test_bytes);
  return options;
}

void prepare_local_owner_memory(ubsm_test::SharedMemory &memory,
                                uint64_t test_bytes, char role) {
  constexpr uint8_t kInitialSeed = 0x5a;
  memory.map();
  ubsm_test::write_pattern(memory.bytes(), test_bytes, kInitialSeed);
  ubsm_test::verify_pattern(memory.bytes(), test_bytes, kInitialSeed,
                            "initial local owner pattern");
  std::cout << "PASS node " << role
            << " local owner memory initialized and cached (" << test_bytes
            << " bytes)\n";
}

void run_role_a(const Options &options, ubsm_test::SharedMemory &local_memory,
                ubsm_test::SharedMemory &peer_memory, int &connection) {
  connection = ubsm_test::accept_remote(options.bind_ip, options.port,
                                        options.timeout_sec);
  ubsm_test::expect_stage(connection, 'R');
  peer_memory.map();
  ubsm_test::send_stage(connection, 'M');
  ubsm_test::expect_stage(connection, 'N');

  ubsm_test::write_pattern(peer_memory.bytes(), options.test_bytes, 0x31);
  ubsm_test::send_stage(connection, 'Q');
  ubsm_test::expect_stage(connection, 'S');

  ubsm_test::verify_pattern(local_memory.bytes(), options.test_bytes, 0xc4,
                            "B remote NC store observed by A owner");
  std::cout << "PASS B remote NC store -> A owner cacheable load ("
            << options.test_bytes << " bytes)\n";
  ubsm_test::send_stage(connection, 'V');

  ubsm_test::expect_stage(connection, 'U');
  peer_memory.unmap();
  ubsm_test::send_stage(connection, 'W');
  ubsm_test::expect_stage(connection, 'D');

  local_memory.unmap();
  local_memory.deallocate();
  ubsm_test::send_stage(connection, 'E');
}

void run_role_b(const Options &options, ubsm_test::SharedMemory &local_memory,
                ubsm_test::SharedMemory &peer_memory, int &connection) {
  connection = ubsm_test::connect_to_owner(options.peer_ip, options.port,
                                           options.timeout_sec);
  ubsm_test::send_stage(connection, 'R');
  ubsm_test::expect_stage(connection, 'M');
  peer_memory.map();
  ubsm_test::send_stage(connection, 'N');

  ubsm_test::expect_stage(connection, 'Q');
  ubsm_test::verify_pattern(local_memory.bytes(), options.test_bytes, 0x31,
                            "A remote NC store observed by B owner");
  std::cout << "PASS A remote NC store -> B owner cacheable load ("
            << options.test_bytes << " bytes)\n";

  ubsm_test::write_pattern(peer_memory.bytes(), options.test_bytes, 0xc4);
  ubsm_test::send_stage(connection, 'S');
  ubsm_test::expect_stage(connection, 'V');

  peer_memory.unmap();
  ubsm_test::send_stage(connection, 'U');
  ubsm_test::expect_stage(connection, 'W');
  local_memory.unmap();
  local_memory.deallocate();
  ubsm_test::send_stage(connection, 'D');
  ubsm_test::expect_stage(connection, 'E');
}

} // namespace

int main(int argc, char **argv) {
  int connection = -1;
  try {
    const Options options = parse_options(argc, argv);
    const uint64_t region_bytes =
        ubsm_test::region_bytes_from_mb(options.region_mb);
    const std::string a_name = options.name_prefix + "_a_inbox";
    const std::string b_name = options.name_prefix + "_b_inbox";
    const std::string local_name = options.role == 'a' ? a_name : b_name;
    const std::string peer_name = options.role == 'a' ? b_name : a_name;

    ubsm_test::SdkSession session;
    ubsm_test::SharedMemory local_memory(local_name, region_bytes);
    ubsm_test::SharedMemory peer_memory(peer_name, region_bytes);
    local_memory.allocate_with_provider(
        options.provider_host, options.provider_socket, options.provider_numa,
        options.provider_port);
    prepare_local_owner_memory(local_memory, options.test_bytes, options.role);

    if (options.role == 'a')
      run_role_a(options, local_memory, peer_memory, connection);
    else
      run_role_b(options, local_memory, peer_memory, connection);

    close(connection);
    connection = -1;
    session.finalize();
    std::cout << "PASS bidirectional node " << options.role
              << " test and cleanup\n";
    return 0;
  } catch (const std::exception &error) {
    if (connection >= 0)
      close(connection);
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
