# UB-Tigon 实现与运行说明

## 目标数据路径

每个 coordinator 拥有一个具名 UBS Memory region，并映射所有 peer 的 region。数据库 partition 的 owner 与物理 region 的 owner 完全一致。共享内存中不保存进程虚拟地址；`UBGlobalPtr` 通过 `region_id`、`generation` 和 `offset` 标识对象。

执行 point read/update 时，requester 解析 owner region 中的地址，直接对 tuple 执行 CAS/加锁，将 value 复制到事务本地 buffer 或从中写回，并在 commit/abort 时释放同一把共享锁。owner 不参与这条数据路径。选择 `--shared_memory_backend=ub` 后，数据迁移以及原 CXL SCC bitmap/flush 路径均被禁用。

消息继续采用 copy-based 设计。发送方把序列化的小型 `Message` 复制到目标节点的 UB queue，接收方再把它复制到本地 `Message`。UB queue 使用 acquire/release 完成发布，不执行 `clflush`、`clwb` 或 `_mm_sfence`。

## 已实现的基础组件

- `common/UBGlobalPtr.h`：固定 16-byte 的跨进程指针 ABI。
- `common/UBMemory.*`：UBSM 初始化、每 coordinator 一个 owner region、全 region 映射、generation/range 校验、root directory、CAS bump allocator、importer 优先 unmap 和 owner deallocate。
- `common/UBTuple.h`：requester-side 共享读写锁和 tuple header。
- `common/UBCpu.h`：AArch64/x86 通用的自旋等待 relax。
- `common/UBMPSCRingBuffer.*`：有界、copy-based UB MPSC queue。
- `core/UBBTreeCatalog.*`：root slot 8 中的 version-2 catalog/descriptor ABI；每个 table descriptor 还发布一个稳定的 `+infinity` gap tuple。
- `core/UBBPlusTree.h`：位置无关的 UB B+ Tree、稳定 tuple、有序 leaf、requester-side insert、leaf/inner split 和 root replacement。
- `core/UBBPlusTreeAdapter.h`：UB B+ Tree 的 `ITable` 实现。
- `tests/ub_primitives_test.cpp`：host-only ABI 和锁竞争测试。
- `tests/ub_btree_host_test.cpp`：在进程内 region arena 上测试并发 insert/split、有界 range scan、successor/end-gap locking、tombstone 和同 key 复用；其中 10,000 次 delete/reinsert 用来确认同-key churn 不会继续分配 tuple。
- `tests/ub_btree_compile_test.cpp`、`tests/ub_adapter_compile_test.cpp`：完整模板和 `ITable` 编译检查。
- `tests/ub_tpcc_adapter_compile_test.cpp`：实例化全部 11 个 TPCC UB table/index adapter。

集成后的事务路径支持 YCSB `--query=rmw`、`insert`、`delete`、`scan` 和 `mixed`。数据库初始化会创建 `TableUBBPlusTree`，并把 owner partition 的 tuple 直接写入 owner region。本地和远程 point read/update 使用同一个 requester-side `UBTupleHeader` 锁；远程路径不发送 migration request。

insert 先在 owner index 中预留 state-2 placeholder，commit 时发布为 state 1，abort 时转为 state-0 tombstone。delete 直接锁住 visible tuple，commit 时发布 state 0，abort 时只解锁。range transaction 会锁住每个结果 tuple，以及第一个 visible successor；若范围到达末尾，则锁住稳定的 end-gap tuple。requester-side insert 在持有 tree structural write lock 时必须获得同一个 successor 锁，从而防止 phantom 进入受保护范围。

TPCC 的全部 11 张 primary/secondary table 现在都使用 UB B+ Tree，包括 `customer_name_idx` 和 `order_customer`。共享 item table 只由 owner 初始化。payment/new-order/first-two/mixed 已提供分阶段验证入口，但当前 workspace 只完成了模板和静态检查，尚不能视为已通过 UB 真机验证。

使用 `--use_ub_transport=true` 时，每个 coordinator 在自己 owner region 的 root slot 9 发布一个有界 MPSC inbox。outgoing dispatcher 把序列化 `Message` 复制到目标 inbox，单个 incoming dispatcher 再复制到进程本地 `Message`。这种 fixed-entry transport 禁用 message grouping；单条序列化消息不得超过 `--cxl_trans_entry_struct_size`。

