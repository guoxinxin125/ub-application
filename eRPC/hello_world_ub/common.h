#pragma once

#ifndef ERPC_UB
#error "hello_world_ub must be built with -DTRANSPORT=ub"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "rpc.h"

static constexpr uint16_t kDefaultServerPort = 31850;
static constexpr uint16_t kDefaultClientPort = 31851;
static constexpr uint8_t kUBHelloReqType = 2;
static constexpr size_t kUBHelloDefaultMsgSize = 4096;
static constexpr size_t kUBHelloDefaultRequests = 100000;
static constexpr size_t kUBHelloWarmupRequests = 100;

inline uint64_t ub_hello_parse_u64(const char *text, const char *name) {
  if (text == nullptr || text[0] == '\0') {
    std::fprintf(stderr, "%s must not be empty\n", name);
    std::exit(2);
  }
  char *end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 0);
  if (end == text || *end != '\0') {
    std::fprintf(stderr, "Invalid %s: %s\n", name, text);
    std::exit(2);
  }
  return static_cast<uint64_t>(value);
}

inline uint16_t ub_hello_parse_port(const char *text, const char *name) {
  const uint64_t value = ub_hello_parse_u64(text, name);
  if (value == 0 || value > UINT16_MAX) {
    std::fprintf(stderr, "%s must be in [1, 65535]\n", name);
    std::exit(2);
  }
  return static_cast<uint16_t>(value);
}

inline const char *ub_hello_env_or(const char *name,
                                   const char *default_value) {
  const char *value = std::getenv(name);
  return value == nullptr || value[0] == '\0' ? default_value : value;
}

inline size_t ub_hello_numa_node() {
  const char *numa_node = std::getenv("ERPC_UB_NUMA_NODE");
  if (numa_node == nullptr || numa_node[0] == '\0') {
    numa_node = ub_hello_env_or("ERPC_UB_PROVIDER_NUMA", "0");
  }
  return static_cast<size_t>(
      ub_hello_parse_u64(numa_node, "ERPC_UB_NUMA_NODE"));
}

inline void ub_hello_print_config(const char *role, const std::string &uri,
                                  uint8_t rpc_id) {
  std::printf(
      "UB hello %s: uri=%s rpc_id=%u process_mode=%s memory_mode=%s "
      "machine_id=%s numa_node=%s provider_numa=%s region_mb=%s "
      "arena_mb=%s\n",
      role, uri.c_str(), static_cast<unsigned>(rpc_id),
      ub_hello_env_or("ERPC_UB_PROCESS_MODE", "single"),
      ub_hello_env_or("ERPC_UB_MEMORY_MODE", "one-sided"),
      ub_hello_env_or("ERPC_UB_MACHINE_ID", "hostname-hash"),
      ub_hello_env_or("ERPC_UB_NUMA_NODE",
                      ub_hello_env_or("ERPC_UB_PROVIDER_NUMA", "0")),
      ub_hello_env_or("ERPC_UB_PROVIDER_NUMA", "same-as-numa-node"),
      ub_hello_env_or("ERPC_UB_REGION_MB", "256"),
      ub_hello_env_or("ERPC_UB_ARENA_MB", "16"));
}

inline uint64_t ub_hello_expected_checksum(size_t requests) {
  return requests == 0 ? 0
                       : static_cast<uint64_t>(requests) *
                             static_cast<uint64_t>(requests - 1) / 2;
}
