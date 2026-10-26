# UB Direct/Nested 双机实验包

此目录现在作为 **UB 版本的独立维护源码目录**；后续修改直接改这里，不再从
`eRPC-LRPC/` 生成，也不用维护第二个 trace 目录。`SOURCE_MANIFEST.sha256`
记录传输源码的 SHA-256；修改后运行 `python3 scripts/manifest.py --refresh`
更新清单，传到 A、B 后运行 `python3 scripts/manifest.py --check` 核对。
这里包含 Direct/Nested、只读多连接、PA 探针，以及可选的 UserTimeline/HomeTimeline read 路由接入；
另外提供隔离的[服务域远端写入探针](SOCIAL_WRITE_PROBE.md)，它尚未接入发帖业务。
多连接访问同一 B 侧页的读写/原子更新实验见
[共享页正确性探针](SOCIAL_SHARED_PROBE.md)；它同样不是业务写入。
如果这次同步的是新增的共享读写/原子探针，**从该文档的“第一次部署或代码更新”
开始执行**：A/B 都要重编译，A 还要在设备空闲时重新加载新模块；只运行
`sh scripts/build.sh caller` 不会更新已加载模块。
没有 QEMU、Go、currency、旧 shadow 程序或第三方源码。应用接入需另外同步 `eRPC/`
中的应用与构建依赖，详见 [UserTimeline UB 接入说明](SOCIAL_USER_READ.md)及
[HomeTimeline UB 接入说明](SOCIAL_HOME_READ.md)。
多独立 Client 进程的 UserTimeline/HomeTimeline 并发延迟对照见
[SocialNetwork 并发实验步骤](SOCIAL_CONCURRENT_READ.md)。
下一轮 Direct/Nested 20 轮性能测量及应用验证清单见 [UB 真机实验待办](TODO_UB.md)。
Nested 微测试默认使用 **fused**，备选路径用 `--nested-path legacy` 或
`LRPC_UB_NESTED_PATH=legacy` 显式选择；命令行优先于环境变量。
bench、多连接测试、TLB probe、trace 和直接调用客户端库遵循同一默认值。
SocialNetwork 应用链同样默认 fused；显式设置 `LRPC_UB_NESTED_PATH=legacy`
可选择备选路径。HomeTimeline 接入还需要目标 AArch64 模块与双机验证，见接入说明。
双机应用可用 `scripts/run-social-session.sh` 在每台机器以一个前台命令管理本轮进程；
脚本只会结束本轮自己启动的 owner 或 SSH 隧道，不改变既有数据库/UB 服务。
应用的独立 A/B 模板位于 `config/social-user-read-a.json` 和
`config/social-user-read-b.json`；`prepare-social.py` 默认从本包模板生成私有配置，
不再要求读取旧的机器编号配置。DB 位置和 A/B 执行位置见应用接入说明第 1、4 节。
同步到两端后、构建前可运行 `sha256sum -c SOURCE_MANIFEST.sha256` 检查传输。

源码中的固定发布格式仍包含 Query domain 与指令；本包的运行步骤只执行
Direct/Nested。不要为缩小包而单独删除第 4 个 domain：这会改变当前 ABI。

## 依赖

- 两端均为 AArch64 Linux，安装了 UBS Memory SDK，能访问 UB 设备。
- A（caller）另外需要 OBMM 头文件/库、正在运行内核的匹配 `kernel-devel`
  和 `Module.symvers`。A 构建模块需要 make、编译器；用户态需要 CMake、
  编译器和 CTest。B 的 owner 构建不直接链接 OBMM，也不需要内核开发包；
  UBSM SDK 自身依赖的系统库仍须可用。
- 机器和连接配置、平台限制以实验环境实测为准。当前内核模块适配 Linux 6.6，
  仅用于双方可信、可重启的实验主机。

## 首次上机：先收集配置

下面的命令均为只读检查。把同一份本目录复制到 A、B 后，**两端**先运行：

