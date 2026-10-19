# UB 真机实验待办

这里记录接下来要在两台 UB 机器上执行的实验。A 是运行 caller 和域切换模块的机器，
B 是发布服务域的 owner 机器；当前可用组合是 A 计算 NUMA 0 / B provider NUMA 1。
具体 CPU、主机名与 SDK 路径以当天检查结果为准。完整构建、模块与首次上机步骤见
[README.md](README.md)，UserTimeline 应用步骤见 [SOCIAL_USER_READ.md](SOCIAL_USER_READ.md)。

## 1. Direct / Nested 稳态延迟

- [x] 2026-10-19 完成三条路径各 20 轮测量，见
  [summary.json](results/bench-20261019-a0-b1/summary.json) 和
  [rounds.csv](results/bench-20261019-a0-b1/rounds.csv)。后续 Nested 默认使用 fused，
  legacy 用于显式对照/备选；批量脚本继续同时测两条路径。
- [ ] 补存本批 60 份原始 `round-*.log` 与同时间窗内核日志，供独立重算总体分位数和验收。

- [ ] A/B 同步同一份源码并运行 `python3 scripts/manifest.py --check`；记录 `uname -r`、
  模块类型（EL1 或 VHE）、`modinfo -F ud_nested`、是否启用 trace、A 的允许 CPU、B 的 provider NUMA。
- [ ] 使用不带 `--trace` 的已验证模块，并在 A 单次检查 Direct、Nested legacy、Nested fused
  都通过。更换模块前按 [README 的模块步骤](README.md#nested-默认-fused-与备选-legacy)
  确认本项目连接已退出；不要在其他用户使用设备时卸载模块。
- [ ] 在 B 使用独占的共享内存名称启动可复用 owner，确认输出 `UB_DOMAIN_OWNER_READY multi=1`：

  ```sh
  cd /absolute/path/ub-application/eRPC-LRPC-UB
  ./build-owner/lrpc-ub-domain-owner lrpc_ub_domain --provider-numa 1 --multi
  ```

- [ ] 在 A 同目录、允许的 NUMA 0 CPU 上执行 20 轮。每轮对每条路径预热 100 次、
  测量 1000 次，共 60 个连接、60,000 次计入统计的调用。运行顺序逐轮轮换，
  降低固定顺序带来的时间漂移。下例 CPU `1`、输出目录名均需按本轮实际情况调整。

  ```sh
  cd /absolute/path/ub-application/eRPC-LRPC-UB
  sudo python3 scripts/run-bench-campaign.py --cpu 1 \
    --name lrpc_ub_domain --rounds 20 --warmup 100 --samples 1000 \
    --output results/bench-20261018-a0-b1
  ```

  如果 SDK/OBMM 库使用私有路径，先在运行该命令的 root 实验 shell 中设置真实的
  `LD_LIBRARY_PATH`，并确认 caller 二进制可启动。输出目录必须尚不存在，脚本不会覆盖旧结果。
  它要求 B 的 `--multi` owner；若误用单连接 owner，第一轮后会停止并保留日志。
  A 结束后再停止 B owner。脚本本身不加载模块或启动/停止远端服务。

- [ ] 保存 `config.json`、`rounds.csv`、60 份 `round-*.log`、`summary.json`、B owner
  日志及相同时间窗内的 A `dmesg`。必须看到 `UB_CAMPAIGN_PASS`；任何一轮失败只保留
  已完成的行和失败日志，不把它当作完整的 20 轮结果。
- [ ] 分别报告 Direct、Nested legacy、Nested fused 的每轮 p50/p99/avg 以及跨轮汇总。
  `pooled_p50_ns`、`pooled_p99_ns`、`pooled_avg_ns` 使用每条路径的全部 20,000 个
  原始计时值，属于总体统计；`round_p50_median_ns` / `round_p99_median_ns` 另给出
  20 个逐轮分位数的中位数。三条路径比较必须使用相同模块、CPU、owner、预热/样本数、
  计时口径 `scope=lrpc_invoke`；Direct 的结果应为 142，Nested 应为 43。
- [ ] 如出现长尾或 fused/legacy 差异异常，再单独使用
  [README 的 trace 和 TLB 探针](README.md#可选诊断域切换与远端-load)
  定位；带 trace 模块的延迟不与上述非 trace 性能数据混算。

## 2. SocialNetwork UserTimeline read

- [ ] 按 [接入文档第 2 节](SOCIAL_USER_READ.md#2-同步与编译)同步 `eRPC-LRPC-UB/`
  与同布局 `eRPC/`，在 A/B 构建各自二进制；确认 A 加载的模块支持 `ud_social`，
  B owner 使用 `--social`。先运行[独立路由探针](SOCIAL_USER_READ.md#3-不依赖数据库的路由探针)，
  不要把算术微测试的 `--multi` owner 用于应用。
- [ ] 按 [第 4 节](SOCIAL_USER_READ.md#4-生成私有应用配置)从已经跑通 UB-eRPC 的真实配置
  生成同一份 `config-a.json` / `config-b.json`。确认 MongoDB 地址、A 的 Timeline DB
  可达性、B 的 PostStorage DB、CPU affinity、NUMA、机器 ID、region/arena 容量。
  两种模式使用相同配置、数据集和服务位置：A 为 Client+UserTimeline，B 为 PostStorage；
  domain 的 LB/nginx 是 B 发布的域，RPC 基线的 LB/nginx 是 B 上的真实进程。
- [ ] 先按 [第 5 节](SOCIAL_USER_READ.md#5-双机运行少量端到端请求)运行 10 个
  UserTimeline read 请求，检查 `SN_DOMAIN_READY`、`SN_DOMAIN_E2E`、
  `SN_DOMAIN_CLASS`、MongoDB 加载数量和 `scripts/check-social.py` 的 PASS；检查
  非空帖子是否真正被读取与消费，并留存两端日志和 A `dmesg`。
- [ ] 退出 domain 应用后，按 [第 6 节](SOCIAL_USER_READ.md#6-相同服务位置的原-ub-erpc-对照)
  运行同服务位置的原 UB-eRPC 基线，同样先测 10 次并用 `check-social.py` 验收。
- [ ] 两条路径都通过后，用新私有配置将 `--samples` 提至 1000，均设置相同的
  `ERPC_SN_DOMAIN_WARMUP=10`，保持相同用户读取序列和数据集，交替多轮测量。
  保存每轮 `SN_DOMAIN_CLASS phase=measured class=all`、`SN_DOMAIN_E2E`、空/非空分类、
  完成数和错误日志。若数据分类或请求数不一致，先排查运行条件，不计算性能收益。

现阶段应用范围只有单 Client/lane、并发 1 的 UserTimeline read；HomeTimeline、发帖、
应用级 fused 以及多 nginx 都不在这轮实验中。
