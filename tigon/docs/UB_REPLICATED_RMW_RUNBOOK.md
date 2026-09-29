# UB-Tigon 双副本 YCSB RMW：部署与验证待办

本页针对主项目 `tigon/` 中默认关闭的可选模式 `TIGON_UB_REPLICATED_RMW=1`。它让两台 UB 主机各持有完整的本地 WB 副本；写事务需同时更新两份。当前仅支持双机、`one-sided`、YCSB `rmw`、固定 key 集；不支持 TPCC、insert/delete 或故障恢复。原有运行方式保持 `TIGON_UB_REPLICATED_RMW=0`（或不设置）即可。旧版 cache-off/on 矩阵和 lookup profile 不在这里重跑。所有命令从两台机器各自的 `ub-application` 仓库根目录执行；不要在 spr4numa 上执行本页的 UB 命令。

## 1. 同步主项目代码并在两端编译

双副本代码已并入主目录 `tigon/`，不需要复制 `.experiments/`。推荐将这次 `tigon/` 的改动单独提交到 Git，再在两台 UB 机器检出**同一个提交**；在两端用 `git rev-parse HEAD` 核对。注意 `UB_TIGON_DESIGN_AND_PROGRAMMING_GUIDE.md` 当前被仓库 `.gitignore` 忽略，若要把它纳入提交，需要单独执行 `git add -f tigon/UB_TIGON_DESIGN_AND_PROGRAMMING_GUIDE.md`；不要使用 `git add .`，以免混入仓库中的其它实验文件。不要只同步脚本而漏掉 C++ 代码与新增头文件。若尚未提交，可在 Windows 项目根目录把当前完整的 `tigon/` 源码打包，再分别同步到两台机器的干净工作副本（不要覆盖旧 baseline）：

```powershell
$root = (Resolve-Path '.').Path
$tracked = @(git ls-files -- tigon)
$new = @(git ls-files --others --exclude-standard -- tigon)
$files = @($tracked + $new + 'tigon/UB_TIGON_DESIGN_AND_PROGRAMMING_GUIDE.md' |
  Where-Object { $_ -notmatch '^tigon/(results|formal-logs-collected)/' } |
  Sort-Object -Unique)
tar -czf tigon-replicated-rmw-src.tar.gz -C $root @files
tar -tzf tigon-replicated-rmw-src.tar.gz | Select-Object -First 20
scp tigon-replicated-rmw-src.tar.gz USER@HOST0:~/
scp tigon-replicated-rmw-src.tar.gz USER@HOST1:~/
```

两台 UB 机器各自执行 `mkdir -p "$HOME/ub-replicated-rmw" && tar -xzf "$HOME/tigon-replicated-rmw-src.tar.gz" -C "$HOME/ub-replicated-rmw"`，然后将下面的 `/path/to/ub-application` 换为该目录。归档只包含 `tigon/`，因此其他仓库组件仍需按原环境单独准备。不要沿用 x86-64 或旧 CXL 构建目录。

```bash
cd /path/to/ub-application
export TIGON_REPO_ROOT="$PWD"
export TIGON_BUILD_DIR="$TIGON_REPO_ROOT/tigon/build-ub-replicated-aarch64-gcc"

uname -m
file /usr/local/ubs_mem/lib/libubsm_sdk.so
ldd /usr/local/ubs_mem/lib/libubsm_sdk.so

cmake -S "$TIGON_REPO_ROOT/tigon" -B "$TIGON_BUILD_DIR" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DTIGON_ENABLE_UB=ON \
  -DTIGON_BUILD_UB_REPLICA_PROTOTYPE=ON \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so
cmake --build "$TIGON_BUILD_DIR" -j \
  --target bench_ycsb ub_tigon_two_node_test ub_replicated_tuple_test
ctest --test-dir "$TIGON_BUILD_DIR" -R '^ub_replicated_tuple_test$' \
  --output-on-failure

file "$TIGON_BUILD_DIR/bench_ycsb" "$TIGON_BUILD_DIR/ub_tigon_two_node_test"
ldd "$TIGON_BUILD_DIR/bench_ycsb"
bash -n tigon/scripts/run_ub_ycsb_matrix.sh
bash -n tigon/scripts/run_ub_two_node_compare.sh
python3 -m unittest \
  tigon.tests.test_ub_matrix_scripts.MatrixScriptTests.test_ub_ycsb_dry_run_contains_fairness_flags \
  tigon.tests.test_ub_matrix_scripts.MatrixScriptTests.test_replica_rejects_non_rmw \
  tigon.tests.test_ub_matrix_scripts.ParserTests
```

