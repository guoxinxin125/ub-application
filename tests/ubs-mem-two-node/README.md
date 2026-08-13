# UBS Memory 单机/双机微测试

本目录使用 `ubs-mem` 的公开 SDK 测试单边 CC 共享内存。它与
`tests/ub-two-node` 中直接调用 OBMM 的测试相互独立，不共享构建目录或运行状态。

上一级目录保留五个基础测试和工具：

- `ubsm_shm_admin`：按精确名称查询或清理残留共享内存对象；
- `ubsm_bidirectional_test`：两端各自创建 owner inbox，验证双向 remote store；
- `ubsm_local_one_sided_test`：单机创建、映射和load/store正确性；
- `ubsm_one_sided_owner`：双机测试的物理 owner 端；
- `ubsm_one_sided_remote`：双机测试的 remote/import 端。

五个性能与单边 CC 验证程序已集中到 [`benchmarks/`](benchmarks/README.md)，
不再与基础通路测试混放。所有目标仍由本目录的 `CMakeLists.txt` 统一构建。

所有程序在初始化 SDK 前调用 `ubsmem_set_logger_level(3)`，默认只显示
`ERROR` 和 `CRITICAL`，隐藏 SDK 的 `INFO` 和 `WARN` 日志。

## 1. 测试的缓存语义

创建共享内存时固定使用：

```cpp
UBSM_FLAG_ONLY_IMPORT_NONCACHE | UBSM_FLAG_WR_DELAY_COMP
```

预期语义是：

- owner/export 节点以 cacheable 方式映射；
- remote/import 节点以 `O_SYNC`、non-cacheable 方式映射；
- owner 的读写由硬件维持一致性，不执行 CLWB、flush 或 invalidate；
- remote 直接 load/store NC 映射，不执行软件 cache maintenance；
- 不调用 `ubsmem_shmem_set_ownership()`。

双机测试的数据只通过 UB 映射地址传递。TCP 连接仅传递单字节阶段通知，用来保证
“先写后读”和安全的 unmap/deallocate 顺序，不承载被测试的数据。

测试顺序为：

```text
owner cacheable store
        -> remote NC load 并逐字节校验
        -> remote NC store
        -> owner cacheable load 并逐字节校验
        -> remote unmap
        -> owner unmap + deallocate
```

这同时验证了最基本的单边 CC 正确性：owner 的 cacheable 写入对 remote NC 可见，
remote NC 写入对 owner 的 cacheable 读取可见。

## 2. 与直接 OBMM 测试的区别

应用不再自行调用 `obmm_export()`/`obmm_import()`，也不交换 EID、token 和
`obmm_mem_desc`。应用只使用一个全局共享内存名称：

```text
ubsmem_shmem_allocate(name)
ubsmem_shmem_map(name)
```

`libubsm_sdk.so`、`ubsmd` 和 UBS Engine 负责节点发现、OBMM export/import、
mem_id 管理和异常资源清理。映射成功后的 load/store 仍然直接访问
`/dev/obmm_shmdev<mem_id>` 对应的内存，不经过 daemon 或 RPC。

因此，请先跑通直接 OBMM 测试，再运行本目录测试。这样若失败，可以区分是底层
OBMM/UB 通路问题，还是 UBS Engine/ubsmd 控制面问题。

单机测试和双机 owner 程序都通过 `ubsmem_shmem_allocate_with_provider()` 显式指定
本机 hostname 作为物理 provider，避免共享内存被调度到另一台主机。socket、NUMA 和
UB port 默认传入 `UINT32_MAX` 让 UBS Engine 选择，也可以通过命令行进一步限定。

## 3. 两台机器都要执行的环境检查

### 3.1 操作系统、软件包和内核驱动

```bash
cat /etc/os-release
uname -r

rpm -q ubs-mem-shmem ubs-engine ubs-engine-client-libs ubs-comm-lib

grep -E '^(obmm|ubcore|ubus) ' /proc/modules
ls -l /dev/obmm
```

软件包名字可能随发行版变化。如果 `rpm -q` 找不到，继续用下面的库、服务和文件
检查确认实际安装情况。

第一次成功 create/attach 前没有 `/dev/obmm_shmdev*` 是正常的。

### 3.2 动态库和头文件

