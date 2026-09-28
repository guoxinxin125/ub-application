# UB-Tigon 双机 YCSB RMW 测试待办
## 0. 两端必须先确认的配置

- [ ] 为两套 A/B 实验选一个双方**手动设成相同值**的、此前未用过的 `CAMPAIGN_TAG`。不要在两端分别用 `date` 自动生成，时间差会让 region prefix 不匹配。

以下命令均从各自主机的 `ub-application` 仓库根目录执行。所有 `HOST*_IP`、provider、NUMA 和 `REGION_MB` 占位值都要替换。两端的 `SERVERS`、`REGION_MB`、benchmark 参数和 campaign 必须完全相同；仅 `UB_ID`、本机 provider/NUMA 不同。无需启动 spr4numa VM。

## 1. 每台 UB 主机构建与检查

```bash
cd /path/to/ub-application
export TIGON_REPO_ROOT="$PWD"
export TIGON_BUILD_DIR="$TIGON_REPO_ROOT/tigon/build-ub-aarch64-gcc"

uname -m
file /usr/local/ubs_mem/lib/libubsm_sdk.so
ldd /usr/local/ubs_mem/lib/libubsm_sdk.so

cmake -S "$TIGON_REPO_ROOT/tigon" -B "$TIGON_BUILD_DIR" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DTIGON_ENABLE_UB=ON \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so
cmake --build "$TIGON_BUILD_DIR" -j \
  --target bench_ycsb ub_tigon_two_node_test

file "$TIGON_BUILD_DIR/bench_ycsb" "$TIGON_BUILD_DIR/ub_tigon_two_node_test"
ldd "$TIGON_BUILD_DIR/bench_ycsb"
```

- [ ] 两端都编译成功，产物为 AArch64；`ldd` 无 `not found`。不要复制 x86-64/旧 CXL build cache 到 UB 机器。
- [ ] 可选：运行 `python3 -m unittest tigon.tests.test_ub_matrix_scripts`，确认 matrix/解析器回归测试通过。

## 2. 每台机器设置自己的环境

分别在两台主机的测试终端设置，下面 `UB_ID` 分别取 `0` 和 `1`。若 NUMA 配置未知，先不设 `UB_NUMA_NODE`/`UB_PROVIDER_NUMA`，但需记录实际配置；正式 A/B 两组必须一致。

```bash
cd /path/to/ub-application
export TIGON_REPO_ROOT="$PWD"
export TIGON_BUILD_DIR="$TIGON_REPO_ROOT/tigon/build-ub-aarch64-gcc"
export UB_ID=0                         # host 1 改为 1
export UB_PROVIDER_HOST=ACTUAL_PROVIDER_NAME
export UB_PROVIDER_NUMA=ACTUAL_PROVIDER_NUMA
export UB_NUMA_NODE=ACTUAL_RUN_NUMA
export SERVERS='HOST0_IP:10010;HOST1_IP:10010'
export REGION_MB=VALIDATED_REGION_MB
export CAMPAIGN_TAG=ubcmp_run01       # 两端相同；重跑时换新值

export UB_COMPARE_THREADS=3
export UB_COMPARE_PARTITIONS=2
export UB_COMPARE_KEYS=300000
export UB_COMPARE_ZIPF=0.7
export UB_COMPARE_REGION_MB="$REGION_MB"
export UB_COMPARE_MODES=one-sided
export UB_COMPARE_TRANSPORTS=true
export UB_COMPARE_YCSB_QUERIES=rmw
export TIGON_UB_SCALAR_READ=1         # 两组都使用新版 scalar value read
export TIGON_UB_POINT_PROFILE=0       # 正式吞吐测试关闭采样计时
export GLOG_logtostderr=1             # 先用 smoke 验证 glog 统计确实进入结果日志
```

`UB_TIGON_CPU_LIST` 是可选的具体 CPU list；如设置，两端分别选择自己的 CPU，A/B 期间保持不变。不要用 `sudo` 启动 benchmark，除非已经确认需要且 `sudo` 会保留这些环境变量。

## 3. 先跑双机 correctness

在两端使用相同的新 prefix，先启动 host 0，再尽快启动 host 1：

```bash
export UB_REGION_MB=128  # 仅 correctness；必须是两端已验证可创建的大小
bash tigon/scripts/run_ub_correctness.sh \
  "$UB_ID" HOST0_IP one-sided "${CAMPAIGN_TAG}_correctness" 10000 18951
unset UB_REGION_MB
```