- [ ] 两端构建和本地单测通过；产物为 AArch64，`ldd` 没有 `not found`。本地单测不证明跨机 WB/NC 可见性。

上述 Python 命令只运行本次 UB YCSB/解析器相关用例；完整测试模块还依赖并排存在的 `tigon-spr4` 仓库，UB 机器没有该目录时不必运行整套。

## 2. 两端设置相同的实验参数

`UB_ID` 在 host 0/1 分别填 `0`/`1`；`HOST*_IP`、provider、NUMA 和 `REGION_MB` 用实际值替换。`SERVERS`、region 容量、数据规模、CPU/NUMA 策略和 campaign 在两端保持一致。选一个双方**手动设成相同值**且未使用过的 `CAMPAIGN_TAG`，不要分别用 `date` 生成；每次失败重试也换新值，避免重用旧 region。

```bash
cd /path/to/ub-application
export TIGON_REPO_ROOT="$PWD"
export TIGON_BUILD_DIR="$TIGON_REPO_ROOT/tigon/build-ub-replicated-aarch64-gcc"
export UB_ID=0                         # host 1 改为 1
export UB_PROVIDER_HOST=ACTUAL_PROVIDER_NAME
export SERVERS='HOST0_IP:10010;HOST1_IP:10010'
export REGION_MB=VALIDATED_REGION_MB   # 两端都已验证可创建/映射的大小
export CAMPAIGN_TAG=replica_trial01    # 两端相同；重试时换新值

# 确认实际拓扑后才设置；不确定时不要用占位值。
# export UB_PROVIDER_NUMA=ACTUAL_PROVIDER_NUMA
# export UB_NUMA_NODE=ACTUAL_RUN_NUMA
# export UB_TIGON_CPU_LIST=ACTUAL_CPU_LIST

export UB_COMPARE_THREADS=3
export UB_COMPARE_PARTITIONS=2
export UB_COMPARE_KEYS=300000
export UB_COMPARE_ZIPF=0.7
export UB_COMPARE_REGION_MB="$REGION_MB"
export UB_COMPARE_MODES=one-sided
export UB_COMPARE_TRANSPORTS=true
export UB_COMPARE_YCSB_QUERIES=rmw

export TIGON_UB_REPLICATED_RMW=1
export TIGON_UB_RMW_TUPLE_CACHE=0       # 双副本本地读，不兼容旧远端 offset cache
export TIGON_UB_SCALAR_READ=1
export TIGON_UB_POINT_PROFILE=0
export GLOG_logtostderr=1
```

不要直接 `sudo` 运行 benchmark；若环境要求提权，先确认上述变量会被完整保留。新模式在每台主机都存一整份数据，region 与实际物理内存余量要重新验证，不能沿用旧模式“单机只存一半”的容量判断。

## 3. 上机前检查：基础 UBSM 与容量

若当前内核、驱动、SDK 或 provider 尚未通过双机基础测试，先在两端使用相同的新 prefix，host 0 先启动、host 1 尽快跟上：

```bash
export UB_REGION_MB=128              # 仅基础 correctness；使用已验证容量
bash tigon/scripts/run_ub_correctness.sh \
  "$UB_ID" HOST0_IP one-sided "${CAMPAIGN_TAG}_correctness" 10000 18951
unset UB_REGION_MB
```