```bash
ldconfig -p | grep -E 'libubsm_sdk|libubse-client|libobmm|libhcom'

ls -l /usr/local/ubs_mem/lib/libubsm_sdk.so* 2>/dev/null
ls -l /usr/lib64/libubse-client.so* /usr/lib64/libobmm.so* 2>/dev/null
ls -l /usr/local/ubs_mem/include/ubs_mem.h \
      /usr/local/ubs_mem/include/ubs_mem_def.h 2>/dev/null
```

必须至少能够找到：

```text
libubsm_sdk.so
libubse-client.so.1
libobmm.so.1
ubs_mem.h
ubs_mem_def.h
```

注意：`ubs-mem-master/3rdparty/obmm_ub` 只提供 OBMM 头文件，不能代替
`libobmm.so.1`。当前 `libubsm_sdk.so` 初始化时会加载该库，即使本测试不调用
ownership 切换也仍然需要它。

查看依赖是否有缺失：

```bash
ldd /usr/local/ubs_mem/lib/libubsm_sdk.so | grep 'not found' || true
```

若库已安装但 `ldconfig -p` 找不到，可先为当前终端设置：

```bash
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:${LD_LIBRARY_PATH}
```

### 3.3 UBS Engine 和 ubsmd 服务

```bash
systemctl status ubse.service --no-pager
systemctl status ubsmd --no-pager

systemctl is-active ubse.service
systemctl is-active ubsmd
```

两项都应返回 `active`。若失败，收集：

```bash
journalctl -u ubse.service -b --no-pager | tail -n 200
journalctl -u ubsmd -b --no-pager | tail -n 200
```

### 3.4 NC-CC/snoop 能力

本测试使用 `UBSM_FLAG_ONLY_IMPORT_NONCACHE`。当前 ubs-mem 代码要求 BIOS snoop
开启，对应 `/sys/bus/ub/ub_feature` 的 Bit3 为 0。

```bash
if [ -r /sys/bus/ub/ub_feature ]; then
    value=$(cat /sys/bus/ub/ub_feature)
    printf 'ub_feature=%s\n' "$value"
    printf 'bit3=%d (0 expected for this NC-CC test)\n' "$((value & 0x8 ? 1 : 0))"
else
    echo '/sys/bus/ub/ub_feature 不存在；当前版本会跳过自动兼容性校验'
fi
```

如果 Bit3 为 1，`ubsmem_shmem_allocate()` 预计返回
`UBSM_ERR_NOT_SUPPORTED`（6025）。不要为了绕过检查修改测试 flag；应先确认 BIOS、
ubus 驱动和实际机器的一侧 CC 配置。

### 3.5 NUMA、hugepage 和 OBMM 资源池

```bash
cat /sys/devices/system/node/online
cat /sys/devices/system/node/has_memory
cat /sys/devices/system/node/has_cpu
numactl --hardware

grep . /sys/module/obmm/parameters/* 2>/dev/null
grep -R . /sys/kernel/mm/hugepages/hugepages-2048kB/ 2>/dev/null | \
  grep -E 'nr_hugepages|free_hugepages' || true
```

如果 OBMM 使用 `hugetlb_pmd`，应在实际有内存的 NUMA 节点上准备足够的 2 MiB
hugepage。不要在 `has_memory` 中不存在的 I/O/CPU-only NUMA 节点上预留 hugepage。

如果系统提供 OBMM mempool 状态文件，也应在测试前确认 `total` 或 `available` 不为
零。具体路径随 OBMM 版本变化，可先查找：

```bash
find /sys -path '*obmm*' -type f 2>/dev/null | sort
```

### 3.6 用户权限与主机名

SDK 通过本机 Unix socket 连接 `ubsmd`。两台机器上的测试用户都应有权限访问该
服务：

```bash
id
getent group ubsmd
```

如尚未加入用户组：

```bash
sudo usermod -aG ubsmd "$(id -un)"
```

重新登录后再运行测试。双机共享对象使用 `0660`，建议两台机器使用一致的测试用户
UID/GID，并确认 UBS Memory 的权限策略允许远端 attach。

确认两机 hostname 唯一且可以解析：

```bash
hostname
hostname -f
getent hosts <另一台机器的hostname>
ping -c 3 <另一台机器的管理IP>
```

