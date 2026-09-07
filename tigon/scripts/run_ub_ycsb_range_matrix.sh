#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
export UB_TIGON_QUERIES=${UB_TIGON_RANGE_QUERIES:-"scan mixed"}

exec "$script_dir/run_ub_ycsb_matrix.sh" "$@"
