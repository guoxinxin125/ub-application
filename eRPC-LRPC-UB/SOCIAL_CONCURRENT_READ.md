# UserTimeline / HomeTimeline 多进程 Client 并发对照

比较两条完整路径，不独立混搭 LB 和 nginx：RPC 路径在 B 运行同一组 LB/nginx 进程；域路径由 A 上各 Client 执行 B 发布的 LB/nginx 服务域（默认 fused）。两种模式都在 A 运行同一个 Timeline 进程，在 B 运行 PostStorage，使用相同的配置、数据库、共同服务绑核、请求数和预热数；B 的 LB/nginx CPU 只在 RPC 模式被占用。此对照是**整体部署路径**的比较，不能将差值单独归因于 LB 或 nginx。每个独立 Client 进程固定一条 lane、`concurrency=1`；支持 2～4 个进程，4 个 Client 加 Timeline 共用 5 个域连接槽（当前池容量为 8）。

同步本次修改的 `eRPC/apps/social_network_cxl/{client,load_balance,nginx,domain_proxy.h,domain_proxy_runtime.h,social_network_commons.h,social_network_rpc_type.h,run_ub.sh}` 以及 `eRPC-LRPC-UB/scripts/{prepare-social.py,check-social-cpus.py,run-social.sh,check-social.py,compare-social.py}`、`eRPC-LRPC-UB/config/social-cpu-topology-shared.json` 到机器。两端配置必须由同一次生成得到。此次只改用户态，不更改内核模块 ABI；A 若已有匹配模块，无需重新 `insmod`。不要在共享机器上强制卸载忙碌模块。环境准备沿用 [UserTimeline](SOCIAL_USER_READ.md) 或 [HomeTimeline](SOCIAL_HOME_READ.md) 文档。

## 1. 生成配置与编译

`--samples` 是**每个 Client**的总请求数（含预热）。下例为 UserTimeline；HomeTimeline 将 `--workload` 改为 `home` 并使用单独输出目录。

首次运行新的 Client 数量建议先在独立输出目录用 `--samples 10`、运行时 `--warmup 0` 验证路由/退出；两种模式均通过后，再用 1100/100 正式测量。不要把 smoke 和正式轮次的日志混用。

```sh
cd "$REPO/eRPC-LRPC-UB"
python3 scripts/prepare-social.py --workload user --clients 2 --samples 1100 \
  --output "$REPO/eRPC-LRPC-UB/campaign-user-2clients" \
  --host-a A_IPV4 --host-b B_IPV4 --numa-a 0 --numa-b 1 --b-core-base 13
```

生成后 A 用 `config-a.json`，B 用 `config-b.json`。示例的 `--b-core-base 13` 使 B 侧 nginx/LB/PostStorage 使用 NUMA 内索引 `13/16/19`；换机器时须按 B 的实际 affinity 调整。如需 MongoDB 地址覆盖，仍使用 `--timeline-db-host/--timeline-db-port`；若 A 走原有 SSH 隧道，则保留默认数据库地址。生成器给各 Client 分配不同端口/RPC ID/三 CPU 预留块，Timeline 在它们之后。检查 A/B 实际允许的 NUMA CPU；若需调整 A 的 CPU，修改两份私有配置里的 `domain_proxy.clients[*].bind_core_offset` 和 Timeline 的 `bind_core_offset`（首个 Client 的 `client.bind_core_offset` 也同步改，避免配置含义不一致），两种模式须用同一组调整。

### 当前共享机器：4 Client 自动安排 CPU

`config/social-cpu-topology-shared.json` 记录了当前 A/B 各 NUMA 的 CPU 列表和你提供的允许列表；它是**静态快照**，若机器 affinity 变化须先更新它。生成器只使用同一 NUMA 中允许的三 CPU 块，B 启动前的 live preflight 仍会再次核验。此模式不要再加 `--b-core-base`。

```sh
cd "$REPO/eRPC-LRPC-UB"
python3 scripts/prepare-social.py --workload user --clients 4 --samples 1100 \
  --output "$REPO/eRPC-LRPC-UB/campaign-user-4clients" \
  --host-a A_IPV4 --host-b B_IPV4 --numa-a 0 --numa-b 1 \
  --cpu-topology config/social-cpu-topology-shared.json
```

本例输出 `A_clients=0,3,6,21 A_timeline=24 B_nginx=13 B_load_balance=16 B_post_storage=19`。这些是 **NUMA CPU 列表索引**，对应 A CPU `0–8,21–26`，B CPU `61–69`，没有使用 A 不允许的 `10–20` 或 B 不允许的 `50–60`。若改用 A NUMA2/B NUMA3，把参数改为 `--numa-a 2 --numa-b 3`；生成器会根据同一拓扑文件重新选块。A/B 要使用同一次生成的配置，并在各自机器上先运行 `python3 scripts/check-social-cpus.py --config <对应配置> --side A|B --mode rpc`。不得只在 A 上用其本机 affinity 代替 B 检查。