```sh
hostname
uname -a
uname -m
getconf PAGESIZE
command -v cmake cc make ctest taskset sha256sum
ls -l /usr/local/ubs_mem/include/ubs_mem.h \
      /usr/local/ubs_mem/include/ubs_mem_def.h \
      /usr/local/ubs_mem/lib/libubsm_sdk.so
lscpu -e=CPU,NODE,ONLINE | head -30
taskset -pc $$
sha256sum -c SOURCE_MANIFEST.sha256
```

`lscpu` 和 `taskset` 的输出用于从 A 的 NUMA0 中选择一个实际允许的 CPU；
下文的 CPU `1` 只是示例。若 SDK 路径不同，记录真实头文件和库路径，供
`UBSM_INCLUDE_DIR`、`UBSM_LIBRARY` 使用。两端应有同一份源码清单校验结果。
当前已确认的测试组合是 A NUMA0/B provider NUMA1，CPU 1 可用于 A；
A NUMA2/B provider NUMA3 是另一组，切勿把 B 的 provider NUMA 写成 A 的编号。

**仅 A（caller）** 再运行：

```sh
ls -l /lib/modules/$(uname -r)/build/Module.symvers
awk '$2=="arm64_mm_context_get" || $2=="arm64_mm_context_put" {print $2,$4}' \
  /lib/modules/$(uname -r)/build/Module.symvers
grep -E '^(CONFIG_ARM64_PSEUDO_NMI|CONFIG_KASAN|CONFIG_KCSAN)=' \
  /boot/config-$(uname -r)
ls -l /usr/local/include/libobmm.h /usr/local/ubs_mem/lib/libobmm.so
```

最后一条若提示文件不存在，OBMM 可能安装在别处，记录实际路径即可；
`grep` 没有输出的配置项也可能只是未启用。`CONFIG_ARM64_PSEUDO_NMI=y`
本身并不等于运行时启用了优先级屏蔽，模块会在打开设备时再检查。
把 A/B 输出、之前双机共享内存测试的成功标志，以及拟用的 NUMA0 CPU
发给协作者核对，再按以下顺序构建和启动：B owner → A caller 与模块 →
PA 探针 → prepare-only → 单次 Direct/Nested → TLB 对照。

## 多连接与只读并发（可选）

新增 `--multi` owner 和 `lrpc-ub-domain-multi`。默认 owner 不变，仍是旧的单连接协议；
**本节之外原有的“每次连接前重启 owner”要求适用于 legacy owner。** 新 caller 自动识别
两种协议。`--multi` owner 在成功关闭连接后可以继续使用，不需要每轮重启。

### 范围与隔离

- 当前是一台 A 的内核连接一台 B，单个共享区域最多 **8 个同时存活的连接**。
  支持 A 上同一进程的多个线程，也支持 A 上多个进程。不是多台 A 共享同一个连接池。
- 每个连接由创建它的线程使用、固定在一个 CPU 上；不要跨线程共享同一个 handle。
  一个连接仍包含 Direct/Middle/Leaf/Query 四个域，不是一个服务只能有一个 client。
- B 只初始化一次只读 heap、服务镜像和初始 context。每个连接有独立的 B 端页表槽；
  A 的参数页/A-stack、E-stack、代码副本、ASID、completion 和调用暂存对象也相互独立。
  页表中的服务 heap 是 RO/NX，服务代码是 RX；业务执行不修改 B 的共享数据。
- 连接建立/释放使用 A 内核的局部锁；调用热路径没有新增跨连接全局锁或远端原子 RMW。
  成功连接关闭时先定向失效它的 ASID，再回收槽位，不清空其他连接的 TLB。
- 同一进程内，同名共享区域只导入一次，并对 SDK 初始化和导入映射分别引用计数。
  关闭一个 handle 不会 unmap 其他 handle 使用的区域，最后一个才 unmap/finalize。
  默认 `LRPC_UBSM_SDK_MODE=owned`；只有嵌入已自行初始化 SDK 的程序时才使用 `external`，
  此时外部程序必须保证 SDK 比所有 LRPC 连接活得更久，库不负责初始化/结束 SDK。