两端都应 `PASS`、退出码 0。此测试只检查基础单边共享内存，不覆盖新双副本写协议。

在两端运行以下只读检查，确认 provider、region 容量和宿主压力；若曾出现 `ret=800`/`6050` 或整机重启，**不要连续重试正式矩阵**，先查底层日志和资源限制：

```bash
free -h
numactl --hardware
cat /proc/pressure/memory
df -h .
journalctl -u ubsmd -b --no-pager | tail -n 100
journalctl -u ubse.service -b --no-pager | tail -n 100
dmesg | grep -iE 'obmm|ubse|oom|alloc|huge|memory|mempool' | tail -n 100
```

- [ ] 两端基础 correctness 通过，`REGION_MB` 在当前 provider 上已单独验证可创建/映射，且两端有容纳完整副本的余量。
- [ ] 若报 `ret=800`/`6050`，查看 `ubsmd`/UBSE 日志中的底层错误；不要把所有错误都当作“对端还没启动”。只在明确是短暂 `NOT_FOUND` 且最终成功时接受启动期重试。

## 4. 双机 smoke：先验证协议，再看吞吐

两端在各自终端（长任务建议各用 `tmux`）几乎同时启动**相同**命令。先以 `rw=95, cross=0/100` 各跑一次；任一端失败，双方都停止，不要继续推进到下一个 point。若先想最小化风险，可把 `UB_COMPARE_CROSS_RATIOS` 暂设为 `'0'`，成功后使用**新 campaign** 测 `'100'`。

```bash
export UB_COMPARE_FIRST_REP=1
export UB_COMPARE_REPETITIONS=1
export UB_COMPARE_RW_RATIOS='95'
export UB_COMPARE_CROSS_RATIOS='0 100'
export UB_COMPARE_WARMUP_SECONDS=5
export UB_COMPARE_RUN_SECONDS=10
export UB_RESULT_DIR="$TIGON_REPO_ROOT/tigon/results/${CAMPAIGN_TAG}_replica_smoke"

bash tigon/scripts/run_ub_two_node_compare.sh \
  ycsb "$UB_ID" "$SERVERS" "${CAMPAIGN_TAG}_replica_smoke"
```

本机日志检查（两端分别执行）：

```bash
grep -hE 'TIGON_RUN_META|UB_REPLICA_CHECKSUM|Global Stats: total_commit:|TIGON_RUN_END|SIGFPE|SIGSEGV|SIGABRT|CHECK failed|FATAL' \
  "$UB_RESULT_DIR"/ub_ycsb_*.log
```

- [ ] 每个 point 两端都有 `TIGON_RUN_END ... status=ok exit_code=0`，host 0 同一日志有 `Global Stats: total_commit:`；`TIGON_RUN_META` 记录 `replicated_rmw=1 rmw_tuple_cache=0`、同样的 workload 参数。
- [ ] 每个 point 两端都有 `UB_REPLICA_CHECKSUM count=600000 hash=...`，其中 `600000 = 2 partitions × 300000 keys`，两端 hash 完全一致；无崩溃、映射超时、校验或锁协议错误。不能只凭退出码判定双副本正确。
- [ ] 高冲突 `cross=100` 也稳定完成；两端检查 CPU、内存/OBMM 压力。若出现重启、写后另一端看不到更新或 checksum 不一致，停止性能测试，保存日志并定位 WB/NC 可见性与完成语义。

当前写完成采用写者侧远端 readback 试验，**尚未证明** peer WB 视图在所有硬件上及时可见；也尚无 mid-commit 故障恢复。若需正式的正确性结论，还要补做双端同 key 并发读/写、写/写、multi-key abort、提交中故障注入及重建测试。下面的矩阵仅在 smoke 通过后作为探索性性能数据。

## 5. 正式矩阵（仅通过 smoke 后）

