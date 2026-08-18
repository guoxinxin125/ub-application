#pragma once

#ifdef ERPC_UB

#include <ubs_mem_def.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace erpc {
namespace ub_config {

static constexpr size_t kMTU = 2048;
static constexpr size_t kMaxRpcEndpoints = 64;
static constexpr size_t kSpscQueueDepth = 64;
static constexpr size_t kAllocationAlignment = 4ULL * 1024ULL * 1024ULL;
static constexpr size_t kDefaultMachineRegionBytes = 256ULL * 1024ULL * 1024ULL;
static constexpr size_t kDefaultEndpointArenaBytes = 16ULL * 1024ULL * 1024ULL;
static constexpr size_t kBandwidthGbps = 64;
static constexpr uint32_t kRegionMagic = 0x55424552U;  // "UBER"
static constexpr uint16_t kRegionVersion = 2;
static constexpr uint32_t kSdkLogLevel = 3;

enum class ProcessMode : uint8_t { kSingle = 1, kMulti = 2 };

inline uint64_t parse_u64_env(const char *name, uint64_t default_value) {
  const char *text = std::getenv(name);
  if (text == nullptr || text[0] == '\0') return default_value;
  char *end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 0);
  if (end == text || *end != '\0') return default_value;
  return static_cast<uint64_t>(value);
}

inline std::string region_prefix() {
  const char *value = std::getenv("ERPC_UB_REGION_PREFIX");
  return value == nullptr || value[0] == '\0' ? "erpc_ub_rx" : value;
}

inline ProcessMode process_mode() {
  const char *mode = std::getenv("ERPC_UB_PROCESS_MODE");
  if (mode == nullptr || mode[0] == '\0' || std::string(mode) == "single")
    return ProcessMode::kSingle;
  if (std::string(mode) == "multi") return ProcessMode::kMulti;
  throw std::invalid_argument("ERPC_UB_PROCESS_MODE must be single or multi");
}

inline std::string manager_socket_path() {
  const char *value = std::getenv("ERPC_UB_MANAGER_SOCKET");
  return value == nullptr || value[0] == '\0' ? "/tmp/erpc_ub_manager.sock"
                                              : value;
}

inline size_t machine_region_bytes() {
  const uint64_t configured = parse_u64_env(
      "ERPC_UB_REGION_MB", kDefaultMachineRegionBytes / (1024ULL * 1024ULL));
  const uint64_t bytes = configured * 1024ULL * 1024ULL;
  return static_cast<size_t>(
      ((bytes + kAllocationAlignment - 1) / kAllocationAlignment) *
      kAllocationAlignment);
}

inline size_t endpoint_arena_bytes() {
  const uint64_t configured = parse_u64_env(
      "ERPC_UB_ARENA_MB", kDefaultEndpointArenaBytes / (1024ULL * 1024ULL));
  const uint64_t bytes = configured * 1024ULL * 1024ULL;
  return static_cast<size_t>((bytes + 63ULL) & ~63ULL);
}

inline uint64_t allocation_flags() {
  const char *mode = std::getenv("ERPC_UB_MEMORY_MODE");
  if (mode != nullptr && std::string(mode) == "nocache") {
    return UBSM_FLAG_NONCACHE | UBSM_FLAG_WR_DELAY_COMP;
  }
  if (mode == nullptr || mode[0] == '\0' || std::string(mode) == "one-sided") {
    return UBSM_FLAG_ONLY_IMPORT_NONCACHE | UBSM_FLAG_WR_DELAY_COMP;
  }
  throw std::invalid_argument(
      "ERPC_UB_MEMORY_MODE must be one-sided or nocache");
}

inline uint32_t provider_socket() {
  return static_cast<uint32_t>(
      parse_u64_env("ERPC_UB_PROVIDER_SOCKET", UINT32_MAX));
}

inline uint32_t provider_port() {
  return static_cast<uint32_t>(
      parse_u64_env("ERPC_UB_PROVIDER_PORT", UINT32_MAX));
}

inline uint32_t provider_numa(size_t constructor_numa_node) {
  return static_cast<uint32_t>(parse_u64_env(
      "ERPC_UB_PROVIDER_NUMA", static_cast<uint64_t>(constructor_numa_node)));
}

}  // namespace ub_config
}  // namespace erpc

#endif  // ERPC_UB
