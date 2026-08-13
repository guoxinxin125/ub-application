# UBS Memory 单边 CC benchmark

本目录只存放性能测量和单边 CC 语义验证程序。基础通路测试、双向 inbox
测试和清理工具仍位于上一级目录。机器环境检查、SDK 路径配置和完整故障排查请先阅读
[上一级 README](../README.md)。

## 1. 程序与测试点

| 程序 | 默认名称 | 默认端口 | 测试点 |
|---|---|---:|---|
| `ubsm_local_latency_test` | `ubsm_local_latency` | 无 | 本地8B/64B load/store及8B fetch-add/CAS延迟 |
| `ubsm_remote_rw_latency_test` | `ubsm_remote_rw_latency` | 18531 | remote NC 8B/64B load/store延迟 |
| `ubsm_remote_atomic_test` | `ubsm_remote_atomic` | 18532 | remote 单进程 8B fetch-add/CAS平均延迟及正确性 |
| `ubsm_remote_atomic_multiprocess_test` | `ubsm_atomic_mp` | 18535 | 多个 remote 进程竞争同一 8B CAS tail 的正确性 |
| `ubsm_owner_remote_atomic_contention_test` | `ubsm_owner_remote_atomic` | 18537 | owner cacheable 与 remote NC 竞争同一 8B fetch-add/CAS word 的正确性 |
| `ubsm_remote_multithread_load_test` | `ubsm_remote_multithread_load` | 18536 | remote 多线程读取同一个8B地址的延迟与聚合吞吐 |
| `ubsm_owner_to_remote_cc_test` | `ubsm_owner_to_remote_cc` | 18533 | owner 不 flush 写，remote NC 读 |
| `ubsm_remote_to_owner_cc_test` | `ubsm_remote_to_owner_cc` | 18534 | remote NC 写，owner 不 invalidate 的正确性及首次 load 延迟 |

八个程序相互独立，分别使用自己的共享内存名称和端口，便于单独运行、定位失败并清理。

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
  ubsm_remote_atomic_multiprocess_test \
  ubsm_owner_remote_atomic_contention_test \
  ubsm_remote_multithread_load_test \
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
local_load_fenced_8b_avg_ns
local_store_fenced_8b_avg_ns
local_load_fenced_64b_avg_ns
local_store_fenced_64b_avg_ns
local_fetch_add_8b_avg_ns
local_cas_8b_avg_ns
```

load和store都在每轮完整尺寸访问后执行一次
`std::atomic_thread_fence(std::memory_order_seq_cst)`。64B操作每轮先连续访问8个
`uint64_t`，再执行一次fence。它们衡量的是同一条热缓存行上的访问，不能解释为
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
remote_nc_load_fenced_8b_avg_ns
remote_nc_load_fenced_64b_avg_ns
remote_nc_store_fenced_8b_avg_ns
remote_nc_store_fenced_64b_avg_ns
```

8B和64B使用相同的标量模板，每轮分别访问1个和8个连续 `uint64_t`；这里不是
NEON/vector测试。load和store都在每轮完成对应尺寸的访问后执行一次
`std::atomic_thread_fence(std::memory_order_seq_cst)`。remote完成全部测试后，owner会
校验8B和完整64B的最终值，确认remote写入可被owner直接观察。

remote 使用 NC 映射，因此不执行软件 invalidate 或 writeback。两个 CC 测试分别验证：

- owner 写本地数据时不 flush，remote 仍能通过 NC 映射读到正确内容；
- remote 写 owner 的内存后，owner 不 invalidate 缓存也能直接读到正确内容。

`ubsm_remote_to_owner_cc_test` 保留 `kOldSeed`/`kNewSeed` 的整块数据单次正确性验证，
然后进行逐轮 8B load 延迟测试。每轮 owner 先通过 TCP 请求本轮写入，remote 写入新的
约定序号值并执行 fence，再通过 TCP 通知写完；owner 收到通知后才开始计时 load，并
立即校验是否读到本轮约定值。remote 在收到下一轮请求前不会再次写入，因此无需 owner
忙等共享内存或回写共享确认变量。TCP 收发全部位于计时窗口之外。输出为：

```text
owner_cc_load_after_remote_store_fenced_8b_avg_ns
```

每轮计时包含 owner 的一次 8B load 和紧随其后的 `seq_cst` fence，不包含 remote store、
fence 或 TCP 通知时间，因此表示 remote 写完后 owner 读取新值的 load 延迟，而不是端到端
store-to-load 可见延迟。轮数由 `--iterations` 指定。

remote 原子测试直接使用 GCC `__atomic_fetch_add` 和
`__atomic_compare_exchange_n`。若发生异常或最终值校验失败，应判定远端原子操作在
当前硬件或映射模式上不受支持，不能只依据延迟输出判断通过。

`ubsm_remote_atomic_test` 使用 `--atomic-iterations` 作为两个计时循环的迭代次数，输出：

```text
remote_nc_fetch_add_8b_avg_ns
remote_nc_cas_8b_avg_ns
```

正确性验证word、fetch-add计时word和CAS计时word分别位于不同的64字节缓存行。
计时结束后owner会检查fetch-add和CAS的最终值都等于 `atomic-iterations`，并继续检查
原有的fetch-add返回值、成功CAS、失败CAS以及最终magic值，只有两端全部通过才输出PASS。

### 4.1 remote 多线程共享地址 load 测试

该程序在一个 remote 进程中创建多个线程，共享同一个 UBSM 映射，并让所有线程反复
load同一个8B地址，用于观察共享热点的load延迟是否随线程数增加。程序会自动测试
1、2、4……直到 `--max-threads`；如果最大线程数不是2的幂，也会额外测试该值。
每个线程执行 `--iterations` 次操作，因此线程增加时总操作量同步增加。

