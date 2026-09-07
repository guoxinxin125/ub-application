#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace star
{

// Process virtual addresses are never stored in UB shared memory.  Every
// persistent/shared reference uses this fixed-width coordinate instead.
struct UBGlobalPtr {
        static constexpr uint32_t null_region = std::numeric_limits<uint32_t>::max();

        uint32_t region_id;
        uint32_t generation;
        uint64_t offset;

        constexpr UBGlobalPtr()
                : region_id(null_region)
                , generation(0)
                , offset(0)
        {}

        constexpr UBGlobalPtr(uint32_t region_id, uint32_t generation, uint64_t offset)
                : region_id(region_id)
                , generation(generation)
                , offset(offset)
        {}

        bool is_null() const { return region_id == null_region; }
        explicit operator bool() const { return !is_null(); }
};

static_assert(sizeof(UBGlobalPtr) == 16, "UBGlobalPtr is part of the shared ABI");
static_assert(std::is_trivially_copyable<UBGlobalPtr>::value,
              "UBGlobalPtr must be safe to copy through shared memory");

} // namespace star
