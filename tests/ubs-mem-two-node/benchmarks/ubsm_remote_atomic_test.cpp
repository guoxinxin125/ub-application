#include "ubsm_benchmark_common.h"

namespace {

constexpr uint64_t kAtomicBase = 0x100000ULL;
constexpr uint64_t kAtomicMagic = 0xa5a5a5a55a5a5a5aULL;

void run_owner(const ubsm_bench::TwoNodeOptions &options,
               ubsm_test::SharedMemory &memory, int connection) {
  uint64_t *word = ubsm_bench::atomic_word(memory);
  uint64_t *fetch_add_word = ubsm_bench::atomic_word_at(
      memory, ubsm_bench::kRemoteFetchAddOffset);
  uint64_t *cas_word =
      ubsm_bench::atomic_word_at(memory, ubsm_bench::kRemoteCasOffset);
  __atomic_store_n(word, kAtomicBase, __ATOMIC_SEQ_CST);
  __atomic_store_n(fetch_add_word, 0, __ATOMIC_SEQ_CST);
  __atomic_store_n(cas_word, 0, __ATOMIC_SEQ_CST);
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
  const uint64_t expected_iterations = options.atomic_iterations;
  const uint64_t fetch_add_actual =
      __atomic_load_n(fetch_add_word, __ATOMIC_SEQ_CST);
  if (fetch_add_actual != expected_iterations) {
    ubsm_test::fail("owner observed wrong remote fetch-add benchmark result: "
                    "expected=" +
                    std::to_string(expected_iterations) +
                    ", actual=" + std::to_string(fetch_add_actual));
  }
  const uint64_t cas_actual = __atomic_load_n(cas_word, __ATOMIC_SEQ_CST);
  if (cas_actual != expected_iterations) {
    ubsm_test::fail("owner observed wrong remote CAS benchmark result: expected=" +
                    std::to_string(expected_iterations) +
                    ", actual=" + std::to_string(cas_actual));
  }
  std::cout
      << "PASS remote timed fetch-add/CAS and atomic validation observed by "
         "owner\n";
  ubsm_test::send_stage(connection, 'V');
  ubsm_bench::owner_cleanup(memory, connection);
}

void run_remote(const ubsm_bench::TwoNodeOptions &options,
                ubsm_test::SharedMemory &memory, int connection) {
  memory.map();
  ubsm_test::send_stage(connection, 'H');
  ubsm_test::expect_stage(connection, 'R');
  uint64_t *fetch_add_word = ubsm_bench::atomic_word_at(
      memory, ubsm_bench::kRemoteFetchAddOffset);
  uint64_t *cas_word =
      ubsm_bench::atomic_word_at(memory, ubsm_bench::kRemoteCasOffset);
  ubsm_test::print_latency(
      "remote_nc_fetch_add_8b_avg_ns",
      ubsm_bench::benchmark_fetch_add_8b(fetch_add_word,
                                         options.atomic_iterations));
  ubsm_test::print_latency(
      "remote_nc_cas_8b_avg_ns",
      ubsm_bench::benchmark_cas_8b(cas_word, options.atomic_iterations));
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
