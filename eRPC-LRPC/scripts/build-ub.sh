#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${LRPC_UB_BUILD_DIR:-"$root/build-ub"}
ubsm_include=${UBSM_INCLUDE_DIR:-/usr/local/ubs_mem/include}
ubsm_library=${UBSM_LIBRARY:-/usr/local/ubs_mem/lib/libubsm_sdk.so}

cmake -S "$root" -B "$build_dir" -G Ninja \
	-DLRPC_BACKEND=ubsm \
	-DUBSM_INCLUDE_DIR="$ubsm_include" \
	-DUBSM_LIBRARY="$ubsm_library"
cmake --build "$build_dir"
