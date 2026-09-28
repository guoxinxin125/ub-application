#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace star {

// Opt-in, sampled timing for the UB point lookup path. Each process (host)
// reports its own totals; no counter is stored in shared UB memory.
class UBPointProfile {
public:
        using Clock = std::chrono::steady_clock;

        struct Sample {
                uint64_t lookup_ns = 0;
                uint64_t lock_ns = 0;
                uint64_t copy_ns = 0;
                uint64_t value_bytes = 0;
                bool found = false;
                bool success = false;
        };

        struct Counters {
                std::atomic<uint64_t> samples{0};
                std::atomic<uint64_t> found{0};
                std::atomic<uint64_t> success{0};
                std::atomic<uint64_t> lookup_ns{0};
                std::atomic<uint64_t> lock_ns{0};
                std::atomic<uint64_t> copy_ns{0};
                std::atomic<uint64_t> value_bytes{0};
                std::atomic<uint64_t> write_samples{0};
                std::atomic<uint64_t> write_ns{0};
                std::atomic<uint64_t> write_bytes{0};
        };

        static constexpr uint64_t sample_period = 256;

        static bool enabled()
        {
                static const bool value = [] {
                        const char *env = std::getenv("TIGON_UB_POINT_PROFILE");
                        return env != nullptr && env[0] == '1' && env[1] == '\0';
                }();
                return value;
        }

        static bool should_sample()
        {
                if (!enabled())
                        return false;
                static thread_local uint64_t sequence = 0;
                return (sequence++ % sample_period) == 0;
        }

        static bool should_sample_write()
        {
                if (!enabled())
                        return false;
                static thread_local uint64_t sequence = 0;
                return (sequence++ % sample_period) == 0;
        }

        static uint64_t elapsed_ns(Clock::time_point start)
        {
                return static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - start).count());
        }

        static void record(bool remote, const Sample &sample)
        {
                Counters &c = counters(remote);
                c.samples.fetch_add(1, std::memory_order_relaxed);
                c.found.fetch_add(sample.found, std::memory_order_relaxed);
                c.success.fetch_add(sample.success, std::memory_order_relaxed);
                c.lookup_ns.fetch_add(sample.lookup_ns, std::memory_order_relaxed);
                c.lock_ns.fetch_add(sample.lock_ns, std::memory_order_relaxed);
                c.copy_ns.fetch_add(sample.copy_ns, std::memory_order_relaxed);
                c.value_bytes.fetch_add(sample.value_bytes, std::memory_order_relaxed);
        }

        static void record_write(bool remote, uint64_t ns, uint64_t bytes)
        {
                Counters &c = counters(remote);
                c.write_samples.fetch_add(1, std::memory_order_relaxed);
                c.write_ns.fetch_add(ns, std::memory_order_relaxed);
                c.write_bytes.fetch_add(bytes, std::memory_order_relaxed);
        }

        static Counters &counters(bool remote)
        {
                static Counters local;
                static Counters peer;
                return remote ? peer : local;
        }
};

} // namespace star
