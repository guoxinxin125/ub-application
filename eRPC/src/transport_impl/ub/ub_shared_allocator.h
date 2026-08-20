#pragma once

#ifdef ERPC_UB

#include <cstddef>
#include <cstdint>

#include "transport_impl/ub/ub_atomic.h"
#include "transport_impl/ub/ub_machine.h"
#include "util/buffer.h"

namespace erpc {

static constexpr uint32_t kUBArenaMagic = 0x55424152U;
static constexpr uint32_t kUBBlockMagic = 0x5542424cU;
static constexpr size_t kUBAllocatorClassCount = 18;

enum class UBBlockState : uint32_t { kFree = 0, kAllocated = 1 };

struct alignas(64) UBArenaHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t class_count;
  uint64_t machine_id;
  uint64_t arena_offset;
  uint64_t arena_size;
  uint64_t bump_offset;
  uint64_t active_blocks;
  uint64_t free_heads[kUBAllocatorClassCount];
  uint32_t class_locks[kUBAllocatorClassCount];
  uint8_t padding[48];
};
static_assert(sizeof(UBArenaHeader) == 320, "UB arena header layout changed");

struct alignas(64) UBBlockMetadata {
  uint32_t magic;
  uint16_t version;
  uint16_t size_class;
  uint32_t state;
  uint32_t generation;
  uint32_t ref_count;
  uint32_t reserved;
  uint64_t machine_id;
  uint64_t arena_offset;
  uint64_t block_offset;
  uint64_t payload_capacity;
  uint64_t next_free_offset;
};
static_assert(sizeof(UBBlockMetadata) == 64,
              "UB block metadata must occupy one cache line");

void ub_initialize_arena(void *machine_base, uint64_t machine_id,
                         uint64_t arena_offset, uint64_t arena_size);

struct UBResolveProfileSample {
  size_t bounds_ticks = 0;
  size_t state_ticks = 0;
  size_t metadata_ticks = 0;
  size_t checks_ticks = 0;
};

class UBSharedAllocator {
 public:
  UBSharedAllocator(UBMachineContext *context,
                    const UBEndpointHandle &endpoint);
  Buffer alloc(size_t size);
  void add_ref(Buffer buffer);
  void free(Buffer buffer);
  bool is_shared_ptr(const void *ptr) const;
  uint64_t offset_of(const void *ptr) const;
  uint32_t generation_of(const void *ptr) const;
  uint8_t *resolve_payload(void *machine_base, uint64_t machine_id,
                           uint64_t block_offset, uint64_t payload_offset,
                           uint32_t generation, size_t length,
                           UBResolveProfileSample *profile = nullptr);

 private:
  static size_t class_capacity(size_t index);
  static size_t class_for(size_t size);
  static UBBlockMetadata *metadata_from_buffer(Buffer buffer);
  static void release_metadata(UBBlockMetadata *metadata);

  UBMachineContext *context_;
  UBEndpointHandle endpoint_;
  uint8_t *machine_base_;
  UBArenaHeader *arena_;
};

}  // namespace erpc

#endif
