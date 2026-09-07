#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace star
{

// Requesters execute these atomics directly on the mapped owner region.  No
// lock request is forwarded to an owner thread.
class UBSharedRWLock {
    public:
        static constexpr uint64_t writer_bit = 1ULL << 63;
        static constexpr uint64_t reader_mask = 0xffffffffULL;

        UBSharedRWLock()
                : state_(0)
        {}

        bool try_lock_shared()
        {
                uint64_t observed = state_.load(std::memory_order_acquire);
                while ((observed & writer_bit) == 0) {
                        const uint64_t readers = observed & reader_mask;
                        if (readers == reader_mask)
                                return false;
                        if (state_.compare_exchange_weak(observed, observed + 1,
                                                         std::memory_order_acquire,
                                                         std::memory_order_relaxed))
                                return true;
                }
                return false;
        }

        void unlock_shared()
        {
                uint64_t observed = state_.load(std::memory_order_relaxed);
                while (true) {
                        if ((observed & writer_bit) != 0 || (observed & reader_mask) == 0)
                                throw std::logic_error("invalid UB shared-lock release");
                        if (state_.compare_exchange_weak(observed, observed - 1,
                                                         std::memory_order_release,
                                                         std::memory_order_relaxed))
                                return;
                }
        }

        bool try_lock()
        {
                uint64_t unlocked = 0;
                return state_.compare_exchange_strong(unlocked, writer_bit,
                                                       std::memory_order_acquire,
                                                       std::memory_order_relaxed);
        }

        void unlock()
        {
                uint64_t writer = writer_bit;
                if (!state_.compare_exchange_strong(writer, 0,
                                                     std::memory_order_release,
                                                     std::memory_order_relaxed))
                        throw std::logic_error("invalid UB write-lock release");
        }

        uint32_t reader_count() const
        {
                return static_cast<uint32_t>(state_.load(std::memory_order_acquire) & reader_mask);
        }

        bool write_locked() const
        {
                return (state_.load(std::memory_order_acquire) & writer_bit) != 0;
        }

    private:
        std::atomic<uint64_t> state_;
};

struct alignas(64) UBTupleHeader {
        UBSharedRWLock lock;
        std::atomic<uint64_t> version;
        std::atomic<uint32_t> valid;
        uint32_t value_size;
        uint32_t key_size;
        uint32_t reserved;

        // valid: 0=tombstone, 1=visible tuple, 2=transactional placeholder.
        UBTupleHeader(uint32_t key_size, uint32_t value_size, uint32_t valid = 1)
                : lock()
                , version(0)
                , valid(valid)
                , value_size(value_size)
                , key_size(key_size)
                , reserved(0)
        {}

        static UBTupleHeader *from_version(std::atomic<uint64_t> *version)
        {
                return reinterpret_cast<UBTupleHeader *>(
                        reinterpret_cast<char *>(version) - offsetof(UBTupleHeader, version));
        }
};

static_assert(alignof(UBTupleHeader) == 64, "tuple locks must be cache-line aligned");

} // namespace star
