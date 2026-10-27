# 实验性 3 nginx 只读域：双机验证

**状态：本地夹具通过；尚未完成 AArch64 Linux 模块编译或真实 UB 验证。**
这是独立域路由实验，不是三个真实 nginx 进程；三个实例使用相同 B 发布的受限
nginx 代码，但各有独立的 B 页表、A 本地代码页/栈和 ASID。LB 在 A CPU 上执行
B 发布的路由指令，gate 检查其目标，进入对应域；回程使用请求携带的 nginx 身份。
按设计，单 nginx 和写入探针保持旧布局；该版本仍需分别回归。

当前是固定轮转契约：A 内核根据请求 key 预选目标根页表；B 发布的 LB 指令
独立计算目标，gate 在切换前核对两者一致，不一致立即返回失败。它不是允许
任意 LB 动态选路的通用机制。应用回程先核验请求来源中的 nginx 身份，再进入相应域。

三实例使用每连接四组额外 B 页表；这是本项目的新实验性协议，A/B 必须同步同一版源码。
只支持 User/Home read、1 个 lane/Client、1～4 个 Client、每 Client 并发 1；
仍保留 8 个连接槽。相同 `--nginx-count 3` 配置也可用于 RPC 轮：B 启动三个独立
nginx 进程，LB 和 Timeline 使用业务请求编号选择同一个 nginx。两种路径尚未在真机比较。
三实例 RPC 占用 B 的五个三核 CPU 块（PostStorage、LB 和三个 nginx），必须先检查 affinity。

## 1. 双端准备与构建

延续 [远端代码文档](SOCIAL_REMOTE_CODE.md)的 SDK/OBMM、MongoDB、权限和机器分工；
两端先结束本项目旧连接，确认共享设备没有其他使用者，不更改机器启动配置。
以下 `/absolute/path/ub-application`、IP、`USER@B` 和 CPU/NUMA 必须按实际环境替换。
两端分别设置 `config/social-env.local.sh`，A 为 `SOCIAL_HOST_SIDE=A`，B 为 `B`。

先在 A 生成新的 User 配置，不覆盖已有目录；把生成的 `config-b.json` 复制到 B 的
同一仓库相对路径。`--samples 10` 是总请求数（包含预热），B CPU 索引按实际 affinity 选择。

```sh
export REPO=/absolute/path/ub-application
cd "$REPO/eRPC-LRPC-UB"
python3 scripts/prepare-social.py --workload user --nginx-count 3 \
  --output "$REPO/eRPC-LRPC-UB/campaign-user-3nginx" \
  --host-a A_IP --host-b B_IP --numa-a 0 --numa-b 1 \
  --b-core-base 13 --samples 10
# 在 B 创建同名目录后，将 campaign-user-3nginx/config-b.json 复制过去。
```

**B：**

```sh
export REPO=/absolute/path/ub-application
cd "$REPO/eRPC-LRPC-UB"
source scripts/social-env.sh --mode domain --workload user \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-3nginx/config-b.json" --run-id nginx3
python3 scripts/manifest.py --check
sh scripts/build.sh owner
./build-owner/lrpc-ub-remote-social-code-gen "$REPO/eRPC-LRPC-UB/social-user-3nginx.bin" \
  --service user --nginx-count 3 --nop-padding 0
export LRPC_UB_SOCIAL_CODE_BUNDLE="$REPO/eRPC-LRPC-UB/social-user-3nginx.bin"
export LRPC_UB_ENABLE_3NGINX=1
cmake -S "$REPO/eRPC" -B "$REPO/eRPC/cmake-build-social-ub-rpc" \
  -DAPP=social_network_cxl -DTRANSPORT=ub -DPERF=ON \
  -DERPC_SN_DOMAIN_PROXY=OFF -DERPC_SN_DOMAIN_BACKEND=ub \
  -DUBSM_INCLUDE_DIR="$UBSM_INCLUDE_DIR" -DUBSM_LIBRARY="$UBSM_LIBRARY"
cmake --build "$REPO/eRPC/cmake-build-social-ub-rpc" -j2 --target erpc_ub_manager post_storage
```

**A：**在 root 实验 shell 内执行；模块构建成功不代表已加载。检查 vermagic 与 `uname -r`
一致；只在所有本项目连接退出且确认没有其他使用者时更换本项目模块。

