# eRPC-LRPC: executable ivshmem LRPC prototype

The tree now has two selectable backends. `ivshmem` remains the default and
preserves the original x86/QEMU prototype. `ubsm` adds the real-UB AArch64
path: multi-procedure metadata, nested calls, eRPC compatibility, and a
gRPC-Go/DeathStarBench Geo demo. The caller/shadow handoff and A-stack stay on
the importing host; service data is owned by the remote UB host and imported
only by shadows. See the real-UB commands below and `docs/ub-bringup.md`.

```sh
# Existing x86/QEMU build
cmake -S . -B build -G Ninja -DLRPC_BACKEND=ivshmem

# Real UB build (normally on an AArch64 UB host)
sh scripts/build-ub.sh
```

This directory is a Linux/QEMU proof of concept for the mechanism described in
`UB跨机LRPC专利交底-Typst.pdf`.  Guest B publishes a position-independent RX
service image and exported service data into an ivshmem BAR. In guest A, an
independent passive shadow process registers with the kernel driver and maps
the code RX, service data RW, shared A-stack, and a private E-stack. The caller
maps only the A-stack. `CALL` blocks the caller and wakes the CPU-pinned shadow;
the normal Linux scheduler switches to the shadow's distinct `mm_struct`/CR3.
`RETURN` blocks the shadow again and wakes the caller. There is no request
queue and no guest-B CPU on the invocation datapath.

The Linux driver enforces role-based mappings, W^X, distinct caller/shadow
address spaces, same-CPU handoff, and procedure validation. Up to 16 published
services have independent shadow registrations and 64 KiB A-stack slots, so a
shadow may synchronously invoke another service without overwriting its outer
call frame. The pinned upstream eRPC tree is built with an LRPC
datapath: ordinary `Rpc::enqueue_request` invokes `lrpc_invoke()` locally and
feeds a standard response completion back to eRPC. UDP remains only for eRPC's
session-management handshake.

The framework-neutral `lrpc_invoke_bytes()` API carries serialized messages up
to 60 KiB. `adapters/grpc-go` implements the unary
`grpc.ClientConnInterface`, allowing generated protobuf clients to use LRPC.

## Security and emulation boundary

QEMU `ivshmem-plain` is a functional stand-in for a UB coherent memory window;
it does not model UB latency, cache invalidation, poison, or fabric faults.  The
prototype uses a separate Linux process as a ChCore-style passive shadow
thread, so caller and service execution have distinct `mm_struct`s. This is a
scheduler-mediated handoff rather than a new in-kernel architecture-specific
context-switch primitive. Arbitrary normal C/C++ handlers cannot be
copied as code blobs because relocations and process-local pointers would be
invalid; published handlers must use the fixed PIC ABI.

## Build

```sh
./scripts/fetch-deps.sh       # only needed in a fresh checkout
./scripts/build.sh
./scripts/build-kernel-module.sh
./scripts/build-vm.sh
ACCEL=kvm ./scripts/run-qemu.sh  # hardware virtualization (recommended)
# ./scripts/run-qemu.sh          # TCG fallback
```

The QEMU scripts build a small Linux 6.6 LTS kernel/initramfs, attach one 4 MiB
ivshmem backing file to both guests, and check separate caller/shadow PIDs,
denied caller code/data mappings, a result that consumes exported B data, and
equal CPU IDs before/during/after the service call. See `docs/design.md` for the ABI
and remaining work toward a production eRPC backend.

The caller guest also runs a dependent-load microbenchmark over its 64 KiB
BAR2 A-stack mapping. The default remains the original noncached mapping. Use
separate runs to compare guest page attributes:

```sh
LRPC_NET_PORT=22345 LRPC_CACHE_MODE=noncached ACCEL=kvm ./scripts/run-qemu.sh
LRPC_NET_PORT=22346 LRPC_CACHE_MODE=cached ACCEL=kvm ./scripts/run-qemu.sh
```

To approximate `UBSM_FLAG_ONLY_IMPORT_NONCACHE`, keep the publisher mapping
cached and the importing caller/shadow guest noncached:

```sh
LRPC_NET_PORT=22347 LRPC_PUBLISHER_CACHE_MODE=cached \
LRPC_CALLER_CACHE_MODE=noncached ACCEL=kvm ./scripts/run-qemu.sh
```

`BAR_LOAD_LATENCY` reports 21 batches of 100,000 serialized loads through a
random cache-line pointer chain, so hardware prefetch and memory-level
parallelism cannot hide the measured dependency latency. `BAR_PAT` lines are
read from x86 debugfs when available. These measurements validate the guest
mapping policy and compare local KVM/ivshmem modes; ivshmem does not model UB
fabric latency or coherence behavior.

The same run also executes 1000 nested `root -> middle -> leaf` requests. This
validates 4000 directed address-space switches and checks that all three
processes have distinct PIDs while remaining on one CPU.

It also builds the original DeathStarBench Hotel Reservation Geo protobuf and
generated gRPC-Go client. Guest A registers a Go shadow for procedure 4 and
invokes `GeoClient.Nearby()` through `grpc.ClientConnInterface` without a TCP
data path. A KVM run on 2026-09-01 produced:

