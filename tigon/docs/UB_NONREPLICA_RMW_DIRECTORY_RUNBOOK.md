# 非双副本 UB-Tigon：固定键 YCSB RMW 远端查找目录试验

本试验只改变 `one-sided + YCSB rmw + 非双副本` 的 **远端 key→tuple 地址定位**。新开关 `TIGON_UB_RMW_LOOKUP_DIR=1` 默认关闭；`TIGON_UB_RMW_TUPLE_CACHE=1` 仍保留旧的每线程 4096 项直接映射缓存。两个开关不能同时开启。双副本 `TIGON_UB_REPLICATED_RMW=1` 的目录和提交协议没有修改，不应在双副本运行时设置新开关。

目录第一次遇到远端分区时扫描该分区的 B+ 树，把当前可见的固定键映射为 requester 进程本地的、带 region generation 的 tuple offset。目录命中后不再遍历 B+ 树，但仍解析 generation、核对 tuple key/`valid`，并由原 TwoPLPasha 路径取得 tuple 锁、读取远端 value/版本。目录不缓存 value；普通写不改变 key→tuple 地址，所以不向其他 requester 推送缓存更新。缺项、无效 tuple 或 region 重建后回退远端树；generation 变化触发重建。**仅固定键 RMW 是本次支持的实验范围**；insert/delete、并发结构变更及故障恢复不在性能结论内。

## 1. 两台 UB 机器部署和编译

将本次改动同步到 98、99 两台 AArch64 UB 机器，同一源码版本。需要同步的文件见本次修改清单；不要只同步脚本而漏掉 `UBBPlusTreeAdapter.h` 或 `Database.h`。在两端分别设置本机真实值：

```bash
cd /path/to/ub-application
export TIGON_REPO_ROOT="$PWD"
export TIGON_BUILD_DIR="$TIGON_REPO_ROOT/tigon/build-ub-rmw-directory-aarch64"

# 两端不同：98 设 0，99 设 1；以下均替换为现场真实值。
export UB_ID=0
export UB_PROVIDER_HOST='YOUR_PROVIDER_NAME'
export SERVERS='HOST98_IP:10010;HOST99_IP:10010'
export UB_NUMA_NODE=0
export REGION_MB=VALIDATED_REGION_MB  # 替换为两端均已验证的数字容量
# 仅在确认 provider NUMA 后设置；它不一定等于进程内存 NUMA。
# export UB_PROVIDER_NUMA=0

cmake -S "$TIGON_REPO_ROOT/tigon" -B "$TIGON_BUILD_DIR" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DTIGON_ENABLE_UB=ON \
  -DTIGON_BUILD_UB_UNIT_TESTS=ON \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so
cmake --build "$TIGON_BUILD_DIR" -j \
  --target bench_ycsb ub_btree_host_test ub_adapter_compile_test
ctest --test-dir "$TIGON_BUILD_DIR" \
  -R '^(ub_btree_host_test|ub_adapter_compile_test)$' --output-on-failure
bash -n tigon/scripts/run_ub_ycsb_matrix.sh
bash -n tigon/scripts/run_ub_two_node_compare.sh
python3 -m unittest \
  tigon.tests.test_ub_matrix_scripts.MatrixScriptTests.test_ub_ycsb_dry_run_contains_fairness_flags \
  tigon.tests.test_ub_matrix_scripts.ParserTests.test_parser_keeps_remote_directory_variants_separate
```

若机器没有 `UB_PROVIDER_NUMA` 或 `UB_NUMA_NODE` 所设节点的可用内存，应改为现场已验证节点；不要沿用历史上的 98/99 NUMA 值。正式启动前在两端确认 `free -h`、`numactl --hardware` 和 region 容量。以下假设 `REGION_MB` 是**两端已单独验证可创建**的容量；不要因为曾跑通另一种数据布局就推断本次也有足够余量。

## 2. 两端共同实验参数

以下命令在两台机器各自终端执行；`CAMPAIGN_TAG`、`SERVERS`、所有 `UB_COMPARE_*` 值必须一致。`UB_ID`、provider 名与 NUMA 可以按机器实际情况不同。每次失败后用新 campaign，避免重用旧 region。建议在 `tmux` 内运行，两端尽量同时启动相同点。

```bash
export UB_COMPARE_THREADS=3
export UB_COMPARE_PARTITIONS=2
export UB_COMPARE_KEYS=300000
export UB_COMPARE_ZIPF=0.7
export UB_COMPARE_REGION_MB="$REGION_MB"
export UB_COMPARE_MODES=one-sided
export UB_COMPARE_TRANSPORTS=true
export UB_COMPARE_YCSB_QUERIES=rmw

export TIGON_UB_REPLICATED_RMW=0
export TIGON_UB_SCALAR_READ=1
export TIGON_UB_POINT_PROFILE=0
export GLOG_logtostderr=1
```

### 新目录单点检查

先在两端各用相同的 `CAMPAIGN_TAG` 跑一个跨分区点，确认完整扫描及正常退出。30 秒预热给首次扫描留时间；如果扫描未在预热内结束，正式矩阵需加长预热，而不是把建目录成本混入计量窗口。

