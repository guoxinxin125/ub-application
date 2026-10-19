#pragma once
// Opt-in, bounded UserTimeline measurements. No logging/allocation on completion.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace sn_measure {
inline uint64_t number(const char *name, uint64_t fallback, uint64_t maximum) {
  const char *s = std::getenv(name);
  if (!s) return fallback;
  if (!*s) throw std::invalid_argument(name);
  uint64_t value = 0;
  for (const char *p = s; *p; ++p) {
    if (*p < '0' || *p > '9' || value > maximum / 10)
      throw std::invalid_argument(name);
    value = value * 10 + static_cast<unsigned>(*p - '0');
    if (value > maximum) throw std::invalid_argument(name);
  }
  return value;
}
inline bool enabled() {
  static const bool on = number("ERPC_SN_DOMAIN_MEASURE", 0, 1) != 0;
  return on;
}
inline bool rpc_baseline() {
  static const bool on = number("ERPC_SN_RPC_BASELINE", 0, 1) != 0;
  return on && enabled();
}
inline bool diagnostic() {
  static const bool on = number("ERPC_SN_DOMAIN_DIAGNOSTIC", 0, 1) != 0;
  return on;
}
inline uint32_t warmup() {
  static const uint32_t n = static_cast<uint32_t>(number("ERPC_SN_DOMAIN_WARMUP", 0, 5000));
  return enabled() ? n : 0;
}
inline bool measured(uint32_t request) { return request > warmup(); }
inline bool sampled(uint32_t request) {
  if (diagnostic() && !rpc_baseline()) return true;
  static const uint64_t every = number("ERPC_SN_DOMAIN_KERNEL_EVERY", 0, 1000000000);
  return every && measured(request) && (request - warmup()) % every == 0;
}
struct Record {
  uint32_t request = 0, wire = 0, count = 0;
  uint64_t elapsed = 0, forward = 0, bytes = 0;
};
struct Statistics {
  std::vector<uint64_t> values;
  uint64_t total = 0;
  void add(uint64_t ns) { values.push_back(ns); total += ns; }
  uint64_t percentile(size_t p) const {
    return values.empty() ? 0 : values[(values.size() * p + 99) / 100 - 1];
  }
};
class Campaign {
 public:
  Statistics statistics(bool formal, unsigned kind = 0) const {
    Statistics stats;
    for (size_t i = 1; i < records_.size(); ++i) {
      const Record &r = records_[i];
      if (measured(r.request) != formal || (kind && r.count != kind-1)) continue;
      stats.add(r.elapsed);
    }
    std::sort(stats.values.begin(), stats.values.end());
    return stats;
  }
  void configure(uint64_t total) {
    if (!enabled()) return;
    if (total <= warmup() || total > 10000) throw std::invalid_argument("measurement request count");
    slow_ns_ = number("ERPC_SN_DOMAIN_SLOW_NS", 50000, UINT64_C(1000000000000));
    slow_limit_ = number("ERPC_SN_DOMAIN_SLOW_LIMIT", 32, 1000);
    records_.resize(static_cast<size_t>(total) + 1);
  }
  bool record(uint32_t request, uint32_t wire, uint32_t count, uint64_t elapsed,
              uint64_t forward, uint64_t bytes) {
    if (!enabled()) return true;
    if (!request || request >= records_.size() || count > 1 || records_[request].request) return false;
    Record &r = records_[request];
    r.request = request; r.wire = wire; r.count = count;
    r.elapsed = elapsed; r.forward = forward; r.bytes = bytes;
    return true;
  }
  bool report(uint32_t source, size_t lane) const {
    if (!enabled()) return true;
    for (size_t i = 1; i < records_.size(); ++i) if (records_[i].request != i) return false;
    std::fprintf(stderr, "SN_DOMAIN_MEASUREMENT source=%u lane=%zu abi=2 warmup=%u measured=%zu total=%zu clock=steady_ns slow_ns=%llu slow_limit=%llu logging=after-join legacy_timer=off\n",
        source, lane, warmup(), records_.size()-1-warmup(), records_.size()-1,
        static_cast<unsigned long long>(slow_ns_), static_cast<unsigned long long>(slow_limit_));
    for (unsigned phase = 0; phase < 2; ++phase) {
      const char *phase_name = phase ? "measured" : "warmup";
      for (unsigned kind = 0; kind < 3; ++kind) {
        const Statistics stats = statistics(phase != 0, kind);
        std::fprintf(stderr, "SN_DOMAIN_CLASS source=%u lane=%zu phase=%s class=%s count=%zu min_ns=%llu p50_ns=%llu p99_ns=%llu avg_ns=%.1f max_ns=%llu clock=steady_ns scope=call-to-consume fixture_validation_included=%u\n",
            source, lane, phase_name, kind == 0 ? "all" : (kind == 1 ? "empty" : "nonempty"), stats.values.size(),
            static_cast<unsigned long long>(stats.values.empty() ? 0 : stats.values.front()),
            static_cast<unsigned long long>(stats.percentile(50)), static_cast<unsigned long long>(stats.percentile(99)),
            stats.values.empty() ? 0.0 : static_cast<double>(stats.total)/static_cast<double>(stats.values.size()),
            static_cast<unsigned long long>(stats.values.empty() ? 0 : stats.values.back()),
            number("ERPC_SN_DOMAIN_FIXTURE", 0, 1) ? 1U : 0U);
      }
      std::vector<Record> slow;
      for (size_t i = 1; i < records_.size(); ++i)
        if (measured(records_[i].request) == (phase != 0) && records_[i].elapsed >= slow_ns_) slow.push_back(records_[i]);
      std::sort(slow.begin(), slow.end(), [](const Record &a, const Record &b) {
        return a.elapsed != b.elapsed ? a.elapsed > b.elapsed : a.request < b.request;
      });
      const size_t printed = std::min(slow.size(), static_cast<size_t>(slow_limit_));
      for (size_t i = 0; i < printed; ++i) {
        const Record &r = slow[i];
        std::fprintf(stderr, "SN_DOMAIN_SLOW source=%u lane=%zu phase=%s request=%u wire=%u class=%s count=%u payload_bytes=%llu e2e_ns=%llu forward_ns=%llu kernel_sampled=%u\n",
            source, lane, phase_name, r.request, r.wire, r.count ? "nonempty" : "empty", r.count,
            static_cast<unsigned long long>(r.bytes), static_cast<unsigned long long>(r.elapsed),
            static_cast<unsigned long long>(r.forward), sampled(r.request) ? 1U : 0U);
      }
      std::fprintf(stderr, "SN_DOMAIN_SLOW_SUMMARY source=%u lane=%zu phase=%s matched=%zu printed=%zu dropped=%zu threshold_ns=%llu\n",
          source, lane, phase_name, slow.size(), printed, slow.size()-printed, static_cast<unsigned long long>(slow_ns_));
    }
    return true;
  }
 private:
  // A unique validated response owns each slot; read only after receiver joins.
  std::vector<Record> records_;
  uint64_t slow_ns_ = 50000, slow_limit_ = 32;
};
}  // namespace sn_measure
