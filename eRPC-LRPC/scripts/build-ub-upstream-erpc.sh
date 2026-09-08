#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$root/third_party/eRPC"
build_dir=${LRPC_UB_ERPC_BUILD_DIR:-"$root/build-ub/upstream-erpc"}
output=${LRPC_UB_BUILD_DIR:-"$root/build-ub"}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}

if [ ! -f "$source_dir/CMakeLists.txt" ]; then
	echo "missing pinned eRPC source; run scripts/fetch-deps.sh first" >&2
	exit 1
fi
cmake -S "$source_dir" -B "$build_dir" \
	-DTRANSPORT=fake -DPERF=ON -DLOG_LEVEL=warn
cmake --build "$build_dir" --target hello_server hello_client -j"$jobs"
mkdir -p "$output"
install -m 755 "$source_dir/build/hello_server" \
	"$output/upstream-erpc-ub-server"
install -m 755 "$source_dir/build/hello_client" \
	"$output/upstream-erpc-ub-client"
echo "UB upstream eRPC binaries: $output/upstream-erpc-ub-{server,client}"
