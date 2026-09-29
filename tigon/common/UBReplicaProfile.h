#pragma once

#include "common/UBPointProfile.h"

#include <atomic>
#include <cstdint>

namespace star {

// Process-local, opt-in samples. Lookup samples are per key; commit samples
// cover every written key in one sampled transaction.
class UBReplicaProfile {
public:
        using Clock = UBPointProfile::Clock;
        static constexpr uint64_t sample_period = UBPointProfile::sample_period;

        struct LookupSample {
                uint64_t local_lookup_ns = 0;
                uint64_t peer_lookup_ns = 0;
                uint64_t lock_read_ns = 0; // read: lock + copy; write: two-lock acquisition
                uint64_t local_copy_ns = 0; // write: copy old value under both locks
                bool found = false;
                bool success = false;
        };

        struct CommitSample {
                uint64_t write_keys = 0;
                uint64_t write_bytes = 0;
                uint64_t local_write_ns = 0;
                uint64_t peer_write_ns = 0;
                uint64_t data_complete_ns = 0;
                uint64_t local_version_ns = 0;
                uint64_t peer_version_ns = 0;
                uint64_t version_complete_ns = 0;
                uint64_t peer_version_verify_ns = 0;
                uint64_t unlock_ns = 0;
        };

        struct LookupCounters {
                std::atomic<uint64_t> samples{0}, found{0}, success{0};
                std::atomic<uint64_t> local_lookup_ns{0}, peer_lookup_ns{0};
                std::atomic<uint64_t> lock_read_ns{0}, local_copy_ns{0};
        };

        struct CommitCounters {
                std::atomic<uint64_t> transactions{0}, write_keys{0}, write_bytes{0};
                std::atomic<uint64_t> local_write_ns{0}, peer_write_ns{0};
                std::atomic<uint64_t> data_complete_ns{0};
                std::atomic<uint64_t> local_version_ns{0}, peer_version_ns{0};
                std::atomic<uint64_t> version_complete_ns{0};
                std::atomic<uint64_t> peer_version_verify_ns{0}, unlock_ns{0};
        };

        static bool enabled() { return UBPointProfile::enabled(); }

        static bool sample_lookup()
        {
                if (!enabled()) return false;
                static thread_local uint64_t sequence = 0;
                return (sequence++ % sample_period) == 0;
        }

        static bool sample_commit()
        {
                if (!enabled()) return false;
                static thread_local uint64_t sequence = 0;
                return (sequence++ % sample_period) == 0;
        }

        static uint64_t elapsed_ns(Clock::time_point start)
        {
                return UBPointProfile::elapsed_ns(start);
        }

        static LookupCounters &lookup_counters(bool write)
        {
                static LookupCounters reads, writes;
                return write ? writes : reads;
        }

        static CommitCounters &commit_counters()
        {
                static CommitCounters counters;
                return counters;
        }

        static void record_lookup(bool write, const LookupSample &sample)
        {
                auto &c = lookup_counters(write);
                c.samples.fetch_add(1, std::memory_order_relaxed);
                c.found.fetch_add(sample.found, std::memory_order_relaxed);
                c.success.fetch_add(sample.success, std::memory_order_relaxed);
                c.local_lookup_ns.fetch_add(sample.local_lookup_ns, std::memory_order_relaxed);
                c.peer_lookup_ns.fetch_add(sample.peer_lookup_ns, std::memory_order_relaxed);
                c.lock_read_ns.fetch_add(sample.lock_read_ns, std::memory_order_relaxed);
                c.local_copy_ns.fetch_add(sample.local_copy_ns, std::memory_order_relaxed);
        }

        static void record_commit(const CommitSample &sample)
        {
                auto &c = commit_counters();
                c.transactions.fetch_add(1, std::memory_order_relaxed);
                c.write_keys.fetch_add(sample.write_keys, std::memory_order_relaxed);
                c.write_bytes.fetch_add(sample.write_bytes, std::memory_order_relaxed);
                c.local_write_ns.fetch_add(sample.local_write_ns, std::memory_order_relaxed);
                c.peer_write_ns.fetch_add(sample.peer_write_ns, std::memory_order_relaxed);
                c.data_complete_ns.fetch_add(sample.data_complete_ns, std::memory_order_relaxed);
                c.local_version_ns.fetch_add(sample.local_version_ns, std::memory_order_relaxed);
                c.peer_version_ns.fetch_add(sample.peer_version_ns, std::memory_order_relaxed);
                c.version_complete_ns.fetch_add(sample.version_complete_ns, std::memory_order_relaxed);
                c.peer_version_verify_ns.fetch_add(sample.peer_version_verify_ns, std::memory_order_relaxed);
                c.unlock_ns.fetch_add(sample.unlock_ns, std::memory_order_relaxed);
        }
};

} // namespace star
