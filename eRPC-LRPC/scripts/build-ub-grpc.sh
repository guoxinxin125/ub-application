#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${LRPC_UB_BUILD_DIR:-"$root/build-ub"}
ubsm_include=${UBSM_INCLUDE_DIR:-/usr/local/ubs_mem/include}
ubsm_library=${UBSM_LIBRARY:-/usr/local/ubs_mem/lib/libubsm_sdk.so}
ubsm_library_dir=$(dirname -- "$ubsm_library")

if [ ! -f "$root/third_party/DeathStarBench/hotelReservation/services/geo/proto/go.mod" ]; then
	echo "missing DeathStarBench Geo dependency; run scripts/fetch-deps.sh first" >&2
	exit 1
fi
if [ ! -f "$output/liblrpc.a" ]; then
	echo "missing $output/liblrpc.a; run scripts/build-ub.sh first" >&2
	exit 1
fi
mkdir -p "$output"
(cd "$root/demo/deathstar-grpc" && \
	CGO_ENABLED=1 \
	CGO_CFLAGS="${CGO_CFLAGS:-} -I$ubsm_include" \
	CGO_LDFLAGS="${CGO_LDFLAGS:-} -L$output -L$ubsm_library_dir -Wl,-rpath,$ubsm_library_dir" \
	go build -tags ubsm -o "$output/deathstar-grpc-ub-lrpc" .)
echo "UB gRPC binary: $output/deathstar-grpc-ub-lrpc"
