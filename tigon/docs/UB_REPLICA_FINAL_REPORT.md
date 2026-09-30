# UB-Tigon 双副本＋目录方案：两节点 YCSB RMW 实验报告

截至 2026-09-29。本报告将 **UB 双副本＋目录**作为当前固定键集 YCSB RMW 的验收方案，记录实现、实验条件和已收集的结果。它不是 Tigon 论文八节点实验的复现，也不是 UB 与 CXL 硬件的等条件性能比较。

## 1. 问题与实验目标

原始 UB-Tigon 的单边模式把分区的 B+ 树、tuple 和锁放在 owner 的共享 region。跨分区事务在 requester 上逐层读取远端 B+ 树，再访问远端 tuple；owner CPU 不执行读请求。对 UB importer 的 non-cacheable 映射而言，重复的远端索引读取和 tuple 读取会累积很高延迟。此前的 requester 本地 key→tuple-offset 缓存消除了部分树遍历，但读 value 仍然需要远端访问。

本实验要回答两个问题：① 能否利用 UB 的**跨机单边 load/store/原子操作**，让每台机器直接维护两份副本而不把每次读变成远端 RPC；② 在固定键集读多写少的 RMW 工作负载下，吞吐能否随着 `cross_ratio` 增加仍保持稳定。这里的“单边 CC”指跨机共享内存及原子/一致性能力，不表示两份数据自动原子提交；双副本的锁、写入完成和版本发布仍由软件协议负责。没有这种单边访问能力也可以设计复制协议，但通常要让对端 CPU 处理复制消息，**不能直接套用本方案的同步路径和开销假设**。

## 2. 实验环境与实现

| 系统 | 部署和共享内存 | 本次测量的 Tigon 路径 |
| --- | --- | --- |
| spr4numa | 一台 x86-64 主机上的两台 QEMU/KVM VM，每台 5 vCPU、10 GiB guest RAM；64 GiB `ivshmem-doorbell` 共享 BAR 的 tmpfs backing 绑在 NUMA node 5。node 5 是 CXL/DAX 上线为 system-RAM 的 memory-only node，但 guest 访问的是 ivshmem BAR。 | `TwoPLPasha + Clock + WriteThrough`，200 MiB `hw_cc_budget`。单个 BAR 在两 VM 均为 WB；SCC 路径以 `clflush`/`clwb`/fence 显式维护，HWCC 元数据路径不变。这是 **WB+维护模拟 SCC 非缓存访问**，不是真正的 UC SCC 映射。每个实验点使用新 VM。 |
| UB 98/99 | 两台独立的 AArch64 UB 机器；各自在本地 UB region 保存完整数据副本，并导入对端 region。本地 owner 映射为 WB，远端 importer 在 `one-sided` 模式下为 NC。 | `bench_ycsb --query=rmw`，`one-sided/ubq`，`TIGON_UB_REPLICATED_RMW=1`、`TIGON_UB_REPLICA_LOOKUP_DIR=1`、`TIGON_UB_RMW_TUPLE_CACHE=0`。UB 的 CPU/NUMA 绑核、SDK/固件和链路时钟未随本批 CSV 保存，不能据此断言两平台硬件等价。 |

UB 双副本的每台机器都创建**所有分区**的本地 B+ 树和 tuple，而不只保存自己原来负责的分区。初始化值采用相同种子；两个 region 的 tuple offset 不假定相等。读事务只访问本地 WB 副本，并在本地取得 tuple 共享锁、检查 `valid` 和读取 value。写事务先通过本地树找到本地 tuple，再找到对端对应 tuple；按 region ID 固定顺序尝试取得**两份 tuple 的写锁**，检查有效位和版本一致，分别写本地和对端 value，确认远端写完成后再发布相同的新版本并解锁。锁获取失败会使事务 abort/retry；写入后若完成检查失败，进入 in-doubt 状态，不能把两份锁当作安全提交而释放。

目录优化针对固定键集 YCSB：每个表首次需要写对端时扫描一次对端 B+ 树，在 requester 的进程本地建立密集 `key→带 generation 的 peer tuple offset` 目录。后续写命中目录时仍解析当前 region generation，并检查 tuple key/`valid`；目录**不缓存 value 或锁**，失效/未命中回退对端树。这样多数读不再访问对端，多数写也不再反复遍历对端树；写的跨机锁、数据更新和版本发布并未省略。形式化矩阵采用 30 秒预热，以容纳首次建目录成本。

