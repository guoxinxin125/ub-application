#pragma once

#include <cstddef>
#include <cstdint>

#include "ub_slow_trace.h"

#ifdef ERPC_UB
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>
#endif

// Application-level timings are opt-in and independent of transport profiling.
// A thread reports every 100,000 samples and once more when it exits.
namespace sn_profile {

enum class Stage : size_t {
  kClientImport,
  kClientMetadata,
  kClientFields,
  kClientFieldsContiguous,
  kClientFieldsScalars,
  kClientFieldsUsername,
  kClientFieldsText,
  kClientFieldsMedia,
  kClientFieldsMentions,
  kClientFieldsUrls,
  kClientRelease,
  kProxyForwardRxPin,
  kProxyForwardRxQueue,
  kProxyForwardQueueHandoff,
  kProxyForwardTxPrepare,
  kProxyForwardTxEnqueue,
  kProxyForwardCallbackRelease,
  kProxyReverseRxPin,
  kProxyReverseRxQueue,
  kProxyReverseQueueHandoff,
  kProxyReverseTxPrepare,
  kProxyReverseTxEnqueue,
  kProxyReverseCallbackRelease,
  kTimelineRxPin,
  kTimelineRxCopy,
  kTimelineRxQueue,
  kTimelineRxAck,
  kTimelineQueueHandoff,
  kTimelineWorkerLookupQueue,
  kTimelineWorkerRelease,
  kTimelineForwardQueueHandoff,
  kTimelineTxPrepare,
  kTimelineTxEnqueue,
  kTimelineStorageRtt,
  kTimelineCallbackBuild,
  kTimelineCallbackRelease,
  kTimelineCallbackForward,
  kStorageReadLock,
  kStorageReadLookup,
  kStorageReadRetain,
  kStorageReadResponse,
  kStorageReadResponseBuild,
  kStorageReadResponseEnqueue,
  kStorageWriteAlloc,
  kStorageWriteCopy,
  kStorageWriteValidate,
  kStorageWriteMap,
  kStorageWriteLock,
  kStorageWriteLookup,
  kStorageWriteInsert,
  kStorageWriteOldRelease,
  kStorageWriteResponse,
  kStorageWriteResponseBuild,
  kStorageWriteResponseEnqueue,
  kCount
};

#ifdef ERPC_UB
inline bool enabled() {
  static const bool value = [] {
    const char *env = std::getenv("ERPC_SN_PROFILE");
    return env != nullptr && env[0] == '1' && env[1] == '\0';
  }();
  return value;
}

// A diagnostic-only, per-request timeline. Both switches must be explicit so
// ordinary campaign runs retain only the existing coarse measurement cost.
inline bool flow_enabled() {
  static const bool value = [] {
    const char *env = std::getenv("ERPC_SN_DOMAIN_PROFILE");
    return enabled() && env != nullptr && env[0] == '1' && env[1] == '\0';
  }();
  return value;
}

inline uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

inline uint64_t start() { return enabled() ? now_ns() : 0; }

inline uint64_t timestamp_overhead_ns() {
  uint64_t minimum = UINT64_MAX;
  for (size_t i = 0; i < 256; ++i) {
    const uint64_t before = now_ns();
    const uint64_t elapsed = now_ns() - before;
    if (elapsed < minimum) minimum = elapsed;
  }
  return minimum;
}

inline const char *stage_name(Stage stage) {
  static const char *const names[] = {"client_import",
                                      "client_metadata",
                                      "client_fields",
                                      "client_fields_contiguous",
                                      "client_fields_scalars",
                                      "client_fields_username",
                                      "client_fields_text",
                                      "client_fields_media",
                                      "client_fields_mentions",
                                      "client_fields_urls",
                                      "client_release",
                                      "proxy_forward_rx_pin",
                                      "proxy_forward_rx_queue",
                                      "proxy_forward_queue_handoff",
                                      "proxy_forward_tx_prepare",
                                      "proxy_forward_tx_enqueue",
                                      "proxy_forward_callback_release",
                                      "proxy_reverse_rx_pin",
                                      "proxy_reverse_rx_queue",
                                      "proxy_reverse_queue_handoff",
                                      "proxy_reverse_tx_prepare",
                                      "proxy_reverse_tx_enqueue",
                                      "proxy_reverse_callback_release",
                                      "timeline_rx_pin",
                                      "timeline_rx_copy",
                                      "timeline_rx_queue",
                                      "timeline_rx_ack",
                                      "timeline_queue_handoff",
                                      "timeline_worker_lookup_queue",
                                      "timeline_worker_release",
                                      "timeline_forward_queue_handoff",
                                      "timeline_tx_prepare",
                                      "timeline_tx_enqueue",
                                      "timeline_storage_rtt",
                                      "timeline_callback_build",
                                      "timeline_callback_release",
                                      "timeline_callback_forward",
                                      "storage_read_lock",
                                      "storage_read_lookup",
                                      "storage_read_retain",
                                      "storage_read_response",
                                      "storage_read_response_build",
                                      "storage_read_response_enqueue",
                                      "storage_write_alloc",
                                      "storage_write_copy",
                                      "storage_write_validate",
                                      "storage_write_map",
                                      "storage_write_lock",
                                      "storage_write_lookup",
                                      "storage_write_insert",
                                      "storage_write_old_release",
                                      "storage_write_response",
                                      "storage_write_response_build",
                                      "storage_write_response_enqueue"};
  static_assert(
      sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Stage::kCount),
      "profile stage names are out of sync");
  return names[static_cast<size_t>(stage)];
}

