#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel=${KDIR:-/lib/modules/$(uname -r)/build}
if [ "$(uname -m)" != aarch64 ]; then
    echo "Build this module on the ARM64 test host with matching Linux 6.6 headers." >&2
    exit 1
fi
make -C "$kernel" M="$root/kernel/ub-domain" modules
