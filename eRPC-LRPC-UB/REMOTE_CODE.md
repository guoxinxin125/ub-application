# B 发布受限代码：direct/nested 可选实验

默认的 owner、caller、模块和 `run-ub-domain-smoke.sh` 不传新选项时，仍走原先的
`services.S` 字节匹配路径。本实验**只支持单连接 direct/nested**，不改变 query、
SocialNetwork、读写探针及其 ABI。B 的代码包只交给 B owner；A 不编译、不持有它。
A 模块只内置一个指令格式验证器，不内置代码包的具体算术指令或立即数。

代码包是 AArch64 little-endian 的固定长度结构。B owner 在连接前读取并验证，
随后发布代码长度和各域的机器码；A 在 `UD_COMMIT` 时复制到本地代码页、重新逐条
验证、清理指令缓存并用原有页表运行。允许的代码只有固定的参数/B 数据读取、
无跳转的整数加减或异或、固定的结果写入，以及固定位置的 `BRK`；最多 32 条
指令/域。`nested` 中的 middle 第 4 条指令必须是级联 `BRK`。不能调用 libc、
系统调用、动态分配、任意内存或外部符号。query 在此模式下被拒绝。

这是一条**受限原生代码**机制验证，不是任意 B 程序装载器。A 的 gate 仍在
关闭中断/抢占的状态下运行，虽然验证器排除了分支和循环，仍只能在约定的
实验窗口测试，不应加载不可信的代码文件。模块参数 `allow_remote_code` 默认
关闭；A 不选择 `--code-mode remote` 也不会进入这条路径。

## 1. 同步与构建

两端同步同一版本 `eRPC-LRPC-UB/` 源码；B 生成的代码包**无需传给 A**。
环境变量 `UBSM_*`、A 的 `OBMM_*` 沿用现有部署。两端先检查源码清单：

```sh
python3 scripts/manifest.py --check
```

B：

```sh
sh scripts/build.sh owner
python3 tests/test_remote_code.py  # AArch64 objdump 独立检查生成的机器码
./build-owner/lrpc-ub-remote-code-gen ./remote-code.bin \
  --direct-bias 0 --middle-add 1 --leaf-add 1
```

生成器应打印 `direct_expected=142 nested_expected=43`。代码包是 B 生成的
测试服务，不是 A 的 `services.S`。B owner 的 `--remote-code` 参数会校验并读取该文件。

A（当前机器如为 VHE-EL2）：

```sh
sh scripts/build.sh caller
sh scripts/build-module.sh --vhe
modinfo -F vermagic kernel/ub-domain/ub_lrpc_domain_vhe.ko
modinfo -p kernel/ub-domain/ub_lrpc_domain_vhe.ko | grep allow_remote_code
```

`vermagic` 开头必须等于 `uname -r`。仅在确认没有其他用户占用本项目设备、
结束自己启动的连接并取得共享机器维护窗口后，才替换自己负责的模块：

```sh
lsmod | grep -E '^ub_lrpc_domain(_vhe)?[[:space:]]'
sudo fuser -v /dev/ub_lrpc_domain
sudo rmmod ub_lrpc_domain_vhe  # 没加载时跳过；busy 时停止，不要强制卸载
sudo insmod kernel/ub-domain/ub_lrpc_domain_vhe.ko \
  experimental=1 vhe_gate=1 allow_remote_code=1
sudo cat /sys/module/ub_lrpc_domain_vhe/parameters/allow_remote_code
```

若 A 是 EL1 而非 VHE，改为 `sh scripts/build-module.sh`、
`ub_lrpc_domain.ko`/`ub_lrpc_domain`，`insmod` 时去掉 `vhe_gate=1`。
构建 `.ko` 不会更新已加载模块；任何卸载失败都先查占用者。

## 2. 先确认默认路径没有回归

B 不带 `--remote-code` 启动旧 owner（保持终端运行）：

