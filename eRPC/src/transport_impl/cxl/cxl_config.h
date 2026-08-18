#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace erpc {
namespace cxl_config {

static constexpr const char *kCacheableDevicePath = "/dev/dax0.0";
static constexpr uintptr_t kCacheableBaseAddr = 0x10000000000ULL;
static constexpr size_t kKiB = 1024;
static constexpr size_t kMiB = 1024 * kKiB;
static constexpr size_t kGiB = 1024 * kMiB;

static constexpr size_t kDefaultCacheableMemorySize = 8 * kGiB;
static constexpr size_t kReservedQueueBytes = 16 * kMiB;
static constexpr size_t kDaxAlignBytes = 2 * kMiB;

static constexpr size_t kDefaultWorkerCount = 64;
static constexpr size_t kMaxRpcEndpoints = 64;

static constexpr size_t kMaxBlockSize = 16 * kMiB;
static constexpr size_t kPtrQueueSize = 16384;
static constexpr size_t kSPSCQueueSize = 32;
static constexpr size_t kSPSCQueuePairSpace = 4 * kKiB;
static constexpr size_t kSPSCQueueBufferOffset = 512;
static constexpr size_t kBandwidthGbps = 64;

#ifdef ENABLE_UNCACHE_MEM
static constexpr const char *kUncacheableDevicePath = "/dev/uncached_queue_dev";
static constexpr uintptr_t kUncacheableBaseAddr = 0x20000000000ULL;
static constexpr size_t kUncacheableMemorySize = 128 * kMiB;
static constexpr size_t kUncacheableSessionSpace = 2 * kMiB;
static constexpr size_t kUncacheableQueueBufferOffset = 4096;
#endif

static constexpr size_t kCacheableQueueSessionSpace = 4096;

inline size_t read_size_env(const char *name, size_t default_value) {
  const char *env = std::getenv(name);
  if (env == nullptr || env[0] == '\0') {
    return default_value;
  }

  char *end = nullptr;
  const unsigned long long value = std::strtoull(env, &end, 0);
  if (end == env || value == 0) {
    return default_value;
  }
  return static_cast<size_t>(value);
}

inline size_t get_worker_count() {
  return read_size_env("ERPC_CXL_WORKER_COUNT", kDefaultWorkerCount);
}

inline size_t get_cacheable_memory_size() {
  return read_size_env("ERPC_CXL_MEMORY_SIZE", kDefaultCacheableMemorySize);
}

}  // namespace cxl_config
}  // namespace erpc
