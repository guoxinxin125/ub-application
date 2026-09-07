#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <id:0|1> <host0-ip:port;host1-ip:port> <unique-prefix-base>" >&2
  echo "env: TIGON_BUILD_DIR UB_PROVIDER_HOST UB_PROVIDER_NUMA UB_NUMA_NODE UB_REGION_MB UB_TIGON_* UB_TPCC_*" >&2
  exit 2
fi

id=$1
servers=$2
prefix_base=$3
if [[ "$id" != "0" && "$id" != "1" ]]; then
  echo "id must be 0 or 1" >&2
  exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tigon_dir=$(cd -- "$script_dir/.." && pwd)
build_dir=${TIGON_BUILD_DIR:-$tigon_dir/build-ub}
binary=$build_dir/bench_tpcc
provider_host=${UB_PROVIDER_HOST:-$(hostname -f)}
provider_numa=${UB_PROVIDER_NUMA:-}
region_mb=${UB_REGION_MB:-8192}
threads=${UB_TIGON_THREADS:-1}
run_seconds=${UB_TIGON_RUN_SECONDS:-20}
warmup_seconds=${UB_TIGON_WARMUP_SECONDS:-5}
neworder_dist=${UB_TPCC_NEWORDER_DIST:-100}
payment_dist=${UB_TPCC_PAYMENT_DIST:-100}

if [[ -n "$provider_numa" && ! "$provider_numa" =~ ^[0-9]+$ ]]; then
  echo "UB_PROVIDER_NUMA must be a non-negative integer" >&2
  exit 2
fi
if [[ ! -x "$binary" ]]; then
  echo "missing executable: $binary" >&2
  exit 1
fi

read -r -a modes <<< "${UB_TIGON_MODES:-one-sided nocache}"
read -r -a queries <<< "${UB_TPCC_QUERIES:-payment neworder first_two mixed}"
read -r -a transports <<< "${UB_TIGON_TRANSPORTS:-false true}"

run_one() {
  local mode=$1
  local query=$2
  local transport=$3
  local transport_name=tcp
  if [[ "$transport" == "true" ]]; then
    transport_name=ubq
  fi
  local prefix="${prefix_base}_${mode}_${query}_${transport_name}"
  local command=(
    "$binary"
    "--id=$id"
    "--servers=$servers"
    "--protocol=TwoPLPasha"
    "--partition_num=2"
    "--threads=$threads"
    "--io=1"
    "--shared_memory_backend=ub"
    "--ub_memory_mode=$mode"
    "--ub_region_prefix=$prefix"
    "--ub_region_mb=$region_mb"
    "--ub_provider_host=$provider_host"
    "--use_ub_transport=$transport"
    "--query=$query"
    "--neworder_dist=$neworder_dist"
    "--payment_dist=$payment_dist"
    "--time_to_run=$run_seconds"
    "--time_to_warmup=$warmup_seconds"
    "--lotus_checkpoint=0"
  )
  if [[ -n "$provider_numa" ]]; then
    command+=("--ub_provider_numa=$provider_numa")
  fi

  echo "TPCC run: id=$id mode=$mode query=$query transport=$transport_name prefix=$prefix provider_numa=${provider_numa:-auto}"
  if [[ -n "${UB_NUMA_NODE:-}" ]]; then
    numactl --cpunodebind="$UB_NUMA_NODE" --membind="$UB_NUMA_NODE" \
      "${command[@]}"
  else
    "${command[@]}"
  fi
}

for mode in "${modes[@]}"; do
  for transport in "${transports[@]}"; do
    for query in "${queries[@]}"; do
      run_one "$mode" "$query" "$transport"
    done
  done
done
