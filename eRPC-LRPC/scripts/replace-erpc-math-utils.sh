#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_file=${ERPC_MATH_UTILS_SOURCE:-"$root/../eRPC/src/util/math_utils.h"}
target_file="$root/third_party/eRPC/src/util/math_utils.h"

if [ ! -f "$source_file" ]; then
	echo "missing adapted math_utils.h: $source_file" >&2
	exit 1
fi

if [ ! -f "$target_file" ]; then
	echo "missing third-party eRPC target: $target_file" >&2
	echo "run scripts/fetch-deps.sh first" >&2
	exit 1
fi

if ! grep -q '__builtin_clz' "$source_file"; then
	echo "source is not the expected portable math_utils.h: __builtin_clz missing" >&2
	exit 1
fi

if grep -q 'bsrl' "$source_file"; then
	echo "source still contains the x86-only bsrl instruction" >&2
	exit 1
fi

cp "$source_file" "$target_file"
chmod 644 "$target_file"

echo "replaced: $target_file"
echo "source:   $source_file"
echo "ERPC_AARCH64_MATH_UTILS_READY"
