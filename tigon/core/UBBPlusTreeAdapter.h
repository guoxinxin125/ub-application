#pragma once

#include "common/ClassOf.h"
#include "common/Encoder.h"
#include "common/StringPiece.h"
#include "common/UBMemory.h"
#include "common/UBPointProfile.h"
#include "common/UBTupleOffsetCache.h"
#include "core/Table.h"
#include "core/UBBPlusTree.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <tuple>

namespace star
{

template <class KeyType, class ValueType, class KeyComparator, class ValueComparator>
class TableUBBPlusTree : public ITable {
    public:
        using MetaDataType = std::atomic<uint64_t>;
        using NativeTree = UBBPlusTree<KeyType, ValueType, KeyComparator>;

        // index_size_hint is retained in the constructor ABI so existing UB
        // command lines remain compatible. B+ Tree growth is allocator-driven.
        TableUBBPlusTree(std::size_t table_id, std::size_t partition_id,
                         uint64_t index_size_hint = 0,
                         bool enable_rmw_cache = false)
                : table_id_(table_id)
                , partition_id_(partition_id)
                , owner_region_(static_cast<uint32_t>(partition_id % ub_memory.region_count()))
                , index_size_hint_(index_size_hint)
                , rmw_cache_enabled_(enable_rmw_cache &&
                        ub_memory.memory_mode() == UBMemory::MemoryMode::ONE_SIDED &&
                        cache_requested())
        {
                if (owner_region_ == ub_memory.local_region_id()) {
                        descriptor_ = NativeTree::create(owner_region_, table_id_,
                                                         partition_id_);
                        descriptor_ptr_ = ub_memory.to_global(owner_region_, descriptor_);
                }
        }

        uint64_t get_plain_key(const void *key) override
        {
                return static_cast<const KeyType *>(key)->get_plain_key();
        }

        int compare_key(const void *a, const void *b) override
        {
                return KeyComparator()(*static_cast<const KeyType *>(a),
                                       *static_cast<const KeyType *>(b));
        }

        std::tuple<MetaDataType *, void *> search(const void *key) override
        {
                const KeyType &typed_key = *static_cast<const KeyType *>(key);
                auto *profile = UBPointProfile::active_lookup();
                auto tree_search = [&]() {
                        const auto start = profile ? UBPointProfile::Clock::now()
                                                   : UBPointProfile::Clock::time_point{};
                        NativeTree &tree = native_tree();
                        if (profile != nullptr)
                                profile->tree_prepare_ns += UBPointProfile::elapsed_ns(start);
                        return tree.search(typed_key);
                };
                if (rmw_cache_enabled_ && owner_region_ != ub_memory.local_region_id()) {
                        UBTupleOffsetCache &cache = local_cache();
                        const uint64_t plain_key = typed_key.get_plain_key();
                        const auto probe_start = profile ? UBPointProfile::Clock::now()
                                                         : UBPointProfile::Clock::time_point{};
                        const UBGlobalPtr cached = cache.lookup(descriptor_, plain_key);
                        if (profile != nullptr)
                                profile->cache_probe_ns += UBPointProfile::elapsed_ns(probe_start);
                        if (cached) {
                                const auto validate_start = profile ? UBPointProfile::Clock::now()
                                                                    : UBPointProfile::Clock::time_point{};
                                // resolve() rejects a previous region generation before
                                // any tuple memory is dereferenced. No negative caching.
                                auto *node = ub_memory.resolve_as<typename NativeTree::TupleNode>(cached);
                                const bool valid = node != nullptr &&
                                        node->tuple.valid.load(std::memory_order_acquire) == 1;
                                if (profile != nullptr) {
                                        profile->cache_validate_ns += UBPointProfile::elapsed_ns(validate_start);
                                        profile->cache_hit = valid;
                                        profile->cache_stale = !valid;
                                }
                                if (valid)
                                        return std::make_tuple(&node->tuple.version,
                                                               static_cast<void *>(&node->value));
                                cache.forget(descriptor_, plain_key);
                        }
                        if (profile != nullptr)
                                profile->cache_miss = true;
                        const auto ref = tree_search();
                        if (!ref)
                                return std::make_tuple(nullptr, nullptr);
                        cache.remember(descriptor_, plain_key,
                                       ub_memory.to_global(owner_region_, ref.node));
                        return std::make_tuple(&ref.header->version,
                                               static_cast<void *>(ref.value));
                }
                const auto ref = tree_search();
                if (!ref)
                        return std::make_tuple(nullptr, nullptr);
                return std::make_tuple(&ref.header->version,
                                       static_cast<void *>(ref.value));
        }

