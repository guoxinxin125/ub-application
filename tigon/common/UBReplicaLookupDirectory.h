#pragma once

#include "common/UBGlobalPtr.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace star {

// Process-local directory for the fixed-key replicated YCSB RMW workload.
// Entries contain tuple addresses, never values; the caller must resolve the
// generation and check key/visibility before using an entry.
class UBReplicaLookupDirectory {
public:
        UBReplicaLookupDirectory(std::size_t count, uint64_t first_key,
                                 uint64_t stride)
                : first_key_(first_key), stride_(stride), entries_(count) {}

        bool remember(uint64_t key, UBGlobalPtr tuple)
        {
                std::size_t index;
                if (!slot(key, index))
                        return false;
                entries_[index] = tuple;
                return true;
        }

        UBGlobalPtr lookup(uint64_t key) const
        {
                std::size_t index;
                return slot(key, index) ? entries_[index] : UBGlobalPtr();
        }

private:
        bool slot(uint64_t key, std::size_t &index) const
        {
                if (stride_ == 0 || key < first_key_)
                        return false;
                const uint64_t difference = key - first_key_;
                if (difference % stride_ != 0 ||
                    difference / stride_ >= entries_.size())
                        return false;
                index = static_cast<std::size_t>(difference / stride_);
                return true;
        }

        uint64_t first_key_;
        uint64_t stride_;
        std::vector<UBGlobalPtr> entries_;
};

} // namespace star
