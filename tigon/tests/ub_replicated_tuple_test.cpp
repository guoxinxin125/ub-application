#include "common/UBReplicatedTuple.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{

void require(bool condition, const char *message)
{
        if (!condition)
                throw std::runtime_error(message);
}

struct Replica {
        star::UBTupleHeader header{8, sizeof(uint64_t)};
        uint64_t value = 0;
};

class HostWriteOps : public star::UBReplicaWriteOps {
    public:
        explicit HostWriteOps(bool fail = false) : fail_(fail) {}
        void copy(void *destination, const void *source, std::size_t size) override
        {
                std::memcpy(destination, source, size);
        }
        bool complete() override
        {
                std::atomic_thread_fence(std::memory_order_seq_cst);
                return !fail_;
        }
    private:
        bool fail_;
};

star::UBReplicaTupleView view(Replica &replica, uint32_t region, uint32_t generation)
{
        return {&replica.header, &replica.value, region, generation};
}

} // namespace

int main()
{
        Replica a, b;
        auto va = view(a, 0, 7);
        auto vb = view(b, 1, 9);
        uint64_t result = 99;
        require(!star::UBReplicatedTuple::read_local(va, 8, &result, sizeof(result)),
                "stale local generation accepted");
        require(star::UBReplicatedTuple::read_local(va, 7, &result, sizeof(result)) && result == 0,
                "initial local read failed");

        HostWriteOps ops;
        {
                star::UBReplicatedTuple::WriteGuard guard(va, vb, 7, 9);
                require(guard.acquired(), "two-copy lock acquisition failed");
                require(!star::UBReplicatedTuple::read_local(vb, 9, &result, sizeof(result)),
                        "reader observed a write-locked peer");
                uint64_t update = 42;
                require(guard.commit(&update, sizeof(update), 1, ops), "commit failed");
        }
        require(a.value == 42 && b.value == 42, "replica values differ");
        require(a.header.version.load() == 1 && b.header.version.load() == 1,
                "replica versions differ");

        {
                star::UBReplicatedTuple::WriteGuard stale(va, vb, 7, 10);
                require(!stale.acquired(), "stale peer generation accepted");
                b.header.lock.try_lock_shared();
                star::UBReplicatedTuple::WriteGuard contended(va, vb, 7, 9);
                require(!contended.acquired(), "writer acquired a read-locked replica");
                b.header.lock.unlock_shared();
                require(!a.header.lock.write_locked(), "failed acquisition leaked first lock");
        }

        constexpr int iterations = 2000;
        std::vector<std::thread> threads;
        for (int id = 0; id < 4; ++id) {
                threads.emplace_back([&, id]() {
                        auto local = id % 2 ? vb : va;
                        auto peer = id % 2 ? va : vb;
                        auto local_generation = id % 2 ? 9u : 7u;
                        auto peer_generation = id % 2 ? 7u : 9u;
                        HostWriteOps writer;
                        for (int i = 0; i < iterations; ++i) {
                                star::UBReplicatedTuple::WriteGuard guard(
                                        local, peer, local_generation, peer_generation);
                                if (!guard.acquired())
                                        continue;
                                uint64_t next = a.value + 1;
                                uint64_t version = a.header.version.load() + 1;
                                require(guard.commit(&next, sizeof(next), version, writer),
                                        "concurrent commit failed");
                        }
                });
        }
        for (auto &thread : threads)
                thread.join();
        require(a.value == b.value && a.header.version.load() == b.header.version.load(),
                "concurrent replicas diverged");
        require(a.value > 42, "concurrent writes made no progress");

        Replica e, f, g, h;
        auto ve = view(e, 0, 1);
        auto vf = view(f, 1, 1);
        auto vg = view(g, 0, 1);
        auto vh = view(h, 1, 1);
        {
                star::UBReplicatedTuple::WriteGuard first(ve, vf, 1, 1);
                star::UBReplicatedTuple::WriteGuard second(vg, vh, 1, 1);
                require(first.acquired() && second.acquired(), "multi-key guards failed");
                uint64_t one = 11, two = 22;
                star::UBCheckedReplicaWriteOps one_ops, two_ops;
                star::UBReplicaProfile::CommitSample profile;
                require(first.write_data(&one, sizeof(one), one_ops, &profile), "first data write failed");
                require(second.write_data(&two, sizeof(two), two_ops, &profile), "second data write failed");
                require(!star::UBReplicatedTuple::read_local(vf, 1, &result, sizeof(result)) &&
                        !star::UBReplicatedTuple::read_local(vh, 1, &result, sizeof(result)),
                        "partial multi-key write became readable");
                require(first.publish(1, one_ops, &profile) && second.publish(1, two_ops, &profile),
                        "multi-key versions failed");
                first.unlock();
                second.unlock();
                require(profile.write_keys == 2 && profile.write_bytes == 2 * sizeof(uint64_t),
                        "profile did not count both written keys");
                star::UBReplicaProfile::record_commit(profile);
                require(star::UBReplicaProfile::commit_counters().transactions.load() == 1 &&
                        star::UBReplicaProfile::commit_counters().write_keys.load() == 2,
                        "commit profile counters were not updated");
        }
        star::UBReplicaProfile::LookupSample lookup;
        lookup.found = true;
        lookup.success = true;
        lookup.local_lookup_ns = 5;
        star::UBReplicaProfile::record_lookup(true, lookup);
        require(star::UBReplicaProfile::lookup_counters(true).samples.load() == 1 &&
                star::UBReplicaProfile::lookup_counters(true).local_lookup_ns.load() == 5 &&
                star::UBReplicaProfile::lookup_counters(false).samples.load() == 0,
                "lookup profile counters were not separated by operation");
        require(e.value == f.value && f.value == 11 &&
                g.value == h.value && h.value == 22,
                "multi-key replica contents differ");

        Replica c, d;
        auto vc = view(c, 0, 1);
        auto vd = view(d, 1, 1);
        HostWriteOps failed(true);
        {
                star::UBReplicatedTuple::WriteGuard guard(vc, vd, 1, 1);
                require(guard.acquired(), "failure-path lock acquisition failed");
                uint64_t update = 1;
                require(!guard.commit(&update, sizeof(update), 1, failed) && guard.in_doubt(),
                        "failed completion did not enter in-doubt state");
        }
        require(c.header.lock.write_locked() && d.header.lock.write_locked(),
                "in-doubt replicas were unlocked");
        return 0;
}