先在 owner 机器运行：

```bash
numactl --cpunodebind=0 --membind=0 \
  ./build-ubsm/ubsm_remote_multithread_load_test \
    --role owner \
    --bind-ip 192.0.2.10 \
    --provider-numa 0 \
    --name ubsm_remote_multithread_load \
    --port 18536 \
    --region-mb 4 \
    --max-threads 8 \
    --iterations 100000 \
    --timeout-sec 120
```

再在 remote 机器运行；应确保绑定的 NUMA 节点至少有足够的可用 CPU：

```bash
numactl --cpunodebind=1 --membind=1 \
  ./build-ubsm/ubsm_remote_multithread_load_test \
    --role remote \
    --owner-ip 192.0.2.10 \
    --name ubsm_remote_multithread_load \
    --port 18536 \
    --region-mb 4 \
    --max-threads 8 \
    --iterations 100000 \
    --timeout-sec 120
```

两端的 `--max-threads` 和 `--iterations` 必须一致。每次8B load后执行一次
`seq_cst` fence。owner在每种线程数开始前写入一个新的约定值；remote在计时前后
校验该值，并校验每个线程在计时循环中读取值的累加结果。输出格式为：

```text
threads=4 operation=remote_nc_shared_load_fenced_8b mean_thread_op_ns=... aggregate_mops=...
```

`mean_thread_op_ns` 是各线程平均每次load的耗时；`aggregate_mops` 是所有线程合计的
百万次load每秒。比较不同线程数的 `mean_thread_op_ns`，即可判断多个线程竞争访问同一
远端位置时，单次load延迟是否增长。

### 4.2 remote 多进程原子竞争正确性测试

owner 创建共享内存并等待所有 remote 进程注册；所有 remote
进程收到统一开始信号后，通过 CAS 竞争同一个 `tail`。`--producer-count`、
`--iterations`、`--name`、`--port` 和 `--region-mb` 必须在两端保持一致。

先在 owner 机器运行（NUMA 0 仅为示例，应替换为 UB provider 所在节点）：

```bash
numactl --cpunodebind=0 --membind=0 \
  ./build-ubsm/ubsm_remote_atomic_multiprocess_test \
    --role owner \
    --bind-ip 192.0.2.10 \
    --provider-numa 0 \
    --name ubsm_atomic_mp \
    --port 18535 \
    --region-mb 4 \
    --producer-count 4 \
    --iterations 100000 \
    --timeout-sec 120
```

再在 remote 机器启动四个独立进程（NUMA 1 同样仅为示例）：

```bash
for i in 0 1 2 3; do
  numactl --cpunodebind=1 --membind=1 \
    ./build-ubsm/ubsm_remote_atomic_multiprocess_test \
      --role remote \
      --owner-ip 192.0.2.10 \
      --name ubsm_atomic_mp \
      --port 18535 \
      --region-mb 4 \
      --producer-count 4 \
      --producer-id "$i" \
      --iterations 100000 \
      --timeout-sec 120 &
done
wait
```

remote 只映射 owner 创建的对象，因此不需要 `--provider-numa`。本测试也不接受
`--test-bytes` 或 `--atomic-iterations`：没有数据块读写阶段，CAS 次数直接由
`--iterations` 指定。若部署环境需要明确 provider 的 host、socket 或 port，可在
owner 命令中继续传入 `--provider-host`、`--provider-socket` 和 `--provider-port`。

### 4.3 owner cacheable 与 remote NC 原子竞争正确性测试

该测试只有两个进程：物理 owner 机器上的一个进程使用 cacheable 映射，remote 机器上的
一个进程使用 non-cacheable 映射。两端分别同时竞争同一个 fetch-add word 和同一个 CAS
word，每端执行 `--atomic-iterations` 次成功操作。

owner 机器：

```bash
numactl --cpunodebind=0 --membind=0 \
  ./build-ubsm/ubsm_owner_remote_atomic_contention_test \
    --role owner \
    --bind-ip 192.0.2.10 \
    --provider-numa 0 \
    --name ubsm_owner_remote_atomic \
    --port 18537 \
    --region-mb 4 \
    --test-bytes 4096 \
    --iterations 100000 \
    --atomic-iterations 100000 \
    --timeout-sec 120
```

remote 机器：

```bash
numactl --cpunodebind=1 --membind=1 \
  ./build-ubsm/ubsm_owner_remote_atomic_contention_test \
    --role remote \
    --owner-ip 192.0.2.10 \
    --name ubsm_owner_remote_atomic \
    --port 18537 \
    --region-mb 4 \
    --test-bytes 4096 \
    --iterations 100000 \
    --atomic-iterations 100000 \
    --timeout-sec 120
```

TCP 只负责阶段同步和回传校验数据，不参与被测原子操作。每轮成功操作取得的旧值都会被
保存并发送给 owner；owner 合并两端结果后，要求 fetch-add 和 CAS 的 ticket 均不重复、
不缺失并严格覆盖 `[0, 2 * atomic-iterations)`，共享 word 的最终值也必须等于该上界。
CAS 阶段还要求至少观察到一次竞争失败，否则测试会提示增大 `--atomic-iterations`。
该测试只检查正确性，不输出延迟。


## 5. 清理

正常退出时程序会解除映射并释放对象。异常退出后，在该对象的 owner 机器上按精确名称清理：

```bash
./build-ubsm/ubsm_shm_admin --remove <NAME>
```

不要直接删除 `/dev/obmm_shmdev*`，也不要为清理单个测试对象重启共享服务。
