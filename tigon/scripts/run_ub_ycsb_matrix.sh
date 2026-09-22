#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $0 <id:0|1> <host0-ip:port;host1-ip:port> <unique-prefix-base>" >&2
  echo "env: TIGON_BUILD_DIR UB_PROVIDER_HOST UB_PROVIDER_NUMA UB_NUMA_NODE UB_REGION_MB UB_RESULT_DIR UB_TIGON_*" >&2
}

[[ $# -eq 3 ]] || { usage; exit 2; }
id=$1
servers=$2
prefix_base=$3
[[ "$id" == 0 || "$id" == 1 ]] || { echo "id must be 0 or 1" >&2; exit 2; }

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tigon_dir=$(cd -- "$script_dir/.." && pwd)
binary=${TIGON_BUILD_DIR:-$tigon_dir/build-ub}/bench_ycsb
provider_host=${UB_PROVIDER_HOST:-$(hostname -f)}
provider_numa=${UB_PROVIDER_NUMA:-}
region_mb=${UB_REGION_MB:-4096}
threads=${UB_TIGON_THREADS:-1}
partitions=${UB_TIGON_PARTITIONS:-2}
keys=${UB_TIGON_KEYS:-200000}
rw_ratio=${UB_TIGON_RW_RATIO:-80}
zipf=${UB_TIGON_ZIPF:-0}
cross_ratio=${UB_TIGON_CROSS_RATIO:-100}
run_seconds=${UB_TIGON_RUN_SECONDS:-20}
warmup_seconds=${UB_TIGON_WARMUP_SECONDS:-5}
cpu_list=${UB_TIGON_CPU_LIST:-}
result_dir=${UB_RESULT_DIR:-}
run_id=${UB_TIGON_RUN_ID:-$prefix_base}
dry_run=${UB_TIGON_DRY_RUN:-0}
repetition=${UB_TIGON_REPETITION:-}

require_uint() { [[ "$2" =~ ^[0-9]+$ ]] || { echo "$1 must be a non-negative integer" >&2; exit 2; }; }
require_percent() { require_uint "$1" "$2"; (( $2 <= 100 )) || { echo "$1 must be in [0,100]" >&2; exit 2; }; }
for pair in "region_mb:$region_mb" "threads:$threads" "partitions:$partitions" "keys:$keys" "run_seconds:$run_seconds" "warmup_seconds:$warmup_seconds"; do
  require_uint "${pair%%:*}" "${pair#*:}"
done
(( region_mb > 0 && threads > 0 && partitions > 0 && keys > 0 && run_seconds > 0 )) || {
  echo "region_mb, threads, partitions, keys and run_seconds must be positive" >&2; exit 2;
}
(( run_seconds > warmup_seconds )) || { echo "UB_TIGON_RUN_SECONDS is total time and must exceed warmup" >&2; exit 2; }
measurement_seconds=$((run_seconds - warmup_seconds))
require_percent rw_ratio "$rw_ratio"
require_percent cross_ratio "$cross_ratio"
[[ "$zipf" =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo "UB_TIGON_ZIPF must be non-negative" >&2; exit 2; }
[[ -z "$provider_numa" || "$provider_numa" =~ ^[0-9]+$ ]] || { echo "UB_PROVIDER_NUMA must be a non-negative integer" >&2; exit 2; }
[[ -z "$cpu_list" || "$cpu_list" =~ ^[0-9,-]+$ ]] || { echo "UB_TIGON_CPU_LIST must be a Linux CPU list" >&2; exit 2; }
[[ "$dry_run" == 1 || -x "$binary" ]] || { echo "missing executable: $binary" >&2; exit 1; }

read -r -a modes <<< "${UB_TIGON_MODES:-one-sided nocache}"
read -r -a queries <<< "${UB_TIGON_QUERIES:-rmw insert delete}"
read -r -a transports <<< "${UB_TIGON_TRANSPORTS:-false true}"
print_command() { printf 'DRY_RUN_COMMAND'; printf ' %q' "$@"; printf '\n'; }

run_one() {
  local mode=$1 query=$2 transport=$3 transport_name=tcp
  [[ "$mode" == one-sided || "$mode" == nocache ]] || { echo "unsupported mode: $mode" >&2; exit 2; }
  [[ "$transport" == false || "$transport" == true ]] || { echo "transport must be false or true" >&2; exit 2; }
  [[ "$transport" == true ]] && transport_name=ubq
  local prefix="${prefix_base}_${mode}_${query}_${transport_name}"
  (( ${#prefix} <= 35 )) || { echo "UB region prefix exceeds 35 bytes: $prefix" >&2; exit 2; }
  local meta="TIGON_RUN_META system=ub workload=ycsb run_id=$run_id host_id=$id repetition=$repetition mode=$mode query=$query transport=$transport_name partitions=$partitions workers=$threads keys=$keys rw_ratio=$rw_ratio zipf=$zipf cross_ratio=$cross_ratio neworder_dist= payment_dist= warmup_seconds=$warmup_seconds run_seconds=$measurement_seconds total_seconds=$run_seconds logging=off"
  local command=("$binary" "--id=$id" "--servers=$servers" --protocol=TwoPLPasha
    "--partition_num=$partitions" "--threads=$threads" --io=1 --shared_memory_backend=ub
    "--ub_memory_mode=$mode" "--ub_region_prefix=$prefix" "--ub_region_mb=$region_mb"
    "--ub_provider_host=$provider_host" "--use_ub_transport=$transport" "--query=$query"
    "--keys=$keys" "--read_write_ratio=$rw_ratio" "--zipf=$zipf" "--cross_ratio=$cross_ratio"
    "--time_to_run=$run_seconds" "--time_to_warmup=$warmup_seconds" --lotus_checkpoint=0)
  [[ -z "$provider_numa" ]] || command+=("--ub_provider_numa=$provider_numa")
  local runner=()
  if [[ -n "${UB_NUMA_NODE:-}" ]]; then
    [[ "$UB_NUMA_NODE" =~ ^[0-9]+$ ]] || { echo "UB_NUMA_NODE must be a non-negative integer" >&2; exit 2; }
    runner+=(numactl)
    [[ -n "$cpu_list" ]] || runner+=("--cpunodebind=$UB_NUMA_NODE")
    runner+=("--membind=$UB_NUMA_NODE")
  fi
  [[ -z "$cpu_list" ]] || runner+=(taskset -c "$cpu_list")

  echo "$meta"
  if [[ "$dry_run" == 1 ]]; then
    print_command "${runner[@]}" "${command[@]}"
    echo "TIGON_RUN_END run_id=$run_id host_id=$id status=dry-run exit_code=0"
  elif [[ -n "$result_dir" ]]; then
    mkdir -p "$result_dir"
    local log="$result_dir/ub_ycsb_${run_id}_host${id}_${mode}_${query}_${transport_name}.log"
    set +e
    { echo "$meta"; "${runner[@]}" "${command[@]}"; rc=$?; echo "TIGON_RUN_END run_id=$run_id host_id=$id status=$([[ $rc -eq 0 ]] && echo ok || echo failed) exit_code=$rc"; exit "$rc"; } 2>&1 | tee "$log"
    local rc=${PIPESTATUS[0]}
    set -e
    return "$rc"
  else
    "${runner[@]}" "${command[@]}"
    echo "TIGON_RUN_END run_id=$run_id host_id=$id status=ok exit_code=0"
  fi
}

for mode in "${modes[@]}"; do
  for transport in "${transports[@]}"; do
    for query in "${queries[@]}"; do run_one "$mode" "$query" "$transport"; done
  done
done
