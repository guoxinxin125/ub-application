# SocialNetwork B 发布路由代码：User/Home 只读

此路径只替换 **LB 与 nginx 的四段受限路由代码**，不复制或执行 B 上完整的
C++ 代理进程。Client、Timeline、PostStorage 的现有业务路径和数据库布局不变。
User/Home 只读域路径必须使用 B 发布代码包；不再支持原先的 A/B 预置代码
逐字节匹配路径。RPC 基线、写入/共享原子探针不启用此功能。每次连接从 B 拷贝代码到 A，
A 内核验证四段指令、复制到本地执行页并清理 I-cache；稳态调用不再复制代码。

本页介绍**单 nginx、每轮单一 User 或 Home 读工作负载**；实验性的三 nginx
域和单独验收步骤见 [SOCIAL_3NGINX.md](SOCIAL_3NGINX.md)。B 代码使用固定
`us_config` 字段读取 nginx/Timeline 目标，并写入 A-local 消息的 action/target；
不允许额外访存、分支、调用或系统调用。B 生成代码包时选择 `user` 或 `home`；
A 启动器从配置文件自动检测对应服务，连接时核对代码包的服务标记。只有尾部
0–4 条 NOP 可以由 B 单独改变，因此“B 代码改变而 A 不重编”首先验证的是
**代码来源与连接时校验机制**，不能据此宣称任意 B 业务函数已可执行。

## 0. 一次性准备配置与环境

本文命令在 **B、A 各一个 Bash 实验终端**执行；A 的域应用终端须有 root 权限。
沿用 [UserTimeline UB 部署说明](SOCIAL_USER_READ.md)中的机器分工、SDK/OBMM 和
已就绪的 MongoDB，**不会启动或重置数据库**。两端同步同一版 `eRPC-LRPC-UB/`
及 `eRPC/`，且保留同一仓库布局。先分别将
`config/social-env.example.sh` 复制为 `config/social-env.local.sh`，在各自主机上填写
`SOCIAL_HOST_SIDE`、机器 ID、NUMA、SDK/OBMM 路径及相同的私有域名。

若还没有 User 或 Home 的两份私有 JSON，在 A 上生成一组；把 `config-b.json`
复制到 B 的同路径。以下以 User、10 次请求为例，实际 IP、NUMA、B CPU 起始索引
按机器情况填写；Home 改为 `--workload home`、`campaign-home-read`。若 A 已能直接访问
Timeline 数据库，生成时另加 `--timeline-db-host ACTUAL_DB_HOST --timeline-db-port 20012`，
后续 A 启动命令就不需要 `--ssh-b`。

```sh
cd /absolute/path/ub-application/eRPC-LRPC-UB
python3 scripts/prepare-social.py --workload user \
  --output /absolute/path/ub-application/eRPC-LRPC-UB/campaign-user-read \
  --host-a A_IP --host-b B_IP --numa-a 0 --numa-b 1 \
  --b-core-base 13 --samples 10
# 将 campaign-user-read/config-b.json 复制到 B 的相同仓库相对路径。
```

如果已按 User/Home 文档生成配置，**不要重新生成或覆盖它**。两端运行前均检查
`python3 scripts/manifest.py --check`；`campaign-*` 和本机环境文件是私有文件，
不由 manifest 管理。

## 1. 构建和模块更新

构建前，在 **B 的普通用户 Bash**、**A 的 root Bash** 中各自执行下面两条；
本机 `social-env.local.sh` 会设置 `REPO`、SDK/OBMM 路径和私有域名。
之后的构建命令分别在标注的机器执行，不能把 A 的模块命令拿到 B 运行。

```sh
cd /absolute/path/ub-application/eRPC-LRPC-UB
source scripts/social-env.sh --mode domain --workload user --run-id remote-user-01
python3 scripts/manifest.py --check
```

Home 轮把 `--workload user` 改成 `home`。B 构建 owner 和代码包生成器：

```sh
cd "$REPO/eRPC-LRPC-UB"
sh scripts/build.sh owner
./build-owner/lrpc-ub-remote-social-code-gen "$REPO/eRPC-LRPC-UB/social-user.bin" \
  --service user --nop-padding 0
python3 tests/test_remote_social.py
```

HomeTimeline 轮改为 `--service home` 并生成另一个文件。`--nop-padding 2`
可作为第二轮 B-only 代码变化测试；它不改变路由结果。两个代码包只在 B 上。

A 重新编译客户端库、内核模块和链接该库的域应用：

```sh
cd "$REPO/eRPC-LRPC-UB"
sh scripts/build.sh caller
sh scripts/build-module.sh --vhe
modinfo -F vermagic kernel/ub-domain/ub_lrpc_domain_vhe.ko
modinfo -F ud_remote_code kernel/ub-domain/ub_lrpc_domain_vhe.ko
# 应显示 direct-nested-opt-in-social-read-required-nginx3-exp-abi1
cmake -S "$REPO/eRPC" -B "$REPO/eRPC/cmake-build-social-ub-domain" \
  -DAPP=social_network_cxl -DTRANSPORT=ub -DPERF=ON \
  -DERPC_SN_DOMAIN_PROXY=ON -DERPC_SN_DOMAIN_BACKEND=ub \
  -DUBSM_INCLUDE_DIR="$UBSM_INCLUDE_DIR" -DUBSM_LIBRARY="$UBSM_LIBRARY" \
  -DOBMM_INCLUDE_DIR="$OBMM_INCLUDE_DIR" -DOBMM_LIBRARY="$OBMM_LIBRARY"
cmake --build "$REPO/eRPC/cmake-build-social-ub-domain" -j2 \
  --target erpc_ub_manager client user_timeline home_timeline
```

