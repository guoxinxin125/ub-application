#include "urma_test_common.h"

#include "ubsm_test_common.h"

namespace {

struct Options {
  std::string role;
  std::string device;
  std::string name = "ubsm_urma_compat";
  std::string bind_ip = "0.0.0.0";
  std::string server_ip;
  std::string provider_host;
  uint16_t port = 18621;
  uint32_t provider_socket = UINT32_MAX;
  uint32_t provider_numa = UINT32_MAX;
  uint32_t provider_port = UINT32_MAX;
  uint32_t eid_index = std::numeric_limits<uint32_t>::max();
  uint32_t tp_type = 0;
  uint64_t region_mb = 4;
  uint64_t iterations = 1000;
  uint64_t warmup = 100;
  std::vector<size_t> sizes{64, 128, 256, 512, 1024, 1300, 2048, 4096};
};

void usage(const char *program) {
  std::cerr << "Usage: " << program
            << " --role owner|remote --device DEV --name NAME"
               " [--bind-ip IP] [--server-ip IP] [--port N]"
               " [--provider-host HOST] [--provider-socket N]"
               " [--provider-numa N] [--provider-port N] [--region-mb N]"
               " [--eid-index N] [--tp-type 0|1|2]"
               " [--iterations N] [--warmup N] [--sizes A,B,...]\n";
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
      urma_test::fail("missing value for " + arg);
    const std::string value(argv[++i]);
    if (arg == "--role")
      options.role = value;
    else if (arg == "--device")
      options.device = value;
    else if (arg == "--name")
      options.name = value;
    else if (arg == "--bind-ip")
      options.bind_ip = value;
    else if (arg == "--server-ip")
      options.server_ip = value;
    else if (arg == "--port")
      options.port = urma_test::parse_port(value);
    else if (arg == "--provider-host")
      options.provider_host = value;
    else if (arg == "--provider-socket")
      options.provider_socket = urma_test::parse_u32(value, arg);
    else if (arg == "--provider-numa")
      options.provider_numa = urma_test::parse_u32(value, arg);
    else if (arg == "--provider-port")
      options.provider_port = urma_test::parse_u32(value, arg);
    else if (arg == "--region-mb")
      options.region_mb = urma_test::parse_u64(value, arg);
    else if (arg == "--eid-index")
      options.eid_index = urma_test::parse_u32(value, arg);
    else if (arg == "--tp-type")
      options.tp_type = urma_test::parse_u32(value, arg);
    else if (arg == "--iterations")
      options.iterations = urma_test::parse_u64(value, arg);
    else if (arg == "--warmup")
      options.warmup = urma_test::parse_u64(value, arg);
    else if (arg == "--sizes")
      options.sizes = urma_test::parse_sizes(value);
    else
      urma_test::fail("unknown option: " + arg);
  }
  if (options.role != "owner" && options.role != "remote")
    urma_test::fail("--role must be owner or remote");
  if (options.device.empty())
    urma_test::fail("--device is required");
  if (options.role == "remote" && options.server_ip.empty())
    urma_test::fail("remote requires --server-ip");
  if (options.iterations == 0)
    urma_test::fail("--iterations must be positive");
  ubsm_test::validate_name(options.name);
  const uint64_t region_bytes =
      ubsm_test::region_bytes_from_mb(options.region_mb);
  const size_t maximum =
      *std::max_element(options.sizes.begin(), options.sizes.end());
  if (maximum > region_bytes)
    urma_test::fail("largest --sizes entry exceeds UBSM region size");
  (void)urma_test::parse_tp_type(options.tp_type);
  return options;
}

void allocate_owner(ubsm_test::SharedMemory &memory, Options *options) {
  if (options->provider_host.empty()) {
    char hostname[MAX_HOST_NAME_DESC_LENGTH]{};
    if (gethostname(hostname, sizeof(hostname)) != 0)
      urma_test::fail("gethostname failed");
    options->provider_host = hostname;
  }
  memory.allocate_with_provider(options->provider_host,
                                options->provider_socket,
                                options->provider_numa, options->provider_port);
  memory.map();
}