## 4. 服务配置要求

如果完整软件栈尚未安装，应优先使用发行版对应的软件包：

```bash
sudo dnf install -y ubs-mem-shmem ubs-engine ubs-engine-client-libs
```

若软件源不提供这些包，需要分别按当前机器版本构建和安装 UBS Engine、UBS Comm、
OBMM 用户态库以及 ubs-mem；仅编译本仓库中的 `ubs-mem-master` 不能补齐运行依赖。

### 4.1 本测试不要求修改 ubsmd RPC 配置

本目录的三个测试只使用：

```text
应用 -> 本机 UBS Memory SDK -> 本机 ubsmd UDS -> 本机 UBS Engine
```

双机共享对象的 export/import 和按名称 attach 由 UBS Engine/OBMM 完成；测试程序
自己的 `--owner-ip`/`--bind-ip` 只用于测试阶段同步。因此，只要现有
`ubse.service` 和 `ubsmd` 正常、UBS Engine 已识别两机拓扑，就先保持公共服务配置
不变，直接运行单机和双机测试。不要仅为本测试重启正在被其他用户使用的 `ubsmd`。

本测试需要开放的额外端口只有：

```text
18525/tcp  测试阶段同步（可通过 --port 修改）
```

`18525` 只有 owner 测试程序启动后才会监听。

### 4.2 哪些功能才依赖 ubsmd 节点间 RPC

`ubsmd.conf` 通常位于：

```text
/usr/local/ubs_mem/config/ubsmd.conf
```

其中的 `ubsm.server.rpc.local.ipseg` 和 `remote.ipseg` 用于 ubsmd 之间的 TCP
RPC、ZenDiscovery、节点信息查询及相关分布式服务。如果后续需要这些功能，或运行
日志明确显示因 RPC 节点配置缺失而失败，再由管理员统一配置。

当前 `ubsmem_options_t` 是空结构，`ubsmem_initialize()` 没有提供从应用传入
local/remote RPC 地址的接口，所以这些 daemon 级拓扑参数不能通过本测试程序覆盖。

假设确实需要启用 ubsmd 双机 RPC：

```text
机器 A：192.0.2.10
机器 B：192.0.2.11
ubsmd RPC 端口：7301
```

机器 A：

```ini
ubsm.discovery.min.nodes = 2
ubsm.server.rpc.local.ipseg = 192.0.2.10:7301
ubsm.server.rpc.remote.ipseg = 192.0.2.11:7301
ubsm.lock.enable = off
```

机器 B：

```ini
ubsm.discovery.min.nodes = 2
ubsm.server.rpc.local.ipseg = 192.0.2.11:7301
ubsm.server.rpc.remote.ipseg = 192.0.2.10:7301
ubsm.lock.enable = off
```

本测试不使用 UBS Memory 分布式锁，不需要为了测试开启 `ubsm.lock.enable`。

生产环境应按项目文档配置 TLS 和证书。只在隔离测试网络中临时验证时，才可以在
明确接受风险后设置：

```ini
ubsm.server.tls.enable = off
```

此时还要确保防火墙允许两机之间的 ubsmd RPC 端口：

```text
7301/tcp   ubsmd RPC（以实际配置为准）
```

`ubsmd` 不支持通过应用参数修改这些 daemon 配置，也没有文档化的配置热加载接口。
修改后通常需要重启，因此必须先确认没有其他用户正在使用 UBS Memory，并在维护窗口
内由管理员操作：

```bash
sudo systemctl enable --now ubse.service
sudo systemctl restart ubsmd

systemctl is-active ubse.service ubsmd
```

配置完成后查询端口：

```bash
ss -lntp | grep ':7301' || true
```

## 5. 编译

推荐使用安装后的 SDK：

```bash
cmake -S . \
      -B ./build-ubsm \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so

cmake --build ./build-ubsm \
  --target ubsm_shm_admin \
           ubsm_bidirectional_test \
           ubsm_local_one_sided_test \
           ubsm_one_sided_owner \
           ubsm_one_sided_remote \
  -j
```

`UBSM_INCLUDE_DIR` 也可以指向源码仓中的：

```text
ubs-mem-master/src/app_lib/include
```

