#ifdef ERPC_UB

#include "transport_impl/ub/ub_shared_allocator.h"

#include <cstring>
#include <stdexcept>

#include "util/timer.h"

namespace erpc {
namespace {
static constexpr size_t kMinClassCapacity = 64;
static constexpr uint16_t kAllocatorVersion = 1;
inline uint64_t align64(uint64_t value) { return (value + 63ULL) & ~63ULL; }
inline void lock_class(UBArenaHeader *arena, size_t size_class) {
  uint32_t expected = 0;
  while (!ub_atomic::compare_exchange(
      &arena->class_locks[size_class], &expected, uint32_t{1}, true,
      ub_atomic::MemoryOrder::kAcquire, ub_atomic::MemoryOrder::kRelaxed)) {
    expected = 0;
  }
}
inline void unlock_class(UBArenaHeader *arena, size_t size_class) {
  ub_atomic::store(&arena->class_locks[size_class], uint32_t{0},
                   ub_atomic::MemoryOrder::kRelease);
}
}  // namespace

void ub_initialize_arena(void *machine_base, uint64_t machine_id,
                         uint64_t arena_offset, uint64_t arena_size) {
  if (machine_base == nullptr || arena_size <= sizeof(UBArenaHeader)) {
    throw std::runtime_error("UB allocator: invalid endpoint arena");
  }
  auto *arena = reinterpret_cast<UBArenaHeader *>(
      static_cast<uint8_t *>(machine_base) + arena_offset);
  std::memset(arena, 0, sizeof(*arena));
  arena->version = kAllocatorVersion;
  arena->class_count = kUBAllocatorClassCount;
  arena->machine_id = machine_id;
  arena->arena_offset = arena_offset;
  arena->arena_size = arena_size;
  arena->bump_offset = align64(arena_offset + sizeof(UBArenaHeader));
  ub_atomic::fence(ub_atomic::MemoryOrder::kRelease);
  ub_atomic::store(&arena->magic, kUBArenaMagic,
                   ub_atomic::MemoryOrder::kRelease);
}

UBSharedAllocator::UBSharedAllocator(UBMachineContext *context,
                                     const UBEndpointHandle &endpoint)
    : context_(context),
      endpoint_(endpoint),
      machine_base_(static_cast<uint8_t *>(context->local_base())),
      arena_(reinterpret_cast<UBArenaHeader *>(machine_base_ +
                                               endpoint.arena_offset)) {
  if (!endpoint.valid() ||
      ub_atomic::load(&arena_->magic, ub_atomic::MemoryOrder::kAcquire) !=
          kUBArenaMagic ||
      arena_->machine_id != context_->local_machine_id() ||
      arena_->arena_offset != endpoint.arena_offset ||
      arena_->arena_size != endpoint.arena_size) {
    throw std::runtime_error("UB allocator: endpoint arena is not initialized");
  }
}

size_t UBSharedAllocator::class_capacity(size_t index) {
  return kMinClassCapacity << index;
}

size_t UBSharedAllocator::class_for(size_t size) {
  size_t index = 0;
  size_t capacity = kMinClassCapacity;
  while (capacity < size && index + 1 < kUBAllocatorClassCount) {
    capacity <<= 1;
    ++index;
  }
  return capacity >= size ? index : kUBAllocatorClassCount;
}

Buffer UBSharedAllocator::alloc(size_t size) {
  const size_t size_class = class_for(size);
  if (size == 0 || size_class == kUBAllocatorClassCount) return Buffer();
  uint64_t block_offset = 0;
  lock_class(arena_, size_class);
  block_offset = ub_atomic::load(&arena_->free_heads[size_class],
                                 ub_atomic::MemoryOrder::kRelaxed);
  if (block_offset != 0) {
    auto *metadata =
        reinterpret_cast<UBBlockMetadata *>(machine_base_ + block_offset);
    ub_atomic::store(&arena_->free_heads[size_class],
                     metadata->next_free_offset,
                     ub_atomic::MemoryOrder::kRelaxed);
  }
  unlock_class(arena_, size_class);
  const size_t capacity = class_capacity(size_class);
  if (block_offset == 0) {
    const uint64_t block_bytes = align64(sizeof(UBBlockMetadata) + capacity);
    const uint64_t arena_end = endpoint_.arena_offset + endpoint_.arena_size;
    uint64_t expected =
        ub_atomic::load(&arena_->bump_offset, ub_atomic::MemoryOrder::kAcquire);
    while (true) {
      if (expected > arena_end || block_bytes > arena_end - expected) {
        return Buffer();
      }
      const uint64_t desired = expected + block_bytes;
      if (ub_atomic::compare_exchange(&arena_->bump_offset, &expected, desired,
                                      true, ub_atomic::MemoryOrder::kAcqRel,
                                      ub_atomic::MemoryOrder::kAcquire)) {
        block_offset = expected;
        break;
      }
    }
  }
  auto *metadata =
      reinterpret_cast<UBBlockMetadata *>(machine_base_ + block_offset);
  // A newly assigned endpoint arena is fully zeroed once by the machine
  // owner. On later endpoint incarnations the block metadata is deliberately
  // preserved, so incrementing here also invalidates stale descriptors that
  // refer to the same block offset from an older arena incarnation.
  uint32_t generation = metadata->generation + 1;
  if (generation == 0) generation = 1;
  metadata->magic = kUBBlockMagic;
  metadata->version = kAllocatorVersion;
  metadata->size_class = static_cast<uint16_t>(size_class);
  metadata->generation = generation;
  metadata->ref_count = 1;
  metadata->machine_id = context_->local_machine_id();
  metadata->arena_offset = endpoint_.arena_offset;
  metadata->block_offset = block_offset;
  metadata->payload_capacity = capacity;
  metadata->next_free_offset = 0;
  ub_atomic::fetch_add(&arena_->active_blocks, uint64_t{1},
                       ub_atomic::MemoryOrder::kAcqRel);
  ub_atomic::store(&metadata->state,
                   static_cast<uint32_t>(UBBlockState::kAllocated),
                   ub_atomic::MemoryOrder::kRelease);
  return Buffer(reinterpret_cast<uint8_t *>(metadata + 1), capacity, 0);
}

UBBlockMetadata *UBSharedAllocator::metadata_from_buffer(Buffer buffer) {
  return buffer.buf_ == nullptr
             ? nullptr
             : reinterpret_cast<UBBlockMetadata *>(buffer.buf_) - 1;
}

void UBSharedAllocator::add_ref(Buffer buffer) {
  UBBlockMetadata *metadata = metadata_from_buffer(buffer);
  if (metadata == nullptr || metadata->magic != kUBBlockMagic ||
      ub_atomic::load(&metadata->state, ub_atomic::MemoryOrder::kAcquire) !=
          static_cast<uint32_t>(UBBlockState::kAllocated)) {
    throw std::runtime_error("UB allocator: add_ref on invalid block");
  }
  ub_atomic::fetch_add(&metadata->ref_count, uint32_t{1},
                       ub_atomic::MemoryOrder::kAcqRel);
}

void UBSharedAllocator::release_metadata(UBBlockMetadata *metadata) {
  const uint32_t old = ub_atomic::fetch_sub(&metadata->ref_count, uint32_t{1},
                                            ub_atomic::MemoryOrder::kAcqRel);
  if (old == 0) throw std::runtime_error("UB allocator: reference underflow");
  if (old != 1) return;
  uint8_t *machine_base =
      reinterpret_cast<uint8_t *>(metadata) - metadata->block_offset;
  auto *arena =
      reinterpret_cast<UBArenaHeader *>(machine_base + metadata->arena_offset);
  if (arena->magic != kUBArenaMagic ||
      metadata->size_class >= kUBAllocatorClassCount) {
    throw std::runtime_error("UB allocator: corrupt arena metadata");
  }
  ub_atomic::store(&metadata->state, static_cast<uint32_t>(UBBlockState::kFree),
                   ub_atomic::MemoryOrder::kRelease);
  lock_class(arena, metadata->size_class);
  metadata->next_free_offset =
      ub_atomic::load(&arena->free_heads[metadata->size_class],
                      ub_atomic::MemoryOrder::kRelaxed);
  ub_atomic::store(&arena->free_heads[metadata->size_class],
                   metadata->block_offset, ub_atomic::MemoryOrder::kRelease);
  unlock_class(arena, metadata->size_class);
  const uint64_t old_active = ub_atomic::fetch_sub(
      &arena->active_blocks, uint64_t{1}, ub_atomic::MemoryOrder::kAcqRel);
  if (old_active == 0)
    throw std::runtime_error("UB allocator: active block underflow");
}

void UBSharedAllocator::free(Buffer buffer) {
  UBBlockMetadata *metadata = metadata_from_buffer(buffer);
  if (metadata == nullptr) return;
  if (metadata->magic != kUBBlockMagic)
    throw std::runtime_error("UB allocator: free on invalid block");
  release_metadata(metadata);
}

bool UBSharedAllocator::is_shared_ptr(const void *ptr) const {
  if (ptr == nullptr) return false;
  const uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
  const uintptr_t begin =
      reinterpret_cast<uintptr_t>(machine_base_) + endpoint_.arena_offset;
  return value >= begin && value < begin + endpoint_.arena_size;
}

uint64_t UBSharedAllocator::offset_of(const void *ptr) const {
  if (!is_shared_ptr(ptr)) return 0;
  return static_cast<uint64_t>(static_cast<const uint8_t *>(ptr) -
                               machine_base_);
}

uint32_t UBSharedAllocator::generation_of(const void *ptr) const {
  if (!is_shared_ptr(ptr)) return 0;
  return (reinterpret_cast<const UBBlockMetadata *>(ptr) - 1)->generation;
}

uint8_t *UBSharedAllocator::resolve_payload(void *machine_base,
                                            uint64_t machine_id,
                                            uint64_t block_offset,
                                            uint64_t payload_offset,
                                            uint32_t generation, size_t length,
                                            UBResolveProfileSample *profile) {
  size_t stage_start = profile == nullptr ? 0 : rdtsc();
  if (profile != nullptr) ub_atomic::compiler_fence();
  const size_t region_size = context_->region_bytes();
  const bool invalid_bounds =
      machine_base == nullptr ||
      block_offset > region_size - sizeof(UBBlockMetadata) ||
      payload_offset > region_size || length > region_size - payload_offset;
  if (profile != nullptr) {
    ub_atomic::compiler_fence();
    profile->bounds_ticks = rdtsc() - stage_start;
  }
  if (invalid_bounds) return nullptr;

  auto *metadata = reinterpret_cast<UBBlockMetadata *>(
      static_cast<uint8_t *>(machine_base) + block_offset);

  stage_start = profile == nullptr ? 0 : rdtsc();
  if (profile != nullptr) ub_atomic::compiler_fence();
  const uint32_t state =
      ub_atomic::load(&metadata->state, ub_atomic::MemoryOrder::kAcquire);
  if (profile != nullptr) {
    ub_atomic::compiler_fence();
    profile->state_ticks = rdtsc() - stage_start;
  }
  if (state != static_cast<uint32_t>(UBBlockState::kAllocated)) return nullptr;

  stage_start = profile == nullptr ? 0 : rdtsc();
  if (profile != nullptr) ub_atomic::compiler_fence();
  const uint32_t magic = metadata->magic;
  const uint32_t metadata_generation = metadata->generation;
  const uint64_t metadata_machine_id = metadata->machine_id;
  const uint64_t payload_capacity = metadata->payload_capacity;
  if (profile != nullptr) {
    ub_atomic::compiler_fence();
    profile->metadata_ticks = rdtsc() - stage_start;
  }

  stage_start = profile == nullptr ? 0 : rdtsc();
  if (profile != nullptr) ub_atomic::compiler_fence();
  const bool invalid_identity =
      magic != kUBBlockMagic || metadata_generation != generation ||
      metadata_machine_id != machine_id ||
      payload_offset < block_offset + sizeof(UBBlockMetadata);
  uint64_t inside = 0;
  bool invalid_capacity = true;
  if (!invalid_identity) {
    inside = payload_offset - block_offset - sizeof(UBBlockMetadata);
    invalid_capacity =
        inside > payload_capacity || length > payload_capacity - inside;
  }
  if (profile != nullptr) {
    ub_atomic::compiler_fence();
    profile->checks_ticks = rdtsc() - stage_start;
  }
  if (invalid_identity || invalid_capacity) return nullptr;
  return static_cast<uint8_t *>(machine_base) + payload_offset;
}

}  // namespace erpc

#endif