B+ Tree leaf 保存 `{key, tuple_offset}`，而不是把 tuple 嵌入 leaf。移动 leaf entry 或执行 split 时不会移动 `UBTupleHeader` 及其事务锁。当前实现先用一把 requester-side tree RW lock 保证 lookup、ordered traversal、insert、split 和 root replacement 的正确性，后续再优化为 node-level OLC。tuple transaction lock 独立存在，并一直持有到 commit/abort。

region allocator 当前保持单调增长。state-0 tombstone 可以在地址和 version 单调的前提下，原地复用于相同逻辑 key。abort 的 placeholder 会保留为 tombstone；tuple 和 B+ Tree node 都不会复用于不同 key。跨-key reuse、leaf 物理删除、merge 和 root shrink 需要共享 epoch/QSBR，以避免 lookup-to-tuple-lock ABA。

为支持 end-gap protection，B+ Tree descriptor ABI 已升级到 version 2。不要复用 version-1 tree 创建的 prefix；升级后的每次运行都必须使用新的 region prefix。

## 两台 UB 主机的前置条件

本文使用下面的固定部署。示例 IP 和 provider hostname 必须替换为机器上真实注册的值：

| 机器 | Tigon ID | 示例 IP | 内存 NUMA node | 该 node 可用 CPU |
| --- | ---: | --- | ---: | ---: |
| 81 | 0 | `192.0.2.10` | 1 | 37 |
| 82 | 1 | `192.0.2.11` | 0 | 20 |

两台 AArch64 主机必须运行相同 Tigon revision 和相互兼容的 UBS Memory 软件栈。机器 82 是 worker 数量的瓶颈。

检查架构、服务、SDK 架构和动态依赖：

```bash
uname -m                         # 预期：aarch64
systemctl is-active ubse.service
systemctl is-active ubsmd
file /usr/local/ubs_mem/lib/libubsm_sdk.so
ldd /usr/local/ubs_mem/lib/libubsm_sdk.so
```

两个服务都必须为 active；`libubsm_sdk.so` 必须是 AArch64 库；`ldd` 不能出现 `not found`。在每个用于编译或运行 Tigon 的 shell 中配置：

```bash
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:${LD_LIBRARY_PATH:-}
export HCOM_CONNECTION_RETRY_TIMES=2
```

应用用户必须能够访问本机 `ubsmd` Unix socket：

```bash
id
getent group ubsmd
```

如果用户不属于该组，管理员可执行 `sudo usermod -aG ubsmd "$(id -un)"`，之后必须重新登录。共享 region 使用 `0660`，建议两台主机使用相同 UID/GID。

每个进程应显式指定自己的本地物理 provider。其值必须与 UBS Engine topology 中登记的 hostname 完全一致，不一定等于 `hostname -f` 返回的 FQDN。

> 请把命令中的 `UB_PROVIDER_HOST` 替换成实际注册的 host name。

```bash
# 仅机器 81 / coordinator 0
export UB_PROVIDER_HOST=host-81
export UB_NUMA_NODE=1
export UB_PROVIDER_NUMA=1

# 仅机器 82 / coordinator 1
export UB_PROVIDER_HOST=host-82
export UB_NUMA_NODE=0
export UB_PROVIDER_NUMA=0
```

确认 hostname 可以解析且管理网络连通。host 0 的 TCP 端口 `18951` 用于独立 correctness test 的 barrier；两台主机的 TCP 端口 `10010` 用于 Tigon coordinator。UBS Engine/OBMM 的发现和数据通路必须已经正常工作。

检查实际 NUMA topology：

```bash
numactl --hardware
```

`UB_NUMA_NODE` 和 `UB_PROVIDER_NUMA` 控制不同层次。脚本只把 `UB_NUMA_NODE` 用于 `numactl --cpunodebind=N --membind=N`，它决定进程线程和普通本地内存的位置；`UB_PROVIDER_NUMA` 会转换为 UBSM provider NUMA 参数，决定 coordinator-owned UB region 的位置。在当前部署中，机器 81 的两个值均为 1，机器 82 均为 0。未设置 `UB_PROVIDER_NUMA` 时由 UBS Engine 选择 provider NUMA；未设置 `UB_NUMA_NODE` 时脚本不会调用 `numactl`。

