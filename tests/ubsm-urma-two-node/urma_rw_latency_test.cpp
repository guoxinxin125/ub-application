#include "urma_test_common.h"

namespace {

struct Options {
  std::string role;
  std::string device;
  std::string bind_ip = "0.0.0.0";
  std::string server_ip;
  uint16_t port = 18620;
  uint32_t eid_index = std::numeric_limits<uint32_t>::max();
  uint32_t tp_type = 0;
  uint64_t iterations = 1000;
  uint64_t warmup = 100;
  std::vector<size_t> sizes{64, 128, 256, 512, 1024, 1300, 2048, 4096};
};

void usage(const char *program) {
  std::cerr << "Usage: " << program
            << " --role server|client --device DEV"
               " [--bind-ip IP] [--server-ip IP] [--port N]"
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
    else if (arg == "--bind-ip")
      options.bind_ip = value;
    else if (arg == "--server-ip")
      options.server_ip = value;
    else if (arg == "--port")
      options.port = urma_test::parse_port(value);
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
  if (options.role != "server" && options.role != "client")
    urma_test::fail("--role must be server or client");
  if (options.device.empty())
    urma_test::fail("--device is required");
  if (options.role == "client" && options.server_ip.empty())
    urma_test::fail("client requires --server-ip");
  if (options.iterations == 0)
    urma_test::fail("--iterations must be positive");
  (void)urma_test::parse_tp_type(options.tp_type);
  return options;
}

void run_server(const Options &options, int fd, void *buffer,
                size_t buffer_size) {
  urma_test::fill_pattern(buffer, buffer_size, 0x35);
  urma_test::send_stage(fd, 'R');
  urma_test::expect_stage(fd, 'R');

  for (size_t index = 0; index < options.sizes.size(); ++index) {
    urma_test::expect_stage(fd, 'W');
    const uint8_t seed = static_cast<uint8_t>(0x80 + index);
    urma_test::verify_pattern(buffer, options.sizes[index], seed,
                              "URMA WRITE destination");
    urma_test::send_stage(fd, 'A');
  }
  urma_test::expect_stage(fd, 'D');
  urma_test::send_stage(fd, 'D');
}

void run_client(const Options &options, int fd, urma_test::Session &session,
                void *buffer) {
  urma_test::expect_stage(fd, 'R');
  for (size_t bytes : options.sizes) {
    std::vector<uint64_t> samples;
    samples.reserve(static_cast<size_t>(options.iterations));
    for (uint64_t i = 0; i < options.warmup + options.iterations; ++i) {
      const uint64_t elapsed =
          session.post_read(buffer, session.remote_base(), bytes);
      if (i >= options.warmup)
        samples.push_back(elapsed);
    }
    urma_test::verify_pattern(buffer, bytes, 0x35, "URMA READ result");
    urma_test::print_stats("urma_read_post_to_completion", bytes, samples);
  }
  urma_test::send_stage(fd, 'R');

  for (size_t index = 0; index < options.sizes.size(); ++index) {
    const size_t bytes = options.sizes[index];
    const uint8_t seed = static_cast<uint8_t>(0x80 + index);
    urma_test::fill_pattern(buffer, bytes, seed);
    std::vector<uint64_t> samples;
    samples.reserve(static_cast<size_t>(options.iterations));
    for (uint64_t i = 0; i < options.warmup + options.iterations; ++i) {
      const uint64_t elapsed =
          session.post_write(buffer, session.remote_base(), bytes);
      if (i >= options.warmup)
        samples.push_back(elapsed);
    }
    urma_test::print_stats("urma_write_post_to_completion", bytes, samples);
    urma_test::send_stage(fd, 'W');
    urma_test::expect_stage(fd, 'A');
  }
  urma_test::send_stage(fd, 'D');
  urma_test::expect_stage(fd, 'D');
}

} // namespace

int main(int argc, char **argv) {
  int fd = -1;
  try {
    const Options options = parse_options(argc, argv);
    const size_t buffer_size =
        *std::max_element(options.sizes.begin(), options.sizes.end());
    urma_test::AlignedBuffer buffer(buffer_size);
    urma_test::Session session(options.device, options.eid_index,
                               urma_test::parse_tp_type(options.tp_type));
    session.register_buffer(buffer.data(), buffer.size());
    const bool server = options.role == "server";
    fd = server
             ? urma_test::listen_and_accept(options.bind_ip, options.port)
             : urma_test::connect_with_retry(options.server_ip, options.port);
    session.connect_peer(fd, server);
    if (server)
      run_server(options, fd, buffer.data(), buffer.size());
    else
      run_client(options, fd, session, buffer.data());
    close(fd);
    std::cout << "PASS pure URMA read/write latency test role=" << options.role
              << '\n';
    return 0;
  } catch (const std::exception &error) {
    if (fd >= 0)
      close(fd);
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
