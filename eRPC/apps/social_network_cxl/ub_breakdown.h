#pragma once

#include <cstddef>
#include <cstdint>

#ifdef ERPC_UB
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#endif

// Application-level timings are opt-in and independent of transport profiling.
// A thread reports every 100,000 samples and once more when it exits.
namespace sn_profile {

enum class Stage : size_t {
  kClientImport,
  kClientMetadata,
  kClientFields,
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

  uint64_t p99_upper_ns() const {
    const uint64_t rank = (calls / 100) * 99 + ((calls % 100) * 99 + 99) / 100;
    uint64_t seen = 0;
    for (size_t i = 0; i < latency_buckets.size(); ++i) {
      seen += latency_buckets[i];
      if (seen >= rank) return UINT64_C(1) << i;
    }
    return max_ns;
  }
};

struct ThreadStats {
  std::array<Counter, static_cast<size_t>(Stage::kCount)> counters{};
  uint64_t samples = 0;
  uint64_t report_index = 0;

  ~ThreadStats() { report(); }

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
      std::fprintf(stderr,
                   "SN_UB_PROFILE thread=%zu interval=%llu stage=%s calls=%llu "
                   "avg_ns=%.1f p99_upper_ns=%llu max_ns=%llu\n",
                   thread_id, static_cast<unsigned long long>(interval),
                   stage_name(static_cast<Stage>(i)),
                   static_cast<unsigned long long>(counter.calls),
                   static_cast<double>(counter.total_ns) / counter.calls,
                   static_cast<unsigned long long>(counter.p99_upper_ns()),
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

#else
inline bool enabled() { return false; }
inline uint64_t start() { return 0; }
inline void record(Stage, uint64_t) {}
#endif

}  // namespace sn_profile
