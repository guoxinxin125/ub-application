# OBMM 微测试

本目录包含三个彼此独立的 OBMM 测试程序。单机测试与双机测试使用不同的可执行文件，避免误用角色参数或复用其他测试的运行状态。

- `obmm_local_nc_test`：单机执行 OBMM export、NC mmap、本地 load/store 正确性和时延测试；
- `obmm_nc_exporter`：双机 NC 可见性测试的 owner/export 端；
- `obmm_nc_importer`：双机 NC 可见性测试的 remote/import 端。

## 1. 当前测试语义

所有测试都通过以下方式映射 `/dev/obmm_shmdev<mem_id>`：

```c
open(path, O_RDWR | O_SYNC);
mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

`O_SYNC` 表示使用 non-cacheable（NC）映射。因此当前程序：

- 不调用 `obmm_set_ownership()`；
- 不执行 CLWB；
- 不执行 cache invalidate；
- owner 和 importer 都通过普通 CPU load/store 访问 NC 映射。

单机测试不使用 TCP，它会独立创建和销毁自己的 export。双机测试中的 TCP 只负责传递 export 描述符和阶段同步消息，测试数据只通过 OBMM mmap 地址传递：

```text
exporter NC store → importer NC load → 逐字节校验
importer NC store → exporter NC load → 逐字节校验
```

## 2. 运行前检查

两台机器都应确认 OBMM 模块已经加载，并且控制设备存在：

```bash
grep '^obmm ' /proc/modules
ls -l /dev/obmm
```

第一次成功执行 export/import 之前不存在 `/dev/obmm_shmdev*` 是正常现象。

先查看本机 UB Controller 入口。它们可能是符号链接：

```bash
ls -la /sys/devices | grep -E 'ub_bus_controller[0-9]+'
```

普通的 `find /sys/devices` 默认不会进入符号链接，因此可能找不到其中的 `ubc/eid`。应使用 `find -L`，或者直接沿 controller 入口读取：

```bash
find -L /sys/devices/ub_bus_controller0 \
        /sys/devices/ub_bus_controller1 \
  \( -name eid -o -name primary_cna -o -name ummu_map -o -name numa \) \
  -print 2>/dev/null
```

一次打印 controller 0 和 1 的全部关键属性：

```bash
for controller in 0 1; do
  for ubc in /sys/devices/ub_bus_controller${controller}/*/ubc; do
    [ -d "$ubc" ] || continue
    echo "===== controller ${controller}: ${ubc} ====="
    for attr in eid primary_cna ummu_map numa; do
      [ -r "$ubc/$attr" ] && printf '%-12s %s\n' "$attr" "$(cat "$ubc/$attr")"
    done
  done
done
```

需要选择实际参与这条 UB 链路的 Controller：

- 单机测试需要本机 EID；
- exporter 需要 owner 机器的 EID；
- importer 需要 importer 机器的 EID 和 `primary_cna`。

`--local-eid` 和 `--local-scna` 必须来自同一个 `ubc`。对于单机测试和 exporter，建议让 `--numa-id` 与该 `ubc` 输出的 `numa` 一致。

## 3. 在哪里设置 OBMM 和 libobmm 目录

这些路径在第一次执行 CMake 配置时，通过 `-D变量=路径` 设置。路径会保存在：

```text
tests/ub-two-node/build-obmm/CMakeCache.txt
```

当前支持以下变量：

| CMake 变量 | 应指向的内容 | 是否必须 |
|---|---|---|
| `OBMM_SOURCE_DIR` | 包含 `libobmm.c`、`libobmm.h`、`vendor_adaptor.c`、`vendor_adaptor.h` 的目录 | 没有 `libobmm.so` 时必须 |
| `OBMM_UAPI_INCLUDE_DIR` | `ub` 目录的直接父目录，使编译器能找到 `<ub/obmm.h>` | 必须 |
| `OBMM_INCLUDE_DIR` | 包含 `libobmm.h` 的目录 | 通常自动从 `OBMM_SOURCE_DIR` 推导 |
| `OBMM_LIBRARY` | 已经编译好的 `libobmm.so` 或 `libobmm.a` | 当前没有该库，不需要设置 |

不需要向 CMake 传递 `obmm.ko.xz` 的路径。`obmm.ko.xz` 是已经由内核运行的驱动模块；测试程序编译时只使用用户态源代码和 UAPI 头文件。

### 3.1 查找 `OBMM_SOURCE_DIR`

在机器的 `obmm-master` 中执行：

```bash
find /实际路径/obmm-master -type f -name libobmm.c -print
find /实际路径/obmm-master -type f -name vendor_adaptor.c -print
```

设置：

```text
OBMM_SOURCE_DIR=../../../obmm-master/libobmm
```

确认该目录完整：

```bash
ls -l /opt/obmm-master/src/libobmm/libobmm.c \
      /opt/obmm-master/src/libobmm/libobmm.h \
      /opt/obmm-master/src/libobmm/vendor_adaptor.c \
      /opt/obmm-master/src/libobmm/vendor_adaptor.h
```

### 3.2 查找 `OBMM_UAPI_INCLUDE_DIR`

执行：

```bash
find /实际路径/obmm-master -type f -path '*/ub/obmm.h' -print
```
设置：

```text
OBMM_UAPI_INCLUDE_DIR=../../../obmm-master/include/uapi
```

注意，应传入 `ub` 目录的父目录，不能传入：

```text
../../../obmm-master/include/uapi/ub
```

可以用下面的命令验证：

```bash
test -f ../../../obmm-master/include/uapi/ub/obmm.h && echo "UAPI 路径正确"
```

## 4. 编译三个独立测试程序

下面假设：

```text
libobmm.c 位于 ../../../obmm-master/libobmm/libobmm.c
UAPI 位于      ../../../obmm-master/include/uapi/ub/obmm.h
```

配置 CMake：

```bash
cmake -S tests/ub-two-node -B tests/ub-two-node/build-obmm \
  -DOBMM_SOURCE_DIR=../../../obmm-master/libobmm \
  -DOBMM_UAPI_INCLUDE_DIR=../../../obmm-master/include/uapi
