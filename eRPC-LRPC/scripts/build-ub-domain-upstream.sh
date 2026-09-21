#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$root/third_party/eRPC"
output=${LRPC_UB_BUILD_DIR:-"$root/build-ub-domain"}
test -f "$source_dir/CMakeLists.txt" || {
    echo "Fetch and apply the existing eRPC/AArch64 patches first." >&2; exit 1;
}
cmake -S "$source_dir" -B "$output/upstream-erpc" \
    -DTRANSPORT=fake -DPERF=ON -DLOG_LEVEL=warn \
    -DCMAKE_PROJECT_eRPC_INCLUDE="$root/cmake/ub-domain-upstream.cmake" \
    -DUBSM_INCLUDE_DIR="${UBSM_INCLUDE_DIR:-/usr/local/ubs_mem/include}" \
    -DUBSM_LIBRARY="${UBSM_LIBRARY:-/usr/local/ubs_mem/lib/libubsm_sdk.so}" "$@"
cmake --build "$output/upstream-erpc" --target hello_server hello_client \
    -j"${JOBS:-2}"
install -m 755 "$source_dir/build/hello_server" "$output/upstream-erpc-domain-server"
install -m 755 "$source_dir/build/hello_client" "$output/upstream-erpc-domain-client"