```text
DEATHSTAR_GRPC_LRPC_RESULT method=/geo.Geo/Nearby hotels=[1 2 3]
DEATHSTAR_GRPC_LRPC_PASS
LRPC_NESTED_PASS calls=1000 switches=4000 depth=2 avg_ns=20401
UPSTREAM_ERPC_LRPC_LATENCY samples=1000 warmup=100 min_ns=3933 p50_ns=4393 p99_ns=4893 max_ns=31879 avg_ns=4429.7
QEMU_E2E_PASS
```

The QEMU Geo handler uses an in-memory response. The UB-tagged build additionally
imports and validates procedure 4 service state from UBS Memory. Both remain
single-method feasibility demos, not the full Hotel Reservation deployment with
Consul, MongoDB, and Memcached.

## UB AArch64：阶段 6-8 编译与运行

下面把 UB 真机称为：B 是共享内存 owner/provider，A 是执行 caller 和
shadow 的 importing host。两台机器必须先跑通
`tests/ubs-mem-two-node`，且 `ubsmd`、UBS Engine、OBMM 与动态库均正常。
当前共享对象固定使用
`UBSM_FLAG_ONLY_IMPORT_NONCACHE | UBSM_FLAG_WR_DELAY_COMP`：B 端映射可缓存，
A 端导入映射为 non-cache。UB 共享区只保存元数据和 service data；A-stack、
E-stack 与 AArch64 代码都在 A 本地，不请求 `PROT_EXEC` 的 UB 映射。

### 1. 编译全部 UB 程序

两台 AArch64 主机使用相同源码。基础 C/C++ 程序和本机控制模块这样编译：

```sh
cd ~/ub-application/eRPC-LRPC

export UBSM_INCLUDE_DIR=/usr/local/ubs_mem/include
export UBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:${LD_LIBRARY_PATH:-}

sh scripts/build-ub.sh
sh scripts/build-ub-kernel-module.sh
```

`build-ub.sh` 生成以下程序：

```text
build-ub/lrpc-ub-owner
build-ub/lrpc-ub-shadow
build-ub/lrpc-ub-client
build-ub/lrpc-ub-nested-shadow
build-ub/lrpc-ub-nested-client
build-ub/erpc-ub-lrpc-client
```

内核头文件不在默认位置时，显式指定正在运行的内核构建目录：

```sh
KERNEL_BUILD=/lib/modules/$(uname -r)/build \
    sh scripts/build-ub-kernel-module.sh
```

阶段 7/8 的第三方依赖只需获取一次。`fetch-deps.sh` 需要访问 GitHub、
kernel.org 和 Go module 源；若 UB 机器不能联网，可在联网机器准备好
`third_party/` 与 Go module cache 后再复制过去。

```sh
sh scripts/fetch-deps.sh
sh scripts/build-ub-upstream-erpc.sh
sh scripts/build-ub-grpc.sh
```

若 SDK 不在默认路径，后两个脚本沿用上面的 `UBSM_INCLUDE_DIR`、
`UBSM_LIBRARY` 和 `LRPC_UB_BUILD_DIR`。检查架构和产物：

```sh
uname -m
file build-ub/lrpc-ub-client \
     build-ub/upstream-erpc-ub-client \
     build-ub/deathstar-grpc-ub-lrpc
```

预期均为 AArch64 ELF。不要把 x86 QEMU 的 `build/` 产物复制到 UB 真机。

### 2. 公共启动与清理顺序

B 端先创建名为 `lrpc_ub_demo` 的 4 MiB UBS Memory 对象，并发布 epoch 1、
procedure 1/2/3/4：

```sh
cd ~/ub-application/eRPC-LRPC
./build-ub/lrpc-ub-owner "$(hostname)" lrpc_ub_demo
```

A 端加载仅负责本机 caller/shadow handoff 和本地 A-stack 的模块：

```sh
cd ~/ub-application/eRPC-LRPC
sudo insmod kernel/ub/ub_lrpc_ctl.ko
ls -l /dev/ub_lrpc_ctl
```

每个 shadow 默认绑定 CPU 1；也可在所有相关命令前统一设置
`LRPC_SHADOW_CPU=N`。caller 在 bind 时会自动绑到相同 CPU。结束时必须先停
A 上所有 shadow，使其 unmap；再在 B 上按 `Ctrl-C`，让 owner unmap 并
deallocate；最后在 A 上执行：

```sh
sudo rmmod ub_lrpc_ctl
```

### 3. 测试一：最小 UB LRPC（procedure 1）

B 上保持 owner 运行。A 的终端 1 启动 shadow，它会导入 procedure 1 的
远端 service data：

```sh
LRPC_SHADOW_CPU=1 ./build-ub/lrpc-ub-shadow \
    lrpc_ub_demo /dev/ub_lrpc_ctl
```

A 的终端 2 运行 caller：

```sh
./build-ub/lrpc-ub-client /dev/ub_lrpc_ctl
```

