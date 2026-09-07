#pragma once

#include "common/UBGlobalPtr.h"
#include "common/UBTuple.h"

#include <atomic>
#include <cstdint>

namespace star
{

class UBBTreeCatalog {
    public:
        static constexpr uint32_t abi_version = 2;
        // Slot 8 replaces the legacy UB hash catalog. Slot 9 is the UB inbox.
        static constexpr uint32_t catalog_root_slot = 8;
        static constexpr uint32_t max_tables_per_region = 128;

        // ready: 0=free, 1=being initialized by the owner, 2=published.
        struct alignas(64) TableDescriptor {
                std::atomic<uint32_t> ready;
                uint32_t abi;
                uint32_t table_id;
                uint32_t partition_id;
                uint32_t owner_region;
                uint32_t region_generation;
                uint32_t key_size;
                uint32_t value_size;
                uint32_t leaf_capacity;
                uint32_t inner_capacity;
                uint32_t leaf_node_size;
                uint32_t inner_node_size;
                UBSharedRWLock tree_lock;
                std::atomic<uint64_t> root_offset;
                std::atomic<uint64_t> end_gap_offset;
                std::atomic<uint64_t> tree_version;
                std::atomic<uint64_t> tuple_count;
                std::atomic<uint64_t> leaf_count;
                std::atomic<uint64_t> inner_count;
                std::atomic<uint64_t> allocated_tuples;
                std::atomic<uint64_t> reused_tuples;
                uint64_t reserved[4];
        };

        struct alignas(64) Catalog {
                uint32_t abi;
                uint32_t owner_region;
                uint32_t region_generation;
                uint32_t reserved;
                std::atomic<uint32_t> table_count;
                uint32_t padding[11];
                TableDescriptor tables[max_tables_per_region];
        };

        static void initialize(uint32_t owner_region);
        static Catalog *find_catalog(uint32_t owner_region);
        static TableDescriptor *find(uint32_t owner_region, uint32_t table_id,
                                     uint32_t partition_id);
        static TableDescriptor *claim(uint32_t owner_region, uint32_t table_id,
                                      uint32_t partition_id);
};

static_assert(alignof(UBBTreeCatalog::TableDescriptor) == 64,
              "UB B+ Tree descriptors must remain cache-line aligned");

} // namespace star
