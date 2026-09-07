#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 4 || $# -gt 6 ]]; then
  echo "usage: $0 <id:0|1> <coordinator-0-ip> <one-sided|nocache> <unique-prefix> [iterations] [sync-port]" >&2
  echo "env: TIGON_BUILD_DIR UB_PROVIDER_HOST UB_PROVIDER_NUMA UB_NUMA_NODE UB_REGION_MB" >&2
  exit 2
fi

id=$1
sync_ip=$2
mode=$3
prefix=$4
iterations=${5:-10000}
sync_port=${6:-18951}

if [[ "$id" != "0" && "$id" != "1" ]]; then
  echo "id must be 0 or 1" >&2
  exit 2
fi
if [[ "$mode" != "one-sided" && "$mode" != "nocache" ]]; then
  echo "mode must be one-sided or nocache" >&2
  exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tigon_dir=$(cd -- "$script_dir/.." && pwd)
build_dir=${TIGON_BUILD_DIR:-$tigon_dir/build-ub}
binary=$build_dir/ub_tigon_two_node_test
provider_host=${UB_PROVIDER_HOST:-$(hostname -f)}
provider_numa=${UB_PROVIDER_NUMA:-}
region_mb=${UB_REGION_MB:-64}

if [[ -n "$provider_numa" && ! "$provider_numa" =~ ^[0-9]+$ ]]; then
  echo "UB_PROVIDER_NUMA must be a non-negative integer" >&2
  exit 2
fi

if [[ ! -x "$binary" ]]; then
  echo "missing executable: $binary" >&2
  echo "build target ub_tigon_two_node_test first" >&2
  exit 1
fi

command=(
  "$binary"
  "--id=$id"
  "--sync-ip=$sync_ip"
  "--port=$sync_port"
  "--mode=$mode"
  "--prefix=$prefix"
  "--region-mb=$region_mb"
  "--iterations=$iterations"
  "--provider-host=$provider_host"
)

if [[ -n "$provider_numa" ]]; then
  command+=("--provider-numa=$provider_numa")
fi

echo "running UB-Tigon correctness: id=$id mode=$mode prefix=$prefix provider_numa=${provider_numa:-auto}"
if [[ -n "${UB_NUMA_NODE:-}" ]]; then
  exec numactl --cpunodebind="$UB_NUMA_NODE" --membind="$UB_NUMA_NODE" \
    "${command[@]}"
else
  exec "${command[@]}"
fi