```sh
export REPO=/absolute/path/ub-application
cd "$REPO/eRPC-LRPC-UB"
source scripts/social-env.sh --mode domain --workload user \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-3nginx/config-a.json" --run-id nginx3
python3 scripts/manifest.py --check
sh scripts/build.sh caller
sh scripts/build-module.sh --vhe
modinfo -F vermagic kernel/ub-domain/ub_lrpc_domain_vhe.ko
modinfo -F ud_remote_code kernel/ub-domain/ub_lrpc_domain_vhe.ko
cmake -S "$REPO/eRPC" -B "$REPO/eRPC/cmake-build-social-ub-domain" \
  -DAPP=social_network_cxl -DTRANSPORT=ub -DPERF=ON \
  -DERPC_SN_DOMAIN_PROXY=ON -DERPC_SN_DOMAIN_BACKEND=ub \
  -DUBSM_INCLUDE_DIR="$UBSM_INCLUDE_DIR" -DUBSM_LIBRARY="$UBSM_LIBRARY" \
  -DOBMM_INCLUDE_DIR="$OBMM_INCLUDE_DIR" -DOBMM_LIBRARY="$OBMM_LIBRARY"
cmake --build "$REPO/eRPC/cmake-build-social-ub-domain" -j2 --target erpc_ub_manager client user_timeline
sudo fuser -v /dev/ub_lrpc_domain
# 旧 VHE 模块已加载且没有其他使用者时才卸载；未加载时跳过。
sudo rmmod ub_lrpc_domain_vhe
sudo insmod kernel/ub-domain/ub_lrpc_domain_vhe.ko experimental=1 vhe_gate=1 allow_remote_code=1
```

`modinfo -F ud_remote_code` 应输出 `direct-nested-opt-in-social-read-required-nginx3-exp-abi1`。
EL1 机器使用不带 `--vhe` 的构建、`ub_lrpc_domain.ko experimental=1 allow_remote_code=1`；
两种模块不能同时加载。busy 时停止，不强制卸载。

## 2. 先跑 6 次路由探针

B 终端保持上述 `LRPC_UB_SOCIAL_CODE_BUNDLE` 和 `LRPC_UB_ENABLE_3NGINX=1`，先启动：

```sh
cd "$REPO/eRPC-LRPC-UB"
sh scripts/run-social.sh --side B --mode domain
```

等 B 打印 `UB_DOMAIN_OWNER_READY ... nginx_count=3`。在 A 的 root 实验 shell：

```sh
cd "$REPO/eRPC-LRPC-UB"
env LRPC_UB_DOMAIN_NAME="$LRPC_UB_DOMAIN_NAME" \
  LRPC_UBSM_SDK_MODE=owned LRPC_UB_SOCIAL_READ_SERVICE=user \
  LRPC_UB_NESTED_PATH=fused taskset -c 1 \
  ./build-caller/lrpc-ub-social-probe --samples 6 --nginx-count 3
```

CPU 1 必须在 A 的允许列表及目标 NUMA 中。应见 domain 2/3/4 各 `calls=2` 和
`UB_SOCIAL_ROUTE_PASS ... nginx_count=3`。探针正常退出后，先结束 B owner，再重新启动
下一轮。若失败或内核日志异常，保留 A/B 日志并停止，不转入端到端轮。

## 3. 运行 UserTimeline 端到端

先启动 B（同一配置、同一 B 代码包，另用新 owner）：

```sh
cd "$REPO/eRPC-LRPC-UB"
source scripts/social-env.sh --mode domain --workload user \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-3nginx/config-b.json" --run-id nginx3
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-user-3nginx-b"
export LRPC_UB_SOCIAL_CODE_BUNDLE="$REPO/eRPC-LRPC-UB/social-user-3nginx.bin"
export LRPC_UB_ENABLE_3NGINX=1
bash scripts/run-social-session.sh --side B --mode domain --config "$SN_CONFIG"
```

看到 B 的 `UB_SOCIAL_OWNER_READY` 后，立即在 A 的 root 实验 shell：

```sh
cd "$REPO/eRPC-LRPC-UB"
source scripts/social-env.sh --mode domain --workload user \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-3nginx/config-a.json" --run-id nginx3
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-user-3nginx-a"
bash scripts/run-social-session.sh --side A --mode domain --config "$SN_CONFIG" --ssh-b USER@B
```

已有数据库直连或独立隧道时，删除 `--ssh-b USER@B`。Client 结束后先 Ctrl-C 停 A，
再停 B。下面在 A 上复制 B 的日志；先把 `USER@B` 和 `B_REPO` 换成 B 的登录地址和仓库绝对路径，
并确保 A 的目标目录尚不存在，避免混入旧结果：

