#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <x86intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>

#include "utils/bypass_cache.h"

static constexpr size_t kCacheLineSize = 64;

static inline uint64_t rdtsc_ordered() {
  unsigned aux;
  return __rdtscp(&aux);
}

static size_t parse_size(const char *arg) {
  char *end = nullptr;
  size_t value = strtoull(arg, &end, 10);
  if (end == arg) return 0;
  if (*end == '\0') return value;
  if ((end[0] == 'K' || end[0] == 'k') && end[1] == '\0') {
    return value * 1024;
  }
  if ((end[0] == 'M' || end[0] == 'm') && end[1] == '\0') {
    return value * 1024 * 1024;
  }
  if ((end[0] == 'G' || end[0] == 'g') && end[1] == '\0') {
    return value * 1024 * 1024 * 1024;
  }
  return 0;
}

static void usage(const char *argv0) {
  printf("Usage: %s <device> [mmap_size] [test_size] [iters] [fixed_base]\n",
         argv0);
  printf("Example: %s /dev/dax0.0 128M 64 100000\n", argv0);
  printf("Example: %s /dev/uncached_mem_dev 128M 64 100000 0x20000000000\n",
         argv0);
}

template <typename Fn>
static double bench_cycles(size_t iters, Fn fn) {
  uint64_t total = 0;
  for (size_t i = 0; i < iters; ++i) {
    const uint64_t start = rdtsc_ordered();
    fn(i);
    const uint64_t end = rdtsc_ordered();
    total += end - start;
  }
  return static_cast<double>(total) / static_cast<double>(iters);
}

template <typename Fn>
static double bench_ns(size_t iters, Fn fn) {
  typedef std::chrono::steady_clock Clock;
  const Clock::time_point start = Clock::now();
  for (size_t i = 0; i < iters; ++i) {
    fn(i);
  }
  const Clock::time_point end = Clock::now();
  const uint64_t ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
          .count();
  return static_cast<double>(ns) / static_cast<double>(iters);
}

