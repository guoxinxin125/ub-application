#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="/home/gxx/pccshm-sdk/eRPC"
BUILD_DIR="$ROOT_DIR/build"
APP_DIR="$ROOT_DIR/apps/social_network_cxl"
CONFIG="$APP_DIR/config/config.json"
LOG_DIR="$APP_DIR/logs"

mkdir -p "$LOG_DIR"

pids=()

start_svc() {
  local name="$1"
  local bin="$BUILD_DIR/$name"
  if [[ ! -x "$bin" ]]; then
    echo "Missing binary: $bin" >&2
    exit 1
  fi
  echo "Starting $name"
  "$bin" --config_file="$CONFIG" >"$LOG_DIR/$name.log" 2>&1 &
  pids+=("$!")
  # sleep 1
}

cleanup() {
  echo "Stopping services"
  for pid in "${pids[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

# gdb --batch -ex run -ex bt --args $BUILD_DIR/post_storage --config_file=$CONFIG >$LOG_DIR/post_storage_gdb.log 2>&1 &
start_svc post_storage
start_svc unique_id
start_svc url_shorten
start_svc user_mention
start_svc user_service
start_svc user_timeline
start_svc home_timeline
start_svc compose_post
# gdb --batch -ex run -ex bt --args $BUILD_DIR/nginx --config_file=$CONFIG > $LOG_DIR/nginx.log 2>&1 &
start_svc nginx
start_svc load_balance
start_svc client

wait
