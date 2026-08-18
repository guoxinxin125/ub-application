#!/usr/bin/env bash

set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${ERPC_BUILD_DIR:-"${script_dir}/../build-ub"}

usage() {
  cat <<'EOF'
Usage:
  bash run.sh server <bind-ip> <machine-id> <numa-node> [server-port] [max-requests]
  bash run.sh client <server-ip> <client-bind-ip> <machine-id> <numa-node> \
    [concurrency] [msg-size] [requests] [server-port] [client-port]

Examples for one valid HLC pair (81:NUMA1 <-> 82:NUMA0):
  # Machine 82, server
  bash run.sh server 192.0.2.82 82 0 31850 10100

  # Machine 81, client
  bash run.sh client 192.0.2.82 192.0.2.81 81 1 1 4096 10000

The script defaults to single-process and one-sided-CC mode. Override with:
  ERPC_UB_PROCESS_MODE=multi
  ERPC_UB_MEMORY_MODE=nocache
  ERPC_UB_REGION_MB=256
  ERPC_UB_ARENA_MB=16
  ERPC_UB_REGION_PREFIX=erpc_ub_rx
  ERPC_UB_PROVIDER_HOST=<provider-hostname>
  ERPC_UB_PROVIDER_SOCKET=<socket-id>
  ERPC_UB_PROVIDER_PORT=<port-id>
  ERPC_BUILD_DIR=/path/to/eRPC/build-ub
  UBSM_LIBRARY_DIR=/path/to/ubs/library/directory
EOF
}

die() {
  echo "run.sh: $*" >&2
  exit 2
}

require_nonempty() {
  local value=$1
  local name=$2
  [[ -n "${value}" ]] || die "${name} must not be empty"
}

require_uint() {
  local value=$1
  local name=$2
  [[ "${value}" =~ ^[0-9]+$ ]] || die "${name} must be a non-negative integer"
}

require_positive_uint() {
  local value=$1
  local name=$2
  require_uint "${value}" "${name}"
  ((value > 0)) || die "${name} must be greater than zero"
}

[[ $# -ge 1 ]] || {
  usage
  exit 2
}

role=$1
shift

export ERPC_UB_PROCESS_MODE=${ERPC_UB_PROCESS_MODE:-single}
export ERPC_UB_MEMORY_MODE=${ERPC_UB_MEMORY_MODE:-one-sided}
export ERPC_UB_REGION_MB=${ERPC_UB_REGION_MB:-256}
export ERPC_UB_ARENA_MB=${ERPC_UB_ARENA_MB:-16}
export ERPC_UB_REGION_PREFIX=${ERPC_UB_REGION_PREFIX:-erpc_ub_rx}

if [[ -n "${UBSM_LIBRARY_DIR:-}" ]]; then
  export LD_LIBRARY_PATH="${UBSM_LIBRARY_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi

case "${role}" in
  server)
    [[ $# -ge 3 && $# -le 5 ]] || {
      usage
      exit 2
    }
    bind_ip=$1
    machine_id=$2
    numa_node=$3
    server_port=${4:-31850}
    max_requests=${5:-0}

    require_nonempty "${bind_ip}" "bind-ip"
    require_positive_uint "${machine_id}" "machine-id"
    require_uint "${numa_node}" "numa-node"
    require_positive_uint "${server_port}" "server-port"
    require_uint "${max_requests}" "max-requests"

    command -v numactl >/dev/null 2>&1 || die "numactl is required"

    binary="${build_dir}/hello_ub_server"
    [[ -x "${binary}" ]] || die "server binary not found: ${binary}; run make first"

    export ERPC_UB_MACHINE_ID=${machine_id}
    export ERPC_UB_NUMA_NODE=${numa_node}
    export ERPC_UB_PROVIDER_NUMA=${numa_node}
    exec numactl --cpunodebind="${numa_node}" --membind="${numa_node}" \
      "${binary}" "${bind_ip}" "${server_port}" "${max_requests}"
    ;;

  client)
    [[ $# -ge 4 && $# -le 9 ]] || {
      usage
      exit 2
    }
    server_ip=$1
    client_ip=$2
    machine_id=$3
    numa_node=$4
    concurrency=${5:-1}
    msg_size=${6:-4096}
    requests=${7:-100000}
    server_port=${8:-31850}
    client_port=${9:-31851}

    require_nonempty "${server_ip}" "server-ip"
    require_nonempty "${client_ip}" "client-bind-ip"
    require_positive_uint "${machine_id}" "machine-id"
    require_uint "${numa_node}" "numa-node"
    require_positive_uint "${concurrency}" "concurrency"
    require_positive_uint "${msg_size}" "msg-size"
    require_positive_uint "${requests}" "requests"
    require_positive_uint "${server_port}" "server-port"
    require_positive_uint "${client_port}" "client-port"

    command -v numactl >/dev/null 2>&1 || die "numactl is required"

    binary="${build_dir}/hello_ub_client"
    [[ -x "${binary}" ]] || die "client binary not found: ${binary}; run make first"

    export ERPC_UB_MACHINE_ID=${machine_id}
    export ERPC_UB_NUMA_NODE=${numa_node}
    export ERPC_UB_PROVIDER_NUMA=${numa_node}
    exec numactl --cpunodebind="${numa_node}" --membind="${numa_node}" \
      "${binary}" "${server_ip}" "${client_ip}" "${concurrency}" \
      "${msg_size}" "${requests}" "${server_port}" "${client_port}"
    ;;

  -h|--help|help)
    usage
    ;;

  *)
    die "unknown role '${role}'; expected server or client"
    ;;
esac