`one-sided` 使用 `UBSM_FLAG_ONLY_IMPORT_NONCACHE | UBSM_FLAG_WR_DELAY_COMP`，因此平台 NC-CC/snoop 配置必须支持该模式。`nocache` 使用 `UBSM_FLAG_NONCACHE | UBSM_FLAG_WR_DELAY_COMP`。

## 编译

编译依赖支持 C++14 的 C/C++ compiler、CMake、pthread、Boost、jemalloc、glog 和 gflags。openEuler 默认使用 GCC/GNU ld；Clang/LLD 是可选项，不属于 ABI 要求。运行时 NUMA placement 推荐安装 `numactl`。

Debian/Ubuntu：

```bash
sudo apt-get install -y \
  cmake clang-15 lld-15 \
  libboost-all-dev libjemalloc-dev \
  libgoogle-glog-dev libgflags-dev numactl
```

openEuler 推荐直接使用系统 GCC，不要求带版本号的 Clang：

```bash
sudo dnf makecache
sudo dnf install -y \
  cmake make gcc gcc-c++ glibc-devel \
  boost-devel jemalloc-devel \
  glog-devel gflags-devel \
  numactl
```

如果之前的 `dnf install` 包含仓库中不存在的 `clang15/lld15`，DNF 可能中止整个事务。请用上面的命令重新安装，并检查：

```bash
cat /etc/openEuler-release
uname -m                         # 预期：aarch64
gcc --version
g++ --version
ld --version
```

如果当前 openEuler repository 提供 Clang，也可以显式选择；较新的发行版通常使用无版本后缀的包名和命令名：

```bash
sudo dnf install -y clang llvm lld
command -v clang clang++ ld.lld
```

不要为了获得特定 compiler version 而启用与当前 openEuler service pack 不匹配的 EPOL repository。上述命令不负责安装 UB SDK；其 header 和 AArch64 `libubsm_sdk.so` 必须已经位于 `/usr/local/ubs_mem`，或通过 `UBSM_INCLUDE_DIR`、`UBSM_LIBRARY` 指定。

### 主机与构建层测试

```bash
cmake -S tigon -B tigon/build-host \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DTIGON_ENABLE_UB=ON \
  -DTIGON_BUILD_UB_UNIT_TESTS=ON \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so

cmake --build tigon/build-host -j --target \
  ub_primitives_test ub_btree_host_test ub_btree_compile_test \
  ub_adapter_compile_test ub_tpcc_adapter_compile_test ub_twopl_compile_test

ctest --test-dir tigon/build-host --output-on-failure \
  -R 'ub_(primitives|btree|adapter|tpcc|twopl)'
```

AArch64 上即使是 test build 也必须指定 `TIGON_ENABLE_UB=ON`，否则会链接旧的 x86-64 `cxlalloc` archive。如果 `build-host` 以前没有使用该选项配置，请先重新执行上述 CMake configure 命令。

### AArch64 UB 应用构建

项目使用 `-march=native`，因此应在两台 AArch64 主机上分别编译。不要复用 x86-64 或 legacy CXL backend 产生的 CMake cache。

在每台主机的 `ub-application` repository 根目录执行：

```bash
export TIGON_REPO_ROOT="$PWD"
export TIGON_BUILD_DIR="$TIGON_REPO_ROOT/tigon/build-ub-aarch64-gcc"

cmake -S "$TIGON_REPO_ROOT/tigon" -B "$TIGON_BUILD_DIR" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DTIGON_ENABLE_UB=ON \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so

cmake --build "$TIGON_BUILD_DIR" -j \
  --target bench_ycsb bench_tpcc ub_tigon_two_node_test
```

`$PWD` 并不要求 build output 必须位于任意当前目录；它只是展开为 shell 当前目录的绝对路径。上述命令只有在 repository 根目录执行时才正确。如果 repository 位于 `/work/ub-application`，也可以写成：

```bash
export TIGON_REPO_ROOT=/work/ub-application
export TIGON_BUILD_DIR="$TIGON_REPO_ROOT/tigon/build-ub-aarch64-gcc"
```

脚本默认使用 `tigon/build-ub`。这里只有因为手册使用了独立的 `build-ub-aarch64-gcc`，才需要设置 `TIGON_BUILD_DIR`。

UB build 不链接 `dependencies/cxlalloc/libcxlalloc_static.a`。该 archive 是为 legacy CXL backend 保留的 x86-64 artifact，与 AArch64 UB host 不兼容。UB build 使用 fail-fast stub 满足遗留 CXL 声明；如果运行时进入 stub，说明选择了尚未适配的 CXL execution path，进程会输出对应函数名并终止。

