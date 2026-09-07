#include "common/UBMPSCRingBuffer.h"

#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>

namespace star
{

uint64_t UBMPSCRingBuffer::align_up(uint64_t value, uint64_t alignment)
{
        return (value + alignment - 1) & ~(alignment - 1);
}

UBGlobalPtr UBMPSCRingBuffer::create(uint32_t owner_region, uint64_t capacity,
                                     uint64_t payload_size)
{
        if (capacity == 0 || payload_size == 0)
                throw std::invalid_argument("UB queue capacity and payload size must be positive");
        const uint64_t stride = align_up(sizeof(SlotPrefix) + payload_size, 64);
        if (capacity > UINT64_MAX / stride)
                throw std::overflow_error("UB queue allocation size overflow");

        const UBGlobalPtr state_ptr =
                ub_memory.allocate(owner_region, sizeof(SharedState), alignof(SharedState));
        const UBGlobalPtr slots_ptr =
                ub_memory.allocate(owner_region, capacity * stride, 64);
        SharedState *state = ub_memory.resolve_as<SharedState>(state_ptr);
        new (state) SharedState();
        state->capacity = capacity;
        state->payload_size = payload_size;
        state->slot_stride = stride;
        state->reserved = 0;
        state->head.store(0, std::memory_order_relaxed);
        state->tail.store(0, std::memory_order_relaxed);
        state->slots = slots_ptr;

        char *slots = ub_memory.resolve_as<char>(slots_ptr, capacity * stride);
        for (uint64_t i = 0; i < capacity; ++i) {
                SlotPrefix *slot = reinterpret_cast<SlotPrefix *>(slots + i * stride);
                new (&slot->sequence) std::atomic<uint64_t>(i);
                slot->length = 0;
        }
        std::atomic_thread_fence(std::memory_order_release);
        return state_ptr;
}

UBMPSCRingBuffer::UBMPSCRingBuffer(UBGlobalPtr state)
        : state_ptr_(state)
        , state_(ub_memory.resolve_as<SharedState>(state))
{
        if (state_ == nullptr)
                throw std::invalid_argument("invalid UB queue state pointer");
        slots_ = ub_memory.resolve_as<char>(state_->slots,
                                            state_->capacity * state_->slot_stride);
        if (slots_ == nullptr)
                throw std::invalid_argument("invalid UB queue slots pointer");
}

UBMPSCRingBuffer::SlotPrefix *UBMPSCRingBuffer::slot(uint64_t index) const
{
        return reinterpret_cast<SlotPrefix *>(slots_ +
                                              (index % state_->capacity) *
                                                      state_->slot_stride);
}

char *UBMPSCRingBuffer::payload(SlotPrefix *target) const
{
        return reinterpret_cast<char *>(target) + sizeof(SlotPrefix);
}

bool UBMPSCRingBuffer::try_enqueue(const void *data, uint64_t length)
{
        if (state_ == nullptr || data == nullptr || length > state_->payload_size)
                return false;
        uint64_t position = state_->tail.load(std::memory_order_relaxed);
        while (true) {
                SlotPrefix *target = slot(position);
                const uint64_t sequence = target->sequence.load(std::memory_order_acquire);
                const int64_t difference = static_cast<int64_t>(sequence - position);
                if (difference == 0) {
                        if (state_->tail.compare_exchange_weak(
                                    position, position + 1, std::memory_order_relaxed,
                                    std::memory_order_relaxed)) {
                                std::memcpy(payload(target), data, length);
                                target->length = length;
                                target->sequence.store(position + 1,
                                                       std::memory_order_release);
                                return true;
                        }
                } else if (difference < 0) {
                        return false;
                } else {
                        position = state_->tail.load(std::memory_order_relaxed);
                }
        }
}

void UBMPSCRingBuffer::enqueue(const void *data, uint64_t length)
{
        if (length > payload_size())
                throw std::length_error("UB message exceeds queue payload size");
        while (!try_enqueue(data, length))
                std::this_thread::yield();
}

uint64_t UBMPSCRingBuffer::try_dequeue(void *buffer, uint64_t capacity)
{
        if (state_ == nullptr || buffer == nullptr)
                return 0;
        const uint64_t position = state_->head.load(std::memory_order_relaxed);
        SlotPrefix *target = slot(position);
        const uint64_t sequence = target->sequence.load(std::memory_order_acquire);
        if (static_cast<int64_t>(sequence - (position + 1)) != 0)
                return 0;
        const uint64_t length = target->length;
        if (length > capacity || length > state_->payload_size)
                throw std::length_error("receiver buffer is smaller than UB message");
        std::memcpy(buffer, payload(target), length);
        state_->head.store(position + 1, std::memory_order_relaxed);
        target->sequence.store(position + state_->capacity,
                               std::memory_order_release);
        return length;
}

uint64_t UBMPSCRingBuffer::size() const
{
        if (state_ == nullptr)
                return 0;
        const uint64_t head = state_->head.load(std::memory_order_acquire);
        const uint64_t tail = state_->tail.load(std::memory_order_acquire);
        return tail - head;
}

uint64_t UBMPSCRingBuffer::payload_size() const
{
        return state_ == nullptr ? 0 : state_->payload_size;
}

} // namespace star
