#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel_build=${KERNEL_BUILD:-/lib/modules/$(uname -r)/build}
make -C "$kernel_build" M="$root/kernel" modules