void run_owner(const Options &options, int fd, void *shared_address,
               size_t maximum_size) {
  urma_test::fill_pattern(shared_address, maximum_size, 0x35);
  urma_test::send_stage(fd, 'R');
  urma_test::expect_stage(fd, 'R');

  for (size_t index = 0; index < options.sizes.size(); ++index) {
    urma_test::expect_stage(fd, 'W');
    const uint8_t seed = static_cast<uint8_t>(0x80 + index);
    urma_test::verify_pattern(shared_address, options.sizes[index], seed,
                              "owner CPU visibility after URMA WRITE");
    urma_test::send_stage(fd, 'A');
  }
  urma_test::expect_stage(fd, 'D');
  urma_test::send_stage(fd, 'D');
}

void run_remote(const Options &options, int fd, urma_test::Session &session,
                void *local_address, void *shared_address,
                size_t maximum_size) {
  urma_test::expect_stage(fd, 'R');
  urma_test::verify_pattern(shared_address, maximum_size, 0x35,
                            "remote CPU UBSM visibility before URMA READ");

  for (size_t bytes : options.sizes) {
    std::vector<uint64_t> samples;
    samples.reserve(static_cast<size_t>(options.iterations));
    for (uint64_t i = 0; i < options.warmup + options.iterations; ++i) {
      const uint64_t elapsed =
          session.post_read(local_address, session.remote_base(), bytes);
      if (i >= options.warmup)
        samples.push_back(elapsed);
    }
    urma_test::verify_pattern(local_address, bytes, 0x35,
                              "URMA READ from UBSM owner mapping");
    urma_test::print_stats("urma_read_from_ubsm", bytes, samples);
  }
  urma_test::send_stage(fd, 'R');

  for (size_t index = 0; index < options.sizes.size(); ++index) {
    const size_t bytes = options.sizes[index];
    const uint8_t seed = static_cast<uint8_t>(0x80 + index);
    urma_test::fill_pattern(local_address, bytes, seed);
    std::vector<uint64_t> samples;
    samples.reserve(static_cast<size_t>(options.iterations));
    for (uint64_t i = 0; i < options.warmup + options.iterations; ++i) {
      const uint64_t elapsed =
          session.post_write(local_address, session.remote_base(), bytes);
      if (i >= options.warmup)
        samples.push_back(elapsed);
    }
    urma_test::print_stats("urma_write_to_ubsm", bytes, samples);
    urma_test::send_stage(fd, 'W');
    urma_test::expect_stage(fd, 'A');
    urma_test::verify_pattern(shared_address, bytes, seed,
                              "remote CPU UBSM visibility after URMA WRITE");
  }
  urma_test::send_stage(fd, 'D');
  urma_test::expect_stage(fd, 'D');
}

} // namespace

int main(int argc, char **argv) {
  int fd = -1;
  try {
    Options options = parse_options(argc, argv);
    const bool owner = options.role == "owner";
    const uint64_t region_bytes =
        ubsm_test::region_bytes_from_mb(options.region_mb);
    const size_t maximum_size =
        *std::max_element(options.sizes.begin(), options.sizes.end());

    ubsm_test::SdkSession ubsm_session;
    ubsm_test::SharedMemory memory(options.name, region_bytes);
    if (owner)
      allocate_owner(memory, &options);
    fd = owner ? urma_test::listen_and_accept(options.bind_ip, options.port)
               : urma_test::connect_with_retry(options.server_ip, options.port);
    if (!owner)
      memory.map();
    void *shared_address = const_cast<uint8_t *>(memory.bytes());

    // The owner deliberately registers the exact UBSM mapping. A failure here
    // is the compatibility result, not a generic URMA heap-buffer failure.
    urma_test::AlignedBuffer local_buffer(maximum_size);
    urma_test::Session urma_session(options.device, options.eid_index,
                                    urma_test::parse_tp_type(options.tp_type));
    if (owner) {
      urma_session.register_buffer(shared_address, region_bytes);
      std::cout << "PASS urma_register_seg accepted UBSM owner mapping address="
                << shared_address << " bytes=" << region_bytes << '\n';
    } else {
      urma_session.register_buffer(local_buffer.data(), local_buffer.size());
    }
    urma_session.connect_peer(fd, owner);

    if (owner)
      run_owner(options, fd, shared_address, maximum_size);
    else
      run_remote(options, fd, urma_session, local_buffer.data(), shared_address,
                 maximum_size);

    close(fd);
    fd = -1;
    std::cout << "PASS UBSM/URMA registration and visibility test role="
              << options.role << '\n';
    return 0;
  } catch (const std::exception &error) {
    if (fd >= 0)
      close(fd);
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