- [ ] 两端均输出 `PASS`、退出码均为 0。若 correctness 失败，停止后续性能测试，先排查 UBSM/provider/权限/连接。

## 4. Smoke：缓存关闭与开启各跑 `rw=95, cross=0/100`

以下各 campaign 都要在两端**几乎同时**启动相同命令；脚本在两端按同样顺序逐 point 推进。一端中途失败时，不要让另一端继续跑下一点；检查原因后换一个新的 `CAMPAIGN_TAG` 重试。

```bash
export UB_COMPARE_REPETITIONS=1
export UB_COMPARE_FIRST_REP=1
export UB_COMPARE_RW_RATIOS='95'
export UB_COMPARE_CROSS_RATIOS='0 100'
export UB_COMPARE_WARMUP_SECONDS=5
export UB_COMPARE_RUN_SECONDS=10      # 测量 10 秒；time_to_run 实际为 15 秒

export TIGON_UB_RMW_TUPLE_CACHE=0
export UB_RESULT_DIR="$TIGON_REPO_ROOT/tigon/results/${CAMPAIGN_TAG}_smoke_off"
bash tigon/scripts/run_ub_two_node_compare.sh \
  ycsb "$UB_ID" "$SERVERS" "${CAMPAIGN_TAG}_smoke_off"
```

缓存关闭组两端全部通过后，在**两台机器都**执行缓存开启组：

```bash
export TIGON_UB_RMW_TUPLE_CACHE=1
export UB_RESULT_DIR="$TIGON_REPO_ROOT/tigon/results/${CAMPAIGN_TAG}_smoke_on"
bash tigon/scripts/run_ub_two_node_compare.sh \
  ycsb "$UB_ID" "$SERVERS" "${CAMPAIGN_TAG}_smoke_on"
```

- [ ] 两组各 2 个 point、每个 point 两端均有 `TIGON_RUN_END ... status=ok exit_code=0`。
- [ ] 日志 `TIGON_RUN_META` 分别记录 `rmw_tuple_cache=0/1`，其他参数相同。
- [ ] host 0 的**同一个 point 日志**同时含 `TIGON_RUN_META`、`Global Stats: total_commit:` 和成功结束标记；host 1 有对应成功结束标记。若 `Global Stats` 只在 `/tmp/bench_ycsb.*.log.INFO.*`，先解决 glog 收集问题再跑正式矩阵；不要把没有统计的日志直接交给解析器。
- [ ] 检查无 `SIGFPE`、`SIGSEGV`、`CHECK failed`、`FATAL`、`0 entries (each 0 Bytes)` 等异常。`ret=606` 若后续重试成功且两端正常结束，不应单独判失败。

快速查看本机日志：

```bash
grep -hE 'TIGON_RUN_META|Global Stats: total_commit:|TIGON_RUN_END|SIGFPE|SIGSEGV|CHECK failed|FATAL' \
  "$UB_RESULT_DIR"/ub_ycsb_*.log
```

## 5. 正式矩阵：两套独立 campaign

保持两端代码、CPU/NUMA、region、数据规模、scalar read 和其它参数不变。每套为 `2` 种 RW 比例 × `5` 种 cross 比例 × `3` 次重复 = `30` 个双机 point；两套共 `60` 个双机 point。每 point 预热 30 秒、测量 30 秒。优先依次跑 cache-off、cache-on；若机器背景负载有明显变化，记录时间和资源状态，不要随意丢弃样本。

```bash
export UB_COMPARE_REPETITIONS=3
export UB_COMPARE_FIRST_REP=1
export UB_COMPARE_RW_RATIOS='95 50'
export UB_COMPARE_CROSS_RATIOS='0 25 50 75 100'
export UB_COMPARE_WARMUP_SECONDS=30
export UB_COMPARE_RUN_SECONDS=30
export TIGON_UB_POINT_PROFILE=0

export TIGON_UB_RMW_TUPLE_CACHE=0
export UB_RESULT_DIR="$TIGON_REPO_ROOT/tigon/results/${CAMPAIGN_TAG}_formal_off"
bash tigon/scripts/run_ub_two_node_compare.sh \
  ycsb "$UB_ID" "$SERVERS" "${CAMPAIGN_TAG}_formal_off"
```

确认两端 cache-off 完成后，两台机器均执行：

