#pragma once

#include "common/UBGlobalPtr.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace star {

// Requester-local, positive-only cache. A cached address is never a cached
// value or a substitute for the tuple lock. The caller must resolve the
// generation-tagged pointer and recheck tuple visibility on every hit.
class UBTupleOffsetCache {
public:
        static constexpr std::size_t capacity = 4096;

        UBGlobalPtr lookup(const void *table, uint64_t key) const
        {
                const Entry &entry = entries_[slot(table, key)];
                return entry.table == table && entry.key == key
                        ? entry.tuple : UBGlobalPtr();
        }

        void remember(const void *table, uint64_t key, UBGlobalPtr tuple)
        {
                entries_[slot(table, key)] = Entry{table, key, tuple};
        }

        void forget(const void *table, uint64_t key)
        {
                Entry &entry = entries_[slot(table, key)];
                if (entry.table == table && entry.key == key)
                        entry = Entry{};
        }

private:
        struct Entry {
                const void *table = nullptr;
                uint64_t key = 0;
                UBGlobalPtr tuple;
        };

        static std::size_t slot(const void *table, uint64_t key)
        {
                // Direct mapping bounds requester memory, including long runs.
                uint64_t mixed = key ^ (static_cast<uint64_t>(
                        reinterpret_cast<std::uintptr_t>(table)) >> 4);
                mixed ^= mixed >> 33;
                mixed *= 0xff51afd7ed558ccdULL;
                mixed ^= mixed >> 33;
                return static_cast<std::size_t>(mixed) & (capacity - 1);
        }

        std::array<Entry, capacity> entries_{};
};

} // namespace star
