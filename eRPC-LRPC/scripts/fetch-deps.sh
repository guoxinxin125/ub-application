#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fetch_linux=${LRPC_FETCH_LINUX:-1}
fetch_erpc=${LRPC_FETCH_ERPC:-1}
mkdir -p "$root/third_party"

if [ "$fetch_erpc" != 0 ]; then
	if [ ! -d "$root/third_party/eRPC" ]; then
		git clone --filter=blob:none --no-checkout https://github.com/erpc-io/eRPC.git \
			"$root/third_party/eRPC"
		git -C "$root/third_party/eRPC" fetch --depth 1 origin \
			de83dab3eab4a0fb19bfc4881c11d4a6b89ff17d
		git -C "$root/third_party/eRPC" checkout --detach FETCH_HEAD
	fi
	if git -C "$root/third_party/eRPC" apply --check "$root/patches/erpc-lrpc.patch" 2>/dev/null; then
		git -C "$root/third_party/eRPC" apply "$root/patches/erpc-lrpc.patch"
	elif grep -q 'UPSTREAM_ERPC_LRPC_RESULT value=142' \
		"$root/third_party/eRPC/hello_world/client.cc" &&
		grep -q '../../lib/lrpc.c' "$root/third_party/eRPC/CMakeLists.txt" &&
		grep -q 'lrpc_invoke(&lrpc_, &call)' \
			"$root/third_party/eRPC/src/transport_impl/fake/fake_transport.cc"; then
		: # Applied; later patches may prevent a reverse-check of this base patch.
	else
		echo "eRPC tree is neither clean nor patched as expected" >&2
		exit 1
	fi
	if git -C "$root/third_party/eRPC" apply --check \
		"$root/patches/erpc-lrpc-timing.patch" 2>/dev/null; then
		git -C "$root/third_party/eRPC" apply \
			"$root/patches/erpc-lrpc-timing.patch"
	elif grep -q 'UPSTREAM_ERPC_LRPC_BREAKDOWN_AVG' \
		"$root/third_party/eRPC/hello_world/client.cc" &&
		grep -q 'fake_transport_get_last_lrpc_timing' \
			"$root/third_party/eRPC/src/transport_impl/fake/fake_transport.cc"; then
		: # Applied.
	else
		echo "eRPC tree has an unexpected timing patch state" >&2
		exit 1
	fi
	if git -C "$root/third_party/eRPC" apply --check \
		"$root/patches/erpc-lrpc-precondition.patch" 2>/dev/null; then
		git -C "$root/third_party/eRPC" apply \
			"$root/patches/erpc-lrpc-precondition.patch"
	elif grep -q 'ra_bench_warm_begin(&warm, "upstream")' \
		"$root/third_party/eRPC/hello_world/client.cc" &&
		grep -q 'ra_bench_audit_end(&audit, "upstream", "baseline")' \
			"$root/third_party/eRPC/hello_world/client.cc"; then
		: # Applied.
	else
		echo "eRPC tree has an unexpected precondition patch state" >&2
		exit 1
	fi
	if git -C "$root/third_party/eRPC" apply --check \
		"$root/patches/erpc-lrpc-first-call.patch" 2>/dev/null; then
		git -C "$root/third_party/eRPC" apply \
			"$root/patches/erpc-lrpc-first-call.patch"
	elif grep -q 'ra_bench_first_call("upstream", first_ns)' \
		"$root/third_party/eRPC/hello_world/client.cc"; then
		: # Applied.
	else
		echo "eRPC tree has an unexpected first-call patch state" >&2
		exit 1
	fi
	if git -C "$root/third_party/eRPC" apply --check \
		"$root/patches/erpc-lrpc-ub-config.patch" 2>/dev/null; then
		git -C "$root/third_party/eRPC" apply \
			"$root/patches/erpc-lrpc-ub-config.patch"
	elif grep -q 'ERPC_LRPC_DEVICE' \
		"$root/third_party/eRPC/src/transport_impl/fake/fake_transport.cc" &&
		grep -q 'lrpc_env("ERPC_SERVER_HOST"' \
			"$root/third_party/eRPC/hello_world/common.h"; then
		: # Applied.
	else
		echo "eRPC tree has an unexpected UB configuration patch state" >&2
		exit 1
	fi
	if git -C "$root/third_party/eRPC" apply --check \
		"$root/patches/erpc-aarch64-util.patch" 2>/dev/null; then
		git -C "$root/third_party/eRPC" apply \
			"$root/patches/erpc-aarch64-util.patch"
	elif grep -q 'defined(__aarch64__)' \
		"$root/third_party/eRPC/src/util/barrier.h" &&
		grep -q 'cntvct_el0' "$root/third_party/eRPC/src/util/timer.h"; then
		: # Applied.
	else
		echo "eRPC tree has an unexpected AArch64 utility patch state" >&2
		exit 1
	fi
fi
if [ "$fetch_linux" != 0 ] && [ ! -d "$root/third_party/linux" ]; then
	tmp=${TMPDIR:-/tmp}/linux-6.6.155.tar.xz
	linux_url=${LRPC_LINUX_URL:-https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.155.tar.xz}
	curl -fsSL "$linux_url" -o "$tmp"
	tar -xJf "$tmp" -C "$root/third_party"
	mv "$root/third_party/linux-6.6.155" "$root/third_party/linux"
fi
if [ ! -d "$root/third_party/DeathStarBench" ]; then
	git clone --filter=blob:none --sparse --depth 1 \
		https://github.com/delimitrou/DeathStarBench.git \
		"$root/third_party/DeathStarBench"
	git -C "$root/third_party/DeathStarBench" sparse-checkout set \
		hotelReservation/services/geo/proto
fi
cp "$root/demo/deathstar-grpc/geo-proto.go.mod" \
	"$root/third_party/DeathStarBench/hotelReservation/services/geo/proto/go.mod"
if [ "$fetch_linux" != 0 ]; then
	sh "$root/scripts/configure-linux.sh"
fi