这是受信任、固定映射、只读服务的实验实现，不是任意 Linux 服务的透明迁移；
不包括业务写入、动态撤销映射、多 A 协议或 B 在线重启。
SocialNetwork 的可选单 lane 接入单独使用 `--social` owner，见
[UserTimeline UB 接入说明](SOCIAL_USER_READ.md)，不能把微测试 owner 当成应用 owner。

### 更新两端并替换 A 模块

把本目录同步到 A/B。B 只重建 owner；A 重建 caller 和对应 EL1/VHE 模块。
保留原来的 SDK/OBMM 路径环境变量，不需要修改系统启动配置，也不需要重启机器。
**先结束所有使用此设备的 A 程序和旧 B owner，再替换本项目模块。** 不要卸载别人的模块。

```sh
# B：构建。启动命令见下一段。
sh scripts/build.sh owner

# A：当前 VHE/EL2 机器
sh scripts/build.sh caller
sh scripts/build-module.sh --vhe
modinfo -F ud_multi kernel/ub-domain/ub_lrpc_domain_vhe.ko
# 应有 pool-v2-8-slots-single-importer-kernel

# 确认所有本项目连接已退出；如原来加载的是 VHE 模块：
sudo rmmod ub_lrpc_domain_vhe
sudo insmod kernel/ub-domain/ub_lrpc_domain_vhe.ko experimental=1 vhe_gate=1
```

尚未加载模块时跳过 `rmmod`。EL1 机器使用 `sh scripts/build-module.sh`、
`ub_lrpc_domain.ko` 和 `experimental=1`，不要设置 `vhe_gate`；两种模块不同时加载。
首次并发验证使用不带 `--trace` 的构建。

### B：启动连接池 owner 并保持运行

```sh
./build-owner/lrpc-ub-domain-owner lrpc_ub_domain --provider-numa 1 --multi
```

等到 `UB_DOMAIN_OWNER_READY multi=1 capacity=8 ...`。B provider NUMA 1 对应 A NUMA 0；
以下示例 CPU 1、2 必须都在 A 当前允许的 CPU 列表中且属于期望的 NUMA 节点。
`--provider-host` 等参数仍可按原方式指定。

### A：先小样本，再多进程，再增大样本

```sh
taskset -pc $$
lscpu -p=CPU,NODE,ONLINE | awk -F, '$1 !~ /^#/ && $2==0 && $3=="Y" {print $1}'

# 两个线程、每人 3 次测量：mixed 会依次覆盖 Direct/Nested/Query。
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  ./build-caller/lrpc-ub-domain-multi --mode threads --cpus 1,2 \
  --test mixed --nested-path fused --warmup 0 --samples 3

# 成功后不必重启 --multi owner；两个独立进程分别建立连接。
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  ./build-caller/lrpc-ub-domain-multi --mode processes --cpus 1,2 \
  --test mixed --nested-path fused --warmup 0 --samples 3

# 正确性均通过后再测稳定延迟；Direct/Nested 分开统计。
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  ./build-caller/lrpc-ub-domain-multi --mode threads --cpus 1,2 \
  --test direct --warmup 10 --samples 100
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  ./build-caller/lrpc-ub-domain-multi --mode threads --cpus 1,2 \
  --test nested --nested-path fused --warmup 10 --samples 100
```

`--cpus` 的每一项对应一个 worker，数量 2–8，例如 `1,2,3,4` 是四个 worker。
不要用外层 `taskset -c 1` 限制这些多 CPU 测试。`1,1` 可以验证同 CPU 交错执行，
但不是物理并行，输出会标注 `unique_cpus=1`。`processes` 模式在初始化 SDK 之前 fork，
不要在自己的程序初始化 SDK 后直接 fork 再使用继承的 SDK 状态。