B 还要构建 PostStorage 和 manager；它们仍是 RPC-only 二进制，不使用远端代码模式：

```sh
cmake -S "$REPO/eRPC" -B "$REPO/eRPC/cmake-build-social-ub-rpc" \
  -DAPP=social_network_cxl -DTRANSPORT=ub -DPERF=ON \
  -DERPC_SN_DOMAIN_PROXY=OFF -DERPC_SN_DOMAIN_BACKEND=ub \
  -DUBSM_INCLUDE_DIR="$UBSM_INCLUDE_DIR" -DUBSM_LIBRARY="$UBSM_LIBRARY"
cmake --build "$REPO/eRPC/cmake-build-social-ub-rpc" -j2 \
  --target erpc_ub_manager post_storage
```

上面的 `--vhe` 仅用于当前 VHE-EL2 机器；EL1 机器按现有文档构建普通模块。
`vermagic` 开头必须与 `uname -r` 一致。构建 `.ko` 不会更新已加载模块；
等所有本项目 A 域连接退出，检查 `lsmod`、`sudo fuser -v /dev/ub_lrpc_domain`，
并确认没有其他使用者后，才在约定实验窗口更换自己加载的模块：

```sh
sudo rmmod ub_lrpc_domain_vhe # 未加载时跳过；busy 时停止，不要强制卸载
sudo insmod kernel/ub-domain/ub_lrpc_domain_vhe.ko \
  experimental=1 vhe_gate=1 allow_remote_code=1
sudo cat /sys/module/ub_lrpc_domain_vhe/parameters/allow_remote_code
```

## 2. User 远端代码：完整双机运行命令

先在 B 的实验终端运行。`source` 会设置 `REPO`、`SN_CONFIG`、`SN_BUILD_DIR`
及一个默认 `SN_LOG_DIR`；下面**紧接着覆盖日志目录**，所以别在 `export SN_LOG_DIR`
后再次 `source`。默认路径也能用，但独立目录更方便与 RPC/legacy 对照，不会混入旧日志。
不要在 B 设置 `LRPC_UB_CODE_MODE`；B 只发布代码。缺少 B 代码包时启动器会拒绝启动。

```sh
# B：在已填写 config/social-env.local.sh、配置 JSON 和代码包之后
cd /absolute/path/ub-application/eRPC-LRPC-UB
source scripts/social-env.sh --mode domain --workload user --run-id remote-user-01
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-user-remote-b"
export LRPC_UB_SOCIAL_CODE_BUNDLE="$REPO/eRPC-LRPC-UB/social-user.bin"
unset LRPC_UB_CODE_MODE
python3 scripts/manifest.py --check
test -f "$SN_CONFIG" && test -f "$LRPC_UB_SOCIAL_CODE_BUNDLE"
printf 'B config=%s logs=%s bundle=%s\n' "$SN_CONFIG" "$SN_LOG_DIR" "$LRPC_UB_SOCIAL_CODE_BUNDLE"
bash scripts/run-social-session.sh --side B --mode domain --config "$SN_CONFIG"
```

B 打印 `UB_SOCIAL_OWNER_READY` 后立即在 A 的 **root 实验终端**运行（在 root shell
内执行 `source`，避免 `sudo` 丢失环境变量）。若现有配置采用 A 本地
`127.0.0.1:22012` 的 MongoDB 隧道，使用示例的 `--ssh-b USER@B`；若数据库直连
或隧道已经单独建立，就删除此选项。两端配置的域名须一致。

```sh
# A：先进入有 CAP_SYS_RAWIO 的 root 实验 shell，再执行下面所有命令
cd /absolute/path/ub-application/eRPC-LRPC-UB
source scripts/social-env.sh --mode domain --workload user --run-id remote-user-01
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-user-remote-a"
export LRPC_UB_CODE_MODE=remote-social
unset LRPC_UB_SOCIAL_CODE_BUNDLE
python3 scripts/manifest.py --check
test -f "$SN_CONFIG" && test -c /dev/ub_lrpc_domain
printf 'A config=%s logs=%s code_mode=%s\n' "$SN_CONFIG" "$SN_LOG_DIR" "$LRPC_UB_CODE_MODE"
bash scripts/run-social-session.sh --side A --mode domain \
  --config "$SN_CONFIG" --ssh-b USER@B
```