构建前再次确认 AArch64 UB SDK 及完整依赖链：

```bash
uname -m                         # 预期：aarch64
file /usr/local/ubs_mem/lib/libubsm_sdk.so
ldd /usr/local/ubs_mem/lib/libubsm_sdk.so
```

如果旧 CMake cache 记录了 `/usr/bin/clang-15`，必须使用新 build directory。configure output 应显示 GCC 路径，并包含：

```text
Tigon C compiler: /usr/bin/gcc
Tigon C++ compiler: /usr/bin/g++
Tigon UB backend enabled for aarch64; the legacy cxlalloc archive will not be linked
```

运行前检查生成的 executable：

```bash
file "$TIGON_BUILD_DIR/bench_ycsb"
file "$TIGON_BUILD_DIR/bench_tpcc"
file "$TIGON_BUILD_DIR/ub_tigon_two_node_test"
ldd "$TIGON_BUILD_DIR/bench_ycsb"
ldd "$TIGON_BUILD_DIR/bench_tpcc"
ldd "$TIGON_BUILD_DIR/ub_tigon_two_node_test"
```

它们必须是 AArch64 executable，且不能存在 unresolved shared library。

每次运行都要使用唯一的 `--ub_region_prefix`，且不能在 `one-sided` 与 `nocache` 之间复用。两台主机必须使用相同的 prefix、region size、coordinator count 和 mode；`--id`、provider host 不同。

## 双机验证流程

socket address 仍用于启动/关闭 barrier，并在 `--use_ub_transport=false` 时作为控制与数据 transport。两个命令应相近时间启动，因为每个进程创建 owner region 后会等待 peer region 出现。

每次运行使用新的 region prefix。不要在 `one-sided`、`nocache` 之间复用，也不要在异常退出后复用，除非已经确认旧 UBS object 被释放。两台主机必须使用相同 prefix、mode、region size、server ordering、key count 和 timing 参数；`--id`、`UB_PROVIDER_HOST` 不同。

### 步骤 1：独立双机正确性测试

首次 smoke test 时，在两台主机设置与本地 OBMM block 配置兼容的 region size。

机器 81：

```bash
export UB_REGION_MB=128
export UB_PROVIDER_HOST=host-81
export UB_NUMA_NODE=1
export UB_PROVIDER_NUMA=1
```

机器 82：

```bash
export UB_REGION_MB=128
export UB_PROVIDER_HOST=host-82
export UB_NUMA_NODE=0
export UB_PROVIDER_NUMA=0
```

先启动机器 81 / coordinator 0。示例中的 `192.0.2.10` 是其 TCP barrier 监听地址：

```bash
bash tigon/scripts/run_ub_correctness.sh \
  0 192.0.2.10 one-sided correctness_001_os 10000 18951
```

然后启动机器 82 / coordinator 1：

```bash
bash tigon/scripts/run_ub_correctness.sh \
  1 192.0.2.10 one-sided correctness_001_os 10000 18951
```

两个进程都必须以 `PASS` 结束。该测试覆盖跨节点 writer-lock increment、placeholder conflict/publication、delete visibility、同-key tombstone reuse、abort placeholder tombstone、并发 requester insert、leaf/inner/root split、ordered scan、queue-full backpressure、双向 queue payload、range successor/end-gap exclusion 和 importer-first cleanup。

换用新 prefix，在两台主机重复 `nocache`：

```bash
# Host 0
bash tigon/scripts/run_ub_correctness.sh \
  0 192.0.2.10 nocache correctness_001_nc 10000 18951

# Host 1
bash tigon/scripts/run_ub_correctness.sh \
  1 192.0.2.10 nocache correctness_001_nc 10000 18951
```

### 步骤 2：单组 YCSB 点操作测试

下面手动运行 `one-sided` RMW，并使用 UB message queue。两个命令应相近时间启动。

机器 81 / coordinator 0：

```bash
"$TIGON_BUILD_DIR/bench_ycsb" \
  --id=0 --servers="192.0.2.10:10010;192.0.2.11:10010" \
  --protocol=TwoPLPasha --partition_num=2 --threads=1 --io=1 \
  --shared_memory_backend=ub --ub_memory_mode=one-sided \
  --ub_region_prefix=tigon_run_001 --ub_region_mb=4096 \
  --ub_provider_host=host-81 --ub_provider_numa=1 \
  --use_ub_transport=true --query=rmw --keys=200000 \
  --cross_ratio=100 --time_to_warmup=5 --time_to_run=20 \
  --lotus_checkpoint=0
```

