#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
linux="$root/third_party/linux"
rootfs="$root/vm/rootfs"
jobs=${JOBS:-32}

if [ ! -f "$root/third_party/DeathStarBench/hotelReservation/services/geo/proto/geo.pb.go" ]; then
	echo "DeathStarBench Geo protobuf sources are missing; run scripts/fetch-deps.sh" >&2
	exit 1
fi
cp "$root/demo/deathstar-grpc/geo-proto.go.mod" \
	"$root/third_party/DeathStarBench/hotelReservation/services/geo/proto/go.mod"

if [ ! -f "$linux/.config" ]; then
	"$root/scripts/configure-linux.sh"
fi

make -C "$linux" -j"$jobs" bzImage modules
make -C "$linux" M="$root/kernel" modules

cmake -S "$root/third_party/eRPC" -B "$root/build/upstream-erpc-vm" \
	-DTRANSPORT=fake -DPERF=ON -DLOG_LEVEL=warn
cmake --build "$root/build/upstream-erpc-vm" \
	--target hello_server hello_client -j"$jobs"

mkdir -p "$root/build/vm" "$rootfs/bin" "$rootfs/dev" "$rootfs/proc" "$rootfs/sys"
cc -static -O2 -Wall -Wextra -Werror \
	-I"$root/include" -I"$root/include/uapi" \
	-c "$root/lib/lrpc.c" -o "$root/build/vm/lrpc.o"
cc -static -O2 -Wall -Wextra -Werror \
	-c "$root/lib/switch_stack.S" -o "$root/build/vm/switch_stack.o"
cc -static -O2 -Wall -Wextra -Werror \
	-I"$root/include" -I"$root/include/uapi" -I"$root/demo" \
	"$root/demo/publisher.c" "$root/build/vm/lrpc.o" \
	"$root/build/vm/switch_stack.o" -o "$rootfs/lrpc-publisher"
cc -static -O2 -Wall -Wextra -Werror \
	-I"$root/include" -I"$root/include/uapi" \
	"$root/demo/shadow.c" "$root/build/vm/lrpc.o" \
	"$root/build/vm/switch_stack.o" -o "$rootfs/lrpc-shadow"
cc -static -O2 -Wall -Wextra -Werror \
	-I"$root/include" -I"$root/include/uapi" \
	"$root/demo/nested_shadow.c" "$root/build/vm/lrpc.o" \
	"$root/build/vm/switch_stack.o" -o "$rootfs/lrpc-nested-shadow"
cc -static -O2 -Wall -Wextra -Werror \
	-I"$root/include" -I"$root/include/uapi" \
	"$root/demo/nested_client.c" "$root/build/vm/lrpc.o" \
	"$root/build/vm/switch_stack.o" -o "$rootfs/lrpc-nested-client"
c++ -static -O2 -Wall -Wextra -Werror -std=c++17 \
	-I"$root/include" -I"$root/include/uapi" -I"$root/demo" \
	"$root/demo/erpc_client.cc" "$root/build/vm/lrpc.o" \
	"$root/build/vm/switch_stack.o" -o "$rootfs/erpc-lrpc-client"
cc -static -O2 -Wall -Wextra -Werror \
	-I"$root/include" -I"$root/include/uapi" \
	"$root/demo/bar_latency.c" "$root/build/vm/lrpc.o" \
	"$root/build/vm/switch_stack.o" -o "$rootfs/lrpc-bar-latency"
# bridge.c includes lib/lrpc.c and the UAPI header from outside this Go
# package. Force cgo to rebuild so ioctl structure-size changes cannot leave
# a stale UB_LRPC_IOC_INFO command encoded in the guest binary.
(cd "$root/demo/deathstar-grpc" && \
	CGO_ENABLED=1 go build -a -o "$rootfs/deathstar-grpc-lrpc" .)

busybox=$(command -v busybox || true)
if [ -z "$busybox" ]; then
	echo "busybox is required to build the VM rootfs" >&2
	exit 1
fi
cp "$busybox" "$rootfs/bin/busybox"
cp "$root/kernel/ub_lrpc.ko" "$rootfs/ub_lrpc.ko"
cp "$root/third_party/eRPC/build/hello_server" "$rootfs/upstream-erpc-server"
cp "$root/third_party/eRPC/build/hello_client" "$rootfs/upstream-erpc-client"
mkdir -p "$rootfs/lib64" "$rootfs/mnt/huge" "$rootfs/tmp"
loader=$(cc -print-file-name=ld-linux-x86-64.so.2)
if [ "$loader" = ld-linux-x86-64.so.2 ] || [ ! -f "$loader" ]; then
	loader=/lib64/ld-linux-x86-64.so.2
fi
cp "$loader" "$rootfs/lib64/ld-linux-x86-64.so.2"
for lib in libnuma.so.1 libstdc++.so.6 libgcc_s.so.1 libc.so.6 libm.so.6 \
	libresolv.so.2 libz.so.1; do
	lib_path=$(cc -print-file-name="$lib")
	if [ "$lib_path" = "$lib" ] || [ ! -f "$lib_path" ]; then
		lib_path=$(ldconfig -p | awk -v name="$lib" '$1 == name { print $NF; exit }')
	fi
	if [ -z "$lib_path" ] || [ ! -f "$lib_path" ]; then
		echo "required runtime library not found: $lib" >&2
		exit 1
	fi
	cp "$lib_path" "$rootfs/lib64/$lib"
done
cp "$root/vm/init" "$rootfs/init"
chmod 755 "$rootfs/init" "$rootfs/lrpc-publisher" "$rootfs/lrpc-shadow" \
	"$rootfs/lrpc-nested-shadow" "$rootfs/lrpc-nested-client" \
	"$rootfs/erpc-lrpc-client" "$rootfs/lrpc-bar-latency" \
	"$rootfs/deathstar-grpc-lrpc"
for applet in sh mount insmod cat sleep poweroff ip mkdir env; do
	ln -sf busybox "$rootfs/bin/$applet"
done

(cd "$rootfs" && find . -print0 | cpio --null -o --format=newc 2>/dev/null) |
	gzip -9 > "$root/vm/initramfs.cpio.gz"
cp "$linux/arch/x86/boot/bzImage" "$root/vm/bzImage"
echo "VM artifacts: $root/vm/bzImage $root/vm/initramfs.cpio.gz"