struct Counter {
  uint64_t calls = 0;
  uint64_t total_ns = 0;
  uint64_t max_ns = 0;
  std::array<uint64_t, 64> latency_buckets{};

  uint64_t percentile_upper_ns(uint64_t numerator,
                               uint64_t denominator = 100) const {
    if (calls == 0) return 0;
    const uint64_t rank = (calls * numerator + denominator - 1) / denominator;
    uint64_t seen = 0;
    for (size_t i = 0; i < latency_buckets.size(); ++i) {
      seen += latency_buckets[i];
      if (seen >= rank) return UINT64_C(1) << i;
    }
    return max_ns;
  }
};

struct ThreadStats {
  struct FlowEvent {
    const char *role;
    const char *stage;
    uint64_t at_ns;
    uint32_t request;
    uint32_t original_request;
  };
  static constexpr size_t kFlowCapacity = 32768;
  std::array<Counter, static_cast<size_t>(Stage::kCount)> counters{};
  uint64_t samples = 0;
  uint64_t report_index = 0;
  std::vector<FlowEvent> flow_events;
  uint64_t flow_dropped = 0;

  ~ThreadStats() { report(); report_flow(); }

  void record_flow(const char *role, uint32_t request, const char *stage,
                   uint64_t at_ns, uint32_t original_request) {
    if (flow_events.size() == kFlowCapacity) {
      ++flow_dropped;
      return;
    }
    if (flow_events.empty()) flow_events.reserve(kFlowCapacity);
    flow_events.push_back({role, stage, at_ns, request, original_request});
  }

  void report_flow() {
    if (flow_events.empty() && !flow_dropped) return;
    const size_t thread = std::hash<std::thread::id>()(std::this_thread::get_id());
    for (const FlowEvent &event : flow_events)
      std::fprintf(stderr,
                   "SN_FLOW_EVENT role=%s thread=%zu request=%u stage=%s at_ns=%llu original=%u clock=steady_ns\n",
                   event.role, thread, event.request, event.stage,
                   static_cast<unsigned long long>(event.at_ns), event.original_request);
    std::fprintf(stderr,
                 "SN_FLOW_CONTRACT thread=%zu events=%zu dropped=%llu clock=steady_ns scope=same-host-only logging=thread-exit\n",
                 thread, flow_events.size(),
                 static_cast<unsigned long long>(flow_dropped));
  }

