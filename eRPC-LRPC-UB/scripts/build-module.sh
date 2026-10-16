#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${KDIR:-/lib/modules/$(uname -r)/build}
case "${1:-}" in
    '') [ "$#" -eq 0 ] || exit 2; build_vhe=0 ;;
    --vhe) [ "$#" -eq 1 ] || exit 2; build_vhe=1 ;;
    *) echo "usage: sh scripts/build-module.sh [--vhe]" >&2; exit 2 ;;
esac
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
make -C "$kernel" M="$root/kernel/ub-domain" UD_BUILD_VHE="$build_vhe" modules