        void *search_value(const void *key) override
        {
                return std::get<1>(search(key));
        }

        MetaDataType *search_metadata(const void *key) override
        {
                return std::get<0>(search(key));
        }

        MetaDataType *search_metadata_including_invalid(const void *key) override
        {
                const auto ref = native_tree().search_including_invalid(
                        *static_cast<const KeyType *>(key));
                return ref ? &ref.header->version : nullptr;
        }

        bool contains(const void *key) override
        {
                return static_cast<bool>(native_tree().search(
                        *static_cast<const KeyType *>(key)));
        }

        void scan(const void *min_key,
                  std::function<bool(const void *, MetaDataType *, void *, bool)>
                          scan_processor) override
        {
                // Legacy unbounded scans keep the original ITable callback
                // convention. Transactions use scan_range() below so the
                // successor/end-gap row is explicit.
                native_tree().scan(*static_cast<const KeyType *>(min_key),
                                   [&](const typename NativeTree::TupleRef &ref) {
                        return scan_processor(ref.key, &ref.header->version,
                                              ref.value, false);
                });
        }

        bool scan_range(
                const void *min_key, const void *max_key, uint64_t limit,
                std::function<bool(const void *, MetaDataType *, void *, bool)>
                        scan_processor) override
        {
                return native_tree().scan_range(
                        *static_cast<const KeyType *>(min_key),
                        *static_cast<const KeyType *>(max_key), limit,
                        [&](const typename NativeTree::TupleRef &ref,
                            bool protection_row) {
                                return scan_processor(
                                        ref.key, &ref.header->version, ref.value,
                                        protection_row);
                        });
        }

        bool insert(const void *key, const void *value,
                    bool is_placeholder = false) override
        {
                const bool inserted = native_tree().insert(*static_cast<const KeyType *>(key),
                                                           *static_cast<const ValueType *>(value),
                                                           is_placeholder ? 2U : 1U);
                if (inserted && rmw_cache_enabled_)
                        local_cache().forget(descriptor_, static_cast<const KeyType *>(key)->get_plain_key());
                return inserted;
        }

        bool insert_lock_next_key(
                const void *key, const void *value,
                std::function<bool(const void *, MetaDataType *, void *)>
                        next_key_processor,
                bool is_placeholder = false) override
        {
                const bool inserted = native_tree().insert_lock_next(
                        *static_cast<const KeyType *>(key),
                        *static_cast<const ValueType *>(value),
                        [&](const typename NativeTree::TupleRef &next) {
                                return next_key_processor(
                                        next.key, &next.header->version,
                                        next.value);
                        },
                        is_placeholder ? 2U : 1U);
                if (inserted && rmw_cache_enabled_)
                        local_cache().forget(descriptor_, static_cast<const KeyType *>(key)->get_plain_key());
                return inserted;
        }

        bool insert_and_process_adjacent_tuples(
                const void *key, const void *value,
                std::function<bool(const void *, MetaDataType *, void *, const void *,
                                   MetaDataType *, void *)> processor,
                bool is_placeholder = false) override
        {
                const bool inserted = native_tree().insert_and_process_adjacent(
                        *static_cast<const KeyType *>(key),
                        *static_cast<const ValueType *>(value),
                        [&](const typename NativeTree::TupleRef *previous,
                            const typename NativeTree::TupleRef *next) {
                                return processor(
                                        previous ? previous->key : nullptr,
                                        previous ? &previous->header->version : nullptr,
                                        previous ? previous->value : nullptr,
                                        next ? next->key : nullptr,
                                        next ? &next->header->version : nullptr,
                                        next ? next->value : nullptr);
                        },
                        is_placeholder ? 2U : 1U);
                if (inserted && rmw_cache_enabled_)
                        local_cache().forget(descriptor_, static_cast<const KeyType *>(key)->get_plain_key());
                return inserted;
        }