机器 82 / coordinator 1：

```bash
"$TIGON_BUILD_DIR/bench_ycsb" \
  --id=1 --servers="192.0.2.10:10010;192.0.2.11:10010" \
  --protocol=TwoPLPasha --partition_num=2 --threads=1 --io=1 \
  --shared_memory_backend=ub --ub_memory_mode=one-sided \
  --ub_region_prefix=tigon_run_001 --ub_region_mb=4096 \
  --ub_provider_host=host-82 --ub_provider_numa=0 \
  --use_ub_transport=true --query=rmw --keys=200000 \
  --cross_ratio=100 --time_to_warmup=5 --time_to_run=20 \
  --lotus_checkpoint=0
```

为隔离故障，第一次先设置 `--use_ub_transport=false`。此时 tuple、lock、index 仍使用 UB，只有 Tigon message 使用 TCP。通过后换新 prefix，设置 `--use_ub_transport=true` 验证 UB queue。

### 步骤 3：分阶段 YCSB 测试矩阵

matrix 脚本默认运行 12 个组合：两种 memory mode、三种 point operation、两种 message transport。先在两台主机只运行一个组合：

```bash
export UB_REGION_MB=4096
export UB_TIGON_THREADS=1
export UB_TIGON_KEYS=200000
export UB_TIGON_RUN_SECONDS=20
export UB_TIGON_WARMUP_SECONDS=5
export UB_TIGON_MODES="one-sided"
export UB_TIGON_QUERIES="rmw"
export UB_TIGON_TRANSPORTS="false"
```

保留步骤 1 中机器各自的 `UB_PROVIDER_HOST`、`UB_PROVIDER_NUMA` 和 `UB_NUMA_NODE`。以下两个命令必须并发运行。

Host 0：

```bash
bash tigon/scripts/run_ub_ycsb_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' ycsb_stage_tcp_001
```

Host 1：

```bash
bash tigon/scripts/run_ub_ycsb_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' ycsb_stage_tcp_001
```

通过后设置 `UB_TIGON_TRANSPORTS=true`，并使用新的 prefix base 测试 UB queue。最后运行完整 matrix：

```bash
export UB_TIGON_MODES="one-sided nocache"
export UB_TIGON_QUERIES="rmw insert delete"
export UB_TIGON_TRANSPORTS="false true"

# Host 0
bash tigon/scripts/run_ub_ycsb_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' ycsb_matrix_001

# Host 1
bash tigon/scripts/run_ub_ycsb_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' ycsb_matrix_001
```

脚本生成的 suffix 会让每个 mode/query/transport 组合使用不同 region prefix。可覆盖的环境变量包括 `TIGON_BUILD_DIR`、`UB_PROVIDER_HOST`、`UB_PROVIDER_NUMA`、`UB_NUMA_NODE`、`UB_REGION_MB`、`UB_TIGON_THREADS`、`UB_TIGON_KEYS`、`UB_TIGON_RUN_SECONDS`、`UB_TIGON_WARMUP_SECONDS`、`UB_TIGON_MODES`、`UB_TIGON_QUERIES` 和 `UB_TIGON_TRANSPORTS`。

### 步骤 4：YCSB 范围查询与幻读保护

point case 全部通过后，单独运行 range transaction，以免较大 matrix 掩盖故障。wrapper 默认选择 `scan mixed`，其他参数和环境变量与 Step 3 相同：

```bash
export UB_TIGON_MODES="one-sided"
export UB_TIGON_TRANSPORTS="false"
export UB_TIGON_RANGE_QUERIES="scan"

# 分别在 Host 0 和 Host 1 并发运行
bash tigon/scripts/run_ub_ycsb_range_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' ycsb_scan_tcp_001
bash tigon/scripts/run_ub_ycsb_range_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' ycsb_scan_tcp_001
```

然后依次测试 `mixed`、UB transport，最后测试 `one-sided nocache`；每个阶段使用新的 prefix base。正确运行时不应出现未锁定 next row、重复/乱序 scan key，或在受并发保护的 gap 中成功 commit insert。

### 步骤 5：TPCC 主索引与二级索引

