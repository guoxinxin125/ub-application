#include "ubsm_benchmark_common.h"

namespace {

constexpr uint8_t kOldSeed = 0x28;
constexpr uint8_t kNewSeed = 0xc7;

void run_owner(const ubsm_bench::TwoNodeOptions &options,
               ubsm_test::SharedMemory &memory, int connection) {
  ubsm_test::write_pattern(ubsm_bench::pattern_at(memory), options.test_bytes,
                           kOldSeed);
  ubsm_test::verify_pattern(ubsm_bench::pattern_at(memory), options.test_bytes,
                            kOldSeed, "owner initial cached pattern");
  ubsm_test::expect_stage(connection, 'H');
  ubsm_test::send_stage(connection, 'R');
  ubsm_test::expect_stage(connection, 'D');

  // Owner deliberately performs no invalidate before reading.
  ubsm_test::verify_pattern(ubsm_bench::pattern_at(memory), options.test_bytes,
                            kNewSeed, "remote NC store observed by owner");
  std::cout << "PASS remote NC store -> owner load without invalidate\n";
  ubsm_test::send_stage(connection, 'V');
  ubsm_bench::owner_cleanup(memory, connection);
}

void run_remote(const ubsm_bench::TwoNodeOptions &options,
                ubsm_test::SharedMemory &memory, int connection) {
  memory.map();
  ubsm_test::send_stage(connection, 'H');
  ubsm_test::expect_stage(connection, 'R');

  // NC mapping has no cache to write back. DSB waits for store completion.
  ubsm_test::write_pattern(ubsm_bench::pattern_at(memory), options.test_bytes,
                           kNewSeed);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  ubsm_test::send_stage(connection, 'D');
  ubsm_test::expect_stage(connection, 'V');
  std::cout << "PASS remote NC write and completion fence\n";
  ubsm_bench::remote_cleanup(memory, connection);
}

} // namespace

int main(int argc, char **argv) {
  int connection = -1;
  try {
    const auto options = ubsm_bench::parse_two_node_options(
        argc, argv, "ubsm_remote_to_owner_cc", 18534);
    const uint64_t region_bytes =
        ubsm_test::region_bytes_from_mb(options.region_mb);
    ubsm_test::SdkSession session;
    ubsm_test::SharedMemory memory(options.name, region_bytes);
    if (options.role == "owner")
      ubsm_bench::allocate_owner(memory, options);
    connection = ubsm_bench::open_connection(options);

    if (options.role == "owner")
      run_owner(options, memory, connection);
    else
      run_remote(options, memory, connection);

    close(connection);
    connection = -1;
    session.finalize();
    std::cout << "PASS remote-to-owner CC test " << options.role
              << " and cleanup\n";
    return 0;
  } catch (const std::exception &error) {
    if (connection >= 0)
      close(connection);
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
