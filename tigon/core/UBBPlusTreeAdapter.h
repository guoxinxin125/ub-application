#pragma once

#include "common/ClassOf.h"
#include "common/Encoder.h"
#include "common/StringPiece.h"
#include "common/UBMemory.h"
#include "core/Table.h"
#include "core/UBBPlusTree.h"

#include <atomic>
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
                         uint64_t index_size_hint = 0)
                : table_id_(table_id)
                , partition_id_(partition_id)
                , owner_region_(static_cast<uint32_t>(partition_id % ub_memory.region_count()))
                , index_size_hint_(index_size_hint)
        {
                if (owner_region_ == ub_memory.local_region_id()) {
                        descriptor_ = NativeTree::create(owner_region_, table_id_,
                                                         partition_id_);
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
                const auto ref = native_tree().search(*static_cast<const KeyType *>(key));
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
                return native_tree().insert(*static_cast<const KeyType *>(key),
                                            *static_cast<const ValueType *>(value),
                                            is_placeholder ? 2U : 1U);
        }

        bool insert_lock_next_key(
                const void *key, const void *value,
                std::function<bool(const void *, MetaDataType *, void *)>
                        next_key_processor,
                bool is_placeholder = false) override
        {
                return native_tree().insert_lock_next(
                        *static_cast<const KeyType *>(key),
                        *static_cast<const ValueType *>(value),
                        [&](const typename NativeTree::TupleRef &next) {
                                return next_key_processor(
                                        next.key, &next.header->version,
                                        next.value);
                        },
                        is_placeholder ? 2U : 1U);
        }

        bool insert_and_process_adjacent_tuples(
                const void *key, const void *value,
                std::function<bool(const void *, MetaDataType *, void *, const void *,
                                   MetaDataType *, void *)> processor,
                bool is_placeholder = false) override
        {
                return native_tree().insert_and_process_adjacent(
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
        }

        bool remove(const void *key) override
        {
                return native_tree().erase(*static_cast<const KeyType *>(key));
        }

        bool discard_placeholder(const void *key) override
        {
                return native_tree().discard_placeholder(
                        *static_cast<const KeyType *>(key));
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
        NativeTree &native_tree()
        {
                if (descriptor_ == nullptr) {
                        while ((descriptor_ = NativeTree::find(
                                        owner_region_, table_id_, partition_id_)) == nullptr)
                                std::this_thread::yield();
                }
                if (tree_.descriptor() != descriptor_)
                        tree_ = NativeTree(descriptor_);
                return tree_;
        }

        std::size_t table_id_;
        std::size_t partition_id_;
        uint32_t owner_region_;
        uint64_t index_size_hint_;
        typename NativeTree::Descriptor *descriptor_ = nullptr;
        NativeTree tree_;
};

} // namespace star
