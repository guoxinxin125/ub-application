#!/usr/bin/env bash
# One foreground session per host. The existing launcher still owns each
# manager/worker lifecycle; this supervisor owns only the optional B publisher
# and A's private SSH tunnel, and stops them after the application exits.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo=$(CDPATH= cd -- "$root/.." && pwd)
side= mode= config= ssh_b=
usage() {
  echo 'usage: bash scripts/run-social-session.sh --side A|B --mode domain|rpc --config /absolute/config.json [--ssh-b USER@B]'
  echo 'B/domain starts owner then PostStorage+manager; A --ssh-b owns a localhost:22012 tunnel for this session.'
}
while (($#)); do
  case "$1" in
    --side|--mode|--config|--ssh-b)
      (($# >= 2)) || { usage >&2; exit 2; }
      case "$1" in
        --side) side=$2 ;; --mode) mode=$2 ;; --config) config=$2 ;; --ssh-b) ssh_b=$2 ;;
      esac
      shift 2 ;;
    --help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done
case "$side:$mode" in A:domain|A:rpc|B:domain|B:rpc) ;; *) usage >&2; exit 2 ;; esac
case "$config" in /*) ;; *) echo 'An absolute --config path is required.' >&2; exit 2 ;; esac
[[ -f "$config" ]] || { echo "Missing config: $config" >&2; exit 1; }
[[ -z "$ssh_b" || "$side" == A ]] || { echo '--ssh-b is only valid on A.' >&2; exit 2; }
: "${ERPC_UB_MACHINE_ID:?Set the application machine ID first}"
if [[ "$mode" == domain ]]; then
  : "${LRPC_UB_DOMAIN_NAME:?Set the same domain name on A and B}"
fi

# Reject an invalid CPU plan before B creates the owner region or A opens SSH.
python3 "$root/scripts/check-social-cpus.py" --config "$config" --side "$side" --mode "$mode"
logdir=${SN_LOG_DIR:-$repo/eRPC/apps/social_network_cxl/logs/ub-$ERPC_UB_MACHINE_ID}

# The ordinary RPC path has no extra process to supervise.
if [[ "$side:$mode" == B:rpc || ( "$side" == A && -z "$ssh_b" ) ]]; then
  exec sh "$root/scripts/run-social.sh" --side "$side" --mode "$mode" --config "$config"
fi

owner_pid= app_pid= control_dir= control_path= tunnel_owned=0
cleanup() {
  trap - EXIT HUP INT TERM
  if [[ -n "$app_pid" ]]; then
    kill -TERM "$app_pid" 2>/dev/null || true
    wait "$app_pid" 2>/dev/null || true
  fi
  if [[ -n "$owner_pid" ]]; then
    kill -TERM "$owner_pid" 2>/dev/null || true
    wait "$owner_pid" 2>/dev/null || true
  fi
  if ((tunnel_owned)); then
    ssh -S "$control_path" -O exit "$ssh_b" >/dev/null 2>&1 || true
  fi
  if [[ -n "$control_dir" ]]; then rmdir "$control_dir" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ "$side" == B ]]; then
  mkdir -p "$logdir"
  sh "$root/scripts/run-social.sh" --side B --mode domain >"$logdir/domain_owner.log" 2>&1 &
  owner_pid=$!
  ready=0
  for ((i=0;i<600;i++)); do
    if grep -q '^UB_DOMAIN_OWNER_READY multi=1 .*profile=social-user-read' "$logdir/domain_owner.log"; then
      ready=1; break
    fi
    if ! kill -0 "$owner_pid" 2>/dev/null; then break; fi
    sleep 0.1
  done
  if (( !ready )); then
    echo "B domain owner did not become ready; inspect $logdir/domain_owner.log" >&2
    tail -n 20 "$logdir/domain_owner.log" >&2 || true
    exit 1
  fi
  echo "UB_SOCIAL_OWNER_READY log=$logdir/domain_owner.log"
else
  # Only the bundled localhost DB endpoint is compatible with this tunnel.
  python3 - "$config" <<'PY'
import json, sys
db = json.load(open(sys.argv[1], encoding='utf-8'))['user_timeline_mongodb']
if (db['addr'], str(db['port'])) != ('127.0.0.1', '22012'):
    sys.exit('--ssh-b requires user_timeline_mongodb=127.0.0.1:22012')
PY
  command -v ssh >/dev/null || { echo 'ssh is required for --ssh-b.' >&2; exit 1; }
  mkdir -p "$logdir"
  control_dir=$(mktemp -d "${TMPDIR:-/tmp}/lrpc-social-ssh.XXXXXXXX")
  control_path=$control_dir/control
  # SSH authenticates interactively, then forks only after the local forward
  # succeeds. The private control socket lets cleanup stop exactly this tunnel.
  ssh -fNT -M -S "$control_path" -o ExitOnForwardFailure=yes \
    -L 127.0.0.1:22012:127.0.0.1:20012 "$ssh_b"
  tunnel_owned=1
  ssh -S "$control_path" -O check "$ssh_b" >/dev/null
  echo 'UB_SOCIAL_TUNNEL_READY local=127.0.0.1:22012 remote=127.0.0.1:20012'
fi

sh "$root/scripts/run-social.sh" --side "$side" --mode "$mode" --config "$config" &
app_pid=$!
while kill -0 "$app_pid" 2>/dev/null; do
  if [[ -n "$owner_pid" ]] && ! kill -0 "$owner_pid" 2>/dev/null; then
    echo "B domain owner exited while the application was running; inspect $logdir/domain_owner.log" >&2
    exit 1
  fi
  if ((tunnel_owned)) && ! ssh -S "$control_path" -O check "$ssh_b" >/dev/null 2>&1; then
    echo 'The owned SSH tunnel closed while the application was running.' >&2
    exit 1
  fi
  sleep 1
done
status=0
wait "$app_pid" || status=$?
app_pid=
exit "$status"