脚本从配置中的 `client.user_num/home_num` 自动导出
`LRPC_UB_SOCIAL_READ_SERVICE`，连接时与 B 代码包标记核对。B 的
`$SN_LOG_DIR/domain_owner.log` 应有 `code_mode=remote-social`；A 的
`client.log` 和 `user_timeline.log` 应有
`UB_DOMAIN_BOUND ... code_mode=remote-social code_source=B-published`。查看结果：

```sh
# A；在运行轮结束后使用 A 那轮设置的日志目录
grep -E 'UB_DOMAIN_BOUND|SN_DOMAIN_E2E|SN_DOMAIN_ERROR' \
  "$REPO/eRPC-LRPC-UB/results/social-user-remote-a/client.log" \
  "$REPO/eRPC-LRPC-UB/results/social-user-remote-a/user_timeline.log"
# 在 A 将 B 日志复制到本机；B 的绝对路径按其真实部署路径调整：
scp -r USER@B:/absolute/path/ub-application/eRPC-LRPC-UB/results/social-user-remote-b \
  "$REPO/eRPC-LRPC-UB/results/"
python3 scripts/check-social.py --workload user --mode domain --chain fused \
  --samples 10 --logs "$REPO/eRPC-LRPC-UB/results/social-user-remote-a" \
  --peer-logs "$REPO/eRPC-LRPC-UB/results/social-user-remote-b"
```

`--peer-logs` 是 **A 上可读的 B 日志副本路径**，检查脚本不会跨机取文件。
Client 完成后先在 A 启动终端按 Ctrl-C、等待本轮进程退出，再在 B 按 Ctrl-C。
不要在 A 的连接仍存活时结束 B。第二轮 `--nop-padding 2` 要先结束第一轮，
只在 B 重新生成同名代码包、重启 B owner；A 模块和应用无需重编，再按同样
顺序运行 A。给第二轮另设 `social-user-remote-nop2-a/b` 日志目录。

Home 轮不可用 User 代码包或 User 配置。其对应完整启动命令为：

```sh
# B：先确保 campaign-home-read/config-b.json 已从同一轮配置同步过来
cd /absolute/path/ub-application/eRPC-LRPC-UB
source scripts/social-env.sh --mode domain --workload home --run-id remote-home-01
./build-owner/lrpc-ub-remote-social-code-gen "$REPO/eRPC-LRPC-UB/social-home.bin" \
  --service home --nop-padding 0
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-home-remote-b"
export LRPC_UB_SOCIAL_CODE_BUNDLE="$REPO/eRPC-LRPC-UB/social-home.bin"
unset LRPC_UB_CODE_MODE
bash scripts/run-social-session.sh --side B --mode domain --config "$SN_CONFIG"
```

```sh
# A：B 打印 READY 后，在 root 实验终端运行；不需要隧道则删去 --ssh-b
cd /absolute/path/ub-application/eRPC-LRPC-UB
source scripts/social-env.sh --mode domain --workload home --run-id remote-home-01
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-home-remote-a"
export LRPC_UB_CODE_MODE=remote-social
unset LRPC_UB_SOCIAL_CODE_BUNDLE
bash scripts/run-social-session.sh --side A --mode domain \
  --config "$SN_CONFIG" --ssh-b USER@B
```

结束后按 User 轮相同顺序停止 A/B，并把 B 的 `social-home-remote-b/` 复制到 A，
再运行：

```sh
python3 scripts/check-social.py --workload home --mode domain --chain fused \
  --samples 10 --logs "$REPO/eRPC-LRPC-UB/results/social-home-remote-a" \
  --peer-logs "$REPO/eRPC-LRPC-UB/results/social-home-remote-b"
grep -E 'UB_DOMAIN_BOUND|SN_DOMAIN_E2E|SN_DOMAIN_ERROR' \
  "$REPO/eRPC-LRPC-UB/results/social-home-remote-a/client.log" \
  "$REPO/eRPC-LRPC-UB/results/social-home-remote-a/home_timeline.log"
```

公平对照需在**另一轮**使用相同配置、CPU/NUMA、预热和样本数，运行
`--mode rpc` 的基线；两轮分别设置独立 `SN_LOG_DIR`。若要比较历史旧域结果，
应明确标注其代码来源和旧版构建，不要把它当作当前默认域路径。RPC 基线完整的构建和
运行命令见 [UserTimeline](SOCIAL_USER_READ.md) / [HomeTimeline](SOCIAL_HOME_READ.md)。
远端代码轮不启动普通 nginx/LB 进程；不能把它和 RPC 轮日志混在一起。

如先做无数据库的 User 路由探针，使用新 B owner 后在 A 运行：

```sh
sudo env LRPC_UB_DOMAIN_NAME="$LRPC_UB_DOMAIN_NAME" \
  LRPC_UB_CODE_MODE=remote-social LRPC_UB_SOCIAL_READ_SERVICE=user \
  LRPC_UBSM_SDK_MODE=owned LRPC_UB_NESTED_PATH=fused taskset -c 1 \
  ./build-caller/lrpc-ub-social-probe --samples 10
```

应有 `UB_SOCIAL_ROUTE_PASS`。停止 A 探针后，结束 B owner，再以新 owner
运行端到端应用；共享机器上不要在线清池或对 busy 模块强制 rmmod。
