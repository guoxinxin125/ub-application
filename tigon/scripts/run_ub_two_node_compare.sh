#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<EOF
usage: $0 <ycsb|tpcc> <id:0|1> <host0-ip:port;host1-ip:port> <campaign>

Run the same command on both UB hosts.  The campaign string must match.
Defaults implement the two-node comparison; override UB_COMPARE_* for smoke or
extended sweeps.  Every point receives a distinct compact UB region prefix.
EOF
}

[[ $# -eq 4 ]] || { usage; exit 2; }
workload=$1
id=$2
servers=$3
campaign=$4
[[ "$workload" == ycsb || "$workload" == tpcc ]] || { usage; exit 2; }
[[ "$id" == 0 || "$id" == 1 ]] || { echo "id must be 0 or 1" >&2; exit 2; }
[[ "$campaign" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "campaign must contain only A-Z, a-z, 0-9, dot, underscore or dash" >&2; exit 2; }

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repetitions=${UB_COMPARE_REPETITIONS:-3}
first_rep=${UB_COMPARE_FIRST_REP:-1}
threads=${UB_COMPARE_THREADS:-3}
measurement_seconds=${UB_COMPARE_RUN_SECONDS:-30}
warmup_seconds=${UB_COMPARE_WARMUP_SECONDS:-30}
require_positive() { [[ "$2" =~ ^[1-9][0-9]*$ ]] || { echo "$1 must be a positive integer" >&2; exit 2; }; }
require_positive UB_COMPARE_REPETITIONS "$repetitions"
require_positive UB_COMPARE_FIRST_REP "$first_rep"
require_positive UB_COMPARE_THREADS "$threads"
require_positive UB_COMPARE_RUN_SECONDS "$measurement_seconds"
[[ "$warmup_seconds" =~ ^[0-9]+$ ]] || { echo "UB_COMPARE_WARMUP_SECONDS must be a non-negative integer" >&2; exit 2; }
total_seconds=$((warmup_seconds + measurement_seconds))

compact_prefix() {
  local checksum
  checksum=$(printf '%s' "$1" | cksum | awk '{print $1}')
  printf 'u%08x' "$checksum"
}

export UB_TIGON_THREADS=$threads
export UB_TIGON_RUN_SECONDS=$total_seconds
export UB_TIGON_WARMUP_SECONDS=$warmup_seconds
export UB_TIGON_MODES=${UB_COMPARE_MODES:-one-sided}
export UB_TIGON_TRANSPORTS=${UB_COMPARE_TRANSPORTS:-true}

last_rep=$((first_rep + repetitions - 1))
if [[ "$workload" == ycsb ]]; then
  export UB_REGION_MB=${UB_COMPARE_REGION_MB:-4096}
  export UB_TIGON_PARTITIONS=${UB_COMPARE_PARTITIONS:-2}
  export UB_TIGON_KEYS=${UB_COMPARE_KEYS:-300000}
  export UB_TIGON_ZIPF=${UB_COMPARE_ZIPF:-0.7}
  export UB_TIGON_QUERIES=${UB_COMPARE_YCSB_QUERIES:-rmw}
  read -r -a rw_ratios <<< "${UB_COMPARE_RW_RATIOS:-95 50}"
  read -r -a cross_ratios <<< "${UB_COMPARE_CROSS_RATIOS:-0 25 50 75 100}"
  for ((rep=first_rep; rep<=last_rep; ++rep)); do
    for rw in "${rw_ratios[@]}"; do
      for cross in "${cross_ratios[@]}"; do
        run_id="${campaign}_y_rw${rw}_c${cross}_r${rep}"
        export UB_TIGON_RW_RATIO=$rw UB_TIGON_CROSS_RATIO=$cross UB_TIGON_RUN_ID=$run_id UB_TIGON_REPETITION=$rep
        bash "$script_dir/run_ub_ycsb_matrix.sh" "$id" "$servers" "$(compact_prefix "$run_id")"
      done
    done
  done
else
  export UB_REGION_MB=${UB_COMPARE_REGION_MB:-8192}
  export UB_TIGON_PARTITIONS=${UB_COMPARE_PARTITIONS:-$((2 * threads))}
  export UB_TPCC_QUERIES=${UB_COMPARE_TPCC_QUERIES:-mixed}
  read -r -a remote_pairs <<< "${UB_COMPARE_TPCC_REMOTE_PAIRS:-0:0 60:90}"
  for ((rep=first_rep; rep<=last_rep; ++rep)); do
    for pair in "${remote_pairs[@]}"; do
      [[ "$pair" =~ ^([0-9]+):([0-9]+)$ ]] || { echo "invalid TPCC remote pair: $pair (expected neworder:payment)" >&2; exit 2; }
      neworder=${BASH_REMATCH[1]}
      payment=${BASH_REMATCH[2]}
      run_id="${campaign}_t_n${neworder}_p${payment}_r${rep}"
      export UB_TPCC_NEWORDER_DIST=$neworder UB_TPCC_PAYMENT_DIST=$payment UB_TIGON_RUN_ID=$run_id UB_TIGON_REPETITION=$rep
      bash "$script_dir/run_ub_tpcc_matrix.sh" "$id" "$servers" "$(compact_prefix "$run_id")"
    done
  done
fi
