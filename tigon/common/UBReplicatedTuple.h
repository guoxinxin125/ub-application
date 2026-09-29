#pragma once

#include "common/UBTuple.h"
#include "common/UBScalarRead.h"
#include "common/UBReplicaProfile.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace star
{

// Experimental two-copy tuple protocol. Both views must refer to the same key
// in different UB regions. A view's generation is checked against the region
// generation captured by the caller; stale mappings must not be reused.
struct UBReplicaTupleView {
        UBTupleHeader *header;
        void *value;
        uint32_t region_id;
        uint32_t generation;
};

// The real UB implementation must provide a completion operation for NC writes.
// A CPU fence alone is not assumed to imply remote visibility when delayed
// completion is enabled. Host-only tests supply ordinary memcpy + CPU fences.
class UBReplicaWriteOps {
    public:
        virtual ~UBReplicaWriteOps() = default;
        virtual void copy(void *destination, const void *source, std::size_t size) = 0;
        virtual bool complete() = 0;
};

// Trial implementation for imported NC mappings. Readback checks what this
// requester sees; two-host tests must still establish the platform's owner-WB
// visibility and ordering before results are considered valid.
class UBCheckedReplicaWriteOps : public UBReplicaWriteOps {
    public:
        void copy(void *destination, const void *source, std::size_t size) override
        {
                std::memcpy(destination, source, size);
                if (++copies_ == 2) {
                        remote_ = destination;
                        expected_ = source;
                        size_ = size;
                }
        }

        bool complete() override
        {
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (remote_ == nullptr)
                        return false;
                std::vector<unsigned char> observed(size_);
                UBScalarRead::copy(observed.data(), remote_, size_);
                return std::memcmp(observed.data(), expected_, size_) == 0;
        }

    private:
        unsigned int copies_ = 0;
        const void *remote_ = nullptr;
        const void *expected_ = nullptr;
        std::size_t size_ = 0;
};

class UBReplicatedTuple {
    public:
        static bool read_local(const UBReplicaTupleView &local,
                               uint32_t current_generation, void *destination,
                               std::size_t size, uint64_t *version = nullptr)
        {
                if (!valid_view(local, current_generation, size) ||
                    !local.header->lock.try_lock_shared())
                        return false;
                const bool visible = local.header->valid.load(std::memory_order_acquire) == 1;
                if (visible) {
                        std::memcpy(destination, local.value, size);
                        if (version)
                                *version = local.header->version.load(std::memory_order_acquire);
                }
                local.header->lock.unlock_shared();
                return visible;
        }

        class WriteGuard {
            public:
                WriteGuard(const UBReplicaTupleView &local,
                           const UBReplicaTupleView &peer,
                           uint32_t local_generation, uint32_t peer_generation)
                        : local_(local), peer_(peer)
                {
                        if (local.region_id == peer.region_id ||
                            !valid_view(local, local_generation, local.header ? local.header->value_size : 0) ||
                            !valid_view(peer, peer_generation, peer.header ? peer.header->value_size : 0) ||
                            local.header->value_size != peer.header->value_size ||
                            local.header->key_size != peer.header->key_size)
                                return;
                        // Non-blocking acquisition avoids deadlock even when a
                        // transaction already holds locks on other keys.
                        first_ = local.region_id < peer.region_id ? &local_ : &peer_;
                        second_ = first_ == &local_ ? &peer_ : &local_;
                        if (!first_->header->lock.try_lock())
                                return;
                        if (!second_->header->lock.try_lock()) {
                                first_->header->lock.unlock();
                                return;
                        }
                        if (local.header->valid.load(std::memory_order_acquire) != 1 ||
                            peer.header->valid.load(std::memory_order_acquire) != 1 ||
                            local.header->version.load(std::memory_order_acquire) !=
                                    peer.header->version.load(std::memory_order_acquire)) {
                                release();
                                return;
                        }
                        acquired_ = true;
                }

                WriteGuard(const WriteGuard &) = delete;
                WriteGuard &operator=(const WriteGuard &) = delete;

                ~WriteGuard()
                {
                        // In-doubt writes are deliberately not unlocked: callers
                        // must stop the experiment and rebuild the region.
                        if (acquired_ && !in_doubt_)
                                release();
                }

                bool acquired() const { return acquired_; }
                bool in_doubt() const { return in_doubt_; }

                bool read_local(void *destination, std::size_t size,
                                uint64_t &version) const
                {
                        if (!acquired_ || destination == nullptr ||
                            size != local_.header->value_size)
                                return false;
                        std::memcpy(destination, local_.value, size);
                        version = local_.header->version.load(std::memory_order_acquire);
                        return true;
                }

                bool write_data(const void *value, std::size_t size,
                                UBReplicaWriteOps &ops,
                                UBReplicaProfile::CommitSample *profile = nullptr)
                {
                        if (!acquired_ || in_doubt_ || value == nullptr ||
                            size != local_.header->value_size)
                                return false;
                        in_doubt_ = true;
                        UBReplicaProfile::Clock::time_point start;
                        if (profile) start = UBReplicaProfile::Clock::now();
                        ops.copy(local_.value, value, size);
                        if (profile) {
                                profile->local_write_ns += UBReplicaProfile::elapsed_ns(start);
                                start = UBReplicaProfile::Clock::now();
                        }
                        ops.copy(peer_.value, value, size);
                        if (profile) {
                                profile->peer_write_ns += UBReplicaProfile::elapsed_ns(start);
                                start = UBReplicaProfile::Clock::now();
                        }
                        const bool completed = ops.complete();
                        if (profile) {
                                profile->data_complete_ns += UBReplicaProfile::elapsed_ns(start);
                                ++profile->write_keys;
                                profile->write_bytes += size;
                        }
                        return completed;
                }

                bool publish(uint64_t new_version, UBReplicaWriteOps &ops,
                             UBReplicaProfile::CommitSample *profile = nullptr)
                {
                        if (!acquired_ || !in_doubt_ ||
                            new_version <= local_.header->version.load(std::memory_order_acquire))
                                return false;
                        UBReplicaProfile::Clock::time_point start;
                        if (profile) start = UBReplicaProfile::Clock::now();
                        local_.header->version.store(new_version, std::memory_order_release);
                        if (profile) {
                                profile->local_version_ns += UBReplicaProfile::elapsed_ns(start);
                                start = UBReplicaProfile::Clock::now();
                        }
                        peer_.header->version.store(new_version, std::memory_order_release);
                        if (profile) {
                                profile->peer_version_ns += UBReplicaProfile::elapsed_ns(start);
                                start = UBReplicaProfile::Clock::now();
                        }
                        const bool completed = ops.complete();
                        if (profile) {
                                profile->version_complete_ns += UBReplicaProfile::elapsed_ns(start);
                                start = UBReplicaProfile::Clock::now();
                        }
                        if (!completed)
                                return false;
                        const bool verified = peer_.header->version.load(std::memory_order_acquire) == new_version;
                        if (profile) profile->peer_version_verify_ns += UBReplicaProfile::elapsed_ns(start);
                        if (!verified)
                                return false;
                        in_doubt_ = false;
                        return true;
                }

                // The caller must keep this guard alive until its transaction
                // commits. The integrated path stages every key before it
                // publishes versions and starts releasing any locks.
                bool commit(const void *value, std::size_t size,
                            uint64_t new_version, UBReplicaWriteOps &ops)
                {
                        if (!acquired_ || in_doubt_ ||
                            new_version <= local_.header->version.load(std::memory_order_acquire))
                                return false;
                        if (!write_data(value, size, ops))
                                return false;
                        if (!publish(new_version, ops))
                                return false;
                        release();
                        return true;
                }

                void unlock()
                {
                        if (!acquired_ || in_doubt_)
                                throw std::logic_error("cannot unlock an in-doubt replica pair");
                        release();
                }

            private:
                void release()
                {
                        second_->header->lock.unlock();
                        first_->header->lock.unlock();
                        acquired_ = false;
                }

                UBReplicaTupleView local_;
                UBReplicaTupleView peer_;
                UBReplicaTupleView *first_ = nullptr;
                UBReplicaTupleView *second_ = nullptr;
                bool acquired_ = false;
                bool in_doubt_ = false;
        };

    private:
        static bool valid_view(const UBReplicaTupleView &view,
                               uint32_t current_generation, std::size_t size)
        {
                return view.header != nullptr && view.value != nullptr &&
                       view.generation == current_generation &&
                       size == view.header->value_size;
        }
};

} // namespace star
