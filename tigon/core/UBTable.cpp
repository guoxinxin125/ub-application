#include "core/UBTable.h"

#include "common/UBMemory.h"

#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>

namespace star
{

void UBTable::initialize_catalog(uint32_t owner_region)
{
        if (owner_region != ub_memory.local_region_id())
                throw std::invalid_argument("only a region owner may initialize its catalog");
        UBGlobalPtr root = ub_memory.read_root(owner_region, catalog_root_slot);
        if (root)
                return;
        root = ub_memory.allocate(owner_region, sizeof(Catalog), alignof(Catalog));
        Catalog *catalog = ub_memory.resolve_as<Catalog>(root);
        new (&catalog->table_count) std::atomic<uint32_t>(0);
        for (uint32_t i = 0; i < max_tables_per_region; ++i)
                new (&catalog->tables[i].ready) std::atomic<uint32_t>(0);
        ub_memory.publish_root(owner_region, catalog_root_slot, root);
}

UBTable::TableDescriptor *UBTable::create(uint32_t owner_region, uint32_t table_id,
                                          uint32_t partition_id, uint32_t key_size,
                                          uint32_t value_size, uint64_t bucket_count)
{
        if (owner_region != ub_memory.local_region_id() || key_size == 0 ||
            value_size == 0 || bucket_count == 0)
                throw std::invalid_argument("invalid native UB table configuration");
        initialize_catalog(owner_region);
        if (TableDescriptor *existing = find(owner_region, table_id, partition_id))
                return existing;

        Catalog *catalog = ub_memory.resolve_as<Catalog>(
                ub_memory.read_root(owner_region, catalog_root_slot));
        const uint32_t index = catalog->table_count.fetch_add(1, std::memory_order_acq_rel);
        if (index >= max_tables_per_region)
                throw std::length_error("too many tables in one UB region");
        TableDescriptor *descriptor = &catalog->tables[index];
        const UBGlobalPtr bucket_ptr =
                ub_memory.allocate(owner_region, sizeof(Bucket) * bucket_count, alignof(Bucket));
        Bucket *buckets = ub_memory.resolve_as<Bucket>(bucket_ptr, bucket_count);
        for (uint64_t i = 0; i < bucket_count; ++i) {
                new (&buckets[i].lock) UBSharedRWLock();
                new (&buckets[i].head_offset) std::atomic<uint64_t>(0);
        }
        descriptor->table_id = table_id;
        descriptor->partition_id = partition_id;
        descriptor->key_size = key_size;
        descriptor->value_size = value_size;
        descriptor->reserved = 0;
        descriptor->bucket_count = bucket_count;
        descriptor->buckets = bucket_ptr;
        new (&descriptor->free_lock) UBSharedRWLock();
        new (&descriptor->free_head_offset) std::atomic<uint64_t>(0);
        new (&descriptor->allocated_nodes) std::atomic<uint64_t>(0);
        new (&descriptor->reused_nodes) std::atomic<uint64_t>(0);
        descriptor->ready.store(1, std::memory_order_release);
        return descriptor;
}

UBTable::TableDescriptor *UBTable::find(uint32_t owner_region, uint32_t table_id,
                                        uint32_t partition_id)
{
        const UBGlobalPtr root = ub_memory.read_root(owner_region, catalog_root_slot);
        if (!root)
                return nullptr;
        Catalog *catalog = ub_memory.resolve_as<Catalog>(root);
        const uint32_t count = catalog->table_count.load(std::memory_order_acquire);
        const uint32_t limit = count < max_tables_per_region ? count : max_tables_per_region;
        for (uint32_t i = 0; i < limit; ++i) {
                TableDescriptor *descriptor = &catalog->tables[i];
                while (descriptor->ready.load(std::memory_order_acquire) == 0)
                        std::this_thread::yield();
                if (descriptor->table_id == table_id &&
                    descriptor->partition_id == partition_id)
                        return descriptor;
        }
        return nullptr;
}

UBTable::UBTable(TableDescriptor *descriptor)
        : descriptor_(descriptor)
{
        if (descriptor_ != nullptr) {
                buckets_ = ub_memory.resolve_as<Bucket>(descriptor_->buckets,
                                                        descriptor_->bucket_count);
                if (buckets_ == nullptr)
                        throw std::invalid_argument("invalid UB table bucket pointer");
        }
}

uint32_t UBTable::owner_region() const
{
        return descriptor_->buckets.region_id;
}

UBTable::Bucket *UBTable::bucket(uint64_t plain_key) const
{
        return &buckets_[plain_key % descriptor_->bucket_count];
}

UBTable::TupleNode *UBTable::node_from_offset(uint64_t offset) const
{
        if (offset == 0)
                return nullptr;
        return ub_memory.resolve_as<TupleNode>(
                UBGlobalPtr(owner_region(), descriptor_->buckets.generation, offset));
}

UBTable::TupleRef UBTable::make_ref(TupleNode *node) const
{
        TupleRef ref;
        if (node == nullptr)
                return ref;
        ref.node = node;
        ref.header = &node->tuple;
        ref.key = node->bytes;
        ref.value = node->bytes + descriptor_->key_size;
        return ref;
}

uint64_t UBTable::tuple_allocation_size() const
{
        return sizeof(TupleNode) + descriptor_->key_size + descriptor_->value_size;
}

UBTable::TupleRef UBTable::search(uint64_t plain_key) const
{
        return search_impl(plain_key, false);
}

UBTable::TupleRef UBTable::search_including_invalid(uint64_t plain_key) const
{
        return search_impl(plain_key, true);
}

UBTable::TupleRef UBTable::search_impl(uint64_t plain_key,
                                       bool include_invalid) const
{
        if (descriptor_ == nullptr)
                return TupleRef();
        Bucket *target = bucket(plain_key);
        while (!target->lock.try_lock_shared())
                std::this_thread::yield();
        TupleNode *node = node_from_offset(target->head_offset.load(std::memory_order_acquire));
        while (node != nullptr) {
                if (node->plain_key == plain_key &&
                    (include_invalid ||
                     node->tuple.valid.load(std::memory_order_acquire) == 1)) {
                        TupleRef ref = make_ref(node);
                        target->lock.unlock_shared();
                        return ref;
                }
                node = node_from_offset(node->next_offset.load(std::memory_order_acquire));
        }
        target->lock.unlock_shared();
        return TupleRef();
}

bool UBTable::insert(uint64_t plain_key, const void *key, const void *value,
                     uint32_t tuple_state)
{
        if (descriptor_ == nullptr || key == nullptr || value == nullptr ||
            (tuple_state != 1 && tuple_state != 2))
                return false;
        Bucket *target = bucket(plain_key);
        while (!target->lock.try_lock())
                std::this_thread::yield();
        TupleNode *existing =
                node_from_offset(target->head_offset.load(std::memory_order_acquire));
        TupleNode *same_key_tombstone = nullptr;
        for (TupleNode *node = existing; node != nullptr;
             node = node_from_offset(node->next_offset.load(std::memory_order_acquire))) {
                if (node->plain_key == plain_key) {
                        const uint32_t state =
                                node->tuple.valid.load(std::memory_order_acquire);
                        if (state != 0) {
                                target->lock.unlock();
                                return false;
                        }
                        if (same_key_tombstone == nullptr)
                                same_key_tombstone = node;
                }
        }

        // Reusing a tombstone for the same logical key is safe without a
        // global epoch: a requester holding an old lookup can only observe the
        // same key and must still acquire this tuple lock before reading.
        if (same_key_tombstone != nullptr) {
                while (!same_key_tombstone->tuple.lock.try_lock())
                        std::this_thread::yield();
                std::memcpy(same_key_tombstone->bytes, key, descriptor_->key_size);
                std::memcpy(same_key_tombstone->bytes + descriptor_->key_size, value,
                            descriptor_->value_size);
                same_key_tombstone->tuple.version.fetch_add(
                        1, std::memory_order_relaxed);
                same_key_tombstone->tuple.valid.store(tuple_state,
                                                       std::memory_order_release);
                same_key_tombstone->tuple.lock.unlock();
                descriptor_->reused_nodes.fetch_add(1, std::memory_order_relaxed);
                target->lock.unlock();
                return true;
        }

        uint64_t node_offset = 0;
        TupleNode *node = pop_free_node(node_offset);
        if (node == nullptr) {
                const UBGlobalPtr node_ptr = ub_memory.allocate(
                        owner_region(), tuple_allocation_size(), alignof(TupleNode));
                node_offset = node_ptr.offset;
                node = ub_memory.resolve_as<TupleNode>(node_ptr);
                new (&node->next_offset) std::atomic<uint64_t>(0);
                new (&node->tuple)
                        UBTupleHeader(descriptor_->key_size, descriptor_->value_size, 0);
                descriptor_->allocated_nodes.fetch_add(1, std::memory_order_relaxed);
        } else {
                node->tuple.version.fetch_add(1, std::memory_order_relaxed);
                descriptor_->reused_nodes.fetch_add(1, std::memory_order_relaxed);
        }
        node->next_offset.store(
                target->head_offset.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        node->plain_key = plain_key;
        std::memcpy(node->bytes, key, descriptor_->key_size);
        std::memcpy(node->bytes + descriptor_->key_size, value, descriptor_->value_size);
        node->tuple.key_size = descriptor_->key_size;
        node->tuple.value_size = descriptor_->value_size;
        node->tuple.valid.store(tuple_state, std::memory_order_release);
        target->head_offset.store(node_offset, std::memory_order_release);
        target->lock.unlock();
        return true;
}

UBTable::TupleNode *UBTable::pop_free_node(uint64_t &offset)
{
        while (!descriptor_->free_lock.try_lock())
                std::this_thread::yield();
        offset = descriptor_->free_head_offset.load(std::memory_order_relaxed);
        TupleNode *node = node_from_offset(offset);
        if (node != nullptr) {
                descriptor_->free_head_offset.store(
                        node->next_offset.load(std::memory_order_relaxed),
                        std::memory_order_release);
        }
        descriptor_->free_lock.unlock();
        return node;
}

void UBTable::push_free_node(TupleNode *node, uint64_t offset)
{
        while (!descriptor_->free_lock.try_lock())
                std::this_thread::yield();
        node->next_offset.store(
                descriptor_->free_head_offset.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        descriptor_->free_head_offset.store(offset, std::memory_order_release);
        descriptor_->free_lock.unlock();
}

bool UBTable::erase(uint64_t plain_key)
{
        TupleRef ref = search(plain_key);
        if (!ref)
                return false;
        while (!ref.header->lock.try_lock())
                std::this_thread::yield();
        const bool was_valid =
                ref.header->valid.exchange(0, std::memory_order_acq_rel) != 0;
        ref.header->version.fetch_add(1, std::memory_order_release);
        ref.header->lock.unlock();
        return was_valid;
}

bool UBTable::discard_placeholder(uint64_t plain_key)
{
        if (descriptor_ == nullptr)
                return false;
        Bucket *target = bucket(plain_key);
        while (!target->lock.try_lock())
                std::this_thread::yield();
        uint64_t current_offset =
                target->head_offset.load(std::memory_order_acquire);
        TupleNode *previous = nullptr;
        while (current_offset != 0) {
                TupleNode *node = node_from_offset(current_offset);
                const uint64_t next =
                        node->next_offset.load(std::memory_order_acquire);
                if (node->plain_key == plain_key &&
                    node->tuple.valid.load(std::memory_order_acquire) == 2) {
                        if (previous == nullptr)
                                target->head_offset.store(next,
                                                          std::memory_order_release);
                        else
                                previous->next_offset.store(next,
                                                            std::memory_order_release);
                        node->tuple.valid.store(0, std::memory_order_release);
                        node->tuple.version.fetch_add(1,
                                                      std::memory_order_relaxed);
                        push_free_node(node, current_offset);
                        target->lock.unlock();
                        return true;
                }
                previous = node;
                current_offset = next;
        }
        target->lock.unlock();
        return false;
}

void UBTable::scan(const std::function<bool(const TupleRef &)> &visitor) const
{
        if (descriptor_ == nullptr)
                return;
        for (uint64_t i = 0; i < descriptor_->bucket_count; ++i) {
                Bucket *target = &buckets_[i];
                while (!target->lock.try_lock_shared())
                        std::this_thread::yield();
                TupleNode *node =
                        node_from_offset(target->head_offset.load(std::memory_order_acquire));
                while (node != nullptr) {
                        if (node->tuple.valid.load(std::memory_order_acquire) == 1) {
                                const TupleRef ref = make_ref(node);
                                if (visitor(ref)) {
                                        target->lock.unlock_shared();
                                        return;
                                }
                        }
                        node = node_from_offset(
                                node->next_offset.load(std::memory_order_acquire));
                }
                target->lock.unlock_shared();
        }
}

void *UBTable::value_from_header(UBTupleHeader *header)
{
        if (header == nullptr)
                return nullptr;
        char *node_base = reinterpret_cast<char *>(header) - offsetof(TupleNode, tuple);
        return node_base + offsetof(TupleNode, bytes) + header->key_size;
}

} // namespace star