测试先让所有连接同时存活，检查不同 slot/identity、相同 owner generation，然后开始调用。
Direct/Nested 使用各 worker 不同的输入校验结果；Query 校验只读链表结果。
测量后 worker 0 关闭并重建连接，其余 worker 保持连接并继续调用；最后检查所有调用计数、
结果和清理状态。每个 worker 额外执行 32 次存活/重连校验，不计入延迟样本。

应有每个 worker 的 `UB_DOMAIN_MULTI_RESULT` 和最终：

```text
UB_DOMAIN_MULTI_PASS mode=threads workers=2 unique_cpus=2 ... read_only=1 slots_unique=1 reconnect=1 peer_survived_close=1
```

每个 worker 独立输出 min/p50/p99/max/avg；`mixed` 是混合工作负载，不能当成单独 Nested
延迟。这里 `scope=invoke-and-verify` 包含请求准备、调用和结果校验，排除建连、预热、
线程间等待和重连；不同于原 bench 的更窄 `scope=lrpc_invoke`，不要直接混为同一口径。
逐步增加到 1000 个样本或 4/8 个 worker 前先检查 `dmesg`，不要一开始压满共享机器。

要测备选 legacy Nested，将上面命令的 `--nested-path fused` 改成 `legacy`，两者使用
同一个 owner 和模块，不需重启 B 的 `--multi` owner。单执行者 fused 已完成 UB 真机
20 轮测量；多连接并发仍需独立真机验证，不能由单执行者结果推断。
已有单连接 bench、TLB probe、trace 也会自动识别 `--multi` owner；成功释放后可继续建连。

### 生命周期与失败处理

正常退出顺序始终是 **所有 A 连接结束，再停止 B owner**。B 存活期间保持代码、共享 heap
和已发布页表不变；关闭/重连只重写已回收槽位的页表。新连接不会重新初始化正在读取的 heap。
不要在还有 A 连接时重启 B、解除共享区域或重新发布数据。

容量耗尽返回 `ENOSPC`。若 PREPARE 已向 A 返回，但后续 PA 校验、发布、COMMIT 或连接建立
失败，模块保守隔离该槽位，避免 B 延迟处理旧请求后覆盖新连接。日志会出现
`quarantined pool slot`；这类失败不会自动复用槽位。排查并保留日志，等所有 A 连接停止后，
再停止 B、卸载本项目模块、重新加载并启动 fresh owner。不是重启整台机器，也不允许运行中
清空连接池。B generation 改变而 A 仍有旧槽位时会拒绝 `ESTALE`。

本地回归（需要 Python、gcc、pthreads；OS/SDK/服务指令有 fixture，不代表 UB 实测）：

```sh
python3 -m unittest discover -s tests -p 'test_multi.py' -v
python3 -m unittest discover -s tests -p 'test_nested_client.py' -v
python3 -m unittest discover -s tests -p 'test_nested_runtime.py' -v
```

覆盖真实发布器/槽位分配器、prepare/commit 的实际 C、8 线程 SDK 引用计数、映射复用、
失败清理、实际多线程测试程序的编排/重连/失败标志。内核 VMA/ASID/MMU、SDK、物理并行访存
不是本地 fixture 能验证的；多进程执行、全模块构建及实际性能必须在 UB 主机确认。

## B：编译并启动 owner

```sh
sh scripts/build.sh owner
./build-owner/lrpc-ub-domain-owner lrpc_ub_domain --provider-numa 1
```

若 UBSM 安装路径不同，在构建时设置 `UBSM_INCLUDE_DIR`、`UBSM_LIBRARY`。
若 UBSE 注册使用的 B 主机名与系统 hostname 不同，owner 加上
`--provider-host REGISTERED_B_HOST`。看到 `UB_DOMAIN_OWNER_READY` 再启动 A。

## A：构建、准备、单次 Direct/Nested

```sh
sh scripts/build.sh caller
sh scripts/build-module.sh
sudo insmod kernel/ub-domain/ub_lrpc_domain.ko experimental=1
```

