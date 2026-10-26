# SocialNetwork 可选 B 发布路由代码：User/Home 只读

此路径只替换 **LB 与 nginx 的四段受限路由代码**，不复制或执行 B 上完整的
C++ 代理进程。Client、Timeline、PostStorage 的现有业务路径和数据库布局不变。
默认 `LRPC_UB_CODE_MODE` 不设置时，继续使用原先的 A/B 字节匹配域路径；
RPC 基线、写入/共享原子探针也不启用此功能。每次连接从 B 拷贝代码到 A，
A 内核验证四段指令、复制到本地执行页并清理 I-cache；稳态调用不再复制代码。

本版只支持**单 nginx、每轮单一 User 或 Home 读工作负载**。B 代码使用固定
`us_config` 字段读取 nginx/Timeline 目标，并写入 A-local 消息的 action/target；
不允许额外访存、分支、调用或系统调用。B 生成代码包时选择 `user` 或 `home`；
A 启动器从配置文件自动检测对应服务，连接时核对代码包的服务标记。只有尾部
0–4 条 NOP 可以由 B 单独改变，因此“B 代码改变而 A 不重编”首先验证的是
**代码来源与连接时校验机制**，不能据此宣称任意 B 业务函数已可执行。

## 构建和模块更新

沿用 [UserTimeline UB 部署说明](SOCIAL_USER_READ.md)中的机器分工、SDK/OBMM、
数据库、配置文件与应用构建步骤。A/B 同步本包同一源码版本，先运行
`python3 scripts/manifest.py --check`。B 构建 owner 和代码包生成器：

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
cmake --build "$REPO/eRPC/cmake-build-social-ub-domain" -j2 \
  --target client user_timeline home_timeline
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

## 首次连接及端到端运行

先用**不设置**以下两个新变量的原命令各测一轮 legacy 域和 RPC 基线，
确认回归。新模式每轮使用新 B owner，不复用上一轮发布的连接。B 在原有
`run-social-session.sh --side B --mode domain --config ...` 命令前设置：

```sh
export LRPC_UB_SOCIAL_CODE_BUNDLE="$REPO/eRPC-LRPC-UB/social-user.bin"
```

启动器会把该文件传给 `--social --remote-code` owner；B 的 `domain_owner.log`
应显示 `profile=social-user-read code_mode=remote-social`。**不要**在 B 上设置
`LRPC_UB_CODE_MODE`；它是 A 侧调用者选择。B 的 PostStorage 仍用原 RPC-only
构建，不重新编译普通 nginx/LB 进程。

A 在原有 `run-social-session.sh --side A --mode domain --config ...` 命令前设置：

```sh
export LRPC_UB_CODE_MODE=remote-social
```

脚本从配置中的 `client.user_num/home_num` 导出 `LRPC_UB_SOCIAL_READ_SERVICE`，
并与 B 代码包标记在连接时核对。A 的 Client 和 Timeline 应各打印
`UB_DOMAIN_BOUND ... code_mode=remote-social code_source=B-published`；再按原
[UserTimeline](SOCIAL_USER_READ.md)或[HomeTimeline](SOCIAL_HOME_READ.md)
说明运行 `scripts/check-social.py`、比较 `SN_DOMAIN_E2E` 与 RPC 基线。
`--nop-padding 2` 第二轮只需 B 重新生成代码包并重新启动 owner；A 已构建的
模块和应用不重编。两轮务必核对相同的工作负载、配置、CPU/NUMA 与样本范围。

如先做无数据库的 User 路由探针，使用新 B owner 后在 A 运行：

```sh
sudo env LRPC_UB_DOMAIN_NAME="$LRPC_UB_DOMAIN_NAME" \
  LRPC_UB_CODE_MODE=remote-social LRPC_UB_SOCIAL_READ_SERVICE=user \
  LRPC_UBSM_SDK_MODE=owned LRPC_UB_NESTED_PATH=fused taskset -c 1 \
  ./build-caller/lrpc-ub-social-probe --samples 10
```

应有 `UB_SOCIAL_ROUTE_PASS`。停止 A 探针后，结束 B owner，再以新 owner
运行端到端应用；共享机器上不要在线清池或对 busy 模块强制 rmmod。
