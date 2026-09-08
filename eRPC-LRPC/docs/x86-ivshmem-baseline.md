# x86/ivshmem baseline before the UBS Memory backend

The UBS Memory work is additive. `LRPC_BACKEND=ivshmem` is the default, uses
the existing `kernel/ub_lrpc.c`, and builds the same publisher, shadow, nested,
eRPC compatibility, BAR latency, upstream eRPC, and DeathStarBench programs.

The last reported KVM run before the backend split completed with:

```text
DEATHSTAR_GRPC_LRPC_PASS
LRPC_NESTED_PASS calls=1000 switches=4000 depth=2 avg_ns=6206
WX_POLICY_PASS
CALLER_ISOLATION_PASS
ERPC_LRPC_PASS
UPSTREAM_ERPC_LRPC_LATENCY samples=1000 warmup=100 min_ns=3362 p50_ns=3458 p99_ns=3997 max_ns=12166 avg_ns=3538.5
UPSTREAM_ERPC_LRPC_PASS
QEMU_E2E_PASS
```

This record is a pre-change observation, not a rerun from the Windows source
workspace. After copying the updated tree to the Linux KVM server, rerun:

```sh
cmake -S . -B build -G Ninja -DLRPC_BACKEND=ivshmem
cmake --build build
sh scripts/build-kernel-module.sh
ACCEL=kvm sh scripts/run-qemu.sh
```
