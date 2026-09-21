# eRPC-LRPC: executable ivshmem LRPC prototype

UB 的 B-owned domain 路径已加入 B 页表/代码发布、ARM64 内核执行 gate、
Direct/Nested/Query 和 eRPC 调用接入。完整编译、预检查及各测试的运行命令见
[UB domain 运行说明](docs/ub-domain-runtime.md)；独立地址探针见
[PA 查询说明](docs/ub-remote-domain.md)。当前是实验性源码实现，尚未取得 UB
内核模块编译及双机执行结果。下文旧 UB shadow demo 仍使用本机调度器。

当前 **B-owned shadow / Linux remote-domain** 主线的中文设计、进展、实验边界与编程规范，见
[Shadow 执行域阶段总结（2026-09-18）](docs/shadow-domain-summary-zh.md)。
该后端不同于下文保留的早期 Linux scheduler-shadow 和独立 standalone RC 实验。

For the independent **B-owned shadow context / BAR2 hardware page-table**
experiment (B initializes only; A executes in ring 3 with a local WB E-stack), see
[remote-context-experiment.md](docs/remote-context-experiment.md).
Build and run it with `bash scripts/run-remote-context-benchmark.sh --runs 1
--samples 100 --output build/remote-context-smoke` (one shell command).
It uses its own guest image, not the Linux/eRPC initramfs. Its latency measures
the context activation mechanism and must not be labeled eRPC end-to-end latency.

The tree now has two selectable backends. `ivshmem` remains the default and
preserves the original x86/QEMU prototype. `ubsm` adds the real-UB AArch64
path: multi-procedure metadata, nested calls, eRPC compatibility, and a
gRPC-Go/DeathStarBench Geo demo. The caller/shadow handoff and A-stack stay on
the importing host; service data is owned by the remote UB host and imported
only by shadows. In the minimal procedure-1 demo, B also publishes an AArch64
PIC code image; A copies and verifies it into a local RX mapping before the
shadow executes it. See the real-UB commands below and `docs/ub-bringup.md`.

The procedure-1 image is deliberately packaged into B's owner executable and
published during owner startup. A does not scrape arbitrary pages from B's
running `.text` mapping. The image is a restricted leaf handler with no dynamic
linker relocations, process-local globals, PLT/GOT calls, or absolute pointers;
remote state is passed separately through `lrpc_astack.service_data`.

```sh
# Existing x86/QEMU build
cmake -S . -B build -G Ninja -DLRPC_BACKEND=ivshmem

# Real UB build (normally on an AArch64 UB host)
sh scripts/build-ub.sh
```

This directory is a Linux/QEMU proof of concept for the mechanism described in
`UB跨机LRPC专利交底-Typst.pdf`.  Guest B publishes a position-independent
service image and exported service data into an ivshmem BAR. During binding in
guest A, the passive shadow maps the remote image read-only, copies it into an
anonymous local mapping, verifies its FNV-1a hash, changes the mapping from RW
to RX, and unmaps the remote code window. It then registers with the kernel.
The service data remains in BAR2, while the shared A-stack, code copy, and
private E-stack are local to A. The caller maps only its driver-allocated
A-stack slot. `CALL` blocks the caller and wakes the CPU-pinned shadow;
the normal Linux scheduler switches to the shadow's distinct `mm_struct`/CR3.
`RETURN` blocks the shadow again and wakes the caller. There is no request
queue and no guest-B CPU on the invocation datapath.

The Linux driver enforces role-based mappings, W^X, distinct caller/shadow
address spaces, same-CPU handoff, and procedure validation. The BAR2 code
window is writable only by the publisher and read-only/NX for a bound shadow;
the final local copy follows an RW-to-RX transition. Up to 16 published
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

The shared handoff UAPI layout changed, and the ivshmem metadata is now ABI
version 3 because `PUBLISH`/`BIND` carry the code hash and exact copy size and
the returned A-stack offset selects A-local driver pages rather than BAR2.
Rebuild both `ub_lrpc.ko` and every guest userspace binary with
`scripts/build-vm.sh`; an old initramfs cannot be mixed with the new header or
module. Real-UB builds must likewise rebuild `ub_lrpc_ctl.ko` and their
userspace programs together.

