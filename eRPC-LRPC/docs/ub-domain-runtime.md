# UB B-owned domain：构建与运行

此路径使用真实 UBS Memory 接口，B 构造服务页表和 heap，A 内核执行固定
ARM64 服务。它独立于原来的 `lrpc-ub-shadow`，使用设备
`/dev/ub_lrpc_domain` 和 `LRPC_EXECUTION_BACKEND=ub-remote-domain`。

当前验证状态：用户态 C/C++ 已做 ARM64 交叉编译，gate/服务汇编已生成
ARM64 对象；8 项 Unicorn 服务指令测试、页表布局测试通过。
**内核 C 模块尚未用真实 UB 内核构建，OBMM 双机、远端 page walk、EL0
异常恢复尚未实跑。本文命令是待执行步骤，不是已经获得的测试结果。**

## 实现与内存位置

| 对象 | 所在位置 |
|---|---|
| 四个 domain 的根及下级页表 | B 的 UBS Memory；表项使用 A 视角 PA |
| 服务 heap，32 条 Query 记录 | B 的 UBS Memory，只读映射给服务 |
| 原始服务指令、初始 args/heap/SP/PC | B 发布 |
| 执行代码 | A 内核复制 B 的指令，逐字节验证后提供 EL0 RX 映射 |
| A-stack / E-stack | A 内核本地分配，每 domain 独立；用户映射 RW、NX |
| caller 内核栈、Linux task/mm | A，调用期间保留 |
| shadow PID | 不存在，输出 0 |

内核保留一份固定指令白名单用于验证；执行页的内容来自 B 发布区。
服务不能调用 libc、Linux syscall、Go runtime、TLS、SIMD 或 malloc。
此版本不迁移原来 gRPC-Go/DeathStarBench 的完整 Go 服务，它们仍走旧
scheduler-shadow 路径。新的 Query 是独立的有界整数查询服务。

连接流程：A 导入 -> 内核解析 OBMM PTE 并分配本地页面 -> A 核对 SDK PA
查询 -> A 发布地址契约 -> B 写页表、代码、初始 context 和 heap -> A
逐项校验并复制代码 -> 执行。B 在连接期发布一次，调用期不执行业务。

执行采用独立的实验模块 gate：临时保存/替换 TTBR0、TCR、VBAR、SP_EL0，
保持 A 的 TTBR1 和内核栈，进入 EL0；固定 BRK 返回点将寄存器保存后恢复
Linux 环境，再回到 ioctl。同步故障也走该恢复入口并返回错误，关闭本次
session 的执行资格。它没有修改 Linux 的通用调度器或增加系统调用编号。
该短路径绕过普通 Linux EL0 入口/退出和 KPTI 流程，不能用于不可信代码。

Direct/Query 两次地址空间切换。Nested 将 middle 保存在本地 context/E-stack，
经 Linux 内核执行 leaf，再恢复 middle，共六次切换。当前每次都刷新本 CPU
TLB，未保留 ASID，不能与 x86 的 PCID-retain/four-switch 数据直接比较。

## 平台约束

- 内核 API 基线 Linux 6.6.x，AArch64；其他版本明确拒绝编译，需核对接口。
- 执行 gate 要求内核运行在 EL1；当前明确拒绝 VHE EL2 内核，未推测其寄存器
  别名与异常返回行为。模块在 open 时检查，不能通过忽略 EOPNOTSUPP 绕过。
- shadow 固定使用 4 KiB、48 位 VA 页表，硬件必须支持 TG0=4K；Linux 自己
  的页大小可以不同，TTBR1 的配置保持不变。导入 PA 必须落在 48 位以内，
  当前拒绝 TCR.IPS>48 位和 DS/LPA2 配置。
- OBMM 映射必须是可通过 PTE 查询的共享设备映射，实际类型为 Normal-NC。
  Device 类型、WB 类型和 huge/block 导入映射会拒绝激活，不能删除检查来
  强行继续。数据页 AttrIndx/MAIR 与 TCR 的 NC page-walk 属性分别设置。
