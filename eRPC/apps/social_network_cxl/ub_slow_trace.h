#pragma once

#include <cstdint>

#ifdef ERPC_UB
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <time.h>
#include <functional>
#include <thread>
#include <vector>
#endif

// Diagnostic-only tracing. Keep stderr I/O out of the measured request path.
namespace sn_slow_trace {

struct Stamp {
  uint64_t wall_ns;
  uint64_t cpu_ns;
  Stamp() : wall_ns(0), cpu_ns(0) {}
  Stamp(uint64_t wall, uint64_t cpu) : wall_ns(wall), cpu_ns(cpu) {}
};

#ifdef ERPC_UB

inline bool enabled() {
  static const bool value = [] {
    const char *env = std::getenv("ERPC_SN_SLOW_TRACE");
    const bool active =
        env != nullptr && env[0] == '1' && env[1] == '\0';
    if (active) std::fprintf(stderr, "SN_SLOW_TRACE=1\n");
    return active;
  }();
  return value;
}

inline uint64_t parse_threshold(const char *name, uint64_t fallback) {
  const char *env = std::getenv(name);
  if (env == nullptr || *env == '\0') return fallback;
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(env, &end, 10);
  return end != env && *end == '\0' && parsed != 0
             ? static_cast<uint64_t>(parsed)
             : fallback;
}

inline uint64_t stage_threshold_ns() {
  static const uint64_t value =
      parse_threshold("ERPC_SN_SLOW_STAGE_NS", 5000);
  return value;
}

inline uint64_t e2e_threshold_ns() {
  static const uint64_t value =
      parse_threshold("ERPC_SN_SLOW_E2E_NS", 50000);
  return value;
}

inline uint64_t wall_now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

inline uint64_t thread_cpu_now_ns() {
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return 0;
  return static_cast<uint64_t>(value.tv_sec) * UINT64_C(1000000000) +
         static_cast<uint64_t>(value.tv_nsec);
}

struct Event {
  const char *stage;
  uint32_t req_number;
  uint64_t wall_ns;
  uint64_t cpu_ns;
  bool cpu_valid;
};

struct ThreadEvents {
  std::vector<Event> events;
  uint64_t dropped = 0;

  ~ThreadEvents() {
    if (events.empty() && dropped == 0) return;
    const size_t thread_id =
        std::hash<std::thread::id>()(std::this_thread::get_id());
    std::fprintf(stderr,
                 "SN_SLOW_TRACE thread=%zu stage_threshold_ns=%llu "
                 "e2e_threshold_ns=%llu events=%zu dropped=%llu\n",
                 thread_id,
                 static_cast<unsigned long long>(stage_threshold_ns()),
                 static_cast<unsigned long long>(e2e_threshold_ns()),
                 events.size(), static_cast<unsigned long long>(dropped));
    for (const Event &event : events) {
      const uint64_t offcpu_ns =
          event.cpu_valid && event.wall_ns > event.cpu_ns
              ? event.wall_ns - event.cpu_ns
              : 0;
      std::fprintf(stderr,
                   "SN_SLOW_TRACE thread=%zu req_number=%u stage=%s "
                   "wall_ns=%llu cpu_ns=%llu offcpu_ns=%llu "
                   "cpu_valid=%u\n",
                   thread_id, event.req_number, event.stage,
                   static_cast<unsigned long long>(event.wall_ns),
                   static_cast<unsigned long long>(event.cpu_ns),
                   static_cast<unsigned long long>(offcpu_ns),
                   event.cpu_valid ? 1u : 0u);
    }
  }

  void add(const Event &event) {
    static const size_t kMaxEvents = 8192;
    if (events.size() < kMaxEvents) {
      events.push_back(event);
    } else {
      ++dropped;
    }
  }
};

inline ThreadEvents &thread_events() {
  static thread_local ThreadEvents value;
  return value;
}

inline Stamp start() {
  if (!enabled()) return {};
  const uint64_t cpu_ns = thread_cpu_now_ns();
  return {wall_now_ns(), cpu_ns};
}

inline void record(const char *stage, uint32_t req_number, Stamp started) {
  if (started.wall_ns == 0) return;
  const uint64_t wall_ns = wall_now_ns() - started.wall_ns;
  if (wall_ns < stage_threshold_ns()) return;
  const uint64_t cpu_finished_ns = thread_cpu_now_ns();
  const bool cpu_valid =
      started.cpu_ns != 0 && cpu_finished_ns >= started.cpu_ns;
  thread_events().add({stage, req_number, wall_ns,
                       cpu_valid ? cpu_finished_ns - started.cpu_ns : 0,
                       cpu_valid});
}

// A queue handoff spans two threads, so thread CPU time is not comparable.
inline void record_handoff(const char *stage, uint32_t req_number,
                           uint64_t started_ns) {
  if (!enabled() || started_ns == 0) return;
  const uint64_t wall_ns = wall_now_ns() - started_ns;
  if (wall_ns >= stage_threshold_ns())
    thread_events().add({stage, req_number, wall_ns, 0, false});
}

inline void record_e2e(const char *stage, uint32_t req_number,
                       uint64_t elapsed_us) {
  if (!enabled() || elapsed_us > UINT64_MAX / 1000) return;
  const uint64_t wall_ns = elapsed_us * 1000;
  if (wall_ns >= e2e_threshold_ns())
    thread_events().add({stage, req_number, wall_ns, 0, false});
}

inline uint32_t &current_request_number() {
  static thread_local uint32_t value = UINT32_MAX;
  return value;
}

class RequestScope {
 public:
  explicit RequestScope(uint32_t req_number)
      : previous_(current_request_number()) {
    current_request_number() = req_number;
  }
  ~RequestScope() { current_request_number() = previous_; }

 private:
  uint32_t previous_;
};

#else

inline bool enabled() { return false; }
inline uint64_t wall_now_ns() { return 0; }
inline Stamp start() { return {}; }
inline void record(const char *, uint32_t, Stamp) {}
inline void record_handoff(const char *, uint32_t, uint64_t) {}
inline void record_e2e(const char *, uint32_t, uint64_t) {}
inline uint32_t current_request_number() { return UINT32_MAX; }
class RequestScope {
 public:
  explicit RequestScope(uint32_t) {}
};

#endif

}  // namespace sn_slow_trace