```sh
scp -r USER@B:B_REPO/eRPC-LRPC-UB/results/social-user-3nginx-b \
  "$REPO/eRPC-LRPC-UB/results/social-user-3nginx-b"
```

然后在 A 上检查：

```sh
python3 scripts/check-social.py --workload user --mode domain --chain fused \
  --nginx-count 3 --samples 10 \
  --logs "$REPO/eRPC-LRPC-UB/results/social-user-3nginx-a" \
  --peer-logs "$REPO/eRPC-LRPC-UB/results/social-user-3nginx-b"
grep -E 'SN_DOMAIN_READY|SN_DOMAIN_ROUTE|SN_DOMAIN_E2E|SN_DOMAIN_ERROR' \
  "$REPO/eRPC-LRPC-UB/results/social-user-3nginx-a/client.log"
sudo dmesg | grep -iE 'ub_domain|BUG:|Oops|panic|lockup|SError|Unhandled' | tail -80
```

Home 需另生成 `--workload home --nginx-count 3` 配置和 `--service home --nginx-count 3`
代码包，再将本节两端 workload、配置和日志目录分别改为 Home；现有无数据库探针仅验证 User。

## 4. 相同配置的三 nginx RPC 对照（实验性）

本节尚未在 UB 真机验证。先完成上面的配置生成，确保 A/B 使用相同的一对配置和
MongoDB 数据。B 需要检查五个互不重叠的三核块；默认 `--b-core-base 13` 会使用
NUMA CPU 列表的索引 13–27，不一定属于你的 affinity，务必按实际机器修改或用
`--cpu-topology` 生成配置。不要直接复用本改动之前生成的三个 nginx 配置，它们
没有为三个 RPC 进程分配独立 CPU 块。

RPC 不需要更换或加载 LRPC 内核模块；但 A、B 必须各自重新编译 RPC 应用。以下分别在
两台机器上执行，先按机器修改 `REPO` 和 `config/social-env.local.sh`：

```sh
export REPO=/absolute/path/ub-application
cd "$REPO/eRPC-LRPC-UB"
source scripts/social-env.sh --mode rpc --workload user \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-3nginx/config-a.json" --run-id rpc3
# B 使用 config-b.json；SOCIAL_HOST_SIDE 必须在本机环境文件中正确设置。
cmake -S "$REPO/eRPC" -B "$REPO/eRPC/cmake-build-social-ub-rpc" \
  -DAPP=social_network_cxl -DTRANSPORT=ub -DPERF=ON \
  -DERPC_SN_DOMAIN_PROXY=OFF -DERPC_SN_DOMAIN_BACKEND=ub \
  -DUBSM_INCLUDE_DIR="$UBSM_INCLUDE_DIR" -DUBSM_LIBRARY="$UBSM_LIBRARY"
cmake --build "$REPO/eRPC/cmake-build-social-ub-rpc" -j2 \
  --target erpc_ub_manager client user_timeline post_storage load_balance nginx
```

先在 B 启动（确认本 shell 的 `SN_CONFIG` 指向 B 配置）：

```sh
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-user-rpc3-b"
bash scripts/run-social-session.sh --side B --mode rpc --config "$SN_CONFIG"
```

另在 A 启动（确认指向 A 配置，MongoDB 已可达；需要隧道时替换 `USER@B`）：

```sh
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-user-rpc3-a"
bash scripts/run-social-session.sh --side A --mode rpc --config "$SN_CONFIG" --ssh-b USER@B
```

数据库直连或已有隧道时删除 `--ssh-b USER@B`。Client 完成后先停止 A、再停止 B，
确保 B 的三个 nginx 日志写出结束统计。在 A 将 B 日志复制到尚不存在的目标目录：

```sh
scp -r USER@B:B_REPO/eRPC-LRPC-UB/results/social-user-rpc3-b \
  "$REPO/eRPC-LRPC-UB/results/social-user-rpc3-b"
cd "$REPO/eRPC-LRPC-UB"
python3 scripts/check-social.py --workload user --mode rpc --nginx-count 3 --samples 10 \
  --logs "$REPO/eRPC-LRPC-UB/results/social-user-rpc3-a" \
  --peer-logs "$REPO/eRPC-LRPC-UB/results/social-user-rpc3-b"
grep 'SN_CXL_PROXY_DONE' "$REPO"/eRPC-LRPC-UB/results/social-user-rpc3-b/nginx-*.log
```

