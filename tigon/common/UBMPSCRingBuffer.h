#pragma once

#include "common/UBGlobalPtr.h"
#include "common/UBMemory.h"

#include <atomic>
#include <cstdint>

namespace star
{

// Fixed-size, copy-based queue. Payload bytes are copied from the sender's
// local Message into the destination region and copied into a receiver-local
// Message on dequeue. UB mappings need no CXL clflush/clwb instructions.
class UBMPSCRingBuffer {
    public:
        static constexpr uint32_t inbox_root_slot = 9;

        struct alignas(64) SharedState {
                uint64_t capacity;
                uint64_t payload_size;
                uint64_t slot_stride;
                uint64_t reserved;
                std::atomic<uint64_t> head;
                std::atomic<uint64_t> tail;
                UBGlobalPtr slots;
        };

        static UBGlobalPtr create(uint32_t owner_region, uint64_t capacity,
                                  uint64_t payload_size);

        UBMPSCRingBuffer() = default;
        explicit UBMPSCRingBuffer(UBGlobalPtr state);

        bool try_enqueue(const void *data, uint64_t size);
        void enqueue(const void *data, uint64_t size);
        uint64_t try_dequeue(void *buffer, uint64_t capacity);
        uint64_t size() const;
        uint64_t payload_size() const;
        bool valid() const { return state_ != nullptr; }

    private:
        struct SlotPrefix {
                std::atomic<uint64_t> sequence;
                uint64_t length;
        };

        static uint64_t align_up(uint64_t value, uint64_t alignment);
        SlotPrefix *slot(uint64_t index) const;
        char *payload(SlotPrefix *slot) const;

        UBGlobalPtr state_ptr_;
        SharedState *state_ = nullptr;
        char *slots_ = nullptr;
};

} // namespace star