```bash
export CAMPAIGN_TAG=rmwdir_smoke01
export TIGON_UB_RMW_TUPLE_CACHE=0
export TIGON_UB_RMW_LOOKUP_DIR=1
export UB_COMPARE_FIRST_REP=1
export UB_COMPARE_REPETITIONS=1
export UB_COMPARE_RW_RATIOS='95'
export UB_COMPARE_CROSS_RATIOS='100'
export UB_COMPARE_WARMUP_SECONDS=30
export UB_COMPARE_RUN_SECONDS=30
export UB_RESULT_DIR="$TIGON_REPO_ROOT/tigon/results/$CAMPAIGN_TAG"

bash tigon/scripts/run_ub_two_node_compare.sh \
  ycsb "$UB_ID" "$SERVERS" "$CAMPAIGN_TAG"
```

单点成功后先检查本机日志：

```bash
grep -Eh 'TIGON_RUN_META|UB_RMW_LOOKUP_DIR|Global Stats: total_commit:|TIGON_RUN_END|FATAL|CHECK failed|SIGSEGV|ret=800|6050' \
  "$UB_RESULT_DIR"/ub_ycsb_*.log
```

至少要求两端 `TIGON_RUN_END ... status=ok exit_code=0`、host 0 有非零 `Global Stats: total_commit:`，且 `TIGON_RUN_META` 显示 `rmw_tuple_cache=0 rmw_lookup_dir=1 replicated_rmw=0`。访问远端分区的进程应看到 `UB_RMW_LOOKUP_DIR ... entries=300000 expected=300000`；若只看到部分条目，查询仍会回退树，但**不能将该点作为“完整目录”结果**。目录只在首次访问远端分区时构建，未访问该分区的 host/点不一定打印对应行。发生映射错误、OOM 或机器重启时停止，不要连续自动重试；先检查两端资源及 UBSM 服务日志。

### 与旧缓存做同版本 A/B 正式矩阵

旧结果来自较早代码/运行时状态。为判断目录收益，建议两端在**同一代码版本**各跑一套独立 campaign，除下列开关之外保持参数一致；两个 campaign 不能同时运行，也不能共用 region prefix。

旧 4096 项缓存：

```bash
export CAMPAIGN_TAG=rmwcache_formal01
export TIGON_UB_REPLICATED_RMW=0
export TIGON_UB_RMW_TUPLE_CACHE=1
export TIGON_UB_RMW_LOOKUP_DIR=0
export UB_COMPARE_FIRST_REP=1
export UB_COMPARE_REPETITIONS=3
export UB_COMPARE_RW_RATIOS='95 50'
export UB_COMPARE_CROSS_RATIOS='0 25 50 75 100'
export UB_COMPARE_WARMUP_SECONDS=30
export UB_COMPARE_RUN_SECONDS=30
export UB_RESULT_DIR="$TIGON_REPO_ROOT/tigon/results/$CAMPAIGN_TAG"
bash tigon/scripts/run_ub_two_node_compare.sh \
  ycsb "$UB_ID" "$SERVERS" "$CAMPAIGN_TAG"
```

新完整目录（旧 campaign 结束后，两端都改成新标识）：

```bash
export CAMPAIGN_TAG=rmwdir_formal01
export TIGON_UB_REPLICATED_RMW=0
export TIGON_UB_RMW_TUPLE_CACHE=0
export TIGON_UB_RMW_LOOKUP_DIR=1
export UB_RESULT_DIR="$TIGON_REPO_ROOT/tigon/results/$CAMPAIGN_TAG"
bash tigon/scripts/run_ub_two_node_compare.sh \
  ycsb "$UB_ID" "$SERVERS" "$CAMPAIGN_TAG"
```

若暂时只关心读多写少，可把两个 campaign 的 `UB_COMPARE_RW_RATIOS` 都设为 `'95'`，但不要只改其中一端。两套正式矩阵均关闭 `TIGON_UB_POINT_PROFILE`；需要诊断缓存命中率时，再单独用新 campaign 开 profile，不将其吞吐并入正式结果。

## 3. 解析与核验

把两端同一 campaign 的 `.log` 放到同一结果目录后运行：

```bash
python3 tigon/scripts/parse/parse_two_node_compare.py "$UB_RESULT_DIR" \
  --raw-output "$UB_RESULT_DIR/two_node_raw.csv" \
  --summary-output "$UB_RESULT_DIR/two_node_summary.csv"

python3 - "$UB_RESULT_DIR/two_node_raw.csv" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1], newline="")))
for r in rows:
    print(r["run_id"], "host0=", r["run_status"],
          "host1=", r["peer_status"], "dir=", r["rmw_lookup_dir"],
          "txn/s=", r["throughput_txn_s"])
PY
```

解析器只计 host 0 的全局吞吐，并要求 host 1 同点成功；`rmw_lookup_dir` 进入汇总分组，避免新旧缓存混合。正式验收还需逐点核对 `UB_RMW_LOOKUP_DIR` 完整度、崩溃/映射错误及原始日志。与双副本不同，非双副本没有双端数据 checksum，解析成功**不等于**跨机一致性证明。若要查看查找成本，在独立诊断 campaign 设置 `TIGON_UB_POINT_PROFILE=1`，查看 `UB_LOOKUP_PROFILE scope=remote` 的 `cache_hits/cache_misses/tree_searches/cache_validate_avg_ns/tree_walk_avg_ns`；profile 日志包含预热期，不应直接视为稳态逐次延迟。

本次改动不修改双副本协议；双副本运行时保持 `TIGON_UB_RMW_LOOKUP_DIR=0`，使用原有 [`UB_REPLICATED_RMW_RUNBOOK.md`](UB_REPLICATED_RMW_RUNBOOK.md)。
