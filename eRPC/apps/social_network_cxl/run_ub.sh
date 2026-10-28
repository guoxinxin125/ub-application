#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$APP_DIR/../.." && pwd)"
BUILD_DIR="${SN_BUILD_DIR:-$ROOT_DIR/build}"
CONFIG="${SN_CONFIG:-$APP_DIR/config/config.json}"
LOG_DIR="${SN_LOG_DIR:-$APP_DIR/logs/ub-${ERPC_UB_MACHINE_ID:-unset}}"

if [[ -z "${ERPC_UB_MACHINE_ID:-}" ]]; then
  echo "ERPC_UB_MACHINE_ID must be set to a nonzero machine ID" >&2
  exit 1
fi

export ERPC_UB_PROCESS_MODE="${ERPC_UB_PROCESS_MODE:-multi}"
export ERPC_UB_MEMORY_MODE="${ERPC_UB_MEMORY_MODE:-one-sided}"
# The client keeps thousands of generated requests in its endpoint arena and
# Post Storage keeps every MongoDB PostData object in its endpoint arena.
export ERPC_UB_REGION_MB="${ERPC_UB_REGION_MB:-2048}"
export ERPC_UB_ARENA_MB="${ERPC_UB_ARENA_MB:-64}"
export ERPC_UB_MANAGER_SOCKET="${ERPC_UB_MANAGER_SOCKET:-/tmp/erpc_ub_social_network.sock}"
export ERPC_UB_SHUTDOWN_TIMEOUT_MS="${ERPC_UB_SHUTDOWN_TIMEOUT_MS:-30000}"
export ERPC_UB_SHUTDOWN_RETRY_MS="${ERPC_UB_SHUTDOWN_RETRY_MS:-500}"

if [[ "$ERPC_UB_PROCESS_MODE" != "multi" ]]; then
  echo "social_network_cxl launches multiple processes and requires ERPC_UB_PROCESS_MODE=multi" >&2
  exit 1
fi

default_services="post_storage unique_id url_shorten user_mention user_service user_timeline home_timeline compose_post nginx load_balance client"
read -r -a services <<< "${SN_SERVICES:-$default_services}"

mkdir -p "$LOG_DIR"
manager_pid=""
worker_pids=()
worker_names=()
client_pids=()
client_logs=()
start_file=""
start_dir=""
cleanup_started=0

# kill -0 also succeeds for a zombie: a child that already exited but has not
# been reaped yet. Treating it as alive makes cleanup wait out the whole grace
# period and then report a misleading "ignored SIGTERM" for a process that
# actually shut down cleanly, and lets the startup check miss a worker that
# died. Read the state field from /proc instead; only Z means "already gone".
worker_alive() {
  local state
  state=$(awk '{ sub(/^.*\) /, ""); print $1 }' "/proc/$1/stat" 2>/dev/null) || return 1
  [[ -n "$state" && "$state" != Z ]]
}

# A worker still alive after both signals is about to be killed. Record where
# each of its threads is blocked: a spinning thread (state R, no wchan) means
# the ctrl_c flag was never raised, while a thread waiting in futex/ep_poll
# means shutdown is stuck inside a driver or library call.
report_stuck_worker() {
  local pid=$1 name=$2 state wchan tid
  state=$(awk '{ sub(/^.*\) /, ""); print $1 }' "/proc/$pid/stat" 2>/dev/null)
  echo "Service $name ignored SIGTERM (pid=$pid state=${state:-?}); killing it so it releases its SM UDP port" >&2
  for tid in "/proc/$pid"/task/*; do
    [[ -r "$tid/stat" ]] || continue
    state=$(awk '{ sub(/^.*\) /, ""); print $1 }' "$tid/stat" 2>/dev/null)
    wchan=$(cat "$tid/wchan" 2>/dev/null)
    echo "  thread tid=${tid##*/} state=${state:-?} wchan=${wchan:-none}" >&2
  done
}

cleanup() {
  if [[ "$cleanup_started" -ne 0 ]]; then
    return
  fi
  cleanup_started=1
  trap - EXIT INT TERM
  if [[ -n "$start_file" ]]; then rm -f -- "$start_file"; fi
  if [[ -n "$start_dir" ]]; then rmdir -- "$start_dir" 2>/dev/null || true; fi

  # Workers own the endpoint registrations and remote mappings. Give them an
  # opportunity to run their SIGINT handlers and unregister while the manager
  # socket and machine region are still alive.
  for pid in "${worker_pids[@]}"; do
    kill -INT "$pid" 2>/dev/null || true
  done

  for _ in $(seq 1 100); do
    workers_alive=0
    for pid in "${worker_pids[@]}"; do
      if worker_alive "$pid"; then
        workers_alive=1
        break
      fi
    done
    [[ "$workers_alive" -eq 0 ]] && break
    sleep 0.05
  done

  # Do not let a worker that cannot complete graceful shutdown keep the local
  # manager alive forever. The manager is still deliberately stopped last.
  for pid in "${worker_pids[@]}"; do
    if worker_alive "$pid"; then
      kill -TERM "$pid" 2>/dev/null || true
    fi
  done
  # Application handlers for SIGINT and SIGTERM only raise a flag, so a worker
  # blocked in a driver call (for example a Timeline waiting on MongoDB) can
  # outlive both signals. Waiting for it forever would leave it alive, still
  # holding its SM UDP port, and the next run's Nexus would fail to bind that
  # port. Escalate to SIGKILL after a bounded grace period.
  for _ in $(seq 1 100); do
    workers_alive=0
    for pid in "${worker_pids[@]}"; do
      if worker_alive "$pid"; then
        workers_alive=1
        break
      fi
    done
    [[ "$workers_alive" -eq 0 ]] && break
    sleep 0.05
  done
  for index in "${!worker_pids[@]}"; do
    if worker_alive "${worker_pids[$index]}"; then
      report_stuck_worker "${worker_pids[$index]}" "${worker_names[$index]}"
      kill -KILL "${worker_pids[$index]}" 2>/dev/null || true
    fi
  done
  for pid in "${worker_pids[@]}"; do
    wait "$pid" 2>/dev/null || true
  done

  if [[ -n "$manager_pid" ]]; then
    kill -TERM "$manager_pid" 2>/dev/null || true
    if ! wait "$manager_pid"; then
      echo "UB manager did not delete its region cleanly; inspect $LOG_DIR/erpc_ub_manager.log" >&2
    fi
  fi
}
trap cleanup EXIT INT TERM