若 OBMM 不在标准路径，构建时设置 `OBMM_INCLUDE_DIR`、`OBMM_LIBRARY`。
先用独立对象做 PA 探针。B 运行
`./build-owner/lrpc-ub-pa-owner lrpc_domain_pa --provider-numa 1`，
看到 `UB_PA_OWNER_READY` 后，A 运行
`./build-caller/lrpc-ub-pa-probe import lrpc_domain_pa`。
探针结束后先退出 A，再在 B 用 Ctrl-C 结束探针 owner，随后启动正式 owner。

首次只检查连接与页表准备，不执行服务。CPU 号选择 A 上允许的 NUMA0 CPU：

```sh
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1 ./build-caller/lrpc-ub-domain-bench --prepare-only
```

看到 `UB_DOMAIN_PREPARE_PASS` 后，先结束 B owner，重新运行 owner 命令。
每轮 A 调用只消费一个连接，下一轮前都要重启 B owner：

```sh
sudo sh scripts/run-ub-domain-smoke.sh --cpu 1 --test direct
# B 重启 owner 并出现 READY 后：
sudo sh scripts/run-ub-domain-smoke.sh --cpu 1 --test nested
```

脚本默认一次调用、零预热；还支持 `--samples N --warmup N`。
Direct 预期结果 142，Nested 预期 43。单次调用验证正确性，不构成延迟统计。
完成首次 Direct/Nested 后，B 重启 owner，可在 A 做定向 TLB 对照：

```sh
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1 ./build-caller/lrpc-ub-domain-tlb-probe \
  --test direct --samples 20 --warmup 2
```

Nested 需再次重启 B owner，改为 `--test nested`。探针每轮先失效该服务的
ASID，再测同一服务的两次 gate；接着测两次不失效的对照。输出每轮的四个
`*_ns` 和校正后的 `paired_effect_p50_ns`。失效操作、ioctl、VMA 校验均不在
gate 计时区间内。一次探针连接总共运行 `2 + 4*(warmup+samples)` 次服务。
`UB_TLB_PROBE_PASS` 仅确认结果与 CPU 一致，`page_walk_proven=0` 明示计时
差异不能单独证明远端页表被读取；还需结合硬件 PMU/UB 计数器。

`run-ub-domain-smoke.sh` 不负责加载模块、重启 owner 或隔离内核故障。
在 B owner 退出之前先结束 A 的连接；出现错误时保留 A/B 日志和 A 的
`dmesg`。模块会在执行期间临时切换地址空间，真机首次运行应选择已约定、
可接控制台的实验时段。

2026-10-16 用户反馈：B owner 与 A caller/EL1 模块已在目标机器编译，
PA 探针输出 `UB_PA_QUERY_PASS page_walk_tested=0 execution_tested=0`；
EL1 模块在该 A 机器 open 时因内核运行于 EL2 被安全拒绝。以上只证明
构建和 PA 查询，不是页表 gate、Direct/Nested 或性能的真机验证。

## VHE/EL2 分支（与原 EL1 模块分开）

当前 A 机器的 Linux 在 EL2，原 `ub_lrpc_domain.ko` 按设计在设备 open 时
拒绝运行。另有实验性 `ub_lrpc_domain_vhe.ko`：共用用户态协议与验证逻辑，
但单独编译 EL2 gate，并要求 CurrentEL=EL2、HCR_EL2.E2H/TGE=1。
默认 `sh scripts/build-module.sh` **只构建 EL1 模块**；若需要先做离线编译检查：

```sh
sh scripts/build-module.sh --vhe
modinfo -F vermagic kernel/ub-domain/ub_lrpc_domain_vhe.ko
```

VHE gate 还要求模块参数 `experimental=1` 和单独的 `vhe_gate=1`；
它与 EL1 模块使用同一个 `/dev/ub_lrpc_domain`，不能同时加载。用户反馈已在
EulerX 真机完成 VHE 模块构建、open/prepare、Direct/Nested 单次调用和
100 次稳态调用；Direct TLB 清除对照也已通过。这些结果不等于异常恢复、
并发安全或硬件 page walk 次数已经验证。共用机器上更换模块应先协调实验
时段，确认没有活动连接；本打点修改尚未在该真机上验证。

