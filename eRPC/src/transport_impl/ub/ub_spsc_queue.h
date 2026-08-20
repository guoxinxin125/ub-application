#pragma once

#ifdef ERPC_UB

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "pkthdr.h"
#include "transport_impl/ub/ub_atomic.h"
#include "transport_impl/ub/ub_config.h"

namespace erpc {

struct alignas(64) UBQueueCounter {
  uint64_t value;
  uint8_t padding[56];
};

static_assert(sizeof(UBQueueCounter) == 64,
              "UB queue counters must use separate cache lines");

struct alignas(64) UBMessageDescriptor {
  pkthdr_t pkthdr;
  uint64_t machine_id;
  uint64_t block_offset;
  uint64_t payload_offset;
  uint32_t payload_length;
  uint32_t block_generation;
  uint8_t entry_type;
  uint8_t reserved[7];
};

static_assert(sizeof(UBMessageDescriptor) == 64,
              "UB descriptors must occupy one cache line");

struct alignas(64) UBSpscQueue {
  UBQueueCounter head;
  UBQueueCounter tail;
  UBMessageDescriptor slots[ub_config::kSpscQueueDepth];

  void initialize() {
    ub_atomic::store(&head.value, uint64_t{0},
                     ub_atomic::MemoryOrder::kRelaxed);
    ub_atomic::store(&tail.value, uint64_t{0},
                     ub_atomic::MemoryOrder::kRelaxed);
  }

  bool try_enqueue(uint64_t &producer_tail, uint64_t &cached_consumer_head,
                   const UBMessageDescriptor &descriptor) {
    // The consumer head is monotonic, so a producer-side cached value is
    // sufficient while it still proves that the queue has free entries. Only
    // refresh the remote head when the cached value makes the queue look full.
    if (producer_tail - cached_consumer_head >= ub_config::kSpscQueueDepth) {
      cached_consumer_head =
          ub_atomic::load(&head.value, ub_atomic::MemoryOrder::kAcquire);
      if (producer_tail - cached_consumer_head >= ub_config::kSpscQueueDepth) {
        return false;
      }
    }

    UBMessageDescriptor &slot =
        slots[producer_tail & (ub_config::kSpscQueueDepth - 1)];
    slot = descriptor;
    ub_atomic::fence(ub_atomic::MemoryOrder::kRelease);
    ++producer_tail;
    ub_atomic::store(&tail.value, producer_tail,
                     ub_atomic::MemoryOrder::kRelease);
    return true;
  }

  bool try_dequeue(uint64_t &consumer_head, UBMessageDescriptor *descriptor) {
    if (descriptor == nullptr) return false;

    const uint64_t producer_tail =
        ub_atomic::load(&tail.value, ub_atomic::MemoryOrder::kAcquire);
    if (consumer_head == producer_tail) return false;

    UBMessageDescriptor &slot =
        slots[consumer_head & (ub_config::kSpscQueueDepth - 1)];
    *descriptor = slot;
    ub_atomic::fence(ub_atomic::MemoryOrder::kRelease);
    ++consumer_head;
    ub_atomic::store(&head.value, consumer_head,
                     ub_atomic::MemoryOrder::kRelease);
    return true;
  }
};

static_assert((ub_config::kSpscQueueDepth & (ub_config::kSpscQueueDepth - 1)) ==
                  0,
              "UB SPSC queue depth must be a power of two");

struct alignas(64) UBEndpointInbox {
  UBSpscQueue queues[ub_config::kMaxRpcEndpoints];
};

}  // namespace erpc

#endif  // ERPC_UB