请求参数是 `20, 22`，shadow 从 B 的 non-cache 导入映射读取 `100`，结果应为
`100 + 20 + 22 = 142`：

```text
UB_LRPC_RESULT value=142 ...
UB_LRPC_PASS
```

### 4. 测试二：多 procedure 与级联调用（procedure 2 -> 3）

B 上仍只需同一个 owner。A 上按 leaf、middle、client 的顺序开三个终端：

```sh
# A terminal 1: leaf，绑定 procedure 3，并读取它的远端数据槽
LRPC_SHADOW_CPU=1 ./build-ub/lrpc-ub-nested-shadow \
    leaf lrpc_ub_demo /dev/ub_lrpc_ctl

# A terminal 2: middle，先 bind procedure 3，再注册为 procedure 2 shadow
LRPC_SHADOW_CPU=1 ./build-ub/lrpc-ub-nested-shadow \
    middle lrpc_ub_demo /dev/ub_lrpc_ctl

# A terminal 3: root caller
./build-ub/lrpc-ub-nested-client /dev/ub_lrpc_ctl
```

输入 41 经过 leaf 和 middle 各加一得到 43；1000 次测量包含每次
caller->middle、middle->leaf、leaf->middle、middle->caller 共四次 handoff：

```text
UB_LRPC_NESTED_RESULT value=43 ...
UB_LRPC_NESTED_PASS calls=1000 switches=4000 depth=2 avg_ns=...
```

### 5. 测试三：轻量 eRPC 兼容 API（procedure 1）

这个测试验证 `erpc::Rpc::enqueue_request()` 形状的接口能够落到 LRPC，尚不
需要 eRPC 的网络 management plane。先按“测试一”启动 procedure 1 shadow，
再在 A 运行：

```sh
./build-ub/erpc-ub-lrpc-client /dev/ub_lrpc_ctl
```

预期：

```text
ERPC_UB_LRPC_RESULT value=142 ...
ERPC_UB_LRPC_PASS
UB_LRPC_BREAKDOWN_AVG samples=1000 ...
```

### 6. 应用一：上游 eRPC hello_world + LRPC datapath

该版本保留 eRPC UDP session-management，但请求数据面由 A 本地的 LRPC
完成。先按“测试一”启动 B 的 owner 和 A 的 procedure 1 shadow。设 A、B
可互通的普通网络 IP 分别为 `A_IP`、`B_IP`。

B 上启动 eRPC 管理面 server：

```sh
ERPC_SERVER_HOST=B_IP ERPC_CLIENT_HOST=A_IP \
    ./build-ub/upstream-erpc-ub-server
```

A 上启动 client，并明确选择 LRPC caller 与真机控制设备：

```sh
ERPC_SERVER_HOST=B_IP ERPC_CLIENT_HOST=A_IP \
ERPC_LRPC_ROLE=caller ERPC_LRPC_DEVICE=/dev/ub_lrpc_ctl \
    ./build-ub/upstream-erpc-ub-client
```

预期 client 输出：

```text
UPSTREAM_ERPC_LRPC_RESULT value=142
UPSTREAM_ERPC_LRPC_LATENCY samples=1000 ...
UPSTREAM_ERPC_LRPC_PASS
```

B 的 server 不应打印 `ERROR_REMOTE_CPU_HANDLER_RAN`；若出现，说明请求走了
远端 CPU handler，而不是预期的本地 LRPC 数据面。

### 7. 应用二：DeathStarBench Geo gRPC-Go（procedure 4）

这个应用使用 DeathStarBench 原始 Geo protobuf/generated client，以及
`grpc.ClientConnInterface` 到 LRPC 的适配器。UB 构建带 `ubsm` tag，Geo
shadow 启动时会导入 procedure 4 的远端数据槽，并在每次请求中检查值 100。
它仍是 Geo 单方法可行性 demo，不会启动完整的 Consul/MongoDB/Memcached。

A 的终端 1：

```sh
LRPC_CONTROL_DEVICE=/dev/ub_lrpc_ctl \
LRPC_UBSM_NAME=lrpc_ub_demo \
LRPC_SHADOW_CPU=1 \
    ./build-ub/deathstar-grpc-ub-lrpc shadow
```

A 的终端 2：

```sh
LRPC_CONTROL_DEVICE=/dev/ub_lrpc_ctl \
    ./build-ub/deathstar-grpc-ub-lrpc
```

预期：

```text
DEATHSTAR_GRPC_LRPC_RESULT method=/geo.Geo/Nearby hotels=[1 2 3]
DEATHSTAR_GRPC_LRPC_PASS
```

### 8. 当前验证边界

这些 UB 程序已把 AArch64 汇编选择、UBSM 创建/导入、non-cache flag、
多 procedure 元数据、epoch 检查、级联 handoff、eRPC API 和 gRPC-Go demo
接到同一代码树中；`LRPC_BACKEND=ivshmem` 的 x86/QEMU 路径保持独立。
但源码静态检查或 x86 配置成功不能替代真机结论：最终仍需在两台 AArch64
UB 主机上完成编译、模块加载、功能输出、远端 load latency 与端到端延迟测试。
