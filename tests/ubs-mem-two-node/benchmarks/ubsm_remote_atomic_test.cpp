#include "ubsm_benchmark_common.h"

namespace {

constexpr uint64_t kAtomicBase = 0x100000ULL;
constexpr uint64_t kAtomicMagic = 0xa5a5a5a55a5a5a5aULL;

void run_owner(const ubsm_bench::TwoNodeOptions &options,
               ubsm_test::SharedMemory &memory, int connection) {
  uint64_t *word = ubsm_bench::atomic_word(memory);
  __atomic_store_n(word, kAtomicBase, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != kAtomicBase)
    ubsm_test::fail("failed to initialize and cache owner atomic word");
  std::atomic_thread_fence(std::memory_order_seq_cst);

  ubsm_test::expect_stage(connection, 'H');
  ubsm_test::send_stage(connection, 'R');
  ubsm_test::expect_stage(connection, 'D');
  const uint64_t actual = __atomic_load_n(word, __ATOMIC_SEQ_CST);
  if (actual != kAtomicMagic) {
    ubsm_test::fail("owner observed wrong remote atomic result: expected=" +
                    std::to_string(kAtomicMagic) +
                    ", actual=" + std::to_string(actual));
  }
  std::cout
      << "PASS remote fetch-add/success-CAS/failed-CAS observed by owner\n";
  ubsm_test::send_stage(connection, 'V');
  ubsm_bench::owner_cleanup(memory, connection);
}

void run_remote(const ubsm_bench::TwoNodeOptions &options,
                ubsm_test::SharedMemory &memory, int connection) {
  memory.map();
  ubsm_test::send_stage(connection, 'H');
  ubsm_test::expect_stage(connection, 'R');
  ubsm_bench::run_atomic_validation(memory, kAtomicBase,
                                    options.atomic_iterations, kAtomicMagic);
  std::cout << "PASS remote atomic instruction return values\n";
  ubsm_test::send_stage(connection, 'D');
  ubsm_test::expect_stage(connection, 'V');
  ubsm_bench::remote_cleanup(memory, connection);
}

} // namespace

int main(int argc, char **argv) {
  int connection = -1;
  try {
    const auto options = ubsm_bench::parse_two_node_options(
        argc, argv, "ubsm_remote_atomic", 18532);
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
    std::cout << "PASS remote atomic test " << options.role << " and cleanup\n";
    return 0;
  } catch (const std::exception &error) {
    if (connection >= 0)
      close(connection);
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
