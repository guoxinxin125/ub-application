#pragma once
// Same-guest diagnostic clock only. Fixed-size per-thread storage is allocated
// before request processing; recording never prints or grows a container.
#include "domain_measure.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
#endif

namespace sn_diag {
inline bool enabled() { return sn_measure::diagnostic(); }
inline uint64_t wall_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Stamp {
  uint64_t wall = 0, cpu = 0, minor = 0, major = 0, voluntary = 0, involuntary = 0;
  bool cpu_valid = false, usage_valid = false;
};
inline Stamp counters() {
  Stamp s;
#ifdef __linux__
  timespec t{};
  if (!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) {
    s.cpu = static_cast<uint64_t>(t.tv_sec)*UINT64_C(1000000000)+static_cast<uint64_t>(t.tv_nsec);
    s.cpu_valid = true;
  }
  rusage r{};
  if (!getrusage(RUSAGE_THREAD, &r)) {
    s.minor = static_cast<uint64_t>(r.ru_minflt); s.major = static_cast<uint64_t>(r.ru_majflt);
    s.voluntary = static_cast<uint64_t>(r.ru_nvcsw); s.involuntary = static_cast<uint64_t>(r.ru_nivcsw);
    s.usage_valid = true;
  }
#endif
  return s;
}
inline Stamp start() {
  if (!enabled()) return {};
  Stamp s = counters(); s.wall = wall_ns(); return s;
}
inline Stamp stop() {
  if (!enabled()) return {};
  const uint64_t end = wall_ns(); Stamp s = counters(); s.wall = end; return s;
}
struct Event {
  const char *stage = nullptr;
  uint32_t wire = 0, source = 0;
  Stamp begin, end;
};
struct Log {
  const char *role = nullptr;
  uint32_t lane = 0, source = 0;
  uint64_t thread = 0;
  uint64_t pid = 0, tid = 0;
  size_t used = 0, dropped = 0;
  std::vector<Event> events;
};
struct Registry {
  std::mutex mutex;
  std::vector<std::unique_ptr<Log>> logs;
  std::atomic<uint64_t> unregistered{0};
};
inline Registry &registry() { static Registry r; return r; }
inline Log *&local() { static thread_local Log *p = nullptr; return p; }
inline void configure(const char *role, uint32_t lane, uint32_t source = 0) {
  if (!enabled() || local()) return;
  const size_t capacity = static_cast<size_t>(sn_measure::number("ERPC_SN_DIAG_CAPACITY", 32768, 262144));
  if (!capacity) throw std::invalid_argument("diagnostic capacity");
  std::unique_ptr<Log> log(new Log);
  log->role = role; log->lane = lane; log->source = source;
  log->thread = static_cast<uint64_t>(std::hash<std::thread::id>()(std::this_thread::get_id()));
#ifdef __linux__
  log->pid = static_cast<uint64_t>(getpid());
  log->tid = static_cast<uint64_t>(syscall(SYS_gettid));
#endif
  log->events.resize(capacity); // Touch records before measurement, not at first slow call.
  // Volatile stores prevent calloc/lazy-zero optimization from leaving pages
  // untouched until a request reaches a previously unused record.
  for (Event &e : log->events) { volatile uint32_t *wire = &e.wire; *wire = 0; }
  std::lock_guard<std::mutex> lock(registry().mutex);
  if (registry().logs.size() >= 32) throw std::invalid_argument("diagnostic thread capacity");
  local() = log.get(); registry().logs.push_back(std::move(log));
}
inline void record(const char *stage, uint32_t wire, Stamp begin, Stamp end, uint32_t source = 0) {
  if (!enabled()) return;
  Log *log = local();
  if (!log) { registry().unregistered.fetch_add(1, std::memory_order_relaxed); return; }
  if (log->used == log->events.size()) { ++log->dropped; return; }
  Event &e = log->events[log->used++];
  e.stage = stage; e.wire = wire; e.source = source ? source : log->source;
  e.begin = begin; e.end = end;
}
inline uint64_t timestamp() { return enabled() ? wall_ns() : 0; }
inline void point_at(const char *stage, uint32_t wire, uint64_t wall, uint32_t source = 0) {
  if (!enabled()) return;
  Stamp s; s.wall = wall; record(stage, wire, s, s, source);
}
inline void point(const char *stage, uint32_t wire, uint32_t source = 0) {
  if (!enabled()) return;
  point_at(stage, wire, wall_ns(), source);
}
inline uint64_t delta(uint64_t a, uint64_t b) { return b >= a ? b-a : 0; }
inline Event *begin(const char *stage, uint32_t wire, uint32_t source = 0) {
  if (!enabled()) return nullptr;
  Log *log = local();
  if (!log) { registry().unregistered.fetch_add(1, std::memory_order_relaxed); return nullptr; }
  if (log->used == log->events.size()) { ++log->dropped; return nullptr; }
  Event &e = log->events[log->used++];
  e.stage = stage; e.wire = wire; e.source = source ? source : log->source;
  e.begin = start(); return &e;
}
inline uint64_t finish(Event *e) {
  if (!e) return 0;
  e->end = stop(); return delta(e->begin.wall, e->end.wall);
}
// Caller must join every instrumented thread first. No TLS destructor logging.
inline bool report(const char *process_role) {
  if (!enabled()) return true;
  uint64_t events = 0, dropped = registry().unregistered.load();
  for (const auto &p : registry().logs) {
    const Log &log = *p; dropped += log.dropped;
    for (size_t i = 0; i < log.used; ++i) {
      const Event &e = log.events[i];
      const uint64_t wall = delta(e.begin.wall, e.end.wall);
      const bool cpu_valid = e.begin.cpu_valid && e.end.cpu_valid && e.end.cpu >= e.begin.cpu;
      const bool usage_valid = e.begin.usage_valid && e.end.usage_valid;
      const uint64_t cpu = cpu_valid ? delta(e.begin.cpu, e.end.cpu) : 0;
      std::fprintf(stderr, "SN_DIAG_EVENT role=%s thread=%llu pid=%llu tid=%llu lane=%u source=%u wire=%u stage=%s begin_ns=%llu end_ns=%llu wall_ns=%llu cpu_ns=%llu cpu_valid=%u gap_ns=%llu usage_valid=%u minor_faults=%llu major_faults=%llu voluntary_switches=%llu involuntary_switches=%llu\n",
          log.role, static_cast<unsigned long long>(log.thread), static_cast<unsigned long long>(log.pid),
          static_cast<unsigned long long>(log.tid), log.lane, e.source, e.wire, e.stage,
          static_cast<unsigned long long>(e.begin.wall), static_cast<unsigned long long>(e.end.wall),
          static_cast<unsigned long long>(wall), static_cast<unsigned long long>(cpu), cpu_valid ? 1U : 0U,
          static_cast<unsigned long long>(cpu_valid && wall > cpu ? wall-cpu : 0), usage_valid ? 1U : 0U,
          static_cast<unsigned long long>(delta(e.begin.minor, e.end.minor)),
          static_cast<unsigned long long>(delta(e.begin.major, e.end.major)),
          static_cast<unsigned long long>(delta(e.begin.voluntary, e.end.voluntary)),
          static_cast<unsigned long long>(delta(e.begin.involuntary, e.end.involuntary)));
      ++events;
    }
  }
  std::fprintf(stderr, "SN_DIAG_CONTRACT role=%s abi=1 events=%llu dropped=%llu clock=steady_ns scope=same-caller-guest logging=after-join\n",
      process_role, static_cast<unsigned long long>(events), static_cast<unsigned long long>(dropped));
  return dropped == 0;
}
}  // namespace sn_diag