将 `USER@B` 和 `B_REPO` 替换为实际地址及 B 仓库绝对路径。检查器要求三个进程都
完成去程和回程，且总调用数匹配。Home 需要生成单独的 Home 配置，并把两端 workload、
目标 `user_timeline`、日志路径和检查器参数改为 `home_timeline` / `home`。

## 5. 2 / 4 Client 并发读对照

下面先做 **UserTimeline、每 Client 10 次、无预热**的 smoke。两条路径都通过后，另建
目录，将 `--samples 10` 改为 `--samples 1100`，并在四个 `source` 命令中将
`--warmup 0` 改为 `--warmup 100`，即**每个 Client 预热 100 次、实测 1000 次**。
每个 Client 一个 lane、同时最多一个未完成请求；2 或 4 指独立进程数量。三 nginx 的
多 Client 组合尚未在 UB 真机验证，若出现异常先停测并保存两端日志。

前置：按第 1 节同步、编译两端，按第 1 节和第 4 节分别编译 domain/RPC 应用；
确保数据库可达，A 的实验模块已在确认无其他使用者后安全更换。两种模式使用同一对
配置、相同数据库和样本数量，分别使用新的日志目录。以下命令中的 `REPO`、A/B IP、
`USER@B`、`B_REPO` 均需替换成实际值。

### 5.1 在 A 生成两套配置

先检查 `config/social-cpu-topology-shared.json` 与两台机器**当前** NUMA 和 affinity
一致；这是静态快照，不一致时先更新快照或另建拓扑文件。三 nginx RPC 在 B 要五组
独立三核 CPU；2 Client 在 A 要三组，4 Client 在 A 要五组。生成后仍由两端启动脚本
按实时 affinity 做预检。以下输出目录必须事先不存在：

```sh
export REPO=/absolute/path/ub-application
cd "$REPO/eRPC-LRPC-UB"
python3 scripts/prepare-social.py --workload user --clients 2 --nginx-count 3 --samples 10 \
  --output "$REPO/eRPC-LRPC-UB/campaign-user-3nginx-2c" \
  --host-a A_IPV4 --host-b B_IPV4 --numa-a 0 --numa-b 1 \
  --cpu-topology config/social-cpu-topology-shared.json
python3 scripts/prepare-social.py --workload user --clients 4 --nginx-count 3 --samples 10 \
  --output "$REPO/eRPC-LRPC-UB/campaign-user-3nginx-4c" \
  --host-a A_IPV4 --host-b B_IPV4 --numa-a 0 --numa-b 1 \
  --cpu-topology config/social-cpu-topology-shared.json
```

数据库不使用模板地址时，在两条生成命令中都明确添加相同的
`--timeline-db-host/--timeline-db-port` 和需要的 storage 数据库参数。分别将生成的
`config-b.json` 复制到 B 的对应目录；下面先在 B 创建目录，再从 A 复制：

```sh
ssh USER@B 'mkdir -p B_REPO/eRPC-LRPC-UB/campaign-user-3nginx-2c B_REPO/eRPC-LRPC-UB/campaign-user-3nginx-4c'
scp "$REPO/eRPC-LRPC-UB/campaign-user-3nginx-2c/config-b.json" USER@B:B_REPO/eRPC-LRPC-UB/campaign-user-3nginx-2c/config-b.json
scp "$REPO/eRPC-LRPC-UB/campaign-user-3nginx-4c/config-b.json" USER@B:B_REPO/eRPC-LRPC-UB/campaign-user-3nginx-4c/config-b.json
```

### 5.2 选人数并运行两轮

在 **A 和 B 各自的 Bash 实验终端**选择相同人数；以下两条选其一，先用 2，再改成 4
重复整节。每台机器的 `REPO` 使用各自的仓库路径：

```sh
export CLIENTS=2  # 2 Client 测试
# export CLIENTS=4  # 4 Client 测试：代替上一行，不要同时执行
export REPO=/absolute/path/ub-application
cd "$REPO/eRPC-LRPC-UB"
export CAMPAIGN="$REPO/eRPC-LRPC-UB/campaign-user-3nginx-${CLIENTS}c"
```

先运行 RPC，**B 先启动**，看到三个 `nginx-1.log`、`nginx-2.log`、`nginx-3.log` 后在
A 启动；实际服务就绪由 A 的 readiness 检查等待：

