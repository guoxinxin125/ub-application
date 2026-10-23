# Copy to config/social-env.local.sh separately on A and B, then edit once.
# This file contains paths and host identity only; never put passwords here.
SOCIAL_HOST_SIDE=A                 # A or B
ERPC_UB_MACHINE_ID=1               # Unique nonzero ID on each host
ERPC_UB_PROVIDER_NUMA=0            # A=0, B=1 in the current A0/B1 pairing
ERPC_UB_NUMA_NODE=0                # Match the actual host's selected NUMA node
LRPC_UB_DOMAIN_NAME=lrpc_social_my_lab  # Must be identical on A and B
ERPC_UB_REGION_PREFIX=sn_lrpc_my_lab    # Private prefix; same on A and B

UBSM_INCLUDE_DIR=/actual/sdk/include
UBSM_LIBRARY=/actual/sdk/lib/libubsm_sdk.so
# Only A/domain builds need these; B may leave them empty.
OBMM_INCLUDE_DIR=/actual/obmm/include
OBMM_LIBRARY=/usr/lib64/libobmm.so

# Optional per-host overrides:
# LRPC_UB_PROVIDER_NUMA=1
# SOCIAL_WARMUP=0
# SOCIAL_REGION_MB=2048
# SOCIAL_ARENA_MB=64
# SOCIAL_NESTED_PATH=fused
# SOCIAL_MANAGER_SOCKET=/tmp/sn_lrpc_my_lab_a.sock  # Optional host-local override
# export LD_LIBRARY_PATH=/actual/sdk/lib:${LD_LIBRARY_PATH:-}
