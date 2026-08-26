#!/usr/bin/env bash

set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${ERPC_BUILD_DIR:-"${script_dir}/../build-ub"}

usage() {
  cat <<'EOF'
Usage:
  bash run_multi_clients.sh \
    <server-ip> <client-bind-ip> <machine-id> <numa-node> <client-count> \
    [concurrency] [msg-size] [requests] [server-port] [first-client-port]

Example: start two clients with RPC IDs 1 and 2, using ports 31851 and 31852:
  bash run_multi_clients.sh \
    192.0.2.82 192.0.2.81 81 1 2 8 4096 10000 31850 31851

The client-side erpc_ub_manager must already be running with the same UB
environment. Optional environment variables:
  ERPC_UB_MANAGER_SOCKET=/tmp/erpc_ub_manager.sock
  ERPC_UB_MEMORY_MODE=one-sided
  ERPC_UB_REGION_MB=256
  ERPC_UB_ARENA_MB=16
  ERPC_UB_REGION_PREFIX=erpc_ub_rx
  ERPC_UB_PROVIDER_HOST=<provider-hostname>
  ERPC_UB_PROVIDER_SOCKET=<socket-id>
  ERPC_UB_PROVIDER_PORT=<port-id>
  ERPC_BUILD_DIR=/path/to/eRPC/build-ub
  ERPC_UB_LOG_DIR=/path/to/client/logs
  UBSM_LIBRARY_DIR=/path/to/ubs/library/directory
EOF
}

die() {
  echo "run_multi_clients.sh: $*" >&2
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
  [[ "${value}" =~ ^[0-9]+$ ]] || die "${name} must be an integer"
}

require_positive_uint() {
  local value=$1
  local name=$2
  require_uint "${value}" "${name}"
  ((value > 0)) || die "${name} must be greater than zero"
}

if [[ $# -eq 1 && ($1 == "-h" || $1 == "--help" || $1 == "help") ]]; then
  usage
  exit 0
fi

[[ $# -ge 5 && $# -le 10 ]] || {
  usage
  exit 2
}

server_ip=$1
client_ip=$2
machine_id=$3
numa_node=$4
client_count=$5
concurrency=${6:-1}
msg_size=${7:-4096}
requests=${8:-100000}
server_port=${9:-31850}
first_client_port=${10:-31851}

require_nonempty "${server_ip}" "server-ip"
require_nonempty "${client_ip}" "client-bind-ip"
require_positive_uint "${machine_id}" "machine-id"
require_uint "${numa_node}" "numa-node"
require_positive_uint "${client_count}" "client-count"
require_positive_uint "${concurrency}" "concurrency"
require_positive_uint "${msg_size}" "msg-size"
require_positive_uint "${requests}" "requests"
require_positive_uint "${server_port}" "server-port"
require_positive_uint "${first_client_port}" "first-client-port"

((client_count <= 63)) || die "client-count must be in [1, 63]"
((server_port <= 65535)) || die "server-port must be in [1, 65535]"
last_client_port=$((first_client_port + client_count - 1))
((last_client_port <= 65535)) ||
  die "client port range exceeds 65535"

command -v numactl >/dev/null 2>&1 || die "numactl is required"

binary="${build_dir}/hello_ub_multi_client"
[[ -x "${binary}" ]] ||
  die "multi-client binary not found: ${binary}; build it first"

if [[ ${ERPC_UB_PROCESS_MODE:-multi} != "multi" ]]; then
  die "multiple client processes require ERPC_UB_PROCESS_MODE=multi"
fi
export ERPC_UB_PROCESS_MODE=multi
export ERPC_UB_MANAGER_SOCKET=${ERPC_UB_MANAGER_SOCKET:-/tmp/erpc_ub_manager.sock}
export ERPC_UB_MEMORY_MODE=${ERPC_UB_MEMORY_MODE:-one-sided}
export ERPC_UB_REGION_MB=${ERPC_UB_REGION_MB:-256}
export ERPC_UB_ARENA_MB=${ERPC_UB_ARENA_MB:-16}
export ERPC_UB_REGION_PREFIX=${ERPC_UB_REGION_PREFIX:-erpc_ub_rx}
export ERPC_UB_MACHINE_ID=${machine_id}
export ERPC_UB_NUMA_NODE=${numa_node}
export ERPC_UB_PROVIDER_NUMA=${numa_node}

if [[ -n "${UBSM_LIBRARY_DIR:-}" ]]; then
  export LD_LIBRARY_PATH="${UBSM_LIBRARY_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi

[[ -S "${ERPC_UB_MANAGER_SOCKET}" ]] ||
  die "manager socket is not ready: ${ERPC_UB_MANAGER_SOCKET}"

log_dir=${ERPC_UB_LOG_DIR:-"${TMPDIR:-/tmp}/erpc-ub-multi-$(date +%Y%m%d-%H%M%S)-$$"}
mkdir -p -- "${log_dir}"

declare -a client_pids=()
declare -a client_logs=()

echo "Starting ${client_count} UB clients; logs: ${log_dir}"
for ((client_index = 0; client_index < client_count; ++client_index)); do
  rpc_id=$((client_index + 1))
  client_port=$((first_client_port + client_index))
  client_log="${log_dir}/client-${rpc_id}.log"
  client_logs+=("${client_log}")

  numactl --cpunodebind="${numa_node}" --membind="${numa_node}" \
    "${binary}" "${server_ip}" "${client_ip}" "${rpc_id}" \
    "${concurrency}" "${msg_size}" "${requests}" "${server_port}" \
    "${client_port}" >"${client_log}" 2>&1 &
  client_pid=$!
  client_pids+=("${client_pid}")
  echo "  client ${rpc_id}: pid=${client_pid} port=${client_port} log=${client_log}"
done

overall_status=0
for ((client_index = 0; client_index < client_count; ++client_index)); do
  rpc_id=$((client_index + 1))
  if wait "${client_pids[client_index]}"; then
    client_status=0
  else
    client_status=$?
    overall_status=1
  fi
  echo "client ${rpc_id}: exit=${client_status} log=${client_logs[client_index]}"
done

if ((overall_status == 0)); then
  echo "All ${client_count} UB clients passed"
else
  echo "At least one UB client failed; inspect ${log_dir}" >&2
fi
exit "${overall_status}"
