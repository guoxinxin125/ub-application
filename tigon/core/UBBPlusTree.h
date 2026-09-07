#pragma once

#include "common/UBCpu.h"
#include "common/UBMemory.h"
#include "common/UBTuple.h"
#include "core/UBBTreeCatalog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace star
{

// A UB-native, position-independent B+ Tree. B0-B3 deliberately use one
// requester-side tree RW lock for structural safety. Tuple locks remain
// independent and stable across leaf splits. Node-local latch/version fields
// are part of the ABI so the structural lock can later be replaced by OLC.
template <class KeyType, class ValueType, class KeyComparator,
          uint32_t LeafCapacity = 15, uint32_t InnerCapacity = 15>
class UBBPlusTree {
    public:
        using Descriptor = UBBTreeCatalog::TableDescriptor;
        static constexpr uint32_t leaf_capacity = LeafCapacity;
        static constexpr uint32_t inner_capacity = InnerCapacity;

        enum class NodeType : uint32_t { LEAF = 1, INNER = 2 };

        struct alignas(64) NodeHeader {
                UBSharedRWLock latch;
                std::atomic<uint64_t> version;
                uint32_t type;
                uint32_t count;
                uint64_t reserved[5];

                explicit NodeHeader(NodeType node_type)
                        : latch()
                        , version(0)
                        , type(static_cast<uint32_t>(node_type))
                        , count(0)
                        , reserved{0, 0, 0, 0, 0}
                {}
        };

        struct LeafEntry {
                KeyType key;
                uint64_t tuple_offset;
        };

        struct alignas(64) LeafNode {
                NodeHeader header;
                uint64_t prev_offset;
                uint64_t next_offset;
                LeafEntry entries[LeafCapacity];

                LeafNode()
                        : header(NodeType::LEAF)
                        , prev_offset(0)
                        , next_offset(0)
                        , entries{}
                {}
        };

        struct alignas(64) InnerNode {
                NodeHeader header;
                KeyType keys[InnerCapacity];
                uint64_t children[InnerCapacity + 1];

                InnerNode()
                        : header(NodeType::INNER)
                        , keys{}
                        , children{}
                {}
        };

        // TupleNode is never moved when leaf entries shift or split.
        struct alignas(64) TupleNode {
                std::atomic<uint64_t> next_free_offset;
                UBTupleHeader tuple;
                KeyType key;
                ValueType value;

                TupleNode(const KeyType &tuple_key, const ValueType &tuple_value,
                          uint32_t tuple_state)
                        : next_free_offset(0)
                        , tuple(sizeof(KeyType), sizeof(ValueType), tuple_state)
                        , key(tuple_key)
                        , value(tuple_value)
                {}
        };

        struct TupleRef {
                TupleNode *node = nullptr;
                UBTupleHeader *header = nullptr;
                KeyType *key = nullptr;
                ValueType *value = nullptr;

                explicit operator bool() const { return node != nullptr; }
        };

        using NextProcessor = std::function<bool(const TupleRef &)>;
        using AdjacentProcessor =
                std::function<bool(const TupleRef *, const TupleRef *)>;

        static_assert(LeafCapacity >= 3, "a UB B+ Tree leaf needs at least 3 entries");
        static_assert(InnerCapacity >= 3, "a UB B+ Tree inner node needs at least 3 keys");
        static_assert(std::is_trivially_copyable<KeyType>::value,
                      "UB B+ Tree keys must be trivially copyable");
        static_assert(std::is_trivially_copyable<ValueType>::value,
                      "UB B+ Tree values must be trivially copyable");
        static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
                      "UB B+ Tree requires lock-free shared 64-bit atomics");
        static_assert(alignof(NodeHeader) == 64, "node headers are a shared ABI");

        static Descriptor *create(uint32_t owner_region, uint32_t table_id,
                                  uint32_t partition_id)
        {
                Descriptor *descriptor = UBBTreeCatalog::claim(
                        owner_region, table_id, partition_id);
                if (descriptor->ready.load(std::memory_order_acquire) == 2) {
                        validate_descriptor(descriptor);
                        return descriptor;
                }

                const UBGlobalPtr descriptor_ptr =
                        ub_memory.to_global(owner_region, descriptor);
                descriptor->abi = UBBTreeCatalog::abi_version;
                descriptor->table_id = table_id;
                descriptor->partition_id = partition_id;
                descriptor->owner_region = owner_region;
                descriptor->region_generation = descriptor_ptr.generation;
                descriptor->key_size = sizeof(KeyType);
                descriptor->value_size = sizeof(ValueType);
                descriptor->leaf_capacity = LeafCapacity;
                descriptor->inner_capacity = InnerCapacity;
                descriptor->leaf_node_size = sizeof(LeafNode);
                descriptor->inner_node_size = sizeof(InnerNode);
                new (&descriptor->tree_lock) UBSharedRWLock();
                new (&descriptor->root_offset) std::atomic<uint64_t>(0);
                new (&descriptor->end_gap_offset) std::atomic<uint64_t>(0);
                new (&descriptor->tree_version) std::atomic<uint64_t>(0);
                new (&descriptor->tuple_count) std::atomic<uint64_t>(0);
                new (&descriptor->leaf_count) std::atomic<uint64_t>(0);
                new (&descriptor->inner_count) std::atomic<uint64_t>(0);
                new (&descriptor->allocated_tuples) std::atomic<uint64_t>(0);
                new (&descriptor->reused_tuples) std::atomic<uint64_t>(0);
                std::memset(descriptor->reserved, 0, sizeof(descriptor->reserved));

                const UBGlobalPtr root_ptr = ub_memory.allocate(
                        owner_region, sizeof(LeafNode), alignof(LeafNode));
                new (ub_memory.resolve_as<LeafNode>(root_ptr)) LeafNode();
                const UBGlobalPtr end_gap_ptr = ub_memory.allocate(
                        owner_region, sizeof(TupleNode), alignof(TupleNode));
                new (ub_memory.resolve_as<TupleNode>(end_gap_ptr))
                        TupleNode(KeyType{}, ValueType{}, 1);
                descriptor->root_offset.store(root_ptr.offset, std::memory_order_release);
                descriptor->end_gap_offset.store(end_gap_ptr.offset,
                                                 std::memory_order_release);
                descriptor->leaf_count.store(1, std::memory_order_relaxed);
                descriptor->ready.store(2, std::memory_order_release);
                return descriptor;
        }

        static Descriptor *find(uint32_t owner_region, uint32_t table_id,
                                uint32_t partition_id)
        {
                Descriptor *descriptor = UBBTreeCatalog::find(
                        owner_region, table_id, partition_id);
                if (descriptor != nullptr)
                        validate_descriptor(descriptor);
                return descriptor;
        }

        explicit UBBPlusTree(Descriptor *descriptor = nullptr)
                : descriptor_(descriptor)
        {
                if (descriptor_ != nullptr)
                        validate_descriptor(descriptor_);
        }

        Descriptor *descriptor() const { return descriptor_; }

        TupleRef search(const KeyType &key) const
        {
                SharedTreeGuard guard(*this);
                TupleRef ref = search_unlocked(key);
                if (ref && ref.header->valid.load(std::memory_order_acquire) != 1)
                        return TupleRef();
                return ref;
        }

        TupleRef search_including_invalid(const KeyType &key) const
        {
                SharedTreeGuard guard(*this);
                return search_unlocked(key);
        }

        bool insert(const KeyType &key, const ValueType &value,
                    uint32_t tuple_state = 1)
        {
                return insert_impl(key, value, tuple_state, NextProcessor(),
                                   AdjacentProcessor());
        }

        bool insert_lock_next(const KeyType &key, const ValueType &value,
                              const NextProcessor &processor,
                              uint32_t tuple_state = 2)
        {
                return insert_impl(key, value, tuple_state, processor,
                                   AdjacentProcessor());
        }

        bool insert_and_process_adjacent(const KeyType &key,
                                         const ValueType &value,
                                         const AdjacentProcessor &processor,
                                         uint32_t tuple_state = 2)
        {
                return insert_impl(key, value, tuple_state, NextProcessor(),
                                   processor);
        }

        bool insert_impl(const KeyType &key, const ValueType &value,
                         uint32_t tuple_state,
                         const NextProcessor &next_processor,
                         const AdjacentProcessor &adjacent_processor)
        {
                if (descriptor_ == nullptr || (tuple_state != 1 && tuple_state != 2))
                        return false;
                ExclusiveTreeGuard guard(*this);

                std::vector<PathEntry> path;
                LeafNode *leaf = find_leaf_unlocked(key, &path);
                const uint32_t position = leaf_lower_bound(leaf, key);
                if (position < leaf->header.count && equal(leaf->entries[position].key, key)) {
                        TupleNode *existing = tuple_from_offset(
                                leaf->entries[position].tuple_offset);
                        if (existing->tuple.valid.load(std::memory_order_acquire) != 0)
                                return false;
                        const TupleRef next = next_visible_unlocked(leaf, position + 1);
                        const TupleRef previous = previous_visible_unlocked(leaf, position);
                        if (next_processor && !next_processor(next))
                                return false;
                        if (adjacent_processor &&
                            !adjacent_processor(previous ? &previous : nullptr, &next))
                                return false;
                        lock_exclusive(existing->tuple.lock);
                        existing->key = key;
                        existing->value = value;
                        existing->tuple.version.fetch_add(1, std::memory_order_relaxed);
                        existing->tuple.valid.store(tuple_state, std::memory_order_release);
                        existing->tuple.lock.unlock();
                        descriptor_->tuple_count.fetch_add(1, std::memory_order_relaxed);
                        descriptor_->reused_tuples.fetch_add(1, std::memory_order_relaxed);
                        finish_mutation(leaf->header);
                        return true;
                }

                const TupleRef next = next_visible_unlocked(leaf, position);
                const TupleRef previous = previous_visible_unlocked(leaf, position);
                if (next_processor && !next_processor(next))
                        return false;
                if (adjacent_processor &&
                    !adjacent_processor(previous ? &previous : nullptr, &next))
                        return false;

                const uint64_t tuple_offset = allocate_tuple(key, value, tuple_state);
                LeafEntry new_entry{key, tuple_offset};
                if (leaf->header.count < LeafCapacity) {
                        insert_leaf_entry(leaf, position, new_entry);
                        descriptor_->tuple_count.fetch_add(1, std::memory_order_relaxed);
                        finish_mutation(leaf->header);
                        return true;
                }

                KeyType separator{};
                uint64_t right_offset = split_leaf_and_insert(
                        leaf, position, new_entry, separator);
                descriptor_->tuple_count.fetch_add(1, std::memory_order_relaxed);
                propagate_split(path, separator, right_offset);
                descriptor_->tree_version.fetch_add(1, std::memory_order_release);
                return true;
        }

        bool erase(const KeyType &key)
        {
                TupleRef ref = search(key);
                if (!ref)
                        return false;
                lock_exclusive(ref.header->lock);
                const bool visible = ref.header->valid.exchange(
                        0, std::memory_order_acq_rel) != 0;
                ref.header->version.fetch_add(1, std::memory_order_release);
                ref.header->lock.unlock();
                if (visible)
                        descriptor_->tuple_count.fetch_sub(1, std::memory_order_relaxed);
                return visible;
        }

        bool discard_placeholder(const KeyType &key)
        {
                TupleRef ref = search_including_invalid(key);
                if (!ref || ref.header->valid.load(std::memory_order_acquire) != 2)
                        return false;
                lock_exclusive(ref.header->lock);
                uint32_t expected = 2;
                const bool discarded = ref.header->valid.compare_exchange_strong(
                        expected, 0, std::memory_order_acq_rel,
                        std::memory_order_acquire);
                if (discarded) {
                        ref.header->version.fetch_add(1, std::memory_order_release);
                        descriptor_->tuple_count.fetch_sub(1, std::memory_order_relaxed);
                }
                ref.header->lock.unlock();
                return discarded;
        }

        void scan(const KeyType &min_key,
                  const std::function<bool(const TupleRef &)> &visitor) const
        {
                std::vector<TupleRef> rows;
                {
                        SharedTreeGuard guard(*this);
                        LeafNode *leaf = find_leaf_unlocked(min_key, nullptr);
                        uint32_t position = leaf_lower_bound(leaf, min_key);
                        while (leaf != nullptr) {
                                for (uint32_t i = position; i < leaf->header.count; ++i) {
                                        TupleNode *tuple = tuple_from_offset(
                                                leaf->entries[i].tuple_offset);
                                        if (tuple->tuple.valid.load(std::memory_order_acquire) == 1)
                                                rows.push_back(make_ref(tuple));
                                }
                                leaf = leaf_from_offset(leaf->next_offset);
                                position = 0;
                        }
                }
                for (const TupleRef &row : rows) {
                        if (visitor(row))
                                return;
                }
        }

        // Invokes the processor while holding the structural shared lock. The
        // final callback has protection_row=true and identifies the successor
        // tuple, or the stable +infinity gap tuple when no successor exists.
        bool scan_range(const KeyType &min_key, const KeyType &max_key,
                        uint64_t limit,
                        const std::function<bool(const TupleRef &, bool)> &processor) const
        {
                SharedTreeGuard guard(*this);
                LeafNode *leaf = find_leaf_unlocked(min_key, nullptr);
                uint32_t position = leaf_lower_bound(leaf, min_key);
                uint64_t emitted = 0;
                while (leaf != nullptr) {
                        for (uint32_t i = position; i < leaf->header.count; ++i) {
                                TupleNode *tuple = tuple_from_offset(
                                        leaf->entries[i].tuple_offset);
                                if (tuple->tuple.valid.load(std::memory_order_acquire) != 1)
                                        continue;
                                const TupleRef ref = make_ref(tuple);
                                if (comparator_(*ref.key, max_key) > 0 ||
                                    (limit != 0 && emitted == limit))
                                        return processor(ref, true);
                                if (!processor(ref, false))
                                        return false;
                                ++emitted;
                        }
                        leaf = leaf_from_offset(leaf->next_offset);
                        position = 0;
                }
                return processor(end_gap_ref(), true);
        }

    private:
        struct PathEntry {
                InnerNode *node;
                uint32_t child_index;
        };

        class SharedTreeGuard {
            public:
                explicit SharedTreeGuard(const UBBPlusTree &tree)
                        : tree_(tree)
                {
                        tree_.lock_shared();
                }
                ~SharedTreeGuard() { tree_.descriptor_->tree_lock.unlock_shared(); }
            private:
                const UBBPlusTree &tree_;
        };

        class ExclusiveTreeGuard {
            public:
                explicit ExclusiveTreeGuard(UBBPlusTree &tree)
                        : tree_(tree)
                {
                        tree_.lock_exclusive(tree_.descriptor_->tree_lock);
                }
                ~ExclusiveTreeGuard() { tree_.descriptor_->tree_lock.unlock(); }
            private:
                UBBPlusTree &tree_;
        };

        static void validate_descriptor(const Descriptor *descriptor)
        {
                if (descriptor->abi != UBBTreeCatalog::abi_version ||
                    descriptor->key_size != sizeof(KeyType) ||
                    descriptor->value_size != sizeof(ValueType) ||
                    descriptor->leaf_capacity != LeafCapacity ||
                    descriptor->inner_capacity != InnerCapacity ||
                    descriptor->leaf_node_size != sizeof(LeafNode) ||
                    descriptor->inner_node_size != sizeof(InnerNode))
                        throw std::runtime_error("incompatible UB B+ Tree table ABI");
        }

        static void lock_exclusive(UBSharedRWLock &lock)
        {
                uint32_t spins = 0;
                while (!lock.try_lock()) {
                        ub_cpu_relax();
                        if (++spins % 4096 == 0)
                                std::this_thread::yield();
                }
        }

        void lock_shared() const
        {
                uint32_t spins = 0;
                while (!descriptor_->tree_lock.try_lock_shared()) {
                        ub_cpu_relax();
                        if (++spins % 4096 == 0)
                                std::this_thread::yield();
                }
        }

        bool equal(const KeyType &a, const KeyType &b) const
        {
                return comparator_(a, b) == 0;
        }

        UBGlobalPtr global(uint64_t offset) const
        {
                if (offset == 0)
                        return UBGlobalPtr();
                return UBGlobalPtr(descriptor_->owner_region,
                                   descriptor_->region_generation, offset);
        }

        NodeHeader *node_from_offset(uint64_t offset) const
        {
                return ub_memory.resolve_as<NodeHeader>(global(offset));
        }

        LeafNode *leaf_from_offset(uint64_t offset) const
        {
                return offset == 0 ? nullptr : ub_memory.resolve_as<LeafNode>(global(offset));
        }

        InnerNode *inner_from_offset(uint64_t offset) const
        {
                return offset == 0 ? nullptr : ub_memory.resolve_as<InnerNode>(global(offset));
        }

        TupleNode *tuple_from_offset(uint64_t offset) const
        {
                return offset == 0 ? nullptr : ub_memory.resolve_as<TupleNode>(global(offset));
        }

        uint64_t offset_of(const void *address) const
        {
                return ub_memory.to_global(descriptor_->owner_region, address).offset;
        }

        TupleRef make_ref(TupleNode *node) const
        {
                TupleRef ref;
                ref.node = node;
                ref.header = &node->tuple;
                ref.key = &node->key;
                ref.value = &node->value;
                return ref;
        }

        uint32_t leaf_lower_bound(const LeafNode *leaf, const KeyType &key) const
        {
                uint32_t first = 0;
                uint32_t count = leaf->header.count;
                while (count != 0) {
                        const uint32_t step = count / 2;
                        const uint32_t middle = first + step;
                        if (comparator_(leaf->entries[middle].key, key) < 0) {
                                first = middle + 1;
                                count -= step + 1;
                        } else {
                                count = step;
                        }
                }
                return first;
        }

        uint32_t child_index(const InnerNode *inner, const KeyType &key) const
        {
                uint32_t index = 0;
                while (index < inner->header.count &&
                       comparator_(key, inner->keys[index]) >= 0)
                        ++index;
                return index;
        }

        LeafNode *find_leaf_unlocked(const KeyType &key,
                                     std::vector<PathEntry> *path) const
        {
                NodeHeader *node = node_from_offset(
                        descriptor_->root_offset.load(std::memory_order_acquire));
                if (node == nullptr)
                        throw std::runtime_error("UB B+ Tree has no root");
                while (node->type == static_cast<uint32_t>(NodeType::INNER)) {
                        InnerNode *inner = reinterpret_cast<InnerNode *>(node);
                        const uint32_t index = child_index(inner, key);
                        if (path != nullptr)
                                path->push_back(PathEntry{inner, index});
                        node = node_from_offset(inner->children[index]);
                        if (node == nullptr)
                                throw std::runtime_error("invalid UB B+ Tree child offset");
                }
                if (node->type != static_cast<uint32_t>(NodeType::LEAF))
                        throw std::runtime_error("invalid UB B+ Tree node type");
                return reinterpret_cast<LeafNode *>(node);
        }

        TupleRef search_unlocked(const KeyType &key) const
        {
                if (descriptor_ == nullptr)
                        return TupleRef();
                LeafNode *leaf = find_leaf_unlocked(key, nullptr);
                const uint32_t position = leaf_lower_bound(leaf, key);
                if (position == leaf->header.count ||
                    !equal(leaf->entries[position].key, key))
                        return TupleRef();
                return make_ref(tuple_from_offset(leaf->entries[position].tuple_offset));
        }

        TupleRef end_gap_ref() const
        {
                return make_ref(tuple_from_offset(
                        descriptor_->end_gap_offset.load(std::memory_order_acquire)));
        }

        TupleRef next_visible_unlocked(LeafNode *leaf, uint32_t position) const
        {
                while (leaf != nullptr) {
                        for (uint32_t i = position; i < leaf->header.count; ++i) {
                                TupleNode *tuple = tuple_from_offset(
                                        leaf->entries[i].tuple_offset);
                                if (tuple->tuple.valid.load(std::memory_order_acquire) == 1)
                                        return make_ref(tuple);
                        }
                        leaf = leaf_from_offset(leaf->next_offset);
                        position = 0;
                }
                return end_gap_ref();
        }

        TupleRef previous_visible_unlocked(LeafNode *leaf, uint32_t position) const
        {
                while (leaf != nullptr) {
                        while (position > 0) {
                                --position;
                                TupleNode *tuple = tuple_from_offset(
                                        leaf->entries[position].tuple_offset);
                                if (tuple->tuple.valid.load(std::memory_order_acquire) == 1)
                                        return make_ref(tuple);
                        }
                        leaf = leaf_from_offset(leaf->prev_offset);
                        if (leaf != nullptr)
                                position = leaf->header.count;
                }
                return TupleRef();
        }

        uint64_t allocate_tuple(const KeyType &key, const ValueType &value,
                                uint32_t tuple_state)
        {
                const UBGlobalPtr ptr = ub_memory.allocate(
                        descriptor_->owner_region, sizeof(TupleNode), alignof(TupleNode));
                new (ub_memory.resolve_as<TupleNode>(ptr)) TupleNode(key, value, tuple_state);
                descriptor_->allocated_tuples.fetch_add(1, std::memory_order_relaxed);
                return ptr.offset;
        }

        LeafNode *allocate_leaf(uint64_t &offset)
        {
                const UBGlobalPtr ptr = ub_memory.allocate(
                        descriptor_->owner_region, sizeof(LeafNode), alignof(LeafNode));
                LeafNode *leaf = new (ub_memory.resolve_as<LeafNode>(ptr)) LeafNode();
                offset = ptr.offset;
                descriptor_->leaf_count.fetch_add(1, std::memory_order_relaxed);
                return leaf;
        }

        InnerNode *allocate_inner(uint64_t &offset)
        {
                const UBGlobalPtr ptr = ub_memory.allocate(
                        descriptor_->owner_region, sizeof(InnerNode), alignof(InnerNode));
                InnerNode *inner = new (ub_memory.resolve_as<InnerNode>(ptr)) InnerNode();
                offset = ptr.offset;
                descriptor_->inner_count.fetch_add(1, std::memory_order_relaxed);
                return inner;
        }

        void insert_leaf_entry(LeafNode *leaf, uint32_t position,
                               const LeafEntry &entry)
        {
                for (uint32_t i = leaf->header.count; i > position; --i)
                        leaf->entries[i] = leaf->entries[i - 1];
                leaf->entries[position] = entry;
                ++leaf->header.count;
        }

        uint64_t split_leaf_and_insert(LeafNode *leaf, uint32_t position,
                                       const LeafEntry &entry, KeyType &separator)
        {
                std::array<LeafEntry, LeafCapacity + 1> entries;
                uint32_t source = 0;
                for (uint32_t i = 0; i < LeafCapacity + 1; ++i) {
                        if (i == position)
                                entries[i] = entry;
                        else
                                entries[i] = leaf->entries[source++];
                }

                uint64_t right_offset = 0;
                LeafNode *right = allocate_leaf(right_offset);
                const uint32_t left_count = (LeafCapacity + 1) / 2;
                const uint32_t right_count = LeafCapacity + 1 - left_count;
                for (uint32_t i = 0; i < left_count; ++i)
                        leaf->entries[i] = entries[i];
                for (uint32_t i = 0; i < right_count; ++i)
                        right->entries[i] = entries[left_count + i];
                leaf->header.count = left_count;
                right->header.count = right_count;

                right->prev_offset = offset_of(leaf);
                right->next_offset = leaf->next_offset;
                if (LeafNode *old_next = leaf_from_offset(leaf->next_offset))
                        old_next->prev_offset = right_offset;
                leaf->next_offset = right_offset;
                separator = right->entries[0].key;
                leaf->header.version.fetch_add(1, std::memory_order_release);
                right->header.version.fetch_add(1, std::memory_order_release);
                return right_offset;
        }

        void insert_inner_entry(InnerNode *inner, uint32_t child_position,
                                const KeyType &separator, uint64_t right_offset)
        {
                for (uint32_t i = inner->header.count; i > child_position; --i)
                        inner->keys[i] = inner->keys[i - 1];
                for (uint32_t i = inner->header.count + 1; i > child_position + 1; --i)
                        inner->children[i] = inner->children[i - 1];
                inner->keys[child_position] = separator;
                inner->children[child_position + 1] = right_offset;
                ++inner->header.count;
                inner->header.version.fetch_add(1, std::memory_order_release);
        }

        void split_inner_and_insert(InnerNode *inner, uint32_t child_position,
                                    const KeyType &separator, uint64_t right_child,
                                    KeyType &promoted, uint64_t &new_right_offset)
        {
                std::array<KeyType, InnerCapacity + 1> keys;
                std::array<uint64_t, InnerCapacity + 2> children;
                for (uint32_t i = 0; i < inner->header.count; ++i)
                        keys[i] = inner->keys[i];
                for (uint32_t i = 0; i <= inner->header.count; ++i)
                        children[i] = inner->children[i];
                for (uint32_t i = inner->header.count; i > child_position; --i)
                        keys[i] = keys[i - 1];
                for (uint32_t i = inner->header.count + 1; i > child_position + 1; --i)
                        children[i] = children[i - 1];
                keys[child_position] = separator;
                children[child_position + 1] = right_child;

                InnerNode *right = allocate_inner(new_right_offset);
                const uint32_t total_keys = InnerCapacity + 1;
                const uint32_t middle = total_keys / 2;
                promoted = keys[middle];
                inner->header.count = middle;
                for (uint32_t i = 0; i < middle; ++i)
                        inner->keys[i] = keys[i];
                for (uint32_t i = 0; i <= middle; ++i)
                        inner->children[i] = children[i];

                const uint32_t right_keys = total_keys - middle - 1;
                right->header.count = right_keys;
                for (uint32_t i = 0; i < right_keys; ++i)
                        right->keys[i] = keys[middle + 1 + i];
                for (uint32_t i = 0; i <= right_keys; ++i)
                        right->children[i] = children[middle + 1 + i];
                inner->header.version.fetch_add(1, std::memory_order_release);
                right->header.version.fetch_add(1, std::memory_order_release);
        }

        void propagate_split(std::vector<PathEntry> &path, KeyType separator,
                             uint64_t right_offset)
        {
                while (!path.empty()) {
                        const PathEntry entry = path.back();
                        path.pop_back();
                        if (entry.node->header.count < InnerCapacity) {
                                insert_inner_entry(entry.node, entry.child_index,
                                                   separator, right_offset);
                                return;
                        }
                        KeyType promoted{};
                        uint64_t new_right_offset = 0;
                        split_inner_and_insert(entry.node, entry.child_index,
                                               separator, right_offset,
                                               promoted, new_right_offset);
                        separator = promoted;
                        right_offset = new_right_offset;
                }

                const uint64_t old_root = descriptor_->root_offset.load(
                        std::memory_order_relaxed);
                uint64_t new_root_offset = 0;
                InnerNode *new_root = allocate_inner(new_root_offset);
                new_root->header.count = 1;
                new_root->keys[0] = separator;
                new_root->children[0] = old_root;
                new_root->children[1] = right_offset;
                new_root->header.version.fetch_add(1, std::memory_order_release);
                descriptor_->root_offset.store(new_root_offset, std::memory_order_release);
        }

        void finish_mutation(NodeHeader &header)
        {
                header.version.fetch_add(1, std::memory_order_release);
                descriptor_->tree_version.fetch_add(1, std::memory_order_release);
        }

        Descriptor *descriptor_ = nullptr;
        KeyComparator comparator_{};
};

} // namespace star
