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

if [[ "$ERPC_UB_PROCESS_MODE" != "multi" ]]; then
  echo "social_network_cxl launches multiple processes and requires ERPC_UB_PROCESS_MODE=multi" >&2
  exit 1
fi

default_services="post_storage unique_id url_shorten user_mention user_service user_timeline home_timeline compose_post nginx load_balance client"
read -r -a services <<< "${SN_SERVICES:-$default_services}"

mkdir -p "$LOG_DIR"
pids=()

cleanup() {
  for pid in "${pids[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT INT TERM

manager="$BUILD_DIR/erpc_ub_manager"
if [[ ! -x "$manager" ]]; then
  echo "Missing UB manager: $manager" >&2
  exit 1
fi

"$manager" >"$LOG_DIR/erpc_ub_manager.log" 2>&1 &
pids+=("$!")

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
  pids+=("$!")
done

wait
