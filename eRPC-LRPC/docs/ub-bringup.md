# Real-UB minimal LRPC demo

This backend keeps the existing x86 ivshmem prototype intact. The real-UB
variant runs the caller and shadow on the importing AArch64 host, stores their
shared A-stack in local memory owned by `ub_lrpc_ctl.ko`, executes a locally
compiled AArch64 handler, and lets only the shadow map the remote UBS Memory
object.

## Prerequisites

On both UB hosts, first verify the existing `tests/ubs-mem-two-node`
owner/remote program. The minimal LRPC demo assumes that `ubsmd`, UBS Engine,
OBMM, the SDK headers, and `libubsm_sdk.so` are already operational. The shared
object uses:

```text
UBSM_FLAG_ONLY_IMPORT_NONCACHE | UBSM_FLAG_WR_DELAY_COMP
```

The provider mapping is cacheable and the importing shadow mapping is
non-cacheable. The public SDK is only asked for one whole-object mapping at
offset zero and is never asked for `PROT_EXEC`.

## Build on the AArch64 UB hosts

```sh
cd eRPC-LRPC
sh scripts/build-ub.sh
sh scripts/build-ub-kernel-module.sh
```

Override SDK paths when necessary:

```sh
UBSM_INCLUDE_DIR=/path/to/include \
UBSM_LIBRARY=/path/to/libubsm_sdk.so \
sh scripts/build-ub.sh
```

The kernel module must be built against the exact running kernel:

```sh
KERNEL_BUILD=/lib/modules/$(uname -r)/build \
sh scripts/build-ub-kernel-module.sh
```

## Run

Choose a UBS Memory object name of at most 48 bytes. The examples use
`lrpc_ub_demo`. On provider/owner host B:

```sh
./build-ub/lrpc-ub-owner "$(hostname)" lrpc_ub_demo
```

On importing/caller host A, load the local handoff module and start the shadow:

```sh
sudo insmod kernel/ub/ub_lrpc_ctl.ko
ls -l /dev/ub_lrpc_ctl
LRPC_SHADOW_CPU=1 ./build-ub/lrpc-ub-shadow \
    lrpc_ub_demo /dev/ub_lrpc_ctl
```

In another terminal on host A, invoke the service:

```sh
./build-ub/lrpc-ub-client /dev/ub_lrpc_ctl
```

Expected result:

```text
UB_LRPC_RESULT value=142 ...
UB_LRPC_PASS
```

Stop the shadow first with `Ctrl-C`, then stop the owner. The owner performs
the final unmap and deallocation. If a process crashes, use the existing
`ubsm_shm_admin` helper to inspect or remove the exact object name.

## Current validation boundary

The backend now also contains the phase 6-8 multi-procedure, nested-call, eRPC,
and gRPC-Go demos. Their complete per-program commands are in `README.md`.
They still do not execute code from UBS Memory, use a remote A-stack, provide
concurrent callers per procedure, or automate cross-host readiness. The owner
must be ready before an importing shadow starts. The x86 ivshmem/QEMU build
remains available with `-DLRPC_BACKEND=ivshmem` and continues to use the
original PCI driver and demos.
