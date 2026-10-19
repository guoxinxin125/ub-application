#pragma once
#include "domain_measure.h"
#include "domain_diagnostic.h"

// Bounded routing only: no MsgBuffer, allocator, socket or eRPC inside shadow.
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef ERPC_SN_DOMAIN_PROXY
#include <sched.h>
#ifdef ERPC_SN_DOMAIN_UB
#include <lrpc/ub_social_client.h>
#else
#include <lrpc/multi_client.h>
static_assert(RM_CLIENTS == 8 && RM_SERVICES == 4, "update domain topology capacity checks");
#endif
#endif

namespace sn_domain {
constexpr uint32_t kSlots = 15360;
// Version 2 separates campaign-wide request identity from the LB selection key.
constexpr uint32_t kWireAbi = 2;
constexpr uint32_t kUserEndpoint = 102;
struct Origin {
  uint32_t abi, source, lane, receiver_rpc, original_request, nginx_domain;
};
static_assert(sizeof(Origin) == 24, "domain provenance layout changed");

inline bool boolean_env(const char *name) {
  const char *s = std::getenv(name);
  if (s == nullptr || std::strcmp(s, "0") == 0) return false;
  if (std::strcmp(s, "1") == 0) return true;
  throw std::invalid_argument(std::string(name) + " must be 0 or 1");
}
inline bool enabled() {
  static const bool value = boolean_env("ERPC_SN_DOMAIN_PROXY");
  return value;
}
inline uint64_t now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
enum class Stage : size_t {
  kForward, kReverse, kSend, kAck, kReceive, kE2e, kCount
};
inline const char *stage_name(Stage stage) {
  static const char *const names[] = {"forward_domains", "reverse_domains",
      "enqueue", "ack_rtt", "receive_dispatch", "client_e2e"};
  return names[static_cast<size_t>(stage)];
}
struct Counter {
  uint64_t calls = 0, total = 0, maximum = 0;
  std::array<uint64_t, 65> buckets{};
  void add(uint64_t ns) {
    ++calls; total += ns; if (ns > maximum) maximum = ns;
    size_t b = 0; uint64_t upper = 1;
    while (upper < ns && b < 64) { ++b; if (b != 64) upper <<= 1; }
    ++buckets[b];
  }
  uint64_t percentile(unsigned p) const {
    const uint64_t rank = (calls * p + 99) / 100;
    uint64_t n = 0;
    for (size_t i = 0; i < buckets.size(); ++i) {
      n += buckets[i]; if (n >= rank) return i == 64 ? UINT64_MAX : UINT64_C(1) << i;
    }
    return maximum;
  }
};
struct Metrics {
  bool on = false;
  const char *role = "unknown";
  uint32_t lane = 0;
  uint32_t source = 0;
  uint64_t samples = 0;
  std::array<Counter, static_cast<size_t>(Stage::kCount)> counters{};
  void configure(const char *r, uint32_t l, uint32_t s = 0) {
    role = r; lane = l; source = s; on = boolean_env("ERPC_SN_DOMAIN_PROFILE");
    sn_diag::configure(r, l, s);
  }
  uint64_t start() const { return on ? now_ns() : 0; }
  void finish(Stage stage, uint64_t begin) {
    if (begin) add(stage, now_ns() - begin);
  }
  void add(Stage stage, uint64_t ns) {
    if (!on) return;
    counters[static_cast<size_t>(stage)].add(ns);
    if (++samples == 100000) report();
  }
  void report() {
    if (!samples) return;
    for (size_t i = 0; i < counters.size(); ++i) {
      const Counter &c = counters[i]; if (!c.calls) continue;
      std::fprintf(stderr, "SN_DOMAIN_PROFILE mode=%s role=%s lane=%u source=%u stage=%s calls=%llu avg_ns=%.1f p50_upper_ns=%llu p99_upper_ns=%llu max_ns=%llu\n",
          enabled() ? "domain" : "rpc", role, lane, source, stage_name(static_cast<Stage>(i)),
          static_cast<unsigned long long>(c.calls), static_cast<double>(c.total) / static_cast<double>(c.calls),
          static_cast<unsigned long long>(c.percentile(50)), static_cast<unsigned long long>(c.percentile(99)),
          static_cast<unsigned long long>(c.maximum));
    }
    counters = {}; samples = 0;
  }
  // Application threads explicitly flush before exit. Avoid logging through
  // TLS destructors during runtime/library teardown (including native mocks).
};
inline Metrics &metrics() { static thread_local Metrics m; return m; }
[[noreturn]] inline void fail(const char *role, uint32_t lane, const char *operation,
                             uint32_t request, int error, uint32_t service = 0,
                             uint32_t function = 0, uint32_t vector = 0, uint32_t request_source = 0) {
  std::fprintf(stderr, "SN_DOMAIN_ERROR role=%s lane=%u source=%u request_source=%u op=%s request=%u service=%u function=%u errno=%d reason=%s vector=%u fallback=none\n",
      role, lane, metrics().source, request_source, operation, request, service, function, error,
      std::strerror(error), vector);
  std::fflush(stderr);
  // Fail-stop: no retry/fallback after a possibly completed domain operation.
  std::exit(EXIT_FAILURE);
}
inline uint32_t request_stride(uint32_t lanes, uint32_t sources) {
  // A power-of-two divisor of kSlots keeps each source/lane's ring disjoint,
  // including after modulo wrap. Reserve one Timeline executor per lane too.
  if (!lanes || !sources || lanes > 4 || sources > 7 || (sources + 1) * lanes > 8)
    throw std::invalid_argument("domain connection budget exceeded (maximum 8)");
  uint32_t stride = 1;
  while (stride < lanes * sources) stride <<= 1;
  return stride;
}
inline uint32_t route_key(uint32_t request) {
  if (!request) throw std::invalid_argument("zero application request");
  return request - 1;  // Per-client/lane round robin; independent of provenance.
}
inline uint32_t selected_nginx(uint32_t key, uint32_t proxies) {
  if (!proxies || proxies > 3) throw std::invalid_argument("nginx count must be 1..3");
  return 2 + (key % kSlots) % proxies;
}
inline uint32_t wire_request(uint32_t request, uint32_t lane, uint32_t lanes,
                             uint32_t source_index = 0, uint32_t sources = 1) {
  const uint32_t stride = request_stride(lanes, sources);
  const uint64_t wire = request ? (static_cast<uint64_t>(request) - 1) * stride + source_index * lanes + lane + 1 : 0;
  if (!request || lane >= lanes || source_index >= sources || wire > UINT32_MAX)
    throw std::invalid_argument("invalid/overflowed domain request identity");
  return static_cast<uint32_t>(wire);
}
inline bool valid_origin(const Origin &o, uint32_t wire, uint32_t lanes, uint32_t receivers,
                         uint32_t receiver_base, uint32_t proxies, uint32_t source = 1,
                         uint32_t source_index = 0, uint32_t sources = 1) {
  const uint32_t stride = request_stride(lanes, sources);
  return o.abi == kWireAbi && o.source == source && source && receivers && o.lane < lanes &&
      o.receiver_rpc == receiver_base + o.lane % receivers && o.receiver_rpc < 64 &&
      source_index < sources && o.original_request &&
      static_cast<uint64_t>(o.original_request - 1) * stride + source_index * lanes + o.lane + 1 == wire &&
      proxies && proxies <= 3 && o.nginx_domain == selected_nginx(route_key(o.original_request), proxies);
}
struct Pending {
  std::atomic<uint32_t> wire{0};
  Origin origin{};
  void store(uint32_t request, const Origin &o) {
    if (!request || wire.load(std::memory_order_acquire))
      fail("user_timeline", o.lane, "pending-slot-busy", request, EBUSY);
    // Exactly one receiving thread publishes to this table. ACK clear releases
    // the slot; the next acquire precedes overwriting its provenance.
    origin = o; wire.store(request, std::memory_order_release);
  }
  Origin get(uint32_t request) const {
    if (!request || wire.load(std::memory_order_acquire) != request)
      fail("user_timeline", 0, "pending-slot-identity", request, EPROTO);
    return origin;
  }
  void clear(uint32_t request) {
    uint32_t expected = request;
    if (!request || !wire.compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
      fail("user_timeline", 0, "pending-completion-identity", request, EPROTO);
  }
};

class Runtime {
 public:
  Runtime() = default;
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  ~Runtime() {
    close();
  }
  void close() {
#ifdef ERPC_SN_DOMAIN_UB
    if (opened_) {
      std::fprintf(stderr, "SN_DOMAIN_DONE role=%s lane=%u source=%u calls=%llu backend=ub\n",
                   role_, lane_, source_, static_cast<unsigned long long>(calls_));
      if (us_client_close(&ub_client_)) fail(role_, lane_, "ub-close", 0, errno);
      opened_ = false;
    }
#elif defined(ERPC_SN_DOMAIN_PROXY)
    if (opened_) {
      for (size_t i = 0; i < slow_profiles_used_; ++i) report_profile(slow_profiles_[i]);
      if (sn_diag::enabled())
        std::fprintf(stderr, "SN_DIAG_KERNEL_SUMMARY role=%s lane=%u captured=%zu dropped=%zu threshold_ns=%llu all_calls_profiled=1 calls=%llu\n",
            role_, lane_, slow_profiles_used_, slow_profiles_dropped_, static_cast<unsigned long long>(slow_threshold_),
            static_cast<unsigned long long>(calls_));
      std::fprintf(stderr, "SN_DOMAIN_DONE role=%s lane=%u source=%u calls=%llu\n", role_, lane_, source_, static_cast<unsigned long long>(calls_));
      if (metrics().on)
        for (uint32_t i = 0; i < proxies_; ++i)
          std::fprintf(stderr, "SN_DOMAIN_ROUTE role=%s lane=%u source=%u domain=%u calls=%llu\n",
              role_, lane_, source_, i + 2, static_cast<unsigned long long>(routes_[i]));
      rm_client_close(&client_);
    }
    opened_ = false;
#endif
  }
  void open(const char *role, uint32_t lane, uint32_t proxies, uint32_t source = 0) {
    role_ = role; lane_ = lane; proxies_ = proxies; source_ = source;
    if (!enabled()) return;
#ifdef ERPC_SN_DOMAIN_PROXY
    if (opened_) fail(role, lane, "already-prepared", 0, EALREADY);
    if (!proxies || proxies > 3) fail(role, lane, "prepare-list", 0, EINVAL);
    // Existing leaders pin new threads asynchronously. Wait for that binding,
    // rather than preparing on a transient CPU and later migrating the owner.
    const uint64_t deadline = now_ns() + UINT64_C(10000000000);
    for (;;) {
      cpu_set_t cpus;
      if (sched_getaffinity(0, sizeof(cpus), &cpus)) fail(role, lane, "affinity", 0, errno);
      if (CPU_COUNT(&cpus) == 1) break;
      if (now_ns() >= deadline) fail(role, lane, "await-pinned-thread", 0, EXDEV);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#ifdef ERPC_SN_DOMAIN_UB
    if (proxies != 1 || lane != 0) fail(role, lane, "ub-first-version-single-lane-proxy", 0, ENOTSUP);
    const char *sdk = std::getenv("LRPC_UBSM_SDK_MODE");
    const char *sampling = std::getenv("ERPC_SN_DOMAIN_KERNEL_EVERY");
    if (!sdk || std::strcmp(sdk, "external")) fail(role, lane, "ub-sdk-mode-must-be-external", 0, EINVAL);
    if (sn_diag::enabled() || (sampling && std::strcmp(sampling, "0")))
      fail(role, lane, "ub-x86-kernel-profile-unsupported", 0, ENOTSUP);
    ub_client_.fd = -1;
    if (us_client_open(&ub_client_)) fail(role, lane, "ub-prepare", 0, errno);
    opened_ = true;
    struct ud_connection_info info{};
    if (us_client_info(&ub_client_, &info) || !info.multi || !info.identity || info.slot >= UD_CLIENTS)
      fail(role, lane, "ub-prepare-info", 0, EPROTO);
    std::fprintf(stderr, "SN_DOMAIN_READY role=%s lane=%u source=%u wire_abi=%u slot=%u identity=%llu services=2 grants=0 call_path=mmap backend=ub config=B code=A astack=A chain=legacy entries=2 transitions=4\n",
                 role, lane, source, kWireAbi, info.slot, static_cast<unsigned long long>(info.identity));
#else
    struct rm_prepare plan{}; plan.abi = RM_ABI; plan.count = proxies + 1;
    for (uint32_t i = 0; i < plan.count; ++i) plan.service[i] = i + 1;
    client_.fd = -1;
    if (rm_client_open(&client_, &plan)) fail(role, lane, "prepare", 0, errno);
    opened_ = true;
    if (sn_diag::enabled()) {
      slow_profiles_.resize(64);
      for (SlowProfile &r : slow_profiles_) { volatile uint64_t *wall = &r.wall; *wall = 0; }
      slow_threshold_ = sn_measure::number("ERPC_SN_DOMAIN_SLOW_NS", 50000, UINT64_C(1000000000000));
    }
    if (client_.info.abi != RM_ABI || !client_.info.prepared || client_.info.services != plan.count ||
        client_.info.slot >= RM_CLIENTS || !client_.info.identity)
      fail(role, lane, "prepare-info", 0, EPROTO);
    for (uint32_t i = 0; i < plan.count; ++i) {
      struct rm_config_report r{}; r.abi = RM_ABI; r.service = i + 1;
      if (rm_client_config_report(&client_, &r)) fail(role, lane, "config-audit", 0, errno, i + 1);
      const struct rm_config expected = config(i, proxies);
      if (std::memcmp(&r.config, &expected, sizeof(expected))) fail(role, lane, "config-mismatch", 0, EPROTO, i + 1);
    }
    const char *sample = std::getenv("ERPC_SN_DOMAIN_KERNEL_EVERY");
    if (sample) {
      char *end = nullptr; errno = 0;
      const unsigned long long n = std::strtoull(sample, &end, 10);
      if (errno || sample == end || *end || sample[0] == '-' || n > 1000000000ULL)
        fail(role, lane, "kernel-sampling", 0, EINVAL);
      kernel_every_ = n;
    }
    std::fprintf(stderr, "SN_DOMAIN_READY role=%s lane=%u source=%u wire_abi=%u slot=%u identity=%llu services=%u grants=0 call_path=%s kernel_every=%llu\n",
        role, lane, source, kWireAbi, client_.info.slot, static_cast<unsigned long long>(client_.info.identity), plan.count,
        client_.mapped ? "mmap" : "copy", static_cast<unsigned long long>(kernel_every_));
#endif
#else
    fail(role, lane, "build-option-disabled", 0, ENOTSUP);
#endif
  }
  uint32_t forward(uint32_t wire, uint32_t key, uint32_t source) {
    return call(false, wire, source, 1, 0, 0, 0, 0, 0, key);
  }
  void reverse(uint32_t wire, const Origin &o, uint64_t machine, uint64_t block,
               uint64_t offset, uint64_t bytes) {
    if (call(true, wire, o.source, o.nginx_domain, o.receiver_rpc, machine, block, offset, bytes, route_key(o.original_request)) != o.receiver_rpc)
      fail(role_, lane_, "return-target", wire, EPROTO);
  }
 private:
#if defined(ERPC_SN_DOMAIN_PROXY) && !defined(ERPC_SN_DOMAIN_UB)
  struct SlowProfile {
    struct rm_profile profile{};
    struct rm_call call{};
    uint32_t source = 0, wire = 0;
    bool reverse = false;
    uint64_t wall = 0;
  };
  void report_profile(const SlowProfile &r) const {
    uint64_t sum = 0;
    for (unsigned i = 0; i < RM_PROFILE_STAGES; ++i) sum += r.profile.ns[i];
    std::fprintf(stderr, "SN_DIAG_CALL role=%s lane=%u source=%u wire=%u direction=%s wall_ns=%llu kernel_ns=%llu outside_kernel_ns=%llu outside_valid=%u partition_valid=%u logging=after-calls\n",
        role_, lane_, r.source, r.wire, r.reverse ? "reverse" : "forward",
        static_cast<unsigned long long>(r.wall), static_cast<unsigned long long>(r.profile.total_ns),
        static_cast<unsigned long long>(sn_diag::delta(r.profile.total_ns, r.wall)),
        r.wall >= r.profile.total_ns ? 1U : 0U, sum == r.profile.total_ns ? 1U : 0U);
    static const char *const names[] = {"ioctl_prepare", "exit_wait", "caller_save", "shadow_prepare",
        "shadow_cr3", "enter_to_trap", "trap_process", "private_readback", "finish_prepare",
        "caller_cr3", "caller_restore", "result_publish", "completion"};
    for (unsigned i = 0; i < RM_PROFILE_STAGES; ++i)
      std::fprintf(stderr, "SN_DOMAIN_KERNEL role=%s lane=%u source=%u request_source=%u request=%u direction=%s stage=%s ns=%llu diagnostic=1 deferred=1\n",
          role_, lane_, source_, r.source, r.wire, r.reverse ? "reverse" : "forward", names[i],
          static_cast<unsigned long long>(r.profile.ns[i]));
    for (unsigned entry = 0; entry < r.profile.entries; ++entry)
      for (unsigned stage = 0; stage < RM_PROFILE_ENTRY_STAGES; ++stage)
        std::fprintf(stderr, "SN_DOMAIN_ENTRY role=%s lane=%u source=%u request_source=%u request=%u direction=%s entry=%u domain=%u stage=%s ns=%llu diagnostic=1 deferred=1\n",
            role_, lane_, source_, r.source, r.wire, r.reverse ? "reverse" : "forward", entry,
            r.call.domain[entry], names[RM_P_SHADOW_PREPARE+stage],
            static_cast<unsigned long long>(r.profile.entry_ns[entry][stage]));
  }
  static struct rm_config config(uint32_t index, uint32_t proxies) {
    struct rm_config c{}; c.abi = RM_ABI; c.kind = index ? RM_NGINX : RM_LB;
    c.slot_modulus = kSlots; c.target_count = proxies;
    for (uint32_t i = 0; i < proxies; ++i) c.nginx[i] = i + 2;
    c.compose_endpoint = 101; c.user_endpoint = kUserEndpoint; c.home_endpoint = 103;
    c.return_domain = RM_LB; return c;
  }
#endif
  uint32_t call(bool reverse, uint32_t wire, uint32_t source, uint32_t service,
                uint32_t receiver, uint64_t machine, uint64_t block, uint64_t offset, uint64_t bytes, uint32_t key) {
#ifdef ERPC_SN_DOMAIN_PROXY
    if (!opened_) fail(role_, lane_, "not-prepared", wire, ENOTCONN);
#ifdef ERPC_SN_DOMAIN_UB
    if (service != (reverse ? 2U : 1U)) fail(role_, lane_, "ub-service", wire, EINVAL);
    struct us_call c{}; c.abi = US_ABI; c.direction = reverse ? US_REVERSE : US_FORWARD;
    c.message.source = source; c.message.request = reverse ? wire : key;
    c.message.value = wire; c.message.type = reverse ? US_USER_RESP : US_USER_REQ;
    c.message.machine = machine; c.message.block = block;
    c.message.payload_offset = offset; c.message.payload_bytes = bytes;
    const struct us_message input = c.message;
    ++calls_;
    const uint64_t begin = calls_ > sn_measure::warmup() ? metrics().start() : 0;
    const int ret = us_client_call(&ub_client_, &c), saved = errno;
    metrics().finish(reverse ? Stage::kReverse : Stage::kForward, begin);
    if (ret) fail(role_, lane_, "ub-domain-call", wire, saved, service, 0, c.fault_vector, source);
    struct us_message result = c.message;
    result.action = input.action; result.target = input.target;
    if (std::memcmp(&input, &result, sizeof(input)) || c.entries != 2 || c.transitions != 4 ||
        c.domain[0] != service || c.domain[1] != (reverse ? 1U : 2U) ||
        c.message.action != (reverse ? US_ACTION_CLIENT : US_ACTION_RPC) ||
        c.message.target != (reverse ? 0U : kUserEndpoint))
      fail(role_, lane_, "ub-result-contract", wire, EPROTO, service);
    if (!reverse && metrics().on) ++routes_[0];
    return reverse ? receiver : 2;
#else
    struct rm_call c{}; c.abi = RM_ABI; c.chain = 1; c.service = service;
    c.function = reverse ? RM_NGINX_RETURN : RM_LB_FORWARD;
    // Existing approved LB code reads message.request for selection. Carry the
    // independent provenance in value, without altering the kernel/image ABI.
    c.message.source = source; c.message.request = reverse ? wire : key;
    c.message.value = wire;
    c.message.type = reverse ? RM_USER_RESP : RM_USER_REQ;
    c.message.machine = machine; c.message.block = block;
    c.message.payload_offset = offset; c.message.payload_bytes = bytes;
    const struct rm_message input = c.message;
    ++calls_;
    const bool collect = calls_ > sn_measure::warmup();
    const bool detailed = sn_diag::enabled();
    const bool sampled = detailed || (collect && kernel_every_ && ((calls_ - sn_measure::warmup()) % kernel_every_ == 0));
    const uint64_t begin = collect ? metrics().start() : 0;
    sn_diag::Event *diagnostic_call = sn_diag::begin(reverse ? "reverse_call" : "forward_call", wire, source);
    const int ret = sampled ? rm_client_profile_call(&client_, &c) : rm_client_call(&client_, &c);
    const int saved = errno;
    const uint64_t diagnostic_elapsed = sn_diag::finish(diagnostic_call);
    metrics().finish(reverse ? Stage::kReverse : Stage::kForward, begin);
    if (ret) fail(role_, lane_, "domain-call", wire, saved, service, c.function, c.fault_vector, source);
    if (c.entries != 2 || c.domain[0] != service ||
        c.domain[1] != (reverse ? RM_LB : selected_nginx(key, proxies_)) ||
        c.message.status || c.fault_vector || c.message.source != input.source ||
        c.message.request != input.request || c.message.value != input.value || c.message.type != input.type ||
        c.message.machine != machine || c.message.block != block ||
        c.message.payload_offset != offset || c.message.payload_bytes != bytes ||
        c.message.action != (reverse ? RM_ACTION_CLIENT : RM_ACTION_RPC) ||
        c.message.target != (reverse ? 0U : kUserEndpoint))
      fail(role_, lane_, "result-contract", wire, EPROTO, service, c.function, c.fault_vector, source);
    if (!reverse && metrics().on) ++routes_[c.domain[1] - 2];
    if (sampled) {
      struct rm_profile p{};
      if (rm_client_profile_report(&client_, &p) || p.abi != RM_PROFILE_ABI || p.entries != 2)
        fail(role_, lane_, "kernel-profile", wire, EPROTO);
      if (detailed) {
        const uint64_t elapsed = diagnostic_elapsed;
        if (elapsed >= slow_threshold_) {
          if (slow_profiles_used_ == slow_profiles_.size()) ++slow_profiles_dropped_;
          else {
            SlowProfile &r = slow_profiles_[slow_profiles_used_++];
            r.profile = p; r.call = c; r.source = source; r.wire = wire; r.reverse = reverse; r.wall = elapsed;
          }
        }
      } else {
      static const char *const names[] = {"ioctl_prepare", "exit_wait", "caller_save", "shadow_prepare",
          "shadow_cr3", "enter_to_trap", "trap_process", "private_readback", "finish_prepare",
          "caller_cr3", "caller_restore", "result_publish", "completion"};
      for (unsigned i = 0; i < RM_PROFILE_STAGES; ++i)
        std::fprintf(stderr, "SN_DOMAIN_KERNEL role=%s lane=%u source=%u request_source=%u request=%u direction=%s stage=%s ns=%llu diagnostic=1\n",
            role_, lane_, source_, source, wire, reverse ? "reverse" : "forward", names[i], static_cast<unsigned long long>(p.ns[i]));
      for (unsigned entry = 0; entry < p.entries; ++entry)
        for (unsigned stage = 0; stage < RM_PROFILE_ENTRY_STAGES; ++stage)
          std::fprintf(stderr, "SN_DOMAIN_ENTRY role=%s lane=%u source=%u request_source=%u request=%u direction=%s entry=%u domain=%u stage=%s ns=%llu diagnostic=1\n",
              role_, lane_, source_, source, wire, reverse ? "reverse" : "forward", entry, c.domain[entry],
              names[RM_P_SHADOW_PREPARE + stage], static_cast<unsigned long long>(p.entry_ns[entry][stage]));
      }
    }
    return reverse ? receiver : c.domain[1];
#endif
#else
    (void)reverse; (void)source; (void)service; (void)receiver; (void)key;
    (void)machine; (void)block; (void)offset; (void)bytes;
    fail(role_, lane_, "build-option-disabled", wire, ENOTSUP);
#endif
  }
  const char *role_ = "unknown";
  uint32_t source_ = 0;
  std::array<uint64_t, 3> routes_{};
  uint32_t lane_ = 0, proxies_ = 0;
#ifdef ERPC_SN_DOMAIN_UB
  struct lrpc_handle ub_client_{};
  bool opened_ = false;
  uint64_t calls_ = 0;
#elif defined(ERPC_SN_DOMAIN_PROXY)
  struct rm_client client_{};
  bool opened_ = false;
  uint64_t kernel_every_ = 0, calls_ = 0;
  std::vector<SlowProfile> slow_profiles_;
  size_t slow_profiles_used_ = 0, slow_profiles_dropped_ = 0;
  uint64_t slow_threshold_ = 50000;
#endif
};
}  // namespace sn_domain