```sh
./build-owner/lrpc-ub-domain-owner lrpc_code_legacy --provider-numa 1
```

A 不带 `--code-mode`，执行原有 direct 测试：

```sh
sudo sh scripts/run-ub-domain-smoke.sh --cpu 1 --name lrpc_code_legacy \
  --test direct --samples 1 --warmup 0
```

应看到 `UB_DOMAIN_TEST_PASS`，且 `UB_DOMAIN_BOUND` 中 `code_mode=legacy`。
A 结束后，再结束**自己启动的** B owner。单连接每轮要用新 owner。

## 3. B 发布代码，A 在不知道代码包内容的条件下执行

B（保持终端运行，NUMA 1 仅为示例）：

```sh
./build-owner/lrpc-ub-domain-owner lrpc_code_remote --provider-numa 1 \
  --remote-code ./remote-code.bin
```

A 先只做连接建立、代码复制和验证，**不进入服务域执行**：

```sh
sudo env LRPC_UB_DOMAIN_NAME=lrpc_code_remote taskset -c 1 \
  ./build-caller/lrpc-ub-domain-bench --prepare-only --code-mode remote
```

应看到 `UB_DOMAIN_PREPARE_PASS execution_tested=0`。该连接完成后结束 B owner，
以同一命令**重新启动一个新 owner**，再做以下 direct 首次执行。单连接 owner
不能复用上一轮的发布记录。

A direct：

```sh
sudo sh scripts/run-ub-domain-smoke.sh --cpu 1 --name lrpc_code_remote \
  --test direct --code-mode remote --samples 1 --warmup 0
```

确认 `code_mode=remote-verified`、`result=142` 和 `UB_DOMAIN_TEST_PASS`。
A 结束后，先结束 B owner，再用**同一代码包、新 owner**重测 nested：

```sh
# B
./build-owner/lrpc-ub-domain-owner lrpc_code_remote --provider-numa 1 \
  --remote-code ./remote-code.bin

# A：另一个终端，B owner 已启动
sudo sh scripts/run-ub-domain-smoke.sh --cpu 1 --name lrpc_code_remote \
  --test nested --nested-path fused --code-mode remote --samples 1 --warmup 0
```

应得到 `result=43`、`UB_DOMAIN_TEST_PASS`。如果要测试 legacy 级联，另起一轮
B owner，并把 A 的 `--nested-path fused` 改为 `legacy`。

## 4. 验证只改变 B 代码，A 不重编

先结束上一轮 B owner，仅在 B 重新生成代码包：

```sh
./build-owner/lrpc-ub-remote-code-gen ./remote-code-v2.bin \
  --direct-bias 7 --middle-add 3 --leaf-add 2
```

生成器应打印 `direct_expected=149 nested_expected=46`。A 的二进制、模块、
代码和配置均不重新构建。每轮 B 新启动一个单连接 owner，A 分别运行：

```sh
# B：先启动；direct 结束后需结束并重启一次再测 nested
./build-owner/lrpc-ub-domain-owner lrpc_code_remote --provider-numa 1 \
  --remote-code ./remote-code-v2.bin

# A：direct 轮
sudo sh scripts/run-ub-domain-smoke.sh --cpu 1 --name lrpc_code_remote \
  --test direct --code-mode remote --expected-direct 149 --samples 1 --warmup 0

# A：nested 轮，B 已重启同一命令
sudo sh scripts/run-ub-domain-smoke.sh --cpu 1 --name lrpc_code_remote \
  --test nested --code-mode remote --expected-nested 46 --samples 1 --warmup 0
```

这轮能区分“代码由 B 提供”与“只读取了 B 的数据”。但 A 仍知道**允许的指令
类别和页表契约**；这不等于可直接运行 B 的现有 C++ 服务函数。失败时保留两端
日志、`dmesg` 的 `ub_domain` 信息和模块版本；不要在共享机器上反复重试故障代码。
