#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cmake -S "$root" -B "$root/build" -G Ninja
cmake --build "$root/build"
