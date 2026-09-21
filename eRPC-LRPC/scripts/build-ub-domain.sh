#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${LRPC_UB_BUILD_DIR:-"$root/build-ub-domain"}
cmake -S "$root" -B "$build_dir" -G Ninja \
    -DLRPC_BACKEND=ubsm -DLRPC_UB_DOMAIN=ON \
    -DUBSM_INCLUDE_DIR="${UBSM_INCLUDE_DIR:-/usr/local/ubs_mem/include}" \
    -DUBSM_LIBRARY="${UBSM_LIBRARY:-/usr/local/ubs_mem/lib/libubsm_sdk.so}" "$@"
cmake --build "$build_dir" --target lrpc-ub-domain-owner lrpc-ub-domain-bench \
    lrpc-ub-pa-probe erpc-ub-lrpc-client test-ub-domain-layout
ctest --test-dir "$build_dir" --output-on-failure