当前 `UBCheckedReplicaWriteOps` 对远端 NC 写执行 fence 和读回比对，版本发布后再读回校验。读回是 writer 的 importer 视角检查，不等于对所有故障、掉电和平台实现证明持久/可见；本方案目前只验收**正常运行、固定键集、两节点**路径。insert/delete、故障中途恢复、region 重建后的副本重同步和严格线性化证明不在本报告结论内。原有非双副本模式保持可选，双副本和目录均可通过配置关闭。

## 3. 实验设计与统计口径

三组数据都测试 YCSB `rmw`，2 个分区、每节点 3 个 worker、每分区 300,000 keys（全局 600,000）、Zipf 0.7、`rw_ratio` 为 95 或 50、预热 30 秒加计量 30 秒、关闭持久日志。`rw_ratio=95` 表示读占比高，`50` 表示写更频繁；`cross_ratio` 是**跨分区事务配置比例**，不是逐次远端内存访问比例。UB 双副本已经改变跨分区事务的物理访问路径，不能把各系统相同 `cross_ratio` 理解为相同的远端访问次数。

最终 UB 方案的 rw95 测了 cross 0/25/50/75/100，各 3 轮；rw50 测了 0/50/100，各 3 轮。非双副本 UB 采用旧 `one-sided/ubq` 且 `TIGON_UB_RMW_TUPLE_CACHE=1` 的正式数据；spr4 采用 SCC-WB 正式矩阵及控制点补跑。表中吞吐为每点各轮 `Global Stats: total_commit` 的**中位数**，单位 txn/s，不是 30 秒总提交数；括号内为本系统同一 rw 下相对于 cross=0 的吞吐百分比。UB 解析只取 host 0 的全局统计，要求 host 1 同点成功结束；双副本点还要求两端 checksum 的 count/hash 相等且 count=600,000，**不将两个 host 的 `Global Stats` 相加**。

## 4. 实验结果

### 4.1 读多写少：rw=95

| Cross (%) | UB 双副本＋目录 | 非双副本 UB（tuple-offset 缓存开） | spr4 SCC-WB 仿真 |
| ---: | ---: | ---: | ---: |
| 0 | 217,506（100.0%） | 330,311（100.0%） | 296,738（100.0%） |
| 25 | 215,639（99.1%） | 68,096（20.6%） | 214,803（72.4%） |
| 50 | 215,015（98.9%） | 38,987（11.8%） | 170,468（57.4%） |
| 75 | 214,478（98.6%） | 26,654（8.1%） | 143,375（48.3%） |
| 100 | 212,974（97.9%） | 20,947（6.3%） | 123,615（41.7%） |

双副本＋目录从 cross=0 到 100 仅下降约 2.1%；旧非双副本 UB 降至 6.3%，spr4 降至 41.7%。在 cross=100，当前 UB 方案的观测中位数约为旧非双副本 UB 的 **10.2 倍**、spr4 的 **1.72 倍**。但 cross=0 时，双副本仅为非双副本 UB 的约 65.8%：复制和双锁/完成检查不是免费操作。旧的“已有双副本、尚未加入目录”rw95 数据为 cross=0/50/100 的 129,066/124,986/122,817 txn/s；新方案在对应点分别提高约 68.5%/72.0%/73.4%。这是一组跨 campaign 对比，支持目录消除远端查树成本的解释，但不能单独量化每个内部步骤的因果贡献。

### 4.2 写更频繁：rw=50

| Cross (%) | UB 双副本＋目录 | 非双副本 UB（tuple-offset 缓存开） | spr4 SCC-WB 仿真 |
| ---: | ---: | ---: | ---: |
| 0 | 41,768（100.0%） | 107,280（100.0%） | 87,285（100.0%） |
| 50 | 40,092（96.0%） | 31,040（28.9%） | 72,436（83.0%） |
| 100 | 40,128（96.1%） | 18,463（17.2%） | 63,676（73.0%） |

rw50 的目录方案同样抑制了 cross 增加带来的吞吐塌陷，但绝对吞吐在 cross=0 只有非双副本 UB 的约 38.9%，在 cross=100 只有 spr4 的约 63.0%。相比之下，读占 95% 时方案收益显著。因此当前证据支持将其定位于**读多写少、固定键集**场景，而不能宣称普遍优于旧模式。rw50 的 cross=25/75 未在本批最终 UB 目录矩阵中测量，不作插值或推断。

### 4.3 有效性与波动

