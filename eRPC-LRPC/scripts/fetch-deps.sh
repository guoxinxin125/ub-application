#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fetch_linux=${LRPC_FETCH_LINUX:-1}
mkdir -p "$root/third_party"

if [ ! -d "$root/third_party/eRPC" ]; then
	git clone --filter=blob:none --no-checkout https://github.com/erpc-io/eRPC.git \
		"$root/third_party/eRPC"
	git -C "$root/third_party/eRPC" fetch --depth 1 origin \
		de83dab3eab4a0fb19bfc4881c11d4a6b89ff17d
	git -C "$root/third_party/eRPC" checkout --detach FETCH_HEAD
fi
if git -C "$root/third_party/eRPC" apply --check "$root/patches/erpc-lrpc.patch" 2>/dev/null; then
	git -C "$root/third_party/eRPC" apply "$root/patches/erpc-lrpc.patch"
elif ! git -C "$root/third_party/eRPC" apply --reverse --check \
	"$root/patches/erpc-lrpc.patch" 2>/dev/null; then
	echo "eRPC tree is neither clean nor patched as expected" >&2
	exit 1
fi
if git -C "$root/third_party/eRPC" apply --check \
	"$root/patches/erpc-lrpc-ub-config.patch" 2>/dev/null; then
	git -C "$root/third_party/eRPC" apply \
		"$root/patches/erpc-lrpc-ub-config.patch"
elif ! git -C "$root/third_party/eRPC" apply --reverse --check \
	"$root/patches/erpc-lrpc-ub-config.patch" 2>/dev/null; then
	echo "eRPC tree has an unexpected UB configuration patch state" >&2
	exit 1
fi
if git -C "$root/third_party/eRPC" apply --check \
	"$root/patches/erpc-aarch64-util.patch" 2>/dev/null; then
	git -C "$root/third_party/eRPC" apply \
		"$root/patches/erpc-aarch64-util.patch"
elif ! git -C "$root/third_party/eRPC" apply --reverse --check \
	"$root/patches/erpc-aarch64-util.patch" 2>/dev/null; then
	echo "eRPC tree has an unexpected AArch64 utility patch state" >&2
	exit 1
fi
if git -C "$root/third_party/eRPC" apply --check \
	"$root/patches/erpc-aarch64-math.patch" 2>/dev/null; then
	git -C "$root/third_party/eRPC" apply \
		"$root/patches/erpc-aarch64-math.patch"
elif ! git -C "$root/third_party/eRPC" apply --reverse --check \
	"$root/patches/erpc-aarch64-math.patch" 2>/dev/null; then
	echo "eRPC tree has an unexpected AArch64 math patch state" >&2
	exit 1
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