但 `UBSM_LIBRARY` 必须指向真实编译、安装好的 `libubsm_sdk.so`，不能用头文件代替。
CMake 配置失败时会分别打印两个头文件和 SDK 库哪个缺失。
头文件与动态库还必须来自兼容的同一版本；不要用本仓库最新头文件配合机器上的旧版
`libubsm_sdk.so`，否则可能出现 ABI 或 flag 语义不一致。

验证产物：

```bash
ls -l tests/ubs-mem-two-node/build-ubsm/ubsm_shm_admin \
      tests/ubs-mem-two-node/build-ubsm/ubsm_bidirectional_test \
      tests/ubs-mem-two-node/build-ubsm/ubsm_local_latency_test \
      tests/ubs-mem-two-node/build-ubsm/ubsm_remote_rw_latency_test \
      tests/ubs-mem-two-node/build-ubsm/ubsm_remote_atomic_test \
      tests/ubs-mem-two-node/build-ubsm/ubsm_remote_atomic_multiprocess_test \
      tests/ubs-mem-two-node/build-ubsm/ubsm_owner_remote_atomic_contention_test \
      tests/ubs-mem-two-node/build-ubsm/ubsm_remote_multithread_load_test \
      tests/ubs-mem-two-node/build-ubsm/ubsm_owner_to_remote_cc_test \
      tests/ubs-mem-two-node/build-ubsm/ubsm_remote_to_owner_cc_test \
      tests/ubs-mem-two-node/build-ubsm/ubsm_local_one_sided_test \
      tests/ubs-mem-two-node/build-ubsm/ubsm_one_sided_owner \
      tests/ubs-mem-two-node/build-ubsm/ubsm_one_sided_remote
```

## 6. 单机测试

先在机器 A 单独运行。选择 `has_memory` 中存在、且与目标 UB Controller 同 socket
的内存 NUMA 节点，例如节点 0：

```bash
numactl --cpunodebind=0 --membind=0 \
  ./build-ubsm/ubsm_local_one_sided_test \
    --provider-host "$(hostname)" \
    --provider-numa 0 \
    --name ubsm_local_a \
    --region-mb 4 \
    --test-bytes 4096
```

然后在机器 B 使用唯一名字独立运行一次，例如 `ubsm_local_b`。

本测试使用 `ubsmem_shmem_allocate_with_provider()`，不再从 `default` 内存域自动选择
provider。`--provider-host` 应为当前机器的 hostname；`--provider-numa` 应与
`numactl` 绑定的有内存 NUMA 节点一致。`--provider-socket` 和 `--provider-port`
未指定时由 SDK 在该主机内自动选择。

`--region-mb` 最小为 4，必须是 4 的整数倍。UBS Engine 还可能按照自己的
`obmm.memory.block.size` 继续向上取整；如果该配置为 128 MiB，应直接传入
`--region-mb 128`，避免请求大小与实际 block 大小造成误解。

成功输出应包含：

```text
PASS local owner load/store correctness (4096 bytes)
PASS local UBS Memory one-sided test and cleanup
```

程序正常退出时会依次执行 unmap、deallocate 和 finalize，无需手动清理。

## 7. 双机测试

确保两端使用完全相同的 `--name`、`--region-mb` 和 `--test-bytes`。测试程序不会
自动修改 ubsmd 配置，也不会通过 TCP 传输这些参数。

### 7.1 机器 A：启动 owner

```bash
numactl --cpunodebind=0 --membind=0 \
  ./build-ubsm/ubsm_one_sided_owner \
    --bind-ip 192.0.2.10 \
    --provider-host host-a \
    --provider-numa 0 \
    --port 18525 \
    --name ubsm_micro_ab \
    --region-mb 4 \
    --test-bytes 4096 \
    --iterations 100000 \
    --timeout-sec 60
```

owner 会先创建和映射共享内存，再输出：

```text
owner_local_checked_fenced_load_64b_avg_ns=...
owner_local_fenced_store_64b_avg_ns=...
waiting for remote on 192.0.2.10:18525
```

`--provider-host` 应与 UBS Engine 拓扑中登记的 hostname 完全一致；省略时程序调用
`gethostname()` 使用本机名字。`--provider-socket`、`--provider-numa` 和
`--provider-port` 均可省略，此时传入 `UINT32_MAX` 由 UBS Engine 选择。如果明确指定
NUMA，必须选择实际有内存、资源池可用且属于目标 owner socket 的节点。

