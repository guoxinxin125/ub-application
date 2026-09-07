#pragma once

#include "common/UBGlobalPtr.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace star
{

class Context;

class UBMemory {
    public:
        static constexpr uint64_t abi_magic = 0x5449474f4e554231ULL;
        static constexpr uint32_t abi_version = 1;
        static constexpr uint32_t root_slot_count = 64;
        static constexpr uint64_t allocation_alignment = 64;

        enum class MemoryMode : uint32_t { ONE_SIDED = 1, NOCACHE = 2 };

        struct RootSlot {
                std::atomic<uint64_t> sequence;
                std::atomic<uint64_t> region_generation;
                std::atomic<uint64_t> offset;
        };

        struct alignas(64) RegionHeader {
                uint64_t magic;
                uint32_t abi;
                uint32_t region_id;
                uint32_t generation;
                uint32_t coordinator_count;
                uint64_t region_size;
                std::atomic<uint64_t> bump_offset;
                std::atomic<uint32_t> ready;
                uint32_t reserved;
                RootSlot roots[root_slot_count];
        };

        UBMemory() = default;
        UBMemory(const UBMemory &) = delete;
        UBMemory &operator=(const UBMemory &) = delete;
        ~UBMemory();

        void initialize(const Context &context);
        void unmap_imported_regions();
        // All peers must have stopped accessing UB memory before this call.
        void shutdown(bool deallocate_owner = true);
        bool initialized() const { return initialized_; }

        UBGlobalPtr allocate(uint32_t region_id, uint64_t size,
                             uint64_t alignment = allocation_alignment);
        void *resolve(const UBGlobalPtr &ptr, uint64_t length = 1) const;

        template <class T> T *resolve_as(const UBGlobalPtr &ptr, std::size_t count = 1) const
        {
                return static_cast<T *>(resolve(ptr, sizeof(T) * count));
        }

        UBGlobalPtr to_global(uint32_t region_id, const void *address) const;
        bool contains_address(const void *address, uint64_t length = 1) const;
        void publish_root(uint32_t region_id, uint32_t slot, UBGlobalPtr value);
        UBGlobalPtr read_root(uint32_t region_id, uint32_t slot) const;

        uint32_t local_region_id() const { return local_region_id_; }
        uint32_t region_count() const { return static_cast<uint32_t>(regions_.size()); }
        uint64_t region_size(uint32_t region_id) const;
        const std::string &region_name(uint32_t region_id) const;
        MemoryMode memory_mode() const { return memory_mode_; }

    private:
        struct RegionMapping {
                std::string name;
                void *base = nullptr;
                RegionHeader *header = nullptr;
                uint64_t size = 0;
                bool owner = false;
                bool allocated = false;
        };

        static uint64_t align_up(uint64_t value, uint64_t alignment);
        void validate_context(const Context &context) const;
        void initialize_sdk();
        void allocate_local_region(const Context &context);
        using MapDeadline = std::chrono::steady_clock::time_point;
        void map_region_with_retry(uint32_t region_id, MapDeadline deadline);
        void initialize_local_header(uint32_t coordinator_count);
        void validate_remote_header(uint32_t region_id,
                                    MapDeadline deadline) const;

        std::vector<RegionMapping> regions_;
        uint32_t local_region_id_ = UBGlobalPtr::null_region;
        MemoryMode memory_mode_ = MemoryMode::ONE_SIDED;
        bool sdk_initialized_ = false;
        bool initialized_ = false;
};

extern UBMemory ub_memory;

} // namespace star
