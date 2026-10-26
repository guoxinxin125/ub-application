# HomeTimeline read：UB 双机端到端对照

本页默认仍是原来的 A/B 字节匹配域路径；只读的可选 B 发布代码接入见
[SocialNetwork 远端代码模式](SOCIAL_REMOTE_CODE.md)。

多个**独立 Client 进程**的 RPC / fused-domain 对照见 [并发实验步骤](SOCIAL_CONCURRENT_READ.md)。原文以下步骤仍是单 Client 路径。

本实验沿用 [UserTimeline read 的 UB 环境和生命周期说明](SOCIAL_USER_READ.md)，但一次只运行一种 read workload。这里 A 运行 `client + home_timeline`，B 运行 `post_storage`。RPC 基线还在 B 运行 `load_balance + nginx` 进程；域模式改由 B 的 owner 发布这两个只读服务域，由 A 的 CPU 执行。两种模式都由 A 上的 HomeTimeline 查询相同的 MongoDB 数据、向 B 的 PostStorage 读帖子，并由 Client 消费共享帖子句柄。不要把这项对照解释为 HomeTimeline 本身也被替换成域调用。

环境变量可按 [UserTimeline 的一键设置](SOCIAL_USER_READ.md#每台机器一次设置环境推荐) 配置本机的 `config/social-env.local.sh`，每个新 Bash 会话只需：

```sh
source scripts/social-env.sh --mode domain --workload home
# RPC 基线改为 --mode rpc；详细打点轮另加 --profile
```

随后下文启动命令可把 `--config` 后的固定路径换成 `"$SN_CONFIG"`；不需要重复逐项 `export`。首次生成配置前也能用它设置编译环境；正式运行前请检查脚本打印的配置、NUMA、样本数与日志目录。

运行前仍按原 [README_UB](../eRPC/apps/social_network_cxl/README_UB.md) 准备已有数据库和 UB SDK；不由这里的脚本启动、重置或导入 MongoDB。HomeTimeline 目前从 `user_timeline_mongodb` 指向的 `user-timeline.user-timeline` 集合加载帖子 ID；`social_graph_mongodb.connections` 仅作为旧代码的连接池数量配置。生成器会在私有配置中补足这个字段，不改源模板。如果数据库在 B 且 A 使用默认 `127.0.0.1:22012`，可使用下面的 `--ssh-b` 会话隧道。

先在 A 或 B 任一端生成一组私有配置，复制 `config-a.json`/`config-b.json` 到对应机器；IP、NUMA 和 DB 端点替换为实际值。原 UserTimeline 配置不受影响。

```sh
cd "$REPO/eRPC-LRPC-UB"
python3 scripts/prepare-social.py --workload home \
  --output "$REPO/eRPC-LRPC-UB/campaign-home-read" \
  --host-a A_IP --host-b B_IP --numa-a 0 --numa-b 1 --samples 10 \
  --b-core-base 13
```

在 A 构建应用的两种版本，并构建/加载与当前内核完全匹配的域模块；在 B 构建 owner 和 RPC 应用。若机器已运行旧的 `ud_social=user-read-...` 模块，须先结束本项目所有连接，再按 UserTimeline 文档的安全顺序更换模块；不要在其他用户连接仍在时强制卸载。默认域路径要求 B owner 和 A 模块使用同一版 `social_services.S` 服务镜像；可选远端代码模式则使用独立的受限验证规则。

```sh
# A：复用原文档的 UBSM_INCLUDE_DIR/LIBRARY、OBMM_INCLUDE_DIR/LIBRARY 设置
cd "$REPO/eRPC-LRPC-UB"
sh scripts/build.sh caller
sh scripts/build-module.sh --vhe  # EL1 机器改用不带 --vhe 的构建
modinfo -F ud_social kernel/ub-domain/ub_lrpc_domain_vhe.ko
cmake -S "$REPO/eRPC" -B "$REPO/eRPC/cmake-build-social-ub-domain" \
  -DAPP=social_network_cxl -DTRANSPORT=ub -DPERF=ON \
  -DERPC_SN_DOMAIN_PROXY=ON -DERPC_SN_DOMAIN_BACKEND=ub \
  -DUBSM_INCLUDE_DIR="$UBSM_INCLUDE_DIR" -DUBSM_LIBRARY="$UBSM_LIBRARY" \
  -DOBMM_INCLUDE_DIR="$OBMM_INCLUDE_DIR" -DOBMM_LIBRARY="$OBMM_LIBRARY"
cmake --build "$REPO/eRPC/cmake-build-social-ub-domain" -j2 \
  --target erpc_ub_manager client home_timeline
cmake -S "$REPO/eRPC" -B "$REPO/eRPC/cmake-build-social-ub-rpc" \
  -DAPP=social_network_cxl -DTRANSPORT=ub -DPERF=ON \
  -DERPC_SN_DOMAIN_PROXY=OFF -DERPC_SN_DOMAIN_BACKEND=ub \
  -DUBSM_INCLUDE_DIR="$UBSM_INCLUDE_DIR" -DUBSM_LIBRARY="$UBSM_LIBRARY"
cmake --build "$REPO/eRPC/cmake-build-social-ub-rpc" -j2 \
  --target erpc_ub_manager client home_timeline

# B：相同源码版本和 SDK 路径；B 不需要 A 的内核模块
cd "$REPO/eRPC-LRPC-UB"
sh scripts/build.sh owner
cmake -S "$REPO/eRPC" -B "$REPO/eRPC/cmake-build-social-ub-rpc" \
  -DAPP=social_network_cxl -DTRANSPORT=ub -DPERF=ON \
  -DERPC_SN_DOMAIN_PROXY=OFF -DERPC_SN_DOMAIN_BACKEND=ub \
  -DUBSM_INCLUDE_DIR="$UBSM_INCLUDE_DIR" -DUBSM_LIBRARY="$UBSM_LIBRARY"
cmake --build "$REPO/eRPC/cmake-build-social-ub-rpc" -j2 \
  --target erpc_ub_manager post_storage nginx load_balance
```

确认 A 的模块已按原文档加载、`ERPC_UB_MACHINE_ID` 与 `LRPC_UB_DOMAIN_NAME` 等环境变量已设置；A/B 使用相同的私有域名。每轮先启动 B，会话保持前台；再启动 A。`--ssh-b` 仅在需要 A→B 的本地 MongoDB 隧道时使用；数据库若已有可达地址，请在生成配置时传 `--timeline-db-host`/`--timeline-db-port`，省略 `--ssh-b`。

```sh
# B：RPC 基线
bash scripts/run-social-session.sh --side B --mode rpc \
  --config "$REPO/eRPC-LRPC-UB/campaign-home-read/config-b.json"
# A：另一个终端，RPC 基线
bash scripts/run-social-session.sh --side A --mode rpc \
  --config "$REPO/eRPC-LRPC-UB/campaign-home-read/config-a.json" --ssh-b USER@B

# B：域模式；另起一轮，先结束前一轮，再启动 owner+PostStorage
bash scripts/run-social-session.sh --side B --mode domain \
  --config "$REPO/eRPC-LRPC-UB/campaign-home-read/config-b.json"
# A：域模式，默认 fused；显式选择 legacy 可设 LRPC_UB_NESTED_PATH=legacy
bash scripts/run-social-session.sh --side A --mode domain \
  --config "$REPO/eRPC-LRPC-UB/campaign-home-read/config-a.json" --ssh-b USER@B
```

运行结束后，用本轮 A/B 日志目录检查；RPC 和 domain 分别检查，避免混用旧日志。脚本会核对完成数、read 结果分类，以及域模式下 A 两个独立连接的 `SN_DOMAIN_READY/DONE`。

```sh
python3 scripts/check-social.py --workload home --mode rpc --samples 10 \
  --logs "$A_LOG_DIR" --peer-logs "$B_LOG_DIR"
python3 scripts/check-social.py --workload home --mode domain --chain fused --samples 10 \
  --logs "$A_LOG_DIR" --peer-logs "$B_LOG_DIR"
```

正常测延迟时保持 `ERPC_SN_DOMAIN_PROFILE=0 ERPC_SN_PROFILE=0`；需要详细打点时，在 A/B 启动命令前分别 `export ERPC_SN_DOMAIN_PROFILE=1 ERPC_SN_PROFILE=1`。Client 日志中的 `SN_DOMAIN_MEASUREMENT`、`SN_DOMAIN_CLASS`、`SN_DOMAIN_BREAKDOWN` 是同一 `call-to-consume` 口径；A 的 `SN_DOMAIN_PROFILE` 把前向/回程域调用单列，A/B 的 `SN_UB_PROFILE` 给出 RPC、队列、PostStorage 等阶段。逐请求 `SN_FLOW_EVENT` 在各线程退出后写入对应服务的主日志。同步本次 `home_timeline.cpp`、`ub_breakdown.h` 和汇总脚本后，诊断轮需重新构建 A 的 `client home_timeline`；B 的 RPC 轮重建 `post_storage nginx load_balance`，domain 轮只需 `post_storage`。这些都是用户态程序，不需要重新加载内核模块。将 B 的服务日志复制到 A 后，可用以下命令查看各段汇总：

```sh
python3 scripts/analyze-social-flow.py --workload home --mode rpc \
  --a-logs "$A_LOG_DIR" --b-logs "$B_LOG_DIR"
python3 scripts/analyze-social-flow.py --workload home --mode domain \
  --a-logs "$A_LOG_DIR" --b-logs "$B_LOG_DIR"
```

HomeTimeline 可能对同一请求发出多个 PostStorage 子请求；对应分段若有多条时间戳会标为 `ambiguous`，不会挑一条冒充请求耗时。打点本身有开销，应与不打点的正式延迟轮次分开，不把单机时间戳跨机器直接相减。

当前只是代码与本地夹具验证；HomeTimeline 的 UB 真机编译、数据库内容/返回句柄正确性、两种模式端到端性能都需要按上面的顺序复测。