int main(int argc, char **argv) {
  if (argc < 2 || argc > 6) {
    usage(argv[0]);
    return 1;
  }

  const char *device = argv[1];
  const size_t mmap_size = argc >= 3 ? parse_size(argv[2]) : 128 * 1024 * 1024;
  const size_t test_size = argc >= 4 ? parse_size(argv[3]) : 64;
  const size_t iters = argc >= 5 ? parse_size(argv[4]) : 100000;
  void *fixed_base = nullptr;
  if (argc >= 6) {
    fixed_base = reinterpret_cast<void *>(strtoull(argv[5], nullptr, 0));
  }

  if (mmap_size == 0 || test_size == 0 || iters == 0 ||
      test_size > mmap_size) {
    usage(argv[0]);
    return 1;
  }

  int fd = open(device, O_RDWR);
  if (fd < 0) {
    perror("open");
    return 1;
  }

  int flags = MAP_SHARED;
  if (fixed_base != nullptr) flags |= MAP_FIXED;
  void *mapping = mmap(fixed_base, mmap_size, PROT_READ | PROT_WRITE, flags, fd,
                       0);
  if (mapping == MAP_FAILED) {
    perror("mmap");
    close(fd);
    return 1;
  }

  volatile uint8_t *buf = static_cast<volatile uint8_t *>(mapping);
  memset(const_cast<uint8_t *>(buf), 0, mmap_size);
  std::atomic<uint64_t> *atomic_word =
      reinterpret_cast<std::atomic<uint64_t> *>(
          const_cast<uint8_t *>(static_cast<volatile uint8_t *>(buf)));
  atomic_word->store(0, std::memory_order_relaxed);

  const size_t stride = std::max<size_t>(kCacheLineSize, test_size);
  const size_t slots = std::max<size_t>(1, mmap_size / stride);
  volatile uint64_t sink = 0;

  printf("device=%s mmap=%p mmap_size=%zu test_size=%zu iters=%zu slots=%zu\n",
         device, mapping, mmap_size, test_size, iters, slots);

  const double write_cycles = bench_cycles(iters, [&](size_t i) {
    volatile uint8_t *p = buf + (i % slots) * stride;
    for (size_t off = 0; off < test_size; off += sizeof(uint64_t)) {
      *reinterpret_cast<volatile uint64_t *>(
          const_cast<uint8_t *>(p + off)) = i + off;
    }
  });

  const double read_cycles = bench_cycles(iters, [&](size_t i) {
    volatile uint8_t *p = buf + (i % slots) * stride;
    sink += *reinterpret_cast<volatile uint64_t *>(
        const_cast<uint8_t *>(p));
  });

  const double shm_clflush_cycles = bench_cycles(iters, [&](size_t i) {
    volatile uint8_t *p = buf + (i % slots) * stride;
    clflush(const_cast<uint8_t *>(p), test_size);
  });

  const double write_flush_cycles = bench_cycles(iters, [&](size_t i) {
    volatile uint8_t *p = buf + (i % slots) * stride;
    for (size_t off = 0; off < test_size; off += sizeof(uint64_t)) {
      *reinterpret_cast<volatile uint64_t *>(
          const_cast<uint8_t *>(p + off)) = i + off;
    }
    clflush(const_cast<uint8_t *>(p), test_size);
  });

  const double shm_clflush_ns = bench_ns(iters, [&](size_t i) {
    volatile uint8_t *p = buf + (i % slots) * stride;
    clflush(const_cast<uint8_t *>(p), test_size);
  });

  const double write8_cycles = bench_cycles(iters, [&](size_t i) {
    volatile uint64_t *p = reinterpret_cast<volatile uint64_t *>(
        const_cast<uint8_t *>(buf + ((i % slots) * stride)));
    *p = i;
  });

  const double read8_cycles = bench_cycles(iters, [&](size_t i) {
    volatile uint64_t *p = reinterpret_cast<volatile uint64_t *>(
        const_cast<uint8_t *>(buf + ((i % slots) * stride)));
    sink += *p;
  });

  const double atomic_load_cycles = bench_cycles(iters, [&](size_t) {
    sink += atomic_word->load(std::memory_order_acquire);
  });

  const double atomic_store_cycles = bench_cycles(iters, [&](size_t i) {
    atomic_word->store(i, std::memory_order_release);
  });

  const double atomic_fetch_add_cycles = bench_cycles(iters, [&](size_t) {
    sink += atomic_word->fetch_add(1, std::memory_order_acq_rel);
  });

  atomic_word->store(0, std::memory_order_relaxed);
  uint64_t cas_success_value_cycles = 0;
  const double atomic_cas_success_cycles = bench_cycles(iters, [&](size_t) {
    uint64_t expected = cas_success_value_cycles;
    atomic_word->compare_exchange_strong(expected, cas_success_value_cycles + 1,
                                         std::memory_order_acq_rel);
    cas_success_value_cycles++;
  });

  atomic_word->store(1, std::memory_order_relaxed);
  const double atomic_cas_fail_cycles = bench_cycles(iters, [&](size_t i) {
    uint64_t expected = i + 2;
    atomic_word->compare_exchange_strong(expected, i + 3,
                                         std::memory_order_acq_rel);
  });

  const double write8_ns = bench_ns(iters, [&](size_t i) {
    volatile uint64_t *p = reinterpret_cast<volatile uint64_t *>(
        const_cast<uint8_t *>(buf + ((i % slots) * stride)));
    *p = i;
  });

  const double read8_ns = bench_ns(iters, [&](size_t i) {
    volatile uint64_t *p = reinterpret_cast<volatile uint64_t *>(
        const_cast<uint8_t *>(buf + ((i % slots) * stride)));
    sink += *p;
  });

  const double atomic_load_ns = bench_ns(iters, [&](size_t) {
    sink += atomic_word->load(std::memory_order_acquire);
  });

  const double atomic_store_ns = bench_ns(iters, [&](size_t i) {
    atomic_word->store(i, std::memory_order_release);
  });

  const double atomic_fetch_add_ns = bench_ns(iters, [&](size_t) {
    sink += atomic_word->fetch_add(1, std::memory_order_acq_rel);
  });

  atomic_word->store(0, std::memory_order_relaxed);
  uint64_t cas_success_value_ns = 0;
  const double atomic_cas_success_ns = bench_ns(iters, [&](size_t) {
    uint64_t expected = cas_success_value_ns;
    atomic_word->compare_exchange_strong(expected, cas_success_value_ns + 1,
                                         std::memory_order_acq_rel);
    cas_success_value_ns++;
  });

  atomic_word->store(1, std::memory_order_relaxed);
  const double atomic_cas_fail_ns = bench_ns(iters, [&](size_t i) {
    uint64_t expected = i + 2;
    atomic_word->compare_exchange_strong(expected, i + 3,
                                         std::memory_order_acq_rel);
  });

  printf("avg cycles: write=%.2f read=%.2f shm_clflush=%.2f "
         "write+shm_clflush=%.2f\n",
         write_cycles, read_cycles, shm_clflush_cycles, write_flush_cycles);
  printf("avg time: shm_clflush=%.2f ns = %.4f us\n", shm_clflush_ns,
         shm_clflush_ns / 1000.0);
  printf("uc/access cycles: write8=%.2f read8=%.2f atomic_load=%.2f "
         "atomic_store=%.2f fetch_add=%.2f cas_success=%.2f cas_fail=%.2f\n",
         write8_cycles, read8_cycles, atomic_load_cycles, atomic_store_cycles,
         atomic_fetch_add_cycles, atomic_cas_success_cycles,
         atomic_cas_fail_cycles);
  printf("uc/access time: write8=%.2f ns read8=%.2f ns atomic_load=%.2f ns "
         "atomic_store=%.2f ns fetch_add=%.2f ns cas_success=%.2f ns "
         "cas_fail=%.2f ns\n",
         write8_ns, read8_ns, atomic_load_ns, atomic_store_ns,
         atomic_fetch_add_ns, atomic_cas_success_ns, atomic_cas_fail_ns);
  printf("sink=%llu\n", static_cast<unsigned long long>(sink));

  munmap(mapping, mmap_size);
  close(fd);
  return 0;
}