The QEMU scripts build a small Linux 6.6 LTS kernel/initramfs, attach one 4 MiB
ivshmem backing file to both guests, and check separate caller/shadow PIDs,
denied caller code/data mappings, verified local RX code with the remote code
mapping removed, a result that consumes exported B data, and equal CPU IDs
before/during/after the service call. See `docs/design.md` for the ABI and
remaining work toward a production eRPC backend.

The caller guest also runs a dependent-load microbenchmark over a dedicated
64 KiB BAR2 remote-memory window. The default remains noncached. Use separate
runs to compare guest page attributes:

```sh
LRPC_NET_PORT=22345 LRPC_CACHE_MODE=noncached ACCEL=kvm ./scripts/run-qemu.sh
LRPC_NET_PORT=22346 LRPC_CACHE_MODE=cached ACCEL=kvm ./scripts/run-qemu.sh
```

With no `LRPC_TEST_CASE`, `run-qemu.sh` retains the legacy `all` smoke test.
Performance measurements should select exactly one isolated case so unrelated
applications and the long BAR latency loop cannot condition the guest
scheduler before the measured workload:

```sh
LRPC_TEST_CASE=bar-latency  LRPC_CACHE_MODE=noncached ACCEL=kvm ./scripts/run-qemu.sh
LRPC_TEST_CASE=nested      LRPC_CACHE_MODE=noncached ACCEL=kvm ./scripts/run-qemu.sh
LRPC_TEST_CASE=direct-lrpc LRPC_CACHE_MODE=noncached ACCEL=kvm ./scripts/run-qemu.sh
LRPC_TEST_CASE=upstream-erpc LRPC_CACHE_MODE=noncached ACCEL=kvm ./scripts/run-qemu.sh
LRPC_TEST_CASE=deathstar   LRPC_CACHE_MODE=noncached ACCEL=kvm ./scripts/run-qemu.sh
```

Every isolated application case runs its workload first and the behavioral
BAR/A-stack cache probes afterward. Each case starts only the shadow processes
it needs. Because the current mapping API's `lrpc_bind(proc=1)` requires a
registered shadow, Nested and DeathStarBench start a sleeping proc-1 shadow
only after their measured workload, immediately before the cache probes. The
publisher starts the upstream eRPC server only for the `upstream-erpc` case.

To approximate `UBSM_FLAG_ONLY_IMPORT_NONCACHE`, keep the publisher mapping
cached and the importing caller/shadow guest noncached:

```sh
LRPC_NET_PORT=22347 LRPC_PUBLISHER_CACHE_MODE=cached \
LRPC_CALLER_CACHE_MODE=noncached ACCEL=kvm ./scripts/run-qemu.sh
```

The QEMU/x86 driver keeps every 64 KiB A-stack in driver-allocated A-local
normal RAM. The caller and its bound shadow map the same pages through a
synthetic device offset, but these mappings never enter the BAR2 PFN path and
never receive `pgprot_noncached()`: they retain the architecture-default WB
page protection in both cache modes. `ASTACK_CACHE_PROBE` behaviorally verifies
that the local A-stack is `effective-wb` and fails the run otherwise.

BAR2 retains a separate per-procedure remote benchmark slot.
`BAR_LOAD_LATENCY` reports 21 batches of 100,000 serialized loads through a
random cache-line pointer chain in that BAR2 slot, so hardware prefetch and
memory-level parallelism cannot hide the measured dependency latency.
`BAR_CACHE_PROBE` classifies this remote window according to the requested
cached/noncached mode. `BAR_PAT` lines are read from x86 debugfs when available.
These measurements validate the guest mapping policy and compare local
KVM/ivshmem modes; ivshmem does not model UB fabric latency or coherence
behavior.

The procedure-1 shadow prints `LRPC_CODE_LOCALIZED ... permissions=rx
remote_exec=denied remote_unmapped=1` before registration. This is a
connection-time copy: the copy cost is not part of the 1000-call steady-state
latency, and invocation-time instruction fetches use A-local memory. A-stack
loads/stores now use A-local WB pages; only service-data accesses remain on the
BAR2 remote-state path during an LRPC call. Because the code window has already
been unmapped, its `BAR_PAT` range may disappear from the later debugfs
snapshot; the service-data and explicit benchmark ranges are the relevant
remote steady-state entries.