两端同步换一个新 `CAMPAIGN_TAG`，保持同一份代码及第 2 节其它参数不变。矩阵为 `2` 种 RW 比例 × `5` 种 cross 比例 × `3` 次重复 = `30` 个双机 point；每 point 预热 30 秒、测量 30 秒。旧版 cache-off/on 数据已存在，不在这个 campaign 中混跑。

```bash
export CAMPAIGN_TAG=replica_formal01   # 两端同步设成相同的新值
export UB_COMPARE_FIRST_REP=1
export UB_COMPARE_REPETITIONS=3
export UB_COMPARE_RW_RATIOS='95 50'
export UB_COMPARE_CROSS_RATIOS='0 25 50 75 100'
export UB_COMPARE_WARMUP_SECONDS=30
export UB_COMPARE_RUN_SECONDS=30
export UB_RESULT_DIR="$TIGON_REPO_ROOT/tigon/results/${CAMPAIGN_TAG}"

bash tigon/scripts/run_ub_two_node_compare.sh \
  ycsb "$UB_ID" "$SERVERS" "$CAMPAIGN_TAG"
```

`tmux` 断线可重连；若必须中断，先协调两端停止，再用新 campaign/`UB_COMPARE_FIRST_REP` 与缩小后的参数只补缺失点，避免两端 point 错位或重用 region。不要对旧目录执行通配符清理。

## 6. 收集、解析、验收

将**两台机器**同一 campaign 的 `ub_ycsb_*.log` 复制到分析机同一目录；保留原文件名中的 `host0`/`host1`，不要混入 smoke、旧 campaign 或 spr4 日志。解析器只把 host 0 的 `Global Stats total_commit` 当作集群吞吐；不会再叠加 host 1。

```bash
python3 tigon/scripts/parse/parse_two_node_compare.py \
  /path/to/replica-formal-logs-collected \
  --raw-output /path/to/replica_two_node_raw.csv \
  --summary-output /path/to/replica_two_node_summary.csv

python3 - <<'PY'
import csv
with open('/path/to/replica_two_node_raw.csv', newline='') as f:
    rows = list(csv.DictReader(f))
with open('/path/to/replica_two_node_summary.csv', newline='') as f:
    summary = list(csv.DictReader(f))
bad = [r for r in rows if r['run_status'] != 'ok' or
       r['peer_log_found'] != 'yes' or r['peer_status'] != 'ok' or
       r['consistency_passed'] != 'yes' or r['replicated_rmw'] != '1' or
       r['replica_count'] != '600000' or not r['throughput_txn_s']]
print('raw rows:', len(rows), 'bad rows:', len(bad), 'summary groups:', len(summary))
for r in bad:
    print('BAD', r['run_id'], r['source_log'])
for r in summary:
    print('rw=', r['rw_ratio'], 'cross=', r['cross_ratio'],
          'samples=', r['samples'], 'median_txn_s=', r['throughput_median_txn_s'])
PY
```

- [ ] 正式 campaign 有 30 条 host 0 raw 行、`bad rows: 0`、10 个 summary 组且每组 `samples=3`；解析器会排除缺失/不一致的双副本点，缺组时先查原因，不用残缺样本下结论。
- [ ] 同时保存两端源码 commit/差异、编译参数、provider/NUMA/CPU、region 容量、campaign、原始日志和 CSV；记录每组吞吐 median/min/max、abort rate 与 `cross=0` 基准效率。与旧 UB、spr4 结果比较前，确认 workload、线程、region 和采样时长一致，并明确本模式仅是探索性试验。
- [ ] 保存运行前后 `free -h`、`numactl --hardware`、`cat /proc/pressure/memory`；有异常再保存对应时段 `vmstat 1`、`journalctl` 与 `dmesg`，不要无证据删除波动样本。

实现边界与协议说明见 `tigon/docs/UB_REPLICATED_RMW_EXPERIMENT.md`；UB 映射/`ret=800` 排查见 `tigon/docs/ub_tigon.md`。