- 模块以 `CAP_SYS_RAWIO` 和显式 `experimental=1` 启用，设备权限 0600。
  不支持 pseudo-NMI、KASAN、KCSAN 配置。IRQ/抢占屏蔽期只执行固定有界服务。
- 单 B、单连接、单执行者；A 用 taskset 固定一个 CPU。每轮连接需重启 owner。
  B 必须在 A 关闭后才退出，连接期间不得改页表或强制 unimport。
- A 的特权 caller 持有 SDK 导入映射用于建连。此版本没有提供 caller 对 B
  页表/heap 的强隔离，也不防御 B 在校验后修改页表；A/B 都是可信实验端点。
- 仅在可重启、可接控制台的实验机器运行。同步异常恢复不等于能恢复 UB
  链路挂死、异步硬件错误、热拔出或任意 NMI。保留正常可启动内核。

## 1. 环境信息与构建

两端同步相同源码。A 上先记录：

```sh
uname -a
getconf PAGESIZE
ls -l /lib/modules/$(uname -r)/build
```

两端编译用户程序，不依赖 third_party/eRPC 或 Go：

```sh
sh scripts/build-ub-domain.sh
```

非默认 SDK/OBMM 路径：

```sh
UBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
UBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so \
sh scripts/build-ub-domain.sh \
  -DOBMM_INCLUDE_DIR=/actual/path/to/obmm/include \
  -DOBMM_LIBRARY=/actual/path/to/libobmm.so
```

A 上编译匹配运行内核的模块；KDIR 可以指定对应的已配置内核构建目录：

```sh
sh scripts/build-ub-domain-module.sh
sudo insmod kernel/ub-domain/ub_lrpc_domain.ko experimental=1
ls -l /dev/ub_lrpc_domain
```

该步骤若编译失败，保留完整错误与 uname/config，不应改宏绕过版本或
平台检查。本地交叉编译用户程序不代表此模块已经编译通过。

## 2. 地址探针

先按 [PA 探针说明](ub-remote-domain.md) 运行 `lrpc-ub-pa-probe`。
`build-ub-domain` 中也包含此程序。PA 探针结束后释放其独立共享对象。

## 3. 只做准备与校验，不执行 EL0

B 运行，替换实际 provider 名称：

```sh
./build-ub-domain/lrpc-ub-domain-owner B_PROVIDER_HOST lrpc_ub_domain
```

看到 `UB_DOMAIN_OWNER_READY` 后，A 运行。CPU1 必须属于 A 当前允许的 CPU 集合；
否则将 `-c 1` 改为可用 CPU：

```sh
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1 ./build-ub-domain/lrpc-ub-domain-bench --prepare-only
```

预期 `UB_DOMAIN_PREPARE_PASS execution_tested=0`。这验证发布、地址、页表、
代码字节和初始状态；没有验证硬件 walker。A 结束后，在 B Ctrl-C 并重启
同一 owner 命令，为下一轮建立新连接。

## 4. Direct / Nested / Query

B 重启并 READY 后，A 运行：

```sh
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1 ./build-ub-domain/lrpc-ub-domain-bench
```

依次检查结果：Direct=142、Nested=43、Query-1=1000、Query-8=1007、
Query-32=1031。全部正确才输出 `UB_DOMAIN_E2E_PASS`。
Query 的数字是最多遍历的记录数；每条读取 key/next，命中时另读 value，
不能把 Query-8 直接解释为八次远端 load。
每项预热 100 次、测量 1000 次，报告 min/p50/p99/max/avg，单位 ns。
计时范围为 `lrpc_invoke`，包括 ioctl、映射身份校验、拷贝、切换和结果返回；
不包含建连、B 发布、用户缓冲区初次分配。不是单次 UB load 延迟。

如果出现 `UB_DOMAIN_CALL_FAIL`，保存 errno/ESR/FAR 和 `sudo dmesg | tail -80`。
不要把 `PREPARE_PASS`、仅 Direct 成功或失败前的计时作为全套 PASS。