  void report() {
    if (samples == 0) return;
    const uint64_t interval = ++report_index;
    const size_t thread_id =
        std::hash<std::thread::id>()(std::this_thread::get_id());
    std::fprintf(
        stderr,
        "SN_UB_PROFILE thread=%zu interval=%llu timestamp_overhead_ns=%llu\n",
        thread_id, static_cast<unsigned long long>(interval),
        static_cast<unsigned long long>(timestamp_overhead_ns()));
    for (size_t i = 0; i < counters.size(); ++i) {
      const Counter &counter = counters[i];
      if (counter.calls == 0) continue;
      std::fprintf(
          stderr,
          "SN_UB_PROFILE thread=%zu interval=%llu stage=%s calls=%llu "
          "avg_ns=%.1f p50_upper_ns=%llu p90_upper_ns=%llu "
          "p95_upper_ns=%llu p99_upper_ns=%llu max_ns=%llu\n",
          thread_id, static_cast<unsigned long long>(interval),
          stage_name(static_cast<Stage>(i)),
          static_cast<unsigned long long>(counter.calls),
          static_cast<double>(counter.total_ns) / counter.calls,
          static_cast<unsigned long long>(counter.percentile_upper_ns(50)),
          static_cast<unsigned long long>(counter.percentile_upper_ns(90)),
          static_cast<unsigned long long>(counter.percentile_upper_ns(95)),
          static_cast<unsigned long long>(counter.percentile_upper_ns(99)),
          static_cast<unsigned long long>(counter.max_ns));
    }
    counters = {};
    samples = 0;
  }

  void record(Stage stage, uint64_t elapsed_ns) {
    Counter &counter = counters[static_cast<size_t>(stage)];
    ++counter.calls;
    counter.total_ns += elapsed_ns;
    if (elapsed_ns > counter.max_ns) counter.max_ns = elapsed_ns;
    const size_t bucket =
        elapsed_ns <= 1
            ? 0
            : static_cast<size_t>(64 - __builtin_clzll(elapsed_ns - 1));
    ++counter.latency_buckets[bucket];
    if (++samples == 100000) report();
  }
};

inline ThreadStats &thread_stats() {
  static thread_local ThreadStats value;
  return value;
}

inline void record(Stage stage, uint64_t started_ns) {
  if (started_ns == 0) return;
  const uint64_t finished_ns = now_ns();
  thread_stats().record(stage, finished_ns - started_ns);
}

inline void flow_point_at(const char *role, uint32_t request,
                          const char *stage, uint64_t at_ns,
                          uint32_t original_request = 0) {
  if (!flow_enabled() || !request || !at_ns) return;
  thread_stats().record_flow(role, request, stage, at_ns, original_request);
}

inline void flow_point(const char *role, uint32_t request, const char *stage) {
  if (!flow_enabled() || !request) return;
  flow_point_at(role, request, stage, now_ns());
}

#else
inline bool enabled() { return false; }
inline bool flow_enabled() { return false; }
inline uint64_t start() { return 0; }
inline void record(Stage, uint64_t) {}
inline void flow_point_at(const char *, uint32_t, const char *, uint64_t,
                          uint32_t = 0) {}
inline void flow_point(const char *, uint32_t, const char *) {}
#endif

}  // namespace sn_profile

namespace sn_handoff {
inline uint64_t start() {
  const uint64_t profile_start = sn_profile::start();
  return profile_start == 0 && sn_slow_trace::enabled()
             ? sn_slow_trace::wall_now_ns()
             : profile_start;
}

inline void record(sn_profile::Stage stage, const char *label,
                   uint32_t req_number, uint64_t started_ns) {
  sn_slow_trace::record_handoff(label, req_number, started_ns);
  sn_profile::record(stage, started_ns);
}
}  // namespace sn_handoff
