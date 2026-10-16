#!/bin/sh
# One caller invocation only. Start/restart B owner separately before each run.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${LRPC_UB_BUILD_DIR:-"$root/build-caller"}
cpu=${LRPC_UB_CPU:-}
test=direct
samples=1
warmup=0
name=${LRPC_UB_DOMAIN_NAME:-lrpc_ub_domain}
usage() {
    echo "usage: sh scripts/run-ub-domain-smoke.sh --cpu N [--test direct|nested]"
    echo "       [--samples N] [--warmup N] [--name SHM_NAME]"
    echo "defaults: direct, samples=1, warmup=0; CPU may also be set by LRPC_UB_CPU"
    echo "Requires loaded module and a fresh B owner. Does not load/unload modules."
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --help) usage; exit 0 ;;
        --cpu|--test|--samples|--warmup|--name)
            option=$1
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            case "$option" in
                --cpu) cpu=$2 ;; --test) test=$2 ;; --samples) samples=$2 ;;
                --warmup) warmup=$2 ;; --name) name=$2 ;;
            esac
            shift 2 ;;
        *) usage >&2; exit 2 ;;
    esac
done
case "$cpu" in ''|*[!0-9]*) echo 'Specify one allowed CPU with --cpu N.' >&2; exit 2 ;; esac
case "$test" in direct|nested) ;; *) usage >&2; exit 2 ;; esac
[ -n "$name" ] || { echo 'Shared-memory name must not be empty.' >&2; exit 2; }
bench="$build_dir/lrpc-ub-domain-bench"
[ -x "$bench" ] || { echo "Missing $bench; run sh scripts/build.sh caller first." >&2; exit 1; }
echo "UB_DOMAIN_SMOKE cpu=$cpu test=$test warmup=$warmup samples=$samples name=$name"
echo 'One connection: restart B owner before running this script again.'
exec env LRPC_UB_DOMAIN_NAME="$name" taskset -c "$cpu" "$bench" \
    --test "$test" --samples "$samples" --warmup "$warmup"
