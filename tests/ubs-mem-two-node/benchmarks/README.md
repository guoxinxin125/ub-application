# UBS Memory 单边 CC benchmark

本目录只存放性能测量和单边 CC 语义验证程序。基础通路测试、双向 inbox
测试和清理工具仍位于上一级目录。机器环境检查、SDK 路径配置和完整故障排查请先阅读
[上一级 README](../README.md)。

## 1. 程序与测试点

| 程序 | 默认名称 | 默认端口 | 测试点 |
|---|---|---:|---|
| `ubsm_local_latency_test` | `ubsm_local_latency` | 无 | 本地8B/64B load/store及8B fetch-add/CAS延迟 |
| `ubsm_remote_rw_latency_test` | `ubsm_remote_rw_latency` | 18531 | remote NC 8B/64B load/store延迟 |
| `ubsm_remote_atomic_test` | `ubsm_remote_atomic` | 18532 | remote fetch-add、成功/失败 CAS 正确性 |
| `ubsm_owner_to_remote_cc_test` | `ubsm_owner_to_remote_cc` | 18533 | owner 不 flush 写，remote NC 读 |
| `ubsm_remote_to_owner_cc_test` | `ubsm_remote_to_owner_cc` | 18534 | remote NC 写，owner 不 invalidate 直接读 |

五个程序相互独立，分别使用自己的共享内存名称和端口，便于单独运行、定位失败并清理。

## 2. 编译

仍从上一级目录配置同一个构建树，产物也仍位于 `build-ubsm/`，源码移动不会改变运行命令：

```bash
cd tests/ubs-mem-two-node

cmake -S . -B build-ubsm \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so

cmake --build build-ubsm --target \
  ubsm_local_latency_test \
  ubsm_remote_rw_latency_test \
  ubsm_remote_atomic_test \
  ubsm_owner_to_remote_cc_test \
  ubsm_remote_to_owner_cc_test -j
```

## 3. 本地延迟测试

本测试显式指定当前机器为 provider：

```bash
numactl --cpunodebind=0 --membind=0 \
  ./build-ubsm/ubsm_local_latency_test \
    --provider-numa 0 \
    --name ubsm_local_latency_a \
    --region-mb 4 \
    --iterations 100000
```

该程序是本地性能的唯一入口，输出以下操作的平均延迟：

- 8B load、store issue、store fenced、fetch-add和CAS；
- 64B load，以及8次连续8B store后统一执行一次fence的完整缓存行store。

`--iterations` 是每个测试点执行的总操作轮数。每个测试点只在完整循环前后读取一次
时钟，再以总时间除以迭代次数，避免逐次计时开销淹没亚纳秒级热缓存访问。

输出名称包括：

```text
local_load_8b_avg_ns
local_store_issue_8b_avg_ns
local_store_fenced_8b_avg_ns
local_load_64b_avg_ns
local_store_fenced_64b_avg_ns
local_fetch_add_8b_avg_ns
local_cas_8b_avg_ns
```

`store_issue` 的fence位于计时区间之后；`store_fenced` 每次8B store后执行一次
`std::atomic_thread_fence(std::memory_order_seq_cst)`；64B store每轮先连续写入8个
`uint64_t`，再执行一次相同fence。它们衡量的是同一条热缓存行上的访问，不能解释为
冷内存或UB介质访问延迟。

## 4. 四个双机测试

先在机器 A 启动 owner：

```bash
numactl --cpunodebind=0 --membind=0 \
  ./build-ubsm/<TEST> \
    --role owner \
    --bind-ip 192.0.2.10 \
    --provider-numa 0 \
    --name <NAME> \
    --port <PORT> \
    --region-mb 4 \
    --test-bytes 4096 \
    --iterations 100000 \
    --atomic-iterations 1000 \
    --timeout-sec 120
```

再在机器 B 启动 remote。下面的 NUMA 1 只是示例，应替换为 remote 机器实际有内存且
适合目标 UB 设备的节点：

```bash
numactl --cpunodebind=1 --membind=1 \
  ./build-ubsm/<TEST> \
    --role remote \
    --owner-ip 192.0.2.10 \
    --name <NAME> \
    --port <PORT> \
    --region-mb 4 \
    --test-bytes 4096 \
    --iterations 100000 \
    --atomic-iterations 1000 \
    --timeout-sec 120
```

将 `<TEST>/<NAME>/<PORT>` 替换为第 1 节表中的对应值。当前测量方向是机器 B 访问
机器 A 的内存；反向测量时交换 owner/remote 角色，并使用新的共享内存名称。

`ubsm_remote_rw_latency_test` 输出：

```text
remote_nc_load_8b_avg_ns
remote_nc_store_issue_8b_avg_ns
remote_nc_store_fenced_8b_avg_ns
remote_nc_load_64b_avg_ns
remote_nc_store_fenced_64b_avg_ns
```

8B标量和64B缓存行分别使用独立且按64字节对齐的区域。64B load每轮读取8个
`uint64_t`；64B store每轮连续写入8个 `uint64_t` 后执行一次
`std::atomic_thread_fence(std::memory_order_seq_cst)`。remote完成测试后，owner会分别
校验8B最终值和完整64B缓存行，确认remote写入可被owner直接观察。

remote 使用 NC 映射，因此不执行软件 invalidate 或 writeback。两个 CC 测试分别验证：

- owner 写本地数据时不 flush，remote 仍能通过 NC 映射读到正确内容；
- remote 写 owner 的内存后，owner 不 invalidate 缓存也能直接读到正确内容。

remote 原子测试直接使用 GCC `__atomic_fetch_add` 和
`__atomic_compare_exchange_n`。若发生异常或最终值校验失败，应判定远端原子操作在
当前硬件或映射模式上不受支持，不能只依据延迟输出判断通过。

## 5. 清理

正常退出时程序会解除映射并释放对象。异常退出后，在该对象的 owner 机器上按精确名称清理：

```bash
./build-ubsm/ubsm_shm_admin --remove <NAME>
```

不要直接删除 `/dev/obmm_shmdev*`，也不要为清理单个测试对象重启共享服务。
