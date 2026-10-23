#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if ! python3 --version >/dev/null 2>&1 && command -v python >/dev/null 2>&1; then
  python3() { python "$@"; }
fi
export SOCIAL_ENV_LOCAL_FILE="$root/config/social-env.example.sh"
config="$root/config/social-user-read-a.json"

source "$root/scripts/social-env.sh" --mode domain --workload user --profile \
  --warmup 1 --run-id test --config "$config"
[[ "$SOCIAL_HOST_SIDE" == A && "$ERPC_SN_DOMAIN_PROFILE" == 1 ]]
[[ "$ERPC_SN_PROFILE" == 1 && "$ERPC_SN_DOMAIN_WARMUP" == 1 ]]
[[ "$SN_CONFIG" == "$config" && "$SN_LOG_DIR" == *social-user-domain-profile-test-a ]]
[[ "$SN_BUILD_DIR" == */eRPC/cmake-build-social-ub-domain ]]

source "$root/scripts/social-env.sh" --mode rpc --workload user \
  --run-id test-rpc --config "$config"
[[ "$ERPC_SN_DOMAIN_PROFILE" == 0 && "$ERPC_SN_PROFILE" == 0 ]]
[[ "$SN_BUILD_DIR" == */eRPC/cmake-build-social-ub-rpc ]]

if ( source "$root/scripts/social-env.sh" --mode rpc --workload user \
  --warmup 10 --config "$config" ) >/dev/null 2>&1; then
  echo 'Expected warmup >= generate_num to fail' >&2
  exit 1
fi
echo SOCIAL_ENV_TEST_PASS
