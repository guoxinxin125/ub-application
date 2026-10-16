#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${KDIR:-/lib/modules/$(uname -r)/build}
build_vhe=0
build_trace=0
for option in "$@"; do
    case "$option" in
        --vhe) [ "$build_vhe" -eq 0 ] || exit 2; build_vhe=1 ;;
        --trace) [ "$build_trace" -eq 0 ] || exit 2; build_trace=1 ;;
        *) echo "usage: sh scripts/build-module.sh [--vhe] [--trace]" >&2; exit 2 ;;
    esac
done
if [ "$(uname -m)" != aarch64 ]; then
    echo "Build this module on the AArch64 caller host." >&2
    exit 1
fi
if [ ! -f "$kernel/Module.symvers" ]; then
    echo "Missing matching Module.symvers in $kernel." >&2
    exit 1
fi
for symbol in arm64_mm_context_get arm64_mm_context_put; do
    if ! awk -v name="$symbol" '$2 == name { found=1 } END { exit !found }' "$kernel/Module.symvers"; then
        echo "Kernel does not export $symbol; pinned ASID support needs a kernel port." >&2
        exit 1
    fi
done
make -C "$kernel" M="$root/kernel/ub-domain" \
    UD_BUILD_VHE="$build_vhe" UD_TRACE="$build_trace" modules
