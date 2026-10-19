#!/bin/sh
# One caller invocation. Legacy owner needs restart; --multi owner can remain up.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${LRPC_UB_BUILD_DIR:-"$root/build-caller"}
cpu=${LRPC_UB_CPU:-}
test=direct
samples=1
warmup=0
nested_path=${LRPC_UB_NESTED_PATH-fused}
name=${LRPC_UB_DOMAIN_NAME:-lrpc_ub_domain}
emit_samples=
usage() {
    echo "usage: sh scripts/run-ub-domain-smoke.sh --cpu N [--test direct|nested]"
    echo "       [--samples N] [--warmup N] [--name SHM_NAME] [--nested-path legacy|fused] [--emit-samples]"
    echo "defaults: direct, samples=1, warmup=0, nested-path=fused; CPU may also be set by LRPC_UB_CPU"
    echo "Requires loaded module and B owner (legacy: fresh; --multi: running). Does not load/unload modules."
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --help) usage; exit 0 ;;
        --emit-samples) emit_samples=--emit-samples; shift ;;
        --cpu|--test|--samples|--warmup|--name|--nested-path)
            option=$1
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            case "$option" in
                --cpu) cpu=$2 ;; --test) test=$2 ;; --samples) samples=$2 ;;
                --warmup) warmup=$2 ;; --name) name=$2 ;;
                --nested-path) nested_path=$2 ;;
            esac
            shift 2 ;;
        *) usage >&2; exit 2 ;;
    esac
done
case "$cpu" in ''|*[!0-9]*) echo 'Specify one allowed CPU with --cpu N.' >&2; exit 2 ;; esac
case "$test" in direct|nested) ;; *) usage >&2; exit 2 ;; esac
case "$nested_path" in legacy|fused) ;; *) usage >&2; exit 2 ;; esac
[ -n "$name" ] || { echo 'Shared-memory name must not be empty.' >&2; exit 2; }
bench="$build_dir/lrpc-ub-domain-bench"
[ -x "$bench" ] || { echo "Missing $bench; run sh scripts/build.sh caller first." >&2; exit 1; }
echo "UB_DOMAIN_SMOKE cpu=$cpu test=$test warmup=$warmup samples=$samples name=$name nested_path=$nested_path"
echo 'One connection: restart legacy B owner before next run; --multi owner may remain running.'
exec env LRPC_UB_DOMAIN_NAME="$name" taskset -c "$cpu" "$bench" \
    --test "$test" --samples "$samples" --warmup "$warmup" --nested-path "$nested_path" \
    ${emit_samples:+"$emit_samples"}
