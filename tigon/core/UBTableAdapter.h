#pragma once

#include "common/ClassOf.h"
#include "common/Encoder.h"
#include "common/StringPiece.h"
#include "common/UBMemory.h"
#include "core/Table.h"
#include "core/UBTable.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <thread>
#include <tuple>
#include <vector>

namespace star
{

template <class KeyType, class ValueType, class KeyComparator, class ValueComparator>
class TableUBHash : public ITable {
    public:
        using MetaDataType = std::atomic<uint64_t>;

        TableUBHash(std::size_t table_id, std::size_t partition_id,
                    uint64_t bucket_count)
                : table_id_(table_id)
                , partition_id_(partition_id)
                , owner_region_(static_cast<uint32_t>(partition_id % ub_memory.region_count()))
                , bucket_count_(bucket_count)
        {
                if (owner_region_ == ub_memory.local_region_id()) {
                        descriptor_ = UBTable::create(owner_region_, table_id_, partition_id_,
                                                      sizeof(KeyType), sizeof(ValueType),
                                                      bucket_count_);
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
                const UBTable::TupleRef ref = native_table().search(get_plain_key(key));
                if (!ref)
                        return std::make_tuple(nullptr, nullptr);
                return std::make_tuple(&ref.header->version, ref.value);
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
                const UBTable::TupleRef ref = native_table().search_including_invalid(
                        get_plain_key(key));
                return ref ? &ref.header->version : nullptr;
        }

        bool contains(const void *key) override
        {
                return static_cast<bool>(native_table().search(get_plain_key(key)));
        }

        void scan(const void *min_key,
                  std::function<bool(const void *, MetaDataType *, void *, bool)>
                          scan_processor) override
        {
                std::vector<UBTable::TupleRef> rows;
                native_table().scan([&](const UBTable::TupleRef &ref) {
                        if (compare_key(ref.key, min_key) >= 0)
                                rows.push_back(ref);
                        return false;
                });
                std::sort(rows.begin(), rows.end(), [&](const UBTable::TupleRef &a,
                                                        const UBTable::TupleRef &b) {
                        return compare_key(a.key, b.key) < 0;
                });
                for (std::size_t i = 0; i < rows.size(); ++i) {
                        if (scan_processor(rows[i].key, &rows[i].header->version,
                                           rows[i].value, i + 1 == rows.size()))
                                break;
                }
        }

        bool insert(const void *key, const void *value,
                    bool is_placeholder = false) override
        {
                return native_table().insert(get_plain_key(key), key, value,
                                             is_placeholder ? 2U : 1U);
        }

        bool insert_lock_next_key(
                const void *key, const void *value,
                std::function<bool(const void *, MetaDataType *, void *)>,
                bool is_placeholder = false) override
        {
                return insert(key, value, is_placeholder);
        }

        bool insert_and_process_adjacent_tuples(
                const void *key, const void *value,
                std::function<bool(const void *, MetaDataType *, void *, const void *,
                                   MetaDataType *, void *)>,
                bool is_placeholder = false) override
        {
                return insert(key, value, is_placeholder);
        }

        bool remove(const void *key) override
        {
                return native_table().erase(get_plain_key(key));
        }

        bool discard_placeholder(const void *key) override
        {
                return native_table().discard_placeholder(get_plain_key(key));
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
                const UBTable::TupleRef ref = native_table().search(get_plain_key(key));
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
        int tableType() override { return HASHMAP; }

        void move_all_into_cxl(
                std::function<bool(ITable *, const void *,
                                   std::tuple<MetaDataType *, void *> &, bool)>) override
        {
                CHECK(0) << "native UB tuples are never migrated";
        }

    private:
        UBTable &native_table()
        {
                if (descriptor_ == nullptr) {
                        while ((descriptor_ = UBTable::find(owner_region_, table_id_,
                                                            partition_id_)) == nullptr)
                                std::this_thread::yield();
                }
                if (table_.descriptor() != descriptor_)
                        table_ = UBTable(descriptor_);
                return table_;
        }

        std::size_t table_id_;
        std::size_t partition_id_;
        uint32_t owner_region_;
        uint64_t bucket_count_;
        UBTable::TableDescriptor *descriptor_ = nullptr;
        UBTable table_;
};

} // namespace star