manager="$BUILD_DIR/erpc_ub_manager"
if [[ ! -x "$manager" ]]; then
  echo "Missing UB manager: $manager" >&2
  exit 1
fi

"$manager" >"$LOG_DIR/erpc_ub_manager.log" 2>&1 &
manager_pid="$!"

for _ in $(seq 1 100); do
  [[ -S "$ERPC_UB_MANAGER_SOCKET" ]] && break
  sleep 0.05
done
if [[ ! -S "$ERPC_UB_MANAGER_SOCKET" ]]; then
  echo "UB manager socket was not created: $ERPC_UB_MANAGER_SOCKET" >&2
  exit 1
fi

for name in "${services[@]}"; do
  bin="$BUILD_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "Missing binary: $bin" >&2
    exit 1
  fi
  # A worker that dies during startup (for example, an eRPC Nexus that cannot
  # bind its management UDP port) otherwise only shows up as confusing
  # connection errors in the services started after it.
  for index in "${!worker_pids[@]}"; do
    if ! worker_alive "${worker_pids[$index]}"; then
      echo "Service ${worker_names[$index]} exited during startup; inspect $LOG_DIR/${worker_names[$index]}*.log" >&2
      exit 1
    fi
  done
  echo "Starting $name on UB machine $ERPC_UB_MACHINE_ID"
  if [[ "$name" == nginx && "${SN_RPC_NGINX_COUNT:-1}" == 3 ]]; then
    for index in 0 1 2; do
      ERPC_SN_RPC_NGINX_INDEX="$index" "$bin" --config_file="$CONFIG" >"$LOG_DIR/nginx-$((index+1)).log" 2>&1 &
      worker_pids+=("$!")
      worker_names+=("nginx-$((index+1))")
    done
  elif [[ "$name" == client && -n "${SN_CLIENT_IDS:-}" ]]; then
    read -r -a ids <<< "$SN_CLIENT_IDS"
    if (( ${#ids[@]} > 1 )); then
      start_dir=$(mktemp -d "$LOG_DIR/.client-start.XXXXXXXX")
      start_file="$start_dir/go"
      for id in "${ids[@]}"; do
        [[ "$id" =~ ^[1-9][0-9]*$ ]] || { echo "Invalid client ID: $id" >&2; exit 2; }
        log="$LOG_DIR/client-$id.log"
        ERPC_SN_DOMAIN_CLIENT_ID="$id" ERPC_SN_CLIENT_START_FILE="$start_file" \
          "$bin" --config_file="$CONFIG" >"$log" 2>&1 &
        client_pids+=("$!"); worker_pids+=("$!"); worker_names+=("client-$id"); client_logs+=("$log")
      done
    else
      ERPC_SN_DOMAIN_CLIENT_ID="${ids[0]}" "$bin" --config_file="$CONFIG" >"$LOG_DIR/client.log" 2>&1 &
      worker_pids+=("$!")
      worker_names+=("client")
    fi
  else
    "$bin" --config_file="$CONFIG" >"$LOG_DIR/$name.log" 2>&1 &
    worker_pids+=("$!")
    worker_names+=("$name")
  fi
done

if [[ -n "$start_file" ]]; then
  armed=0
  for _ in $(seq 1 1200); do
    armed=0
    for log in "${client_logs[@]}"; do
      if grep -Eq 'SN_DOMAIN_(CONFIG_)?ERROR|terminate called|Assertion.*failed' "$log"; then
        echo "Client failed before start barrier: $log" >&2
        exit 1
      fi
      if grep -q '^SN_CLIENT_ARMED ' "$log"; then ((armed+=1)); fi
    done
    ((armed == ${#client_logs[@]})) && break
    for pid in "${client_pids[@]}"; do
      if ! worker_alive "$pid"; then
        echo "Client exited before start barrier; inspect $LOG_DIR/client-*.log" >&2
        exit 1
      fi
    done
    sleep 0.1
  done
  ((armed == ${#client_logs[@]})) || { echo "Client readiness timed out; inspect $LOG_DIR/client-*.log" >&2; exit 1; }
  touch "$start_file"
  echo "UB_SOCIAL_CLIENTS_STARTED count=${#client_pids[@]}"
  status=0
  for pid in "${client_pids[@]}"; do wait "$pid" || status=1; done
  exit "$status"
fi
wait
