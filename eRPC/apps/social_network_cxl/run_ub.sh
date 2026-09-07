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
export ERPC_UB_REGION_MB="${ERPC_UB_REGION_MB:-1024}"
export ERPC_UB_ARENA_MB="${ERPC_UB_ARENA_MB:-16}"
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
cleanup_started=0

cleanup() {
  if [[ "$cleanup_started" -ne 0 ]]; then
    return
  fi
  cleanup_started=1
  trap - EXIT INT TERM

  # Workers own the endpoint registrations and remote mappings. Give them an
  # opportunity to run their SIGINT handlers and unregister while the manager
  # socket and machine region are still alive.
  for pid in "${worker_pids[@]}"; do
    kill -INT "$pid" 2>/dev/null || true
  done

  for _ in $(seq 1 100); do
    workers_alive=0
    for pid in "${worker_pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null; then
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
    if kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null || true
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
  echo "Starting $name on UB machine $ERPC_UB_MACHINE_ID"
  "$bin" --config_file="$CONFIG" >"$LOG_DIR/$name.log" 2>&1 &
  worker_pids+=("$!")
done

wait
