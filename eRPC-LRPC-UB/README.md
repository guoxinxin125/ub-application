# UB Direct/Nested 双机实验包

此目录由原仓库的 `scripts/package-ub-domain.py` 生成。`SOURCE_MANIFEST.sha256`
记录每个文件的 SHA-256；A、B 使用同一份生成结果。这里仅包含首次运行
Direct/Nested 所需的源码、PA 探针与一个页表布局测试，没有 QEMU、Go、
currency、social network、旧 shadow 程序或第三方源码。
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

## B：编译并启动 owner

```sh
sh scripts/build.sh owner
./build-owner/lrpc-ub-domain-owner lrpc_ub_domain --provider-numa 0
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
`./build-owner/lrpc-ub-pa-owner lrpc_domain_pa --provider-numa 0`，
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

这个包尚未在真实 UB 主机上编译和执行，生成包及本地静态检查不是硬件验证。