```bash
export TIGON_UB_RMW_TUPLE_CACHE=1
export UB_RESULT_DIR="$TIGON_REPO_ROOT/tigon/results/${CAMPAIGN_TAG}_formal_on"
bash tigon/scripts/run_ub_two_node_compare.sh \
  ycsb "$UB_ID" "$SERVERS" "${CAMPAIGN_TAG}_formal_on"
```

长时间运行建议两端各在 `tmux` 会话中执行；断开 SSH 不要杀掉运行中的 benchmark。需要中断时协调两端一起停止，并以新 campaign 跑未完成的点，避免两端 point 错位和 region prefix 重用。

## 6. 收集、解析与验收

把两台机器的 `formal_off`、`formal_on` 日志复制到**同一台分析机器**的同一收集目录，保留原有文件名中的 `host0`/`host1`。不要只收 host 0。解析器递归读取该目录；不要把 smoke/profile 或旧 campaign 混进去：

```bash
python3 tigon/scripts/parse/parse_two_node_compare.py \
  /path/to/formal-logs-collected \
  --raw-output /path/to/two_node_raw.csv \
  --summary-output /path/to/two_node_summary.csv
```

- [ ] `two_node_raw.csv` 中每个 `run_id` 的 host 0 行都显示 `run_status=ok`、`peer_log_found=yes`、`peer_status=ok`，有非空吞吐。
- [ ] `two_node_summary.csv` 应有 `20` 组：10 个 workload point × 2 个缓存状态；每组 `samples=3`。若不足，先定位缺失/失败日志，不用残缺结果下结论。
- [ ] 用 `throughput_median_txn_s` 比较绝对吞吐；用 `remote_efficiency_vs_local` 比较各缓存状态内部以 `cross=0` 为基准的相对吞吐。不能把 host 1 吞吐再加到 host 0：host 0 的 `Global Stats total_commit` 已是集群值。
- [ ] 同时记录 `throughput_min_txn_s`、`throughput_max_txn_s`、`abort_rate_mean` 和主机负载/内存压力；异常波动先关联运行日志与机器状态，再决定是否补跑。

## 7. 单独计时诊断（正式矩阵之后，可选但建议）

当前先定位 lookup 内部瓶颈，不做 cache-off/on 对照。两台机器都同步新代码、重新编译并运行同一个 `rw=95, cross=100` 单点；沿用当前 cache-on 配置：

```bash
export UB_COMPARE_REPETITIONS=1
export UB_COMPARE_RW_RATIOS='95'
export UB_COMPARE_CROSS_RATIOS='100'
export UB_COMPARE_WARMUP_SECONDS=5
export UB_COMPARE_RUN_SECONDS=10
export TIGON_UB_POINT_PROFILE=1

export TIGON_UB_RMW_TUPLE_CACHE=1
export UB_RESULT_DIR="$TIGON_REPO_ROOT/tigon/results/${CAMPAIGN_TAG}_lookup_profile"
bash tigon/scripts/run_ub_two_node_compare.sh \
  ycsb "$UB_ID" "$SERVERS" "${CAMPAIGN_TAG}_lookup_profile"
```

查看两端日志中的 `UB_POINT_PROFILE` 和 `UB_LOOKUP_PROFILE`。后者给出地址缓存命中/未命中/失效次数，以及 `cache_probe`、`cache_validate`、`tree_prepare`、`tree_lock`、`tree_walk`、`tree_leaf`、`tree_tuple` 的平均 ns 和树内部层数。树阶段均值以 `tree_searches` 为分母，缓存校验以命中加失效次数为分母；这些条件均值不能直接相加当作总 lookup 延迟。采样周期为 256，`phase=all_including_warmup`，只用于定位开销；不要与关闭 profile 的正式吞吐样本合并。远端 value copy 暂不优化。

## 8. 最终需要带回的数据

- [ ] 两端 `formal_off` 与 `formal_on` 的全部 `ub_ycsb_*.log`；可附 smoke/profile 日志，但与正式结果分目录。
- [ ] `two_node_raw.csv`、`two_node_summary.csv`，以及两端使用的 commit、构建参数、provider/NUMA/CPU、region 容量和 campaign 名。
- [ ] 每套至少记录运行前后的 `free -h`、`numactl --hardware`、`cat /proc/pressure/memory`；如有异常，再保存对应时段的 `vmstat 1` 与 UBSE/内核报错。

相关入口：`scripts/run_ub_two_node_compare.sh`、`scripts/run_ub_ycsb_matrix.sh`、`scripts/parse/parse_two_node_compare.py`、`docs/ub_tigon.md`。
