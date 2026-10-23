#!/usr/bin/env bash
# Source this file in the shell that will build/run SocialNetwork on one UB host.
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  echo 'Use: source scripts/social-env.sh --mode rpc|domain --workload user|home [--profile] [--warmup N] [--run-id ID]' >&2
  exit 2
fi

_social_env_setup() {
  local script=$1 mode= workload= profile=0 warmup= run_id= config= arg total
  local root local_file side side_lower run_kind
  shift
  while (($#)); do
    arg=$1
    case "$arg" in
      --mode|--workload|--warmup|--run-id|--config)
        if (($# < 2)); then echo "Missing value for $arg" >&2; return 2; fi
        case "$arg" in
          --mode) mode=$2 ;; --workload) workload=$2 ;; --warmup) warmup=$2 ;;
          --run-id) run_id=$2 ;; --config) config=$2 ;;
        esac
        shift 2 ;;
      --profile) profile=1; shift ;;
      *) echo "Unknown option: $arg" >&2; return 2 ;;
    esac
  done
  [[ "$mode" == rpc || "$mode" == domain ]] || { echo 'Set --mode rpc|domain.' >&2; return 2; }
  [[ "$workload" == user || "$workload" == home ]] || { echo 'Set --workload user|home.' >&2; return 2; }

  root=$(CDPATH= cd -- "$(dirname -- "$script")/.." && pwd) || return 1
  local_file=${SOCIAL_ENV_LOCAL_FILE:-$root/config/social-env.local.sh}
  if [[ ! -r "$local_file" ]]; then
    echo "Missing $local_file; copy config/social-env.example.sh and edit it once on this host." >&2
    return 1
  fi
  # This per-host file is deliberately not shipped in SOURCE_MANIFEST.sha256.
  source "$local_file" || return 1
  side=${SOCIAL_HOST_SIDE:-}
  [[ "$side" == A || "$side" == B ]] || { echo 'SOCIAL_HOST_SIDE must be A or B.' >&2; return 2; }
  [[ "${ERPC_UB_MACHINE_ID:-}" =~ ^[1-9][0-9]*$ ]] || { echo 'Set a nonzero ERPC_UB_MACHINE_ID in the local file.' >&2; return 2; }
  [[ "${ERPC_UB_PROVIDER_NUMA:-}" =~ ^[0-9]+$ && "${ERPC_UB_NUMA_NODE:-}" =~ ^[0-9]+$ ]] || {
    echo 'Set numeric ERPC_UB_PROVIDER_NUMA and ERPC_UB_NUMA_NODE in the local file.' >&2; return 2;
  }
  [[ -n "${UBSM_INCLUDE_DIR:-}" && -n "${UBSM_LIBRARY:-}" ]] || {
    echo 'Set UBSM_INCLUDE_DIR and UBSM_LIBRARY in the local file.' >&2; return 2;
  }
  [[ -n "${LRPC_UB_DOMAIN_NAME:-}" && -n "${ERPC_UB_REGION_PREFIX:-}" ]] || {
    echo 'Set a matching private LRPC_UB_DOMAIN_NAME and ERPC_UB_REGION_PREFIX on A/B.' >&2; return 2;
  }
  if [[ "$side" == A && "$mode" == domain && ( -z "${OBMM_INCLUDE_DIR:-}" || -z "${OBMM_LIBRARY:-}" ) ]]; then
    echo 'A/domain needs OBMM_INCLUDE_DIR and OBMM_LIBRARY in the local file.' >&2; return 2
  fi

  side_lower=${side,,}
  [[ -n "$config" ]] || config="$root/campaign-${workload}-read/config-${side_lower}.json"
  [[ "$config" == /* ]] || { echo "Config path must be absolute: $config" >&2; return 2; }
  [[ -n "$warmup" ]] || warmup=${SOCIAL_WARMUP:-0}
  [[ "$warmup" =~ ^[0-9]+$ ]] || { echo 'Warmup must be a nonnegative integer.' >&2; return 2; }
  warmup=$((10#$warmup))
  total=not-yet-generated
  if [[ -f "$config" ]]; then
    total=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["client"]["generate_num"])' "$config") || return 1
    [[ "$total" =~ ^[1-9][0-9]*$ ]] || { echo 'client.generate_num must be positive.' >&2; return 2; }
    if ((warmup >= total || total > 10000)); then
      echo "Invalid measurement: client.generate_num=$total must exceed warmup=$warmup and be <=10000." >&2
      return 2
    fi
  else
    echo "Config not generated yet: $config (build setup is available; generate config before running)." >&2
  fi
  [[ -n "$run_id" ]] || run_id=${SOCIAL_RUN_ID:-$(date +%Y%m%d-%H%M%S)}
  [[ "$run_id" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || { echo 'Invalid --run-id.' >&2; return 2; }

  export REPO="$(CDPATH= cd -- "$root/.." && pwd)" || return 1
  export SOCIAL_HOST_SIDE="$side" ERPC_UB_MACHINE_ID ERPC_UB_PROVIDER_NUMA ERPC_UB_NUMA_NODE
  export UBSM_INCLUDE_DIR UBSM_LIBRARY LRPC_UB_DOMAIN_NAME ERPC_UB_REGION_PREFIX
  if [[ -n "${OBMM_INCLUDE_DIR:-}" && -n "${OBMM_LIBRARY:-}" ]]; then
    export OBMM_INCLUDE_DIR OBMM_LIBRARY
  fi
  export LRPC_UB_PROVIDER_NUMA="${LRPC_UB_PROVIDER_NUMA:-$ERPC_UB_PROVIDER_NUMA}"
  export ERPC_UB_MANAGER_SOCKET="${SOCIAL_MANAGER_SOCKET:-/tmp/${ERPC_UB_REGION_PREFIX}_${ERPC_UB_MACHINE_ID}.sock}"
  export ERPC_UB_MEMORY_MODE=one-sided
  export ERPC_UB_REGION_MB="${SOCIAL_REGION_MB:-2048}" ERPC_UB_ARENA_MB="${SOCIAL_ARENA_MB:-64}"
  export LRPC_UB_NESTED_PATH="${SOCIAL_NESTED_PATH:-fused}"
  export ERPC_SN_DOMAIN_WARMUP="$warmup" ERPC_SN_DOMAIN_PROFILE="$profile" ERPC_SN_PROFILE="$profile"
  export SN_CONFIG="$config"
  if [[ "$side:$mode" == A:domain ]]; then
    export SN_BUILD_DIR="$REPO/eRPC/cmake-build-social-ub-domain"
  else
    export SN_BUILD_DIR="$REPO/eRPC/cmake-build-social-ub-rpc"
  fi
  run_kind=formal
  ((profile)) && run_kind=profile
  export SN_LOG_DIR="$root/results/social-${workload}-${mode}-${run_kind}-${run_id}-${side_lower}"
  printf 'SOCIAL_ENV_READY side=%s machine_id=%s numa=%s/%s workload=%s mode=%s profile=%s warmup=%s total=%s\n  config=%s\n  build=%s\n  logs=%s\n' \
    "$side" "$ERPC_UB_MACHINE_ID" "$ERPC_UB_NUMA_NODE" "$ERPC_UB_PROVIDER_NUMA" \
    "$workload" "$mode" "$profile" "$warmup" "$total" "$SN_CONFIG" "$SN_BUILD_DIR" "$SN_LOG_DIR"
}

if _social_env_setup "${BASH_SOURCE[0]}" "$@"; then
  unset -f _social_env_setup
  return 0
else
  unset -f _social_env_setup
  return 1
fi
