#pragma once
#include "domain_proxy_wire.h"
#include <vector>

namespace sn_domain {
// Ordered immutable campaign registry; never another process's pointers.
struct Registration {
  uint32_t id, rpc_id;
  uint64_t bind_core_offset;
  std::string address;
};
inline uint32_t config_u32(const json &v) {
  if (!v.is_number_integer() || (!v.is_number_unsigned() && v.get<int64_t>() < 0) ||
      v.get<uint64_t>() > UINT32_MAX)
    throw std::invalid_argument("domain registry requires nonnegative uint32 integers");
  return v.get<uint32_t>();
}
inline std::vector<Registration> registrations(const char *key, const char *legacy, bool client_list) {
  std::vector<Registration> result;
  if (config_json_all.contains("domain_proxy") && config_json_all.at("domain_proxy").contains(key)) {
    const auto &list = config_json_all.at("domain_proxy").at(key);
    if (!list.is_array() || list.empty()) throw std::invalid_argument("empty/non-array domain registry");
    for (const auto &item : list) {
      const uint32_t id = client_list ? config_u32(item.at("id")) : static_cast<uint32_t>(result.size()) + 2;
      const uint32_t base = config_u32(item.at("rpc_id"));
      const uint64_t bind = item.contains("bind_core_offset") ? config_u32(item.at("bind_core_offset")) : 0;
      result.push_back({id, base, bind, item.at("server_addr").get<std::string>()});
    }
  } else {
    const auto &item = config_json_all.at(legacy);
    result.push_back({client_list ? 1U : 2U, config_u32(item.at("rpc_id")),
        item.contains("bind_core_offset") ? config_u32(item.at("bind_core_offset")) : 0,
        item.at("server_addr").get<std::string>()});
  }
  for (size_t i = 0; i < result.size(); ++i) {
    const auto &r = result[i];
    if (!r.id || r.rpc_id >= 64 || r.address.empty() || r.address.find(',') != std::string::npos)
      throw std::invalid_argument("invalid domain registry id/rpc_id/address");
    for (size_t j = 0; j < i; ++j)
      if (result[j].id == r.id || result[j].address == r.address)
        throw std::invalid_argument("duplicate domain registry id/address");
  }
  return result;
}
inline const std::vector<Registration> &clients() {
  static const auto value = registrations("clients", "client", true); return value;
}
inline const std::vector<Registration> &nginx_instances() {
  static const auto value = registrations("nginx_instances", "nginx", false); return value;
}
inline uint32_t proxy_count() {
  const size_t n = nginx_instances().size();
  if (!n || n > 3) throw std::invalid_argument("domain registry supports 1..3 nginx instances");
  return static_cast<uint32_t>(n);
}
inline uint32_t source_count() { return static_cast<uint32_t>(clients().size()); }
inline ReadService read_service() {
  return config_u32(config_json_all.at("client").at("home_num")) == 1 ?
      ReadService::Home : ReadService::User;
}
inline uint32_t source_index() {
  static const uint32_t index = [] {
    const char *env = std::getenv("ERPC_SN_DOMAIN_CLIENT_ID");
    if (!env && clients().size() == 1) return uint32_t(0);
    if (!env || !*env) throw std::invalid_argument("multiple clients require ERPC_SN_DOMAIN_CLIENT_ID");
    char *end = nullptr; errno = 0;
    const unsigned long long id = std::strtoull(env, &end, 10);
    if (errno || env == end || *end || *env == '-' || !id || id > UINT32_MAX)
      throw std::invalid_argument("invalid ERPC_SN_DOMAIN_CLIENT_ID");
    for (size_t i = 0; i < clients().size(); ++i)
      if (clients()[i].id == id) return static_cast<uint32_t>(i);
    throw std::invalid_argument("ERPC_SN_DOMAIN_CLIENT_ID not registered");
  }();
  return index;
}
inline uint32_t source_id() { return clients()[source_index()].id; }
inline uint32_t receiver_base() { return clients()[source_index()].rpc_id; }
inline uint32_t client_wire(uint32_t request, uint32_t lane) {
  return wire_request(request, lane, static_cast<uint32_t>(FLAGS_client_num), source_index(), source_count());
}
inline void pin_worker(const char *role, uint32_t lane) {
  if (!enabled()) return;
#ifdef ERPC_SN_DOMAIN_PROXY
  const auto cpus = erpc::get_lcores_for_numa_node(FLAGS_numa_client_node);
  const size_t index = app_client_bind_core(lane);
  if (index >= cpus.size() || cpus[index] >= CPU_SETSIZE) fail(role, lane, "worker-cpu-range", 0, EINVAL);
  cpu_set_t affinity; CPU_ZERO(&affinity);
  CPU_SET(cpus[index], &affinity);
  if (sched_setaffinity(0, sizeof(affinity), &affinity)) fail(role, lane, "pin-worker", 0, errno);
#else
  fail(role, lane, "build-option-disabled", 0, ENOTSUP);
#endif
}
inline void validate_config(const char *role) {
  (void)boolean_env("ERPC_SN_DOMAIN_PROFILE");
  const bool baseline_multi = sn_measure::rpc_baseline() && source_count() > 1;
  if (!enabled() && !baseline_multi) {
    std::fprintf(stderr, "SN_DOMAIN_CONFIG role=%s enabled=0 path=original-rpc\n", role); return;
  }
#ifndef ERPC_SN_DOMAIN_PROXY
  if (enabled()) fail(role, 0, "build-option-disabled", 0, ENOTSUP);
#endif
  const auto &client = config_json_all.at("client");
  if (FLAGS_client_num > 4 || FLAGS_server_num != FLAGS_client_num)
    throw std::invalid_argument("matching client_num/server_num must be 1..4");
  const uint32_t lanes = static_cast<uint32_t>(FLAGS_client_num);
  const uint32_t stride = request_stride(lanes, source_count());
  if (FLAGS_concurrency == 0 || FLAGS_concurrency > kSlots / stride ||
      config_u32(client.at("generate_num")) == 0 || config_u32(client.at("generate_num")) > UINT32_MAX / stride ||
      !((config_u32(client.at("user_num")) == 1 && config_u32(client.at("home_num")) == 0) ||
        (config_u32(client.at("user_num")) == 0 && config_u32(client.at("home_num")) == 1)) ||
      config_u32(client.at("write_num")) != 0)
    throw std::invalid_argument("single user/home read workload or disjoint in-flight budget invalid");
#ifndef ERPC_SN_DOMAIN_UB
  if (enabled() && read_service() == ReadService::Home)
    throw std::invalid_argument("HomeTimeline domain route currently requires the UB backend");
#endif
  (void)proxy_count();
#ifdef ERPC_SN_DOMAIN_UB
  if (enabled() && (lanes != 1 || source_count() > 4 || proxy_count() != 1 || FLAGS_concurrency != 1))
    throw std::invalid_argument("UB social domain requires 1..4 Client processes, one lane each, one nginx, concurrency=1");
#endif
  bool used[64]{};
  std::vector<std::string> addresses;
  auto reserve = [&](uint32_t base, const std::string &address) {
    if (address.empty() || address.find(',') != std::string::npos)
      throw std::invalid_argument("require an explicit service address");
    for (const auto &a : addresses) if (a == address) throw std::invalid_argument("duplicate service address");
    addresses.push_back(address);
    for (unsigned sender = 0; sender < 2; ++sender)
      for (uint32_t i = 0; i < lanes; ++i) {
        const uint64_t id = static_cast<uint64_t>(base) + (sender ? 20U : 0U) + i;
        if (id >= 64 || used[id]) throw std::invalid_argument("rpc-id-overlap-or-range");
        used[id] = true;
      }
  };
  for (const auto &item : config_json_all.items()) {
    const auto &v = item.value();
    if (item.key() == "client" || item.key() == "nginx" || !v.is_object() ||
        !v.contains("rpc_id") || !v.contains("server_addr")) continue;
    reserve(config_u32(v.at("rpc_id")), v.at("server_addr").get<std::string>());
  }
  for (const auto &r : clients()) reserve(r.rpc_id, r.address);
  for (const auto &r : nginx_instances()) reserve(r.rpc_id, r.address);
  uint32_t source = 0;
  if (std::strcmp(role, "client") == 0) {
    const auto &r = clients()[source_index()]; source = r.id;
    FLAGS_server_addr = r.address; FLAGS_rpc_id = r.rpc_id; FLAGS_bind_core_offset = r.bind_core_offset;
    if (source_count() > 1) FLAGS_latency_file += "-client-" + std::to_string(source);
  }
  std::fprintf(stderr, "SN_DOMAIN_CONFIG role=%s enabled=%u path=%s wire_abi=%u scope=%s-timeline-read source=%u clients=%u lanes=%u receivers=%u proxies=%u stride=%u connections=%u routing=per-source-lane-round-robin fallback=none\n",
      role, enabled() ? 1U : 0U, enabled() ? "domain" : "original-rpc", kWireAbi,
      read_service() == ReadService::Home ? "home" : "user",
      source, source_count(), lanes, lanes, proxy_count(), stride, (source_count() + 1) * lanes);
  for (const auto &r : clients())
    std::fprintf(stderr, "SN_DOMAIN_PEER role=%s source=%u receiver_base=%u lanes=%u\n", role, r.id, r.rpc_id, lanes);
  for (const auto &r : nginx_instances())
    std::fprintf(stderr, "SN_DOMAIN_INSTANCE role=%s domain=%u receiver_base=%u\n", role, r.id, r.rpc_id);
}
inline bool origin_valid(const Origin &origin, uint32_t wire) {
  for (size_t i = 0; i < clients().size(); ++i)
    if (clients()[i].id == origin.source)
      return valid_origin(origin, wire, static_cast<uint32_t>(FLAGS_client_num), static_cast<uint32_t>(FLAGS_server_num),
          clients()[i].rpc_id, proxy_count(), clients()[i].id, static_cast<uint32_t>(i), source_count());
  return false;
}
}  // namespace sn_domain
