#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
role=${1:-}
if [ "$#" -ne 1 ] || { [ "$role" != owner ] && [ "$role" != caller ]; }; then
    echo "usage: sh scripts/build.sh owner|caller" >&2
    exit 2
fi
build_dir="$root/build-$role"
set -- -DUBLRPC_ROLE="$role" \
    -DUBSM_INCLUDE_DIR="${UBSM_INCLUDE_DIR:-/usr/local/ubs_mem/include}" \
    -DUBSM_LIBRARY="${UBSM_LIBRARY:-/usr/local/ubs_mem/lib/libubsm_sdk.so}"
if [ -n "${OBMM_INCLUDE_DIR:-}" ]; then
    set -- "$@" -DOBMM_INCLUDE_DIR="$OBMM_INCLUDE_DIR"
fi
if [ -n "${OBMM_LIBRARY:-}" ]; then
    set -- "$@" -DOBMM_LIBRARY="$OBMM_LIBRARY"
fi
cmake -S "$root" -B "$build_dir" "$@"
cmake --build "$build_dir"
if [ "$role" = caller ]; then
    ctest --test-dir "$build_dir" --output-on-failure
fi
