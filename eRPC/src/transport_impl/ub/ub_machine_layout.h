#pragma once

#ifdef ERPC_UB

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "transport_impl/ub/ub_atomic.h"
#include "transport_impl/ub/ub_config.h"
#include "transport_impl/ub/ub_spsc_queue.h"

namespace erpc {

enum class UBEndpointState : uint32_t { kFree = 0, kActive = 1 };

struct alignas(64) UBMachineRegionHeader {
  uint32_t magic;
  uint16_t version;
  uint8_t process_mode;
  uint8_t reserved0;
  uint32_t max_endpoints;
  uint32_t queue_depth;
  uint32_t mtu;
  uint32_t reserved1;
  uint64_t machine_id;
  uint64_t allocation_flags;
  uint64_t region_bytes;
  uint64_t next_free_offset;
  uint64_t manager_generation;
};

static_assert(sizeof(UBMachineRegionHeader) == 64,
              "UB machine header must occupy one cache line");

struct alignas(64) UBEndpointRegistryEntry {
  uint32_t state;
  uint32_t generation;
  uint32_t process_id;
  uint16_t sm_udp_port;
  uint8_t rpc_id;
  uint8_t reserved0;
  uint64_t inbox_offset;
  uint64_t arena_offset;
  uint64_t arena_size;
  uint8_t padding[24];
};

static_assert(sizeof(UBEndpointRegistryEntry) == 64,
              "UB endpoint registry entry must occupy one cache line");

struct alignas(64) UBMachineRegionMetadata {
  UBMachineRegionHeader header;
  UBEndpointRegistryEntry registry[ub_config::kMaxRpcEndpoints];
};

static constexpr size_t kUBMachineDataOffset = ub_config::kAllocationAlignment;

struct UBEndpointHandle {
  uint8_t slot = UINT8_MAX;
  uint8_t rpc_id = UINT8_MAX;
  uint16_t reserved = 0;
  uint32_t generation = 0;
  uint64_t inbox_offset = 0;
  uint64_t arena_offset = 0;
  uint64_t arena_size = 0;

  UBEndpointHandle() = default;
  UBEndpointHandle(uint8_t endpoint_slot, uint8_t endpoint_rpc_id,
                   uint32_t endpoint_generation, uint64_t endpoint_offset,
                   uint64_t endpoint_arena_offset = 0,
                   uint64_t endpoint_arena_size = 0)
      : slot(endpoint_slot),
        rpc_id(endpoint_rpc_id),
        generation(endpoint_generation),
        inbox_offset(endpoint_offset),
        arena_offset(endpoint_arena_offset),
        arena_size(endpoint_arena_size) {}

  bool valid() const {
    return slot < ub_config::kMaxRpcEndpoints &&
           rpc_id < ub_config::kMaxRpcEndpoints && inbox_offset != 0 &&
           arena_offset != 0 && arena_size != 0;
  }
};

inline UBEndpointInbox *ub_inbox_at(void *base, uint64_t offset) {
  return reinterpret_cast<UBEndpointInbox *>(static_cast<uint8_t *>(base) +
                                             offset);
}

inline const UBEndpointInbox *ub_inbox_at(const void *base, uint64_t offset) {
  return reinterpret_cast<const UBEndpointInbox *>(
      static_cast<const uint8_t *>(base) + offset);
}

inline void ub_initialize_inbox(UBEndpointInbox *inbox) {
  std::memset(inbox, 0, sizeof(*inbox));
  for (size_t source = 0; source < ub_config::kMaxRpcEndpoints; ++source) {
    inbox->queues[source].initialize();
  }
  ub_atomic::fence(ub_atomic::MemoryOrder::kRelease);
}

}  // namespace erpc

#endif  // ERPC_UB
