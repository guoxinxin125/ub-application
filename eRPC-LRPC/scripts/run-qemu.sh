#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
qemu=${QEMU:-qemu-system-x86_64}
accel=${ACCEL:-tcg}
cpu_model=${CPU_MODEL:-}
net_port=${LRPC_NET_PORT:-12345}
cache_mode=${LRPC_CACHE_MODE:-noncached}
publisher_cache_mode=${LRPC_PUBLISHER_CACHE_MODE:-$cache_mode}
caller_cache_mode=${LRPC_CALLER_CACHE_MODE:-$cache_mode}
case "$publisher_cache_mode:$caller_cache_mode" in
	cached:cached|cached:noncached|noncached:cached|noncached:noncached) ;;
	*)
		echo "cache modes must be cached or noncached" >&2
		exit 1
		;;
esac
if [ -z "$cpu_model" ]; then
	if [ "$accel" = kvm ]; then
		cpu_model=host
	else
		cpu_model=max
	fi
fi
shared="$root/vm/shared-memory.bin"
publisher_log="$root/build/publisher-vm.log"
caller_log="$root/build/caller-vm.log"
publisher_pid=""
caller_pid=""

cleanup() {
	if [ -n "$publisher_pid" ]; then
		kill "$publisher_pid" 2>/dev/null || true
		wait "$publisher_pid" 2>/dev/null || true
	fi
	if [ -n "$caller_pid" ]; then
		kill "$caller_pid" 2>/dev/null || true
		wait "$caller_pid" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

if [ ! -f "$root/vm/bzImage" ] || [ ! -f "$root/vm/initramfs.cpio.gz" ]; then
	"$root/scripts/build-vm.sh"
fi
mkdir -p "$root/build"
truncate -s 4M "$shared"

common="-machine q35,accel=$accel -cpu $cpu_model -m 512M -smp 2 -kernel $root/vm/bzImage -initrd $root/vm/initramfs.cpio.gz -nographic -no-reboot -object memory-backend-file,id=lrpcmem,mem-path=$shared,size=4M,share=on -device ivshmem-plain,memdev=lrpcmem"
publisher_net="-netdev socket,id=lan,listen=127.0.0.1:$net_port -device e1000,netdev=lan"
caller_net="-netdev socket,id=lan,connect=127.0.0.1:$net_port -device e1000,netdev=lan"

# shellcheck disable=SC2086
$qemu $common $publisher_net -append "console=ttyS0 panic=-1 quiet hugepages=64 lrpc.role=publisher lrpc.cache=$publisher_cache_mode" >"$publisher_log" 2>&1 &
publisher_pid=$!

i=0
while [ "$i" -lt 200 ]; do
	if grep -q 'LRPC_PUBLISHER_IDLE' "$publisher_log"; then
		break
	fi
	if ! kill -0 "$publisher_pid" 2>/dev/null; then
		echo "Publisher VM exited early" >&2
		tail -80 "$publisher_log" >&2
		exit 1
	fi
	i=$((i + 1))
	sleep 0.05
done
if ! grep -q 'LRPC_PUBLISHER_IDLE' "$publisher_log"; then
	echo "Timed out waiting for publisher" >&2
	tail -80 "$publisher_log" >&2
	exit 1
fi

# shellcheck disable=SC2086
$qemu $common $caller_net -append "console=ttyS0 panic=-1 quiet hugepages=64 lrpc.role=caller lrpc.cache=$caller_cache_mode" >"$caller_log" 2>&1 &
caller_pid=$!

i=0
while [ "$i" -lt 1200 ]; do
	if grep -q 'UPSTREAM_ERPC_LRPC_PASS\|ERPC_LRPC_FAIL' "$caller_log"; then
		break
	fi
	if ! kill -0 "$caller_pid" 2>/dev/null; then
		break
	fi
	i=$((i + 1))
	sleep 0.05
done

grep 'LRPC_PUBLISHED\|LRPC_PUBLISHER_IDLE' "$publisher_log"
grep 'UB_LRPC_DEVICE_PATH\|UB_LRPC_PCI_ID\|UB_LRPC_DRIVER\|UB_LRPC_BAR2_RESOURCE\|LRPC_SHADOW_REGISTERED\|LRPC_CALLBACK_SHADOW_REGISTERED\|BAR_PAT\|BAR_LOAD_LATENCY\|DEATHSTAR_GRPC_SHADOW_START\|DEATHSTAR_GRPC_LRPC_RESULT\|DEATHSTAR_GRPC_LRPC_PASS\|LRPC_NESTED_RESULT\|LRPC_NESTED_PASS\|WX_POLICY_PASS\|CALLER_ISOLATION_PASS\|ERPC_LRPC_RESULT\|ERPC_LRPC_PASS\|LRPC_BREAKDOWN_AVG\|UPSTREAM_ERPC_LRPC_RESULT\|UPSTREAM_ERPC_LRPC_LATENCY\|UPSTREAM_ERPC_LRPC_PASS\|ERPC_LRPC_FAIL' "$caller_log"
grep -q 'BAR_LOAD_LATENCY_PASS' "$caller_log"
grep -q 'DEATHSTAR_GRPC_LRPC_PASS' "$caller_log"
grep -q 'LRPC_NESTED_PASS calls=1000 switches=4000 depth=2' "$caller_log"
grep -q 'LRPC_SHADOW_REGISTERED.*mm=separate' "$caller_log"
grep -q 'WX_POLICY_PASS' "$caller_log"
grep -q 'CALLER_ISOLATION_PASS' "$caller_log"
grep -q 'ERPC_LRPC_PASS' "$caller_log"
grep -q 'UPSTREAM_ERPC_LRPC_PASS' "$caller_log"
if grep -q 'ERROR_REMOTE_CPU_HANDLER_RAN' "$publisher_log"; then
	echo "B-side eRPC data handler ran unexpectedly" >&2
	exit 1
fi
echo "QEMU_E2E_PASS"