```

这里会直接编译 `libobmm.c` 和 `vendor_adaptor.c`。

编译三个目标：

```bash
cmake --build tests/ub-two-node/build-obmm \
  --target obmm_local_nc_test obmm_nc_exporter obmm_nc_importer \
  -j
```

确认结果：

```bash
ls -l tests/ub-two-node/build-obmm/obmm_local_nc_test \
      tests/ub-two-node/build-obmm/obmm_nc_exporter \
      tests/ub-two-node/build-obmm/obmm_nc_importer
```

查看 CMake 当前实际保存的路径：

```bash
grep '^OBMM_' tests/ub-two-node/build-obmm/CMakeCache.txt
```

如果第一次填写错了，可以重新执行 `cmake -S ... -B ... -DOBMM_SOURCE_DIR=... -DOBMM_UAPI_INCLUDE_DIR=...` 覆盖缓存中的值，也可以使用一个新的构建目录，例如 `build-obmm-2`。

## 5. 分别执行两台机器的单机测试

先在机器 0 上独立运行，此时机器 1 不需要运行任何测试程序：

```bash
tests/ub-two-node/build-obmm/obmm_local_nc_test \
  --local-eid 0x10 \
  --numa-id 0 \
  --region-mb 2 \
  --test-bytes 4096 \
  --iterations 100000
```

将 `0x10` 替换为机器 0 的实际 EID。

成功输出应包含：

```text
PASS local NC load/store correctness (4096 bytes)
local_nc_load_avg_ns=...
local_nc_fenced_store_avg_ns=...
PASS local OBMM NC test and cleanup
```

其中：

- `local_nc_load_avg_ns`：重复执行 volatile 8 字节 load 的平均耗时；
- `local_nc_fenced_store_avg_ns`：每次 8 字节 store 后执行完整 CPU/compiler fence 的平均耗时。

第二项表示有序 store 的成本，不表示持久化介质已经完成写入。

机器 0 测试退出并完成 unexport 后，再在机器 1 上独立运行相同程序，换成机器 1 的 EID 和有效 NUMA ID。

程序会 mmap 完整的 `--region-mb` 区域，但正确性测试只访问前 `--test-bytes` 字节。如果机器的 OBMM 基础分配粒度大于 2 MiB，需要相应增大 `--region-mb`，例如 32 或 512。

## 6. 执行双机 NC export/import 测试

确认两台机器的单机测试都已退出。正常情况下，测试创建的 shmdev 应已消失：

```bash
ls -l /dev/obmm_shmdev* 2>/dev/null
```

### 6.1 机器 0：启动 exporter

```bash
tests/ub-two-node/build-obmm/obmm_nc_exporter \
  --bind-ip 192.0.2.10 \
  --port 18515 \
  --local-eid 0x10 \
  --numa-id 0 \
  --region-mb 2 \
  --test-bytes 4096
```

替换 IP、EID、NUMA ID 和内存大小。exporter 会先执行 export 和 mmap，然后输出：

```text
waiting for importer on 192.0.2.10:18515
```

此时机器 0 应该可以看到新创建的数据设备：

```bash
ls -l /dev/obmm_shmdev*
```

### 6.2 机器 1：启动 importer

```bash
tests/ub-two-node/build-obmm/obmm_nc_importer \
  --owner-ip 192.0.2.10 \
  --port 18515 \
  --local-eid 0x20 \
  --local-scna 0x1 \
  --test-bytes 4096
```

其中：

- `--owner-ip`：机器 0 的 IP；
- `--local-eid`：机器 1 的本地 UB Controller EID；
- `--local-scna`：机器 1 对应 UB Controller 的 `primary_cna`。

成功时 exporter 输出：

```text
PASS exporter NC store -> importer NC load (4096 bytes)
PASS importer NC store -> exporter NC load (4096 bytes)
PASS two-node OBMM NC exporter and cleanup
```

成功时 importer 输出：

```text
PASS importer observed exporter pattern
PASS importer NC store observed by exporter
PASS two-node OBMM NC importer and cleanup
```

## 7. 清理顺序

双机测试严格使用以下顺序：

```text
importer: munmap → close → obmm_unimport
                         ↓
                   通知 exporter
                         ↓
exporter: munmap → close → obmm_unexport
```

两个程序正常退出后，它们新创建的 `/dev/obmm_shmdev*` 应当消失。

如果测试失败，请保留两端完整输出，并额外收集：

```bash
dmesg | tail -n 100
ls -l /dev/obmm /dev/obmm_shmdev* 2>/dev/null
grep '^OBMM_' tests/ub-two-node/build-obmm/CMakeCache.txt
```