## 可选诊断：域切换与远端 load

此版本在同一代码目录内增加 `UD_TRACE_CALL` 诊断 ioctl 和
`build-caller/lrpc-ub-domain-trace`。`sh scripts/build-module.sh --vhe` 是
原有基准构建（不编译诊断时间戳）；只有
`sh scripts/build-module.sh --vhe --trace` 才打开 `UD_TRACE_ENABLE`，编译
新的诊断 ioctl 和 gate 区间打点。`UD_MAPPED_CALL` 接口不变；重新构建并
加载 trace 模块后也应重测基准，不把带打点与未打点模块的数字直接比较。
把更新后的目录同步到 A，在 A 上沿用前面配置好的 SDK/OBMM 构建环境变量，
编译 caller 与带打点的 VHE 模块：

```sh
sh scripts/build.sh caller
sh scripts/build-module.sh --vhe --trace
modinfo -F ud_trace kernel/ub-domain/ub_lrpc_domain_vhe.ko
```

`modinfo` 应输出 `enabled`（普通 `--vhe` 构建应输出 `disabled`）。
**编译只更新磁盘上的 `.ko`，不会替换当前已加载的旧模块。** 新 ioctl
必须搭配新模块；在共用机器上，先协调实验时段、结束 A 端连接并确认没有
其他用户占用设备，然后才在 A 上执行：

```sh
sudo fuser -v /dev/ub_lrpc_domain
sudo rmmod ub_lrpc_domain_vhe
sudo insmod kernel/ub-domain/ub_lrpc_domain_vhe.ko experimental=1 vhe_gate=1
```

若旧模块本来没有加载，跳过 `rmmod`；若提示 busy，**不要强制卸载**，先查明
占用者。`modinfo` 检查的是磁盘文件，并不能证明旧模块已被替换。EL1 与 VHE
模块不能同时加载，也不要在其他用户使用设备时热替换。

B owner 的协议未改变。每次新连接先在 B 端启动全新 owner，并等待
`UB_DOMAIN_OWNER_READY`：

```sh
./build-owner/lrpc-ub-domain-owner lrpc_ub_domain --provider-numa 1
```

然后在 A 端先做小样本 direct；A 退出后结束并重启 B owner、再次看到
`UB_DOMAIN_OWNER_READY`，才做 nested：

```sh
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1 ./build-caller/lrpc-ub-domain-trace \
  --test direct --warmup 2 --samples 20
# 结束 A 连接，重启 B owner 并再次看到 READY 后：
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1 ./build-caller/lrpc-ub-domain-trace \
  --test nested --warmup 2 --samples 20
```

`UB_DOMAIN_TRACE_SAMPLE` 每次打印完整诊断 ioctl 时间、内核 gate 总时间、
各个 gate 区间、两个内核交接区间，以及实际切换次数；汇总行
`UB_DOMAIN_TRACE_RESULT` 给出 min/p50/p99/max/avg。Direct 应有 1 段、
2 次切换；默认 fused Nested 为一个整链区间、4 次逻辑切换。
显式 `--nested-path legacy` 才有 middle/leaf/middle-return 3 段、6 次切换。
每段 gate 包含进入、EL0 服务执行和异常返回，**不是纯页表切换时间**；
诊断时间戳本身也增加开销，不能与未打点基准直接比较。辅助字段
`a_view_remote_heap_load` 是 A 用户态通过 NC import 单独读取 B heap 的
一次 load，包含计时调用开销；它**不是**服务代码中那次 `[x1]` load 的
精确计时，Nested 服务本身也不读取 B heap。硬件 page walk 次数仍未观测。

## Nested 默认 fused 与备选 legacy

