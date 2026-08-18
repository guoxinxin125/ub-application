#pragma once

#ifdef ERPC_UB

#include <cstdint>
#include <type_traits>

#include "transport_impl/ub/ub_config.h"
#include "transport_impl/ub/ub_machine_layout.h"

namespace erpc {

static constexpr uint32_t kUBManagerProtocolMagic = 0x55424d47U;  // "UBMG"
static constexpr uint16_t kUBManagerProtocolVersion = 2;

enum class UBManagerOperation : uint16_t {
  kRegisterEndpoint = 1,
  kUnregisterEndpoint = 2,
};

struct UBManagerRequest {
  uint32_t magic = kUBManagerProtocolMagic;
  uint16_t version = kUBManagerProtocolVersion;
  uint16_t operation = 0;
  uint32_t process_id = 0;
  uint16_t sm_udp_port = 0;
  uint8_t rpc_id = UINT8_MAX;
  uint8_t slot = UINT8_MAX;
  uint32_t generation = 0;
  uint64_t inbox_offset = 0;
  uint64_t arena_offset = 0;
  uint64_t arena_size = 0;
};

struct UBManagerResponse {
  uint32_t magic = kUBManagerProtocolMagic;
  uint16_t version = kUBManagerProtocolVersion;
  int16_t status = -1;
  uint64_t machine_id = 0;
  uint64_t region_bytes = 0;
  uint64_t allocation_flags = 0;
  UBEndpointHandle endpoint;
};

static_assert(std::is_trivially_copyable<UBManagerRequest>::value,
              "UB manager request must be safe to transfer as bytes");
static_assert(std::is_trivially_copyable<UBManagerResponse>::value,
              "UB manager response must be safe to transfer as bytes");

}  // namespace erpc

#endif  // ERPC_UB