`LRPC_BREAKDOWN_AVG` calls the residual outside the four kernel timestamps
`outside_kernel_timestamps_ns`. It includes request preparation, shared
A-stack writes, response reads/copies, the compatibility wrapper, and user
timing overhead; it must not be interpreted as pure `ioctl()` execution time.

For an isolated five-run cached/noncached comparison, first rebuild the VM
artifacts and then run the application batch driver. Every `(test, mode, run)`
uses a fresh publisher/caller VM pair. Odd repetitions run cached then
noncached; even repetitions reverse the order, avoiding a systematic
mode-versus-time bias. The publisher stays cached; only the caller changes.
Set `QEMU` explicitly if the system QEMU is older than the QEMU 11 build used
for `honor-guest-pat`:

```sh
sh scripts/build-vm.sh
QEMU=/path/to/qemu-11.1.1/bin/qemu-system-x86_64 \
LRPC_RUNS=5 LRPC_BASE_PORT=22360 \
LRPC_HOST_NUMA_NODE=0 \
LRPC_PUBLISHER_HOST_CPUS=40-43 \
LRPC_CALLER_HOST_CPUS=44-47 \
bash scripts/run-qemu-app-benchmark.sh
```

Each run must exit successfully, print `QEMU_E2E_PASS`, and contain no
`ERPC_LRPC_FAIL`; otherwise the batch stops and excludes the failed sample.
The timestamped `build/qemu-app-benchmark-*` directory keeps every caller,
publisher, and combined log, plus `results.csv`, `summary.csv`, and a readable
per-application `summary.txt`. The summary reports min/median/max/average and
the ratio of the noncached median to the cached median. DeathStarBench is
currently a functional PASS case because it does not yet emit a latency
distribution. `LRPC_TEST_CASES` can select a whitespace-separated subset;
`LRPC_RESULTS_DIR`, `LRPC_RUNS`, `LRPC_BASE_PORT`, `QEMU`, and `ACCEL` can also
be overridden. For reproducible host placement, `LRPC_HOST_NUMA_NODE` binds
memory allocation and the two
non-overlapping `LRPC_PUBLISHER_HOST_CPUS` / `LRPC_CALLER_HOST_CPUS` sets bind
the QEMU processes and all inherited vCPU threads. `run-qemu.sh` truncates the
ivshmem backing file to zero before recreating it, so a new batch does not
inherit the previous file pages' NUMA placement. Choose currently idle CPUs
from one memory-bearing node; the example uses node 0 on a 48-core socket.

The cache probes treat both classifications as correctness conditions: the
BAR2 benchmark window must report `effective-wb` for cached runs and
`effective-uc` for noncached runs, while the A-local A-stack must always report
`effective-wb`. A mismatch or `inconclusive` result fails that run instead of
silently contributing it to the statistics. `results.csv` records both
classifications and both probes' raw empty/warm/cold p50 cycle counts.

For a controlled cross-NUMA approximation, use the dedicated wrapper. It uses
the isolated application batch above, placing B plus all 4 MiB of ivshmem
backing pages on node 0 while A's QEMU CPUs and private guest RAM are on node 1:

```sh
QEMU=/path/to/qemu-11.1.1/bin/qemu-system-x86_64 \
LRPC_RUNS=5 LRPC_BASE_PORT=22400 \
bash scripts/run-qemu-cross-numa-benchmark.sh
```

The defaults for the current 96-core, two-socket host are publisher CPUs
`40-43` on node 0 and caller CPUs `88-91` on node 1. Check that they are idle
before running, or override `LRPC_PUBLISHER_HOST_CPUS` and
`LRPC_CALLER_HOST_CPUS`. The wrapper sets:

```text
LRPC_PUBLISHER_HOST_NUMA_NODE=0
LRPC_SHARED_HOST_NUMA_NODE=0
LRPC_CALLER_HOST_NUMA_NODE=1
```

`run-qemu.sh` truncates the backing file and writes all 4 MiB under node 0's
memory policy before starting either VM. Consequently, BAR2 remote benchmark
and service-data pages cannot silently become node-1-local through caller first
touch. A-stacks are not in this file; they come from the caller VM's A-local
driver pages. Publisher and caller QEMU private allocations use their
respective node memory policies. This is a controlled host cross-socket
approximation, not a model of UB fabric latency, coherence traffic, congestion,
or failure behavior.
While each VM is alive, the launcher also records
`QEMU_HOST_SHARED_MAP role=...` from `/proc/<qemu-pid>/numa_maps`. For the
cross-NUMA setup, the `shared-memory.bin` line should report its resident pages
as `N0=...` for both QEMU mappings; preserve these lines with the benchmark
logs as placement evidence.