默认使用 `fused`，可通过 `--nested-path legacy`（或环境变量
`LRPC_UB_NESTED_PATH=legacy`）选择备选路径。选择在连接 PREPARE 前完成，
命令行优先于环境变量。路径在连接准备后固定，
一次连接中的 benchmark、trace、TLB probe 均使用同一选择。
不识别的路径在 bind 前拒绝；旧模块不支持新 ioctl 时明确失败，不回退为 legacy。

2026-10-19 的单执行者实测中，两条 Nested 路径各 20 轮、每轮预热 100 次和测量
1000 次：legacy 总体 avg 2262.559 ns，fused 为 2090.398 ns，降低约 7.6%；
20 轮逐轮平均值均是 fused 更低。见 `results/bench-20261019-a0-b1/summary.json`。
这次默认值调整只涉及用户态：A 同步源码后运行 `sh scripts/build.sh caller`。
当前已加载模块若已支持 legacy/fused，无须因此重新构建或加载模块，B owner 也无须重建。
应用若静态链接了更新的库则需重新构建；SocialNetwork 目前仍使用其独立路由链。

```text
legacy: A 内核 -> middle -> 恢复 A -> leaf -> 恢复 A -> middle -> 恢复 A
fused:  A 内核 -> middle -> gate -> leaf -> gate -> middle -> 恢复 A
```

fused 使用一个新的汇编入口。第一次 middle 的合法 `brk #0x4c1` 保存暂停
上下文，并把参数写入 leaf 的本地 A-stack；leaf 的合法 `brk #0x4c0` 返回后，
将结果放入 middle 的 x2，恢复 middle 的 SP/寄存器，从暂停点下一条指令继续。
middle 最终返回后才恢复 Linux 的原页表、TCR、VBAR、SP_EL0 等环境。
中途不调用 Linux C 函数。每阶段核验异常向量、异常类型、立即数和批准的返回 PC；
错误从当前域恢复 A 并使连接失效，不继续调用下一域。

正常 Nested 的 `transitions=4` 表示 A->middle->leaf->middle->A 四次逻辑地址空间
切换；不计安全切换使用的临时空页表，也不表示只经过四次特权级转换。
三个服务阶段仍各有一次进入 EL0 和一次异常返回。每个域继续持有自己的 pinned
ASID，普通调用不执行 TLBI；A-stack 和 E-stack 本地化、连接对象复用不变。
这仍是固定、可信的 Nested 流程，未新增任意服务或动态嵌套能力。

### A 上重新编译并更换模块

B 的发布协议及服务代码未改变，无须重编 B owner。每次 A 新连接前仍需
结束旧连接并重启 B owner。下面在 **A 的本目录**、沿用 SDK/OBMM 环境变量执行：

```sh
sh scripts/build.sh caller
sh scripts/build-module.sh --vhe
modinfo -F ud_nested kernel/ub-domain/ub_lrpc_domain_vhe.ko
```

`ud_nested` 应输出 `legacy,fused`。编译不会更新已加载的模块；确认没有其他用户
使用设备并结束所有 A 连接后，按前文的模块更换步骤操作：

```sh
sudo fuser -v /dev/ub_lrpc_domain
sudo rmmod ub_lrpc_domain_vhe
sudo insmod kernel/ub-domain/ub_lrpc_domain_vhe.ko experimental=1 vhe_gate=1
```

没有已加载模块时跳过 rmmod，busy 时不强制卸载。EL1 机器则省略构建参数 `--vhe`，
使用 `ub_lrpc_domain.ko`，加载时只传 `experimental=1`。
两种模块都包含 legacy/fused，不能同时加载。

### 先单次正确性，再测稳态对照

B 按原命令启动 owner，看到 READY 后，在 A 执行：

```sh
sudo sh scripts/run-ub-domain-smoke.sh --cpu 1 --test nested \
  --nested-path fused --warmup 0 --samples 1
```

