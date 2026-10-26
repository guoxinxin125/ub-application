#!/bin/sh
# Scoped launcher only: never insmods, restarts UB services, or touches MongoDB.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo=$(CDPATH= cd -- "$root/.." && pwd)
side= mode= config=
usage() {
    echo 'usage: sh scripts/run-social.sh --side A|B --mode domain|rpc [--config /absolute/config.json]'
    echo 'B/domain without --config starts the publisher; with --config starts PostStorage+manager in a separate terminal.'
    echo 'Set ERPC_UB_MACHINE_ID (application roles), LRPC_UB_DOMAIN_NAME (domain roles), SDK library paths.'
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --help) usage; exit 0 ;;
        --side|--mode|--config)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            case "$1" in --side) side=$2 ;; --mode) mode=$2 ;; --config) config=$2 ;; esac
            shift 2 ;;
        *) usage >&2; exit 2 ;;
    esac
done
case "$side:$mode" in A:domain|A:rpc|B:domain|B:rpc) ;; *) usage >&2; exit 2 ;; esac
if [ "$mode" = domain ]; then
    : "${LRPC_UB_DOMAIN_NAME:?Set the same private LRPC_UB_DOMAIN_NAME on A and B}"
fi
if [ "$side:$mode" = B:domain ] && [ -z "$config" ]; then
    owner="$root/build-owner/lrpc-ub-domain-owner"
    [ -x "$owner" ] || { echo 'Build owner first: sh scripts/build.sh owner' >&2; exit 1; }
    # Host is resolved by owner exactly as in the microbenchmark provider path.
    set -- "$owner" "$LRPC_UB_DOMAIN_NAME" --social --provider-numa "${LRPC_UB_PROVIDER_NUMA:-1}"
    if [ -n "${LRPC_UB_PROVIDER_HOST:-}" ]; then set -- "$@" --provider-host "$LRPC_UB_PROVIDER_HOST"; fi
    exec env LRPC_UBSM_SDK_MODE=owned "$@"
fi
case "$config" in /*) ;; *) echo 'Application roles require an absolute --config path.' >&2; exit 2 ;; esac
[ -f "$config" ] || { echo "Missing config: $config" >&2; exit 1; }
: "${ERPC_UB_MACHINE_ID:?Set a unique nonzero machine ID on each machine}"
export SN_CONFIG="$config"
build_mode=rpc
if [ "$side:$mode" = A:domain ]; then build_mode=domain; fi
export SN_BUILD_DIR="${SN_BUILD_DIR:-$repo/eRPC/cmake-build-social-ub-$build_mode}"
export ERPC_UB_REGION_PREFIX="${ERPC_UB_REGION_PREFIX:-sn_lrpc_lab}"
export ERPC_UB_MANAGER_SOCKET="${ERPC_UB_MANAGER_SOCKET:-/tmp/${ERPC_UB_REGION_PREFIX}_${ERPC_UB_MACHINE_ID}.sock}"
export ERPC_SN_DOMAIN_MEASURE=1 ERPC_SN_DOMAIN_FINISH=1
export ERPC_SN_DOMAIN_WARMUP="${ERPC_SN_DOMAIN_WARMUP:-0}"
export ERPC_SN_DOMAIN_KERNEL_EVERY=0 ERPC_SN_DOMAIN_DIAGNOSTIC=0 ERPC_SN_DOMAIN_FIXTURE=0
if [ "$side:$mode" = A:domain ]; then
    [ -c /dev/ub_lrpc_domain ] || { echo 'Load the matching experimental UB module on A first.' >&2; exit 1; }
    [ "$(id -u)" = 0 ] || { echo 'A domain application requires CAP_SYS_RAWIO; run this command as root.' >&2; exit 1; }
    export ERPC_SN_DOMAIN_PROXY=1 ERPC_SN_RPC_BASELINE=0 LRPC_UBSM_SDK_MODE=external
    export LRPC_UB_NESTED_PATH="${LRPC_UB_NESTED_PATH:-fused}"
else
    # B PostStorage uses the same RPC-only binary/readiness path in both modes.
    export ERPC_SN_DOMAIN_PROXY=0 ERPC_SN_RPC_BASELINE=1
fi
SN_SERVICES=$(python3 "$root/scripts/check-social-cpus.py" --config "$SN_CONFIG" --side "$side" --mode "$mode" --print-services)
export SN_SERVICES
if [ "$side" = A ]; then
    SN_CLIENT_IDS=$(python3 - "$SN_CONFIG" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1], encoding='utf-8'))
clients = cfg.get('domain_proxy', {}).get('clients', [{'id': 1}])
if not 1 <= len(clients) <= 3 or [c.get('id') for c in clients] != list(range(1,len(clients)+1)):
    sys.exit('require 1..3 client IDs ordered 1..N')
if len(clients)>1 and (clients[0]['server_addr'] != cfg['client']['server_addr'] or
                       clients[0]['rpc_id'] != cfg['client']['rpc_id']):
    sys.exit('first registered client must match the client endpoint')
print(' '.join(str(c['id']) for c in clients))
PY
    )
    export SN_CLIENT_IDS
fi
python3 "$root/scripts/check-social-cpus.py" --config "$SN_CONFIG" --side "$side" --mode "$mode"
echo "UB_SOCIAL_LAUNCH side=$side mode=$mode services=$SN_SERVICES clients=${SN_CLIENT_IDS:-none} config=$SN_CONFIG"
exec bash "$repo/eRPC/apps/social_network_cxl/run_ub.sh"