`BAR_CACHE_PROBE` additionally tests the effective CPU behavior instead of
trusting the guest PAT record alone. It times the same BAR2 cache line while
warm and immediately after `clflush`. A large cold-versus-warm separation is
reported as `classification=effective-wb`; similarly slow warm and cold loads
are reported as `classification=effective-uc`; noisy results remain
`inconclusive`. Run both modes and compare the two result lines:

```sh
bash -o pipefail -c \
  'LRPC_NET_PORT=22345 LRPC_CACHE_MODE=noncached ACCEL=kvm \
   ./scripts/run-qemu.sh 2>&1 | tee build/noncached-probe.log'
bash -o pipefail -c \
  'LRPC_NET_PORT=22346 LRPC_CACHE_MODE=cached ACCEL=kvm \
   ./scripts/run-qemu.sh 2>&1 | tee build/cached-probe.log'

grep BAR_CACHE_PROBE build/noncached-probe.log build/cached-probe.log
```

If both requested modes report `effective-wb`, KVM/EPT is overriding the guest
NC request. If cached reports `effective-wb` and noncached reports
`effective-uc`, guest PAT is reaching the effective hardware memory type.
This is a behavioral classifier, not a direct EPT page-table dump.

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
A 端导入映射为 non-cache。UB 共享区保存元数据、service data，以及 procedure
1 的 AArch64 PIC code image。A 在连接时把该 image 复制到本地匿名页，校验
hash、同步指令缓存，再将页面从 RW 改为 RX。A-stack、E-stack 和最终执行的
代码副本都在 A 本地，UB 映射本身不请求 `PROT_EXEC`。procedure 2/3/4
暂时仍使用预编译的 `LOCAL_CODE` handler。

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
# UB 真机不需要下载用于 QEMU 的 Linux 6.6.155 源码
LRPC_FETCH_LINUX=0 sh scripts/fetch-deps.sh

# 用仓库中已完成 AArch64 适配的版本替换第三方 eRPC 的 math_utils.h；
# 该脚本会检查源文件包含 __builtin_clz 且不包含 x86 专用的 bsrl。
sh scripts/replace-erpc-math-utils.sh

sh scripts/build-ub-upstream-erpc.sh
sh scripts/build-ub-grpc.sh
```

若适配后的 `math_utils.h` 不在默认的 `../eRPC/src/util/`，可显式指定：

```sh
ERPC_MATH_UTILS_SOURCE=/path/to/adapted/math_utils.h \
    sh scripts/replace-erpc-math-utils.sh
```

如果 `third_party/eRPC` 已经被手工修改，而这里只需要补拉
DeathStarBench Geo 依赖，可跳过 eRPC 的获取和 patch 状态检查：

```sh
LRPC_FETCH_LINUX=0 LRPC_FETCH_ERPC=0 sh scripts/fetch-deps.sh
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
procedure 1/2/3/4；其中 procedure 1 同时发布 PIC code image：

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
远端 code image 和 service data，将代码复制到本地 RX 页后执行：

```sh
LRPC_SHADOW_CPU=1 ./build-ub/lrpc-ub-shadow \
    lrpc_ub_demo /dev/ub_lrpc_ctl
```

shadow 应先打印代码本地化证据；其中 `remote_offset` 是 B 发布的 UB code
slot，`local_entry` 是 A 的本地 RX 地址：

```text
UB_LRPC_CODE_LOCALIZED proc=1 remote_offset=... bytes=... hash=0x... local_entry=0x... permissions=rx remote_value=100
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
procedure 1 的 B 端代码发布与 A 端 RW-to-RX 本地化、多 procedure 元数据、
epoch/hash 检查、级联 handoff、eRPC API 和 gRPC-Go demo 接到同一代码树中；
`LRPC_BACKEND=ivshmem` 的 x86/QEMU 路径保持独立。当前只有 procedure 1
使用发布代码；procedure 2/3/4 仍是 A 端预编译 handler。
但源码静态检查或 x86 配置成功不能替代真机结论：最终仍需在两台 AArch64
UB 主机上完成编译、模块加载、功能输出、远端 load latency 与端到端延迟测试。