应有 `UB_DOMAIN_BOUND ... nested_path=fused`、
`UB_DOMAIN_RESULT case=nested result=43 ... nested_path=fused transitions=4`，
以及 `UB_DOMAIN_TEST_PASS test=nested`。此时只验证正确性，不宣称性能提升。

结束 A 后，在 B 结束并重启 owner，看到新的 READY，再测 legacy：

```sh
sudo sh scripts/run-ub-domain-smoke.sh --cpu 1 --test nested \
  --nested-path legacy --warmup 10 --samples 100
```

再次结束 A、重启 B owner 并看到 READY，再测 fused：

```sh
sudo sh scripts/run-ub-domain-smoke.sh --cpu 1 --test nested \
  --nested-path fused --warmup 10 --samples 100
```

legacy 应报告 6 次、fused 应报告 4 次逻辑切换，结果都为 43。
比较同一模块构建、CPU、样本和预热设置下的 `scope=lrpc_invoke` 延迟。
正式性能使用不带 `--trace` 的模块，建议交替多轮运行。新代码不要求修改启动参数或重启机器。

### fused 的 TLB probe 和 trace

每条命令都是新连接，分别先重启 B owner。TLB probe 不需要 trace 模块：

```sh
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1 ./build-caller/lrpc-ub-domain-tlb-probe \
  --test nested --nested-path fused --warmup 2 --samples 20
```

trace 需要先用 `sh scripts/build-module.sh --vhe --trace` 构建，按前文流程替换
当前模块，再启动新 B owner：

```sh
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1 ./build-caller/lrpc-ub-domain-trace \
  --test nested --nested-path fused --warmup 2 --samples 20
```

fused trace 只有一个 `fused_nested_gate` 整链区间及 `kernel_gate_total`，
不会输出不存在的独立 leaf/resume/handoff 区间。它包括整链的进入、服务执行、
交接、异常检查和恢复，不是纯切换时间。旧 trace 三段仍仅用于 legacy。
辅助远端 heap load 仍是单独测量，不是 Nested 的业务访存。

### 本地回归（不等于 UB 真机验证）

C runtime fixture 编译实际 `ud_run`/`ud_run_fused`，覆盖两条路径、copy/mmap、
trace 开关、错误失效及重复调用；client fixture 覆盖路径选择、CLI、结果切换次数、
陈旧 completion、busy、错误及 CPU 不匹配。需要本地 gcc 与 Python：

```sh
python3 -m unittest discover -s tests -p 'test_nested_runtime.py' -v
python3 -m unittest discover -s tests -p 'test_nested_client.py' -v
```

指令测试需要 Unicorn 2.x；用现有 AArch64 编译器生成临时对象（交叉编译可将
`cc` 换成 `aarch64-linux-gnu-gcc` 或 `zig cc -target aarch64-linux-musl`）：

```sh
TEST_OBJECTS=$(mktemp -d)
cc -c kernel/ub-domain/gate_nested.S -o "$TEST_OBJECTS/gate_nested.o"
cc -c kernel/ub-domain/gate_nested_vhe.S -o "$TEST_OBJECTS/gate_nested_vhe.o"
cc -c kernel/ub-domain/services.S -o "$TEST_OBJECTS/services.o"
cc -c tests/nested_sysregs.S -o "$TEST_OBJECTS/sysregs.o"
cc -DUD_GATE_VHE -c tests/nested_sysregs.S -o "$TEST_OBJECTS/sysregs_vhe.o"
python3 tests/test_nested_gate.py --objects "$TEST_OBJECTS"
```

测试执行真实 gate 和服务指令，核验参数、middle 栈/寄存器恢复、无中途恢复 A、
无 per-call TLBI、EL1/VHE 寄存器编码，以及三个阶段的异常返回路径。
系统寄存器、ERET、异常入口和域映射由 fixture 接管；不模拟真实 MMU、UB 或内核，
不能证明硬件异常恢复和性能。单执行者性能证据见上述真机 20 轮结果，
异常恢复及多连接并发仍需各自的目标机验证。