        bool remove(const void *key) override
        {
                const bool removed = native_tree().erase(*static_cast<const KeyType *>(key));
                if (removed && rmw_cache_enabled_)
                        local_cache().forget(descriptor_, static_cast<const KeyType *>(key)->get_plain_key());
                return removed;
        }

        bool discard_placeholder(const void *key) override
        {
                const bool discarded = native_tree().discard_placeholder(
                        *static_cast<const KeyType *>(key));
                if (discarded && rmw_cache_enabled_)
                        local_cache().forget(descriptor_, static_cast<const KeyType *>(key)->get_plain_key());
                return discarded;
        }

        bool remove_and_process_adjacent_tuples(
                const void *key,
                std::function<bool(const void *, void *, void *, const void *, void *,
                                   void *, const void *, void *, void *)>) override
        {
                return remove(key);
        }

        void update(const void *key, const void *value,
                    std::function<void(const void *, const void *)> on_update =
                            [](const void *, const void *) {}) override
        {
                const auto ref = native_tree().search(*static_cast<const KeyType *>(key));
                CHECK(ref);
                on_update(key, ref.value);
                std::memcpy(ref.value, value, sizeof(ValueType));
        }

        bool search_and_update_next_key_info(
                const void *,
                std::function<void(const void *, void *, void *, const void *, void *,
                                   void *, const void *, void *, void *)>) override
        {
                return true;
        }

        void deserialize_value(const void *key, StringPiece string_piece) override
        {
                ValueType value;
                Decoder decoder(string_piece);
                decoder >> value;
                update(key, &value);
        }

        void serialize_value(Encoder &encoder, const void *value) override
        {
                encoder << *static_cast<const ValueType *>(value);
        }

        std::size_t key_size() override { return sizeof(KeyType); }
        std::size_t value_size() override { return sizeof(ValueType); }
        std::size_t field_size() override { return ClassOf<ValueType>::size(); }
        std::size_t tableID() override { return table_id_; }
        std::size_t partitionID() override { return partition_id_; }
        int tableType() override { return BTREE; }

        void move_all_into_cxl(
                std::function<bool(ITable *, const void *,
                                   std::tuple<MetaDataType *, void *> &, bool)>) override
        {
                CHECK(0) << "native UB tuples are never migrated";
        }

    private:
        static bool cache_requested()
        {
                const char *value = std::getenv("TIGON_UB_RMW_TUPLE_CACHE");
                return value != nullptr && value[0] == '1' && value[1] == '\0';
        }

        static UBTupleOffsetCache &local_cache()
        {
                static thread_local UBTupleOffsetCache cache;
                return cache;
        }

        NativeTree &native_tree()
        {
                if (rmw_cache_enabled_ && descriptor_ != nullptr &&
                    ub_memory.resolve(descriptor_ptr_, sizeof(typename NativeTree::Descriptor)) != descriptor_) {
                        descriptor_ = nullptr;
                        tree_ = NativeTree();
                }
                if (descriptor_ == nullptr) {
                        while ((descriptor_ = NativeTree::find(
                                        owner_region_, table_id_, partition_id_)) == nullptr)
                                std::this_thread::yield();
                        descriptor_ptr_ = ub_memory.to_global(owner_region_, descriptor_);
                }
                if (tree_.descriptor() != descriptor_)
                        tree_ = NativeTree(descriptor_);
                return tree_;
        }

        std::size_t table_id_;
        std::size_t partition_id_;
        uint32_t owner_region_;
        uint64_t index_size_hint_;
        bool rmw_cache_enabled_;
        typename NativeTree::Descriptor *descriptor_ = nullptr;
        UBGlobalPtr descriptor_ptr_;
        NativeTree tree_;
};

} // namespace star