### 7.2 机器 B：启动 remote

```bash
./build-ubsm/ubsm_one_sided_remote \
  --owner-ip 192.0.2.10 \
  --port 18525 \
  --name ubsm_micro_ab \
  --region-mb 4 \
  --test-bytes 4096 \
  --iterations 100000 \
  --timeout-sec 60
```

remote 不调用 allocate。它通过全局名字 `ubsm_micro_ab` 请求 UBS Engine attach，
随后映射 owner 已创建的同一对象。

remote 成功输出应包含：

```text
PASS owner cacheable store -> remote NC load (4096 bytes)
remote_nc_checked_fenced_load_64b_avg_ns=...
remote_nc_fenced_store_64b_avg_ns=...
PASS two-node UBS Memory remote and cleanup
```

owner 成功输出应包含：

```text
PASS remote NC store -> owner cacheable load (4096 bytes)
PASS two-node UBS Memory owner and cleanup
```

### 7.3 双向 owner/inbox 测试

该测试与上面的单 owner 测试独立，默认使用 TCP 端口 `18526`。两台机器分别创建：

```text
<prefix>_a_inbox：物理 owner 为机器 A，A cacheable，B remote NC
<prefix>_b_inbox：物理 owner 为机器 B，B cacheable，A remote NC
```

两端必须使用相同的 `--name-prefix`、`--port`、`--region-mb` 和
`--test-bytes`。先在机器 A（内存节点为 NUMA 0）运行：

```bash
numactl --cpunodebind=0 --membind=0 \
  ./build-ubsm/ubsm_bidirectional_test \
    --role a \
    --bind-ip 192.0.2.10 \
    --provider-numa 0 \
    --port 18526 \
    --name-prefix ubsm_bidir_ab \
    --region-mb 4 \
    --test-bytes 4096 \
    --timeout-sec 60
```

再在机器 B 上运行。以下示例使用机器 B 的 NUMA 1；也可以根据实际 UB 设备亲和性
改成 NUMA 3：

```bash
numactl --cpunodebind=1 --membind=1 \
  ./build-ubsm/ubsm_bidirectional_test \
    --role b \
    --peer-ip 192.0.2.10 \
    --provider-numa 1 \
    --port 18526 \
    --name-prefix ubsm_bidir_ab \
    --region-mb 4 \
    --test-bytes 4096 \
    --timeout-sec 60
```

`--provider-host` 省略时，两端分别自动使用自己的 `gethostname()`。测试的数据路径是：

```text
A remote NC store B_inbox -> B owner cacheable load
B remote NC store A_inbox -> A owner cacheable load
```

机器 B 应输出：

```text
PASS A remote NC store -> B owner cacheable load (4096 bytes)
PASS bidirectional node b test and cleanup
```

机器 A 应输出：

```text
PASS B remote NC store -> A owner cacheable load (4096 bytes)
PASS bidirectional node a test and cleanup
```

两端都返回 0 才算完整通过。TCP 仍然只传递阶段字符，4096 字节 pattern 全部通过
UB共享内存读写。异常退出可能留下 `<prefix>_a_inbox` 或 `<prefix>_b_inbox`；分别在
对应物理 owner 机器上使用 `ubsm_shm_admin --remove` 清理。

### 7.4 Benchmark 测试

本地延迟、远端读写、远端原子以及两个单边 CC 方向的测试源码和独立运行说明位于
[`benchmarks/`](benchmarks/README.md)。构建产物仍在 `build-ubsm/`，原有运行路径不变。

## 8. 结果解释
- `remote_nc_checked_fenced_load_64b_avg_ns`：remote NC 映射上读取完整 64 字节，
  并在计时前后检查全部 8 个 `uint64_t`；
- `remote_nc_fenced_store_64b_avg_ns`：remote NC 映射上写入完整 64 字节后
  执行顺序一致内存栅栏，并检查最终写入的全部 8 个 `uint64_t`。

这些延迟项反复访问同一条 64 字节缓存行，因此 owner 侧仍属于热缓存测试；
它们用于确认完整缓存行访问和数据正确性，不代表 cache miss 或底层共享内存
介质延迟。冷缓存和大工作集延迟需要使用独立的跨缓存行测试。