## 5. eRPC API demo

A 完成上一轮后，B 再次重启 owner，再在 A 执行：

```sh
sudo env LRPC_EXECUTION_BACKEND=ub-remote-domain \
  LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1 ./build-ub-domain/erpc-ub-lrpc-client
```

期望结果 142、`shadow_pid=0`、`ERPC_UB_LRPC_PASS`，以及
`UB_DOMAIN_ERPC_API_LATENCY ... breakdown=unavailable`。
该程序使用仓库的 eRPC API 兼容层，不应称为 upstream eRPC 测试。
新 gate 不提供旧 scheduler 的四个时间戳，不输出虚构的 breakdown。

## 6. upstream eRPC

需要已有的 patched third_party/eRPC（包括 AArch64 timer/barrier/math 和
UB hostname 配置补丁）、CMake >=3.19。新增脚本通过 CMake hook 把新 UB
源文件和 SDK/OBMM 链接进 erpc target，不重复改写 third_party 文件：

```sh
sh scripts/build-ub-domain-upstream.sh
```

此脚本也接受上一节的 SDK 环境变量与 `-DOBMM_*` 参数。
两端分别替换 A_IP/B_IP 为管理网络地址；保留现有 eRPC hugepage/NUMA 运行配置。
B 重启 owner，在另一个 B 终端启动管理面 server：

```sh
env ERPC_CLIENT_HOST=A_IP ERPC_SERVER_HOST=B_IP \
  ./build-ub-domain/upstream-erpc-domain-server
```

A 运行：

```sh
sudo env ERPC_CLIENT_HOST=A_IP ERPC_SERVER_HOST=B_IP \
  ERPC_LRPC_ROLE=caller LRPC_EXECUTION_BACKEND=ub-remote-domain \
  LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1 ./build-ub-domain/upstream-erpc-domain-client
```

必须同时有 `UB_DOMAIN_BOUND`、`UPSTREAM_ERPC_LRPC_RESULT value=142` 和
`UPSTREAM_ERPC_LRPC_PASS`，B 不得出现 `ERROR_REMOTE_CPU_HANDLER_RAN`。
upstream 旧 breakdown 时间戳不适用于此 gate，仅采用其端到端 LATENCY。
如配置了旧 `LRPC_UB_*` 路径变量，不要混用旧二进制；未编译此后端时
选择 `ub-remote-domain` 会返回 ENOTSUP，不会回退 scheduler。

## 7. 退出、测试与同步文件

A 程序先结束，再停止 B 管理 server 和 owner，最后在 A：

```sh
sudo rmmod ub_lrpc_domain
```

页表布局测试由 `build-ub-domain.sh` 自动通过 CTest 执行。
服务指令测试可在开发环境安装 Unicorn 后运行（不证明内核和 UB 硬件行为）：

```sh
aarch64-linux-gnu-gcc -c kernel/ub-domain/services.S -o /tmp/ud-services.o
python3 tests/test_ub_domain_services.py --object /tmp/ud-services.o
```

同步整个本次源码集合：`kernel/ub-domain/`、`include/uapi/linux/ub_lrpc_ub_domain.h`、
`include/lrpc/ub_domain_*.h`、PA 接口及实现、`lib/ub_domain_client.*`、`lib/lrpc.c`、
`demo/ub/domain_*.c`、`demo/erpc_client.cc`、CMakeLists、`cmake/ub-domain-upstream.cmake`、
`scripts/build-ub-domain*.sh`、测试及本文。`build-tools/` 是本地验证工具，不需同步。

内核 PTE 查询与属性实现参考 Linux 6.6 的
[memory.c](https://github.com/torvalds/linux/blob/v6.6/mm/memory.c) 和
[ARM64 pgtable-prot.h](https://github.com/torvalds/linux/blob/v6.6/arch/arm64/include/asm/pgtable-prot.h)。
源码适配与真机验收应分别记录，不能用 Unicorn PASS 替代 UB 测量。
