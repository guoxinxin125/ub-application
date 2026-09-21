# UB B-owned domain：地址接口接入

本页说明独立地址探针：B 分配 UBS Memory，A 导入后查询 A 视角的 OBMM PA，
并验证 `memid + offset -> PA -> memid + offset`。后续 ARM64 内核 gate、
B 页表发布及 Direct/Nested/Query 源码已加入，见
[完整运行说明](ub-domain-runtime.md)。旧 `lrpc-ub-shadow` 仍是 scheduler
版本。`UB_PA_QUERY_PASS` 不能作为远端执行或性能验收结果。

最终布局遵循 [shadow-domain-summary-zh.md](shadow-domain-summary-zh.md)：
B 提供页表和服务 heap；A 保存本地代码副本、A-stack、E-stack 和运行时
context。这里的 PA 是 A 的导入地址窗口，不是 B 的 DIMM PA。

## 编译

在两台 Linux UB 机器同步相同源码，进入 `eRPC-LRPC`。需要已安装 UBSM
SDK、OBMM 用户态库和对应头文件；本步骤不需要 eRPC、Go 或重新编译内核。

```sh
sh scripts/build-ub-pa-probe.sh
```

非默认安装路径可以显式指定（以下路径需替换为机器实际安装路径）：

```sh
UBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
UBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so \
sh scripts/build-ub-pa-probe.sh \
  -DOBMM_INCLUDE_DIR=/path/to/obmm/include \
  -DOBMM_LIBRARY=/path/to/libobmm.so
```

OBMM include 目录需要提供 `libobmm.h`，并能找到匹配的 `linux/obmm.h`。
构建显式链接 libobmm，不依赖 UBSM 库的间接链接。

## 双机运行

确认 UBSM/UBSE 已就绪，在 B 启动，`B_PROVIDER_HOST` 替换为 B 的 provider
主机名。使用独立共享对象名，避免与旧 demo 共用布局。

```sh
./build-ub-pa/lrpc-ub-pa-probe owner lrpc_domain_pa B_PROVIDER_HOST
```

看到 `UB_PA_OWNER_READY` 后，在 A 执行：

```sh
./build-ub-pa/lrpc-ub-pa-probe import lrpc_domain_pa > ub-pa.log 2>&1
rc=$?
cat ub-pa.log
echo "PROBE_EXIT_CODE=$rc"
```

OBMM 查询使用 `/dev/obmm`；遇到 EACCES 时由管理员提供对应访问权限。
两端共享对象的用户/组权限也必须匹配。

A 会查询整个 4 MiB 对象中每个 4 KiB 槽的首尾地址，共 1024 行
`UB_PA_PAGE`。这些是连接期诊断，不放进 RPC 性能计时。
成功结尾为 `UB_PA_QUERY_PASS page_walk_tested=0 execution_tested=0`，退出码 0。
A 结束并解除映射后，再在 B 按 Ctrl-C 释放对象。日志中的 PA 在解除导入后
失效，不能保存后直接交给未来连接的内核使用。

## 接口与后续实现边界

`lrpc_ubsm_import_info` 在 map 后查询本机 mem ID 列表。
`lrpc_ubsm_query_pa` 校验对象名称、大小和分段边界，逐 mem ID 解析偏移，
反向验证身份；不跨 mem ID 假设 PA 连续。返回值遵循现有后端约定：SDK
错误为正值，本地和 OBMM 错误为负 errno。

新内核模块通过实际 VMA/PTE 取址，与本接口的查询结果交叉验证，并持有
对应 OBMM 文件引用；每次执行期间持有 mmap 读锁。外部强制 unimport、
B 提前释放和热拔出仍不在生命周期保证内，不能将 PA 查询称为通用 pin。

`UBSM_FLAG_ONLY_IMPORT_NONCACHE` 控制 SDK mmap 的导入属性。手工页表中的
数据属性还需正确设置 AttrIndx/MAIR；页表遍历属性还涉及 TCR。
PA 查询成功不证明硬件 walker 支持 UB 地址，更不证明 walker 已使用 NC。

## 地址换算单元测试

可在普通开发机运行，不需要 UB 硬件或 OBMM 驱动：

```sh
cc -std=gnu11 -Wall -Wextra -Werror \
  -Iinclude -I../ubs-mem-master/src/app_lib/include -Itests/ub_pa_mock \
  lib/ubsm_pa.c tests/test_ubsm_pa.c -o /tmp/lrpc-test-ub-pa
/tmp/lrpc-test-ub-pa
```

测试使用仓库 SDK 头文件及 OBMM 查询替身，覆盖不连续 mem ID 边界、越界、
失效身份、权限错误、SDK 错误和未映射对象。它不测试真实驱动与一致性。