```sh
# B：RPC
source scripts/social-env.sh --mode rpc --workload user --warmup 0 --run-id "rpc-3nginx-${CLIENTS}c" --config "$CAMPAIGN/config-b.json"
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/user-rpc-3nginx-${CLIENTS}c-b"
bash scripts/run-social-session.sh --side B --mode rpc --config "$SN_CONFIG"
```

```sh
# A：RPC
source scripts/social-env.sh --mode rpc --workload user --warmup 0 --run-id "rpc-3nginx-${CLIENTS}c" --config "$CAMPAIGN/config-a.json"
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/user-rpc-3nginx-${CLIENTS}c-a"
bash scripts/run-social-session.sh --side A --mode rpc --config "$SN_CONFIG" --ssh-b USER@B
```

A 完成后停止 A 会话，再停止 B；不要覆盖这轮日志。然后运行 domain，B 每轮重新启动
owner。代码包需使用 `--nginx-count 3`：

```sh
# B：domain
source scripts/social-env.sh --mode domain --workload user --warmup 0 --run-id "domain-3nginx-${CLIENTS}c" --config "$CAMPAIGN/config-b.json"
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/user-domain-3nginx-${CLIENTS}c-b"
./build-owner/lrpc-ub-remote-social-code-gen "$REPO/eRPC-LRPC-UB/social-user-3nginx.bin" --service user --nginx-count 3 --nop-padding 0
export LRPC_UB_SOCIAL_CODE_BUNDLE="$REPO/eRPC-LRPC-UB/social-user-3nginx.bin"
export LRPC_UB_ENABLE_3NGINX=1
bash scripts/run-social-session.sh --side B --mode domain --config "$SN_CONFIG"
```

```sh
# A：等待 B 打印 UB_SOCIAL_OWNER_READY 后启动 domain
source scripts/social-env.sh --mode domain --workload user --warmup 0 --run-id "domain-3nginx-${CLIENTS}c" --config "$CAMPAIGN/config-a.json"
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/user-domain-3nginx-${CLIENTS}c-a"
bash scripts/run-social-session.sh --side A --mode domain --config "$SN_CONFIG" --ssh-b USER@B
```

如果已有数据库直连或隧道，两条 A 命令均去掉 `--ssh-b USER@B`。domain 完成后先停 A，
再停 B。两个模式的 Client 都会通过启动栅栏并发运行，日志为 `client-1.log` 等。

### 5.3 复制 B 日志并检查

在 A 执行；目标目录应不存在，防止混入旧结果：

```sh
scp -r "USER@B:B_REPO/eRPC-LRPC-UB/results/user-rpc-3nginx-${CLIENTS}c-b" "$REPO/eRPC-LRPC-UB/results/"
scp -r "USER@B:B_REPO/eRPC-LRPC-UB/results/user-domain-3nginx-${CLIENTS}c-b" "$REPO/eRPC-LRPC-UB/results/"
cd "$REPO/eRPC-LRPC-UB"
python3 scripts/check-social.py --workload user --clients "$CLIENTS" --nginx-count 3 --samples 10 --mode rpc \
  --logs "$REPO/eRPC-LRPC-UB/results/user-rpc-3nginx-${CLIENTS}c-a" \
  --peer-logs "$REPO/eRPC-LRPC-UB/results/user-rpc-3nginx-${CLIENTS}c-b"
python3 scripts/check-social.py --workload user --clients "$CLIENTS" --nginx-count 3 --samples 10 --mode domain --chain fused \
  --logs "$REPO/eRPC-LRPC-UB/results/user-domain-3nginx-${CLIENTS}c-a" \
  --peer-logs "$REPO/eRPC-LRPC-UB/results/user-domain-3nginx-${CLIENTS}c-b"
```

检查器核对各 Client 的完成数和运行区间重叠，以及三 nginx 的路由/流量标记；每个
Client 的 p50、p99、avg 在对应 `client-N.log` 的 `SN_DOMAIN_E2E` 中。正式轮须把
检查命令的 `--samples 10` 改为 `--samples 1100`。当前 `compare-social.py` 尚未提供
三 nginx 选项，不要直接把它的默认单 nginx 检查结果当作三 nginx 对照。

Home 测试需分别生成 Home 配置和 B 的 `--service home --nginx-count 3` 代码包，
并将上述 workload、配置目录、日志目录和检查器参数一并改为 Home；不能复用 User
代码包。