最终 UB 两批 CSV 合计 24 个 host-0 点，均记录 `run_status=ok`、`peer_status=ok`、`consistency_passed=yes`、`replica_count=600000`；校验 hash 是逐轮比较两端，**不同轮的 hash 本来可以不同**。rw95 当前方案的 cross=0/100 三轮范围分别为 217,447–217,608 与 212,215–212,980 txn/s，较稳定；cross=50/75 的第三轮偏高，故使用中位数并保留原始 CSV，不随意剔除。spr4 的 rw95 cross=0/100 经控制点补跑后各 7 轮，其余点 3 轮；非双副本 UB 的 rw95 cross=0 只有 2 个正式样本，其余本报告所列点为 3 轮。目录已在单独 profile 日志中观察到 `entries=300000 expected=300000`，但本批仅收集 CSV 而非每轮完整双端日志，**不能逐点独立审计目录填充计数**；正式 CSV 也尚未把 `replica_lookup_dir` 写成独立列。

## 5. 结果解释与结论边界

曲线接近平坦的直接机制是读走本地 WB 副本，写用单边操作更新两份副本；目录又把大多数写的“远端 B+ 树逐层查找”变为本地 offset 查询。单独短时 profile 曾显示 peer lookup 平均延迟明显下降，但其 `phase=all_including_warmup` 包括首次扫描，不能当作稳态逐次命中延迟。本报告以正式 30+30 秒矩阵的端到端吞吐作为性能主证据。

写占比增大后，每次写都需两份锁、两次数据写、远端读回、两份版本发布和完成检查；这些操作仍跨 UB。rw50 的较低绝对吞吐与此机制相符，但**尚无隔离实验证明**各步骤分别贡献多少。另需注意 spr4 的共享 BAR 是单主机两 VM 的 WB+flush 近似，UB 是两台 AArch64 物理机的 WB owner/NC importer；CPU、虚拟化、内存属性、互连及同步语义都不同。因此两平台的绝对值仅是“在各自当前部署下观察到的吞吐”，更可靠的横向信息是同一系统内对 cross 的敏感度，不能据此宣称 UB 硬件本身比 CXL 更快。

**当前验收结论：**在已测的两节点、固定键集 YCSB RMW、30 秒计量窗口下，UB 双副本＋目录方案通过两端正常退出及最终 checksum 一致检查；rw95 的 cross=100 吞吐仍保留本方案 cross=0 的 97.9%，显著改善此前远端读取方案的 cross 敏感性。它牺牲了额外内存和写路径成本，适合优先作为读多写少场景的 UB-Tigon 运行方案。故障恢复、动态键集与严格跨机可见性/线性化还需专项验证，不纳入本次验收声称。

## 6. 数据与实现依据

- 最终 UB：[`../replica-cache/replica_two_node_raw.csv`](../replica-cache/replica_two_node_raw.csv)、[`../replica-cache/replica_two_node_summary.csv`](../replica-cache/replica_two_node_summary.csv)；[`../replica-cache50/replica_two_node_raw.csv`](../replica-cache50/replica_two_node_raw.csv)、[`../replica-cache50/replica_two_node_summary.csv`](../replica-cache50/replica_two_node_summary.csv)。原始双端日志未全部纳入此仓库；[`../replica-cache/meta.txt`](../replica-cache/meta.txt) 仅为部分元信息摘录。
- 旧 UB：[`../formal-logs-collected/two_node_summary.csv`](../formal-logs-collected/two_node_summary.csv) 中 `rmw_tuple_cache=1`、30+30 秒行；旧双副本无目录：[`../replica_two_node_summary.csv`](../replica_two_node_summary.csv)。
- spr4：[`../../tigon-spr4/sccwbformal0926-controls-review.tar.gz`](../../tigon-spr4/sccwbformal0926-controls-review.tar.gz) 内 `two_node_summary_controls.csv`；实现边界见 [`../../tigon-spr4/SCC_WB_EMULATION.md`](../../tigon-spr4/SCC_WB_EMULATION.md)。
- UB 协议与运行：[`../common/UBReplicatedTuple.h`](../common/UBReplicatedTuple.h)、[`../core/UBBPlusTreeAdapter.h`](../core/UBBPlusTreeAdapter.h)、[`UB_REPLICATED_RMW_EXPERIMENT.md`](UB_REPLICATED_RMW_EXPERIMENT.md)、[`UB_REPLICATED_RMW_RUNBOOK.md`](UB_REPLICATED_RMW_RUNBOOK.md)；统一解析器为 [`../scripts/parse/parse_two_node_compare.py`](../scripts/parse/parse_two_node_compare.py)。