这些数字用于微测试对比，不代表数据库事务延迟，也不是持久化延迟。TCP 同步不在
上述计时循环中。

双机逐字节 pattern 校验比单个 64 位标志更严格，可以发现部分 cache line、写入顺序
或映射范围错误。但它仍是基础通路测试，不替代长时间压力测试、并发 writer 协议、
跨 cache line 原子性测试和故障恢复测试。

## 9. 清理与故障排查

### 9.1 查询和清理残留对象

必须在对象的 owner 机器上运行管理工具。按精确名称查询：

```bash
./build-ubsm/ubsm_shm_admin \
  --query ubsm_micro_ab
```

对象存在时返回码为 0，并输出类似：

```text
FOUND name=ubsm_micro_ab size=4194304 mem_num=... mem_unit_size=...
```

本机 import 视图中找不到对象时返回码为 1，并输出：

```text
NOT_FOUND_IN_LOCAL_IMPORT_VIEW name=ubsm_micro_ab
```

`ubsmem_shmem_lookup()` 查询的是本机 import 描述。owner 创建、但尚未被 remote import
的对象可能在这里显示 not found，而 UBS Engine 中的 export 对象仍然存在；再次 create
返回内部错误 601 就是这种情况。因此，查询 not found 不能单独证明 owner/export 对象
不存在。

确认 owner 和 remote 测试进程都已经退出后，按精确名称清理：

```bash
./build-ubsm/ubsm_shm_admin \
  --remove ubsm_micro_ab
```

如果查询不到本机 import 描述，工具仍会继续通过正式 API 尝试 owner-side deallocate。
成功时输出 `REMOVED name=ubsm_micro_ab`。`--remove` 对已经不存在的对象是幂等的，
会输出 `ALREADY_ABSENT` 并返回 0。删除是否解决底层同名 export 残留，最终以重新执行
owner create 不再返回 601 为准。

如果返回 `UBSM_ERR_IN_USING=6024`，说明仍有映射或引用，不能强制删除。先在两台机器上检查：

```bash
pgrep -af 'ubsm_(local_one_sided_test|one_sided_owner|one_sided_remote)'
```

管理工具只接受精确名称，不支持通配删除。不要用 `rm` 删除
`/dev/obmm_shmdev*`，也不要为了清理单个测试对象重启共享的服务。

### 9.2 正常清理和故障信息

正常双机清理顺序是：

```text
remote unmap
    -> 通知 owner
    -> owner unmap
    -> owner deallocate
    -> 两端 finalize
```

不要在 remote 仍映射时手动 deallocate。程序被 `SIGINT`、`SIGKILL`、机器掉电或服务异常
中止时，C++ 清理代码可能无法运行；ubsmd/UBS Engine 可以回收死亡进程的引用记录，但
具名共享内存对象仍可能保留。确认引用已经释放后，使用 `ubsm_shm_admin --remove` 清理。

失败后收集：

```bash
systemctl status ubse.service ubsmd --no-pager
journalctl -u ubse.service -b --no-pager | tail -n 200
journalctl -u ubsmd -b --no-pager | tail -n 200
dmesg | grep -iE 'obmm|ubse|ubus|ummu|alloc|huge|snoop' | tail -n 200
ls -l /dev/obmm /dev/obmm_shmdev* 2>/dev/null
cat /sys/bus/ub/ub_feature 2>/dev/null
```

常见错误：

- `ubsmem_initialize` 失败：优先检查 `ubsmd`、Unix socket 权限、
  `libobmm.so.1` 和 `libubse-client.so.1`；
- 返回 6025：检查 `/sys/bus/ub/ub_feature` Bit3 与 NC-CC 模式；
- allocate 内存不足：检查实际内存 NUMA 节点、hugepage 和 OBMM mempool；
- remote map 找不到名字：检查两端 ubsmd/UBS Engine 拓扑、RPC 地址、hostname、
  防火墙以及两端是否使用同一个 `--name`；
- remote map 权限失败：检查两端 UID/GID、`ubsmd` 用户组和共享对象的 `0660` 权限；
- owner deallocate 返回 in-use：remote 可能尚未成功 unmap/detach，先检查 remote 日志，
  不要反复强制删除。