TPCC 消耗的 region 空间明显更多，launcher 默认每个 owner region 使用 8 GiB。先隔离一种 transaction type 并使用 TCP message：

```bash
export UB_REGION_MB=8192
export UB_TIGON_THREADS=1
export UB_TIGON_MODES="one-sided"
export UB_TIGON_TRANSPORTS="false"
export UB_TPCC_QUERIES="payment"
export UB_TPCC_PAYMENT_DIST=100
export UB_TPCC_NEWORDER_DIST=100

# 分别在 Host 0 和 Host 1 并发运行
bash tigon/scripts/run_ub_tpcc_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' tpcc_payment_tcp_001
bash tigon/scripts/run_ub_tpcc_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' tpcc_payment_tcp_001
```

按以下顺序推进，每个阶段使用新 prefix：`payment`、`neworder`、`first_two`、`mixed`；先 TCP 后 UB queue；先 one-sided 后 nocache。两端的启动和关闭 consistency check 都必须通过。除了通用 matrix 变量，脚本还接受 `UB_TPCC_QUERIES`、`UB_TPCC_PAYMENT_DIST` 和 `UB_TPCC_NEWORDER_DIST`。

设置 `--threads=N` 时，每个进程还包含 manager、incoming dispatcher、outgoing dispatcher 和 coordinator main thread，因此 Tigon 主要线程数约为 `N + 4`。机器 82 理论上限接近 `N=16`，但 `N=14` 或 `N=15` 更安全，可为 UBS 服务与 OS 留出 CPU。建议按 1、4、8、12、14 逐步扩展；两个 coordinator 使用相同 `UB_TIGON_THREADS`。

## 当前支持范围与故障排查

在两台 UB 主机实际完成步骤 4、5 前，已完成真机验证的基线仍是此前的点操作范围。当前源码已经包含事务型 YCSB range/mixed、requester-side next-key/end-gap protection，以及全部 TPCC 主表/二级索引表的 UB B+ Tree adapter。这些新增部分通过了仅主机 B+ Tree 测试和模板检查，但尚未在本工作区完成真实 UBS Memory 运行。

跨-key tuple/node reclamation、通用 N-node shutdown 和 crash recovery/WAL replay 仍未实现。

常见错误：

- `ubs_mem.h was not found`：修正 `UBSM_INCLUDE_DIR`。
- `libubsm_sdk.so: cannot open shared object file`：检查 `LD_LIBRARY_PATH` 和 `ldd`。
- AArch64 上出现 `immintrin.h: No such file or directory`：更新到 portable source，其中 legacy B+ Tree spin loop 使用 `UBCpu.h`，x86 CXL cache intrinsic 有架构保护。不要把 x86 intrinsic header 安装或复制到 ARM 主机；更新后用 `TIGON_ENABLE_UB=ON` 重新运行 CMake。
- `UBSM_ERR_IN_USING`（`6024`）：旧进程或 mapping 仍引用 object。不要强制删除 live region；停止两端旧进程，或在排查时使用新 prefix。
- `UBSM_ERR_NOT_SUPPORTED`（`6025`，one-sided）：检查 BIOS snoop/NC-CC 及 `/sys/bus/ub/ub_feature` compatibility。
- `UBSM_ERR_NET`（`6040`）：检查 UBS node discovery 和 control-plane connectivity。
- `UBSM_ERR_UBSE`（`6050`）：检查 `ubse.service`、OBMM resource pool 和 UBS Engine log。
- peer mapping/header timeout：对比两台主机的 prefix、mode、region size、coordinator count、provider hostname 和启动时间。同一个 `--ub_map_timeout` deadline 同时约束 SDK mapping 和等待 peer 发布 region header。
- 出现 `cxlalloc_* was called while Tigon is using the UB backend` fatal message：进入了尚未适配的 legacy CXL path，例如选择了不支持的 workload。

## 生命周期约定

正常关闭顺序如下：停止 worker、message producer 和 dispatcher；unmap 全部 imported region；执行最终 TCP peer barrier；unmap/deallocate 本地 owner region；调用 `ubsmem_finalize()`。当前 coordinator 对支持的单节点/双节点 YCSB 路径执行该顺序。

单节点没有 peer socket 或 imported region，因此跳过 TCP peer barrier。通用 N-node shutdown、crash cleanup 和 replay 不属于当前 milestone。