已配置好原文档的 CMake build 目录与 SDK/OBMM 后，在 A/B 重编译；A 的 Client 连接数校验和启动栅栏轮询已更新，B 的 LB/nginx 使用消息体中的全局 wire request 做多 Client 转发槽位与回程路由，因此 **A 两版 Client 和 B 的 LB/nginx 必须重编译**。无需为这次改动重建或更换内核模块，也不必重建 B 的 owner：

```sh
# A
cmake --build "$REPO/eRPC/cmake-build-social-ub-domain" -j2 --target client user_timeline home_timeline
cmake --build "$REPO/eRPC/cmake-build-social-ub-rpc" -j2 --target client user_timeline home_timeline
# B
cmake --build "$REPO/eRPC/cmake-build-social-ub-rpc" -j2 --target load_balance nginx post_storage
```

## 2. 运行 RPC 轮和 domain 轮

每轮 B 先启动；B 就绪后 A 再启动。以下命令在各自机器的实验终端执行；`source` 从本机 `config/social-env.local.sh` 载入环境。A/B 同轮用相同 `--run-id`，不同模式用不同 ID。关闭详细 profile。`--warmup 100` 表示每 Client 100 次预热、1000 次正式请求。

```sh
# B：RPC 轮
cd "$REPO/eRPC-LRPC-UB"
source scripts/social-env.sh --mode rpc --workload user --warmup 100 --run-id rpc-2c \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-2clients/config-b.json"
bash scripts/run-social-session.sh --side B --mode rpc --config "$SN_CONFIG"

# A：B 就绪后，在 A 的终端运行
cd "$REPO/eRPC-LRPC-UB"
source scripts/social-env.sh --mode rpc --workload user --warmup 100 --run-id rpc-2c \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-2clients/config-a.json"
bash scripts/run-social-session.sh --side A --mode rpc --config "$SN_CONFIG" --ssh-b USER@B
```

A 的各 Client 在 `SN_CLIENT_ARMED` 后由同一栅栏释放；4 Client 轮次有 `client-1.log` 至 `client-4.log`，B 仍只有一份 LB/nginx/PostStorage。A 会等待所有 Client 完成并正常结束 Timeline/manager。结束 RPC 轮后再分别在 B、A 运行域轮：把上述 `--mode rpc` 改为 `--mode domain`，`--run-id` 改为 `domain-2c`，配置不变。A 上预先安全加载匹配实验模块。无需隧道时去掉 `--ssh-b`。HomeTimeline 把两端 `--workload user` 改为 `home`。4 Client 轮次将上述运行/验收命令中的 `2c` 改为 `4c`、目录改为 `campaign-user-4clients`、`--clients 2` 改为 `--clients 4`；预热和总样本数保持两种模式一致。启动栅栏检查间隔已由 10 ms 缩短到 1 ms，但仍应在日志中核对 `SN_CLIENT_STARTED` 的时间差和 campaign 重叠时间。

## 3. 验收和对比

记录 A/B 两轮的四个日志路径：`RPC_A`、`RPC_B`、`DOMAIN_A`、`DOMAIN_B`。`social-env.sh` 的 `logs=` 会打印本机路径；将 B 的日志复制到 A 的独立分析目录（不要覆盖旧日志），再在 A 运行：

```sh
cd "$REPO/eRPC-LRPC-UB"
python3 scripts/check-social.py --workload user --clients 2 --samples 1100 \
  --mode rpc --logs "$RPC_A" --peer-logs "$RPC_B"
python3 scripts/check-social.py --workload user --clients 2 --samples 1100 \
  --mode domain --chain fused --logs "$DOMAIN_A" --peer-logs "$DOMAIN_B"
python3 scripts/compare-social.py --workload user --clients 2 --samples 1100 \
  --rpc-a "$RPC_A" --rpc-b "$RPC_B" --domain-a "$DOMAIN_A" --domain-b "$DOMAIN_B"
```

HomeTimeline 改 `--workload home`。检查器核对各 Client 的完成数、结果分类、运行区间重叠，以及域模式的独立连接和 Timeline 回程次数。对比脚本输出各 Client 的 p50/p99/avg、加权平均和并发区间吞吐；`avg_reduction_pct > 0` 才表示域模式平均延迟更低。总体 p50/p99 无法从各进程的分位数精确合成，所以不会伪造。真正的 UB 延迟还需在两台机器上实测。
