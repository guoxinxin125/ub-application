#pragma once

#include "common/UBGlobalPtr.h"
#include "common/UBTuple.h"

#include <atomic>
#include <cstdint>
#include <functional>

namespace star
{

class UBTable {
    public:
        static constexpr uint32_t catalog_root_slot = 8;
        static constexpr uint32_t max_tables_per_region = 128;

        struct TableDescriptor {
                std::atomic<uint32_t> ready;
                uint32_t table_id;
                uint32_t partition_id;
                uint32_t key_size;
                uint32_t value_size;
                uint32_t reserved;
                uint64_t bucket_count;
                UBGlobalPtr buckets;
                UBSharedRWLock free_lock;
                std::atomic<uint64_t> free_head_offset;
                std::atomic<uint64_t> allocated_nodes;
                std::atomic<uint64_t> reused_nodes;
        };

        struct Catalog {
                std::atomic<uint32_t> table_count;
                uint32_t reserved[15];
                TableDescriptor tables[max_tables_per_region];
        };

        struct alignas(64) Bucket {
                UBSharedRWLock lock;
                std::atomic<uint64_t> head_offset;
        };

        struct alignas(64) TupleNode {
                std::atomic<uint64_t> next_offset;
                uint64_t plain_key;
                UBTupleHeader tuple;
                char bytes[];
        };

        struct TupleRef {
                TupleNode *node = nullptr;
                UBTupleHeader *header = nullptr;
                void *key = nullptr;
                void *value = nullptr;

                explicit operator bool() const { return node != nullptr; }
        };

        static void initialize_catalog(uint32_t owner_region);
        static TableDescriptor *create(uint32_t owner_region, uint32_t table_id,
                                       uint32_t partition_id, uint32_t key_size,
                                       uint32_t value_size, uint64_t bucket_count);
        static TableDescriptor *find(uint32_t owner_region, uint32_t table_id,
                                     uint32_t partition_id);

        explicit UBTable(TableDescriptor *descriptor = nullptr);
        bool insert(uint64_t plain_key, const void *key, const void *value,
                    uint32_t tuple_state = 1);
        TupleRef search(uint64_t plain_key) const;
        TupleRef search_including_invalid(uint64_t plain_key) const;
        bool erase(uint64_t plain_key);
        bool discard_placeholder(uint64_t plain_key);
        void scan(const std::function<bool(const TupleRef &)> &visitor) const;
        static void *value_from_header(UBTupleHeader *header);

        TableDescriptor *descriptor() const { return descriptor_; }

    private:
        uint32_t owner_region() const;
        Bucket *bucket(uint64_t plain_key) const;
        TupleNode *node_from_offset(uint64_t offset) const;
        TupleRef make_ref(TupleNode *node) const;
        TupleRef search_impl(uint64_t plain_key, bool include_invalid) const;
        uint64_t tuple_allocation_size() const;
        TupleNode *pop_free_node(uint64_t &offset);
        void push_free_node(TupleNode *node, uint64_t offset);

        TableDescriptor *descriptor_ = nullptr;
        Bucket *buckets_ = nullptr;
};

} // namespace star
