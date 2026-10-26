# UB 双机共享页读写与原子更新探针

这是独立于 SocialNetwork 业务数据的正确性实验，不是 ComposePost 或数据库写入。
原来的 `--social-write` 是每连接一页；这里显式选择 `--social-shared`，
所有连接把**同一块 B 持有的 4 KiB 页**映射到服务域的 `UD_WRITE`。
页中索引 1 是普通读写单元，索引 2 是原子计数器。其他 profile 不变；
B 配置页仍只读，A 本地代码仍为 RX。此实验只在双方可信的测试环境运行，
不构成对导入进程或其他服务的访问隔离。

调用顺序：先由不同连接轮流写索引 1，每轮让所有连接读回相同值；
再让所有连接同时对索引 2 执行单条 LSE `LDADDAL` fetch-add。
A 校验返回的旧值恰好覆盖 `0..总次数-1`，无重复和缺号，最后从域内读回计数器；
B owner 独立轮询并记录两个最终值。原来的 `LDAXR/STLXR` 重试路径已经移除；
这次每个请求只执行一次 `LDADDAL`，不在失败后重发原子写入。A CPU 必须支持
Arm LSE（`/proc/cpuinfo` 的 `Features` 含 `atomics`）；异常或计数不一致不能记为通过。
**本地 mock 通过只证明流程和检查器工作；只有真机双端通过才能证明该 UB
映射上的跨连接原子更新可用。** 即使通过，也不代表跨机多对象事务或故障恢复正确。

## 第一次部署或代码更新：按顺序执行

此 profile 继续使用 ABI 2 **结构**，但本次修改了 `social_services.S` 的代码镜像。
即使旧模块已认识 profile 3，也不能只替换 A 的 caller：B 要重新编译 owner，
A 要重新编译 caller 和内核模块，随后把 A 当前加载的旧模块换成新 `.ko`。
A 内核会校验 B 发布的代码与本地镜像逐字节一致；只更新一端会使连接提交失败。
编译 `.ko` 只更新磁盘文件，
不会自动更新已加载模块。两端必须同步同一份 `eRPC-LRPC-UB/`；以下命令均在
该目录执行。沿用此前确认过的 `UBSM_INCLUDE_DIR`、`UBSM_LIBRARY`，A 还需
`OBMM_INCLUDE_DIR`、`OBMM_LIBRARY`（若依赖在默认路径，可不显式设置）。
不需要重启机器或修改启动参数。

1. **先构建，不影响 A 当前加载的模块。** A、B 分别执行；不要把 B 的 owner
   构建命令误放在 A 上。如果 B 的旧 owner 正在运行，先结束自己启动的旧 owner，
   再重编该可执行文件。

   B 机器（先进入同步后的 `eRPC-LRPC-UB/` 目录）：

   ```sh
   python3 scripts/manifest.py --check
   sh scripts/build.sh owner
   objdump -D build-owner/lrpc-ub-domain-owner |
     sed -n '/<us_nginx_forward_shared_start>:/,+75p' | grep -w ldaddal
   ```

   A 机器（也先进入该目录）：

   ```sh
   python3 scripts/manifest.py --check
   grep -m1 '^Features' /proc/cpuinfo | grep -w atomics
   sh scripts/build.sh caller
   sh scripts/build-module.sh --vhe  # 当前 VHE-EL2 机器
   uname -r
   modinfo -F vermagic kernel/ub-domain/ub_lrpc_domain_vhe.ko
   modinfo -F ud_social kernel/ub-domain/ub_lrpc_domain_vhe.ko
   objdump -D kernel/ub-domain/ub_lrpc_domain_vhe.ko |
     sed -n '/<us_nginx_forward_shared_start>:/,+75p' | grep -w ldaddal
   ```

   服务代码存放在只读数据段，等待发布/复制后执行；`objdump -d` 只看可执行段，
   **不能用它判断 B owner 有没有编进服务代码**。这里用 `-D` 反汇编所有段，
   并限定到 `us_nginx_forward_shared_start` 附近，避免误把其他位置的 `ldaddal`
   当成本服务的指令。`atomics`、两端服务代码的 `ldaddal` 检查都必须有输出；
   若缺失，先停止部署并检查
   CPU 能力、汇编器和实际构建产物，不要试跑旧的 `LDAXR/STLXR` 重试路径。
   `vermagic` 开头须与 A 的 `uname -r` 一致，`ud_social` 须包含
   `shared-lse-atomic-probe-abi2`。`--vhe` 会同时构建 EL1 和 VHE 两个 `.ko`，
   当前 VHE 机器只加载 `_vhe.ko`。如果 A 确实运行 EL1，则改为执行
   `sh scripts/build-module.sh`，将上面的两条 `modinfo` 命令及 `objdump`
   命令中的 `_vhe.ko` 改为 `.ko`，检查 `ub_lrpc_domain.ko`。

2. **在共享机器上先确认占用，再换模块。** 结束自己启动的 A 侧连接/探针，
   再结束自己启动的旧 B owner。与其他用户确认设备空闲后，在 A 执行：

   ```sh
   lsmod | grep -E '^ub_lrpc_domain(_vhe)?[[:space:]]'
   sudo fuser -v /dev/ub_lrpc_domain
   ```

   `fuser` 无输出不等于可以卸载别人的模块；先确认 `lsmod` 中实际加载的
   名称、使用者和实验时段。**只替换自己负责的模块**，不要强制卸载。
   对已加载 `ub_lrpc_domain_vhe` 的 VHE 机器，执行：

   ```sh
   sudo rmmod ub_lrpc_domain_vhe
   sudo insmod kernel/ub-domain/ub_lrpc_domain_vhe.ko experimental=1 vhe_gate=1
   lsmod | grep '^ub_lrpc_domain_vhe[[:space:]]'
   ls -l /dev/ub_lrpc_domain
   ```

   如果之前没有加载本项目模块，跳过 `rmmod`。如果 `rmmod` 报 `busy`，
   **停下来查明占用者，不要用强制参数，也不要继续 `insmod`**。
   EL1 机器只在确认为 EL1 且加载的是本项目 EL1 模块时，改用：

   ```sh
   sudo rmmod ub_lrpc_domain  # 未加载时跳过
   sudo insmod kernel/ub-domain/ub_lrpc_domain.ko experimental=1
   lsmod | grep '^ub_lrpc_domain[[:space:]]'
   ```

   EL1 与 VHE 模块使用同一设备，不能同时加载。`modinfo` 检查的是**磁盘上**
   的 `.ko`，不是已加载版本；上面成功卸载再加载后，才是新模块在运行。

3. **先启动 B owner，再在 A 运行探针。** B 的 NUMA 1 与 A 的 CPU 1、2
   都只是示例，按当前机器允许的 NUMA/CPU 修改。两个命令要在各自机器的
   独立终端执行，保持 B owner 运行，直至 A 测试和离线校验完成。

   B 机器（在 `eRPC-LRPC-UB/` 下，保持此终端运行）：

   ```sh
   ./build-owner/lrpc-ub-domain-owner lrpc_ub_domain --provider-numa 1 --social-shared \
     2>&1 | tee shared-owner-lse-1.log
   ```

   A 机器（在 `eRPC-LRPC-UB/` 下，用另一终端）：

   ```sh
   sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
     taskset -c 1,2 ./build-caller/lrpc-ub-domain-shared-probe \
     --cpus 1,2 --rounds 4 --increments 1 --observe-ms 5000 \
     2>&1 | tee shared-a-lse-1.log
   ```

   等 B 日志出现最终观察值后，在保存了两份日志的一台机器运行：

   ```sh
   python3 scripts/check-shared-probe.py \
     --a-log shared-a-lse-1.log --b-log shared-owner-lse-1.log
   ```

   若两份日志分别留在 A、B，先把其中一份复制到**自己的实验目录**，再给
   检查器传真实路径；不能在 A 上直接引用 B 的本地文件名。

每轮先关闭本项目旧连接，再重启**本项目自己的** `--social-shared` owner：
共享计数器只在 owner 初始化时清零，A 探针也会拒绝非零初始状态。

首次通过后，可把 `--increments` 增至 1000 或 10000，最多 8 个不同 CPU/连接；
`--rounds` 范围 2..64，`--observe-ms` 范围 0..30000 毫秒。
`UB_DOMAIN_SHARED_READ_PASS` 表示 A 侧跨连接读回正确；
`UB_DOMAIN_SHARED_ATOMIC_PASS` 表示 A 侧计数器及每次返回值正确；
只有离线检查器输出 `UB_DOMAIN_SHARED_VISIBILITY_PASS` 才说明 B 也观察到了
同一 generation 的最终单元与计数器值。B 轮询可跳过中间状态，因此不要求
B 观察到每次递增。A/B 日志必须来自同一轮 owner；若 B 最终观察晚到，可等几秒
只重跑检查器，不要自动重发原子操作。

若原子阶段失败，请保留 A/B 日志、`dmesg` 中的 `ub_domain` 记录、模块版本、
内核版本、CPU/NUMA 与 UBSM 映射方式。异常向量或计数不一致都应先当成
实现或平台语义待诊断，不能宣称 LSE 原子能力已验证。

### LSE 真机对照与失败诊断

旧版 `LDAXR/STLXR` 在 A 侧一次调用中 256 次均报告失败，但 A/B 计数器均为
256；不能继续执行或复用旧版重试测试。这次是**服务代码镜像修改**，必须先
按上面第 1、2 步同步双端源码、重编 B owner 与 A 模块/caller，并安全替换 A
已加载的旧模块。不能沿用旧诊断版本的“只重编 A caller”步骤。

第一轮每 worker 1 次的 B/A 启动及离线校验命令见上面第 3 步。
若未同时得到 A 的 `UB_DOMAIN_SHARED_ATOMIC_PASS`、
`UB_DOMAIN_SHARED_CAMPAIGN_PASS` 和离线 `UB_DOMAIN_SHARED_VISIBILITY_PASS`，
请停止，不要运行更大样本。第一轮通过后，A 结束并让 B 再观察几秒，
然后 Ctrl+C 结束**自己启动的** B owner。每一轮都要用新的 owner 清零计数器。
以下仍以 B provider NUMA 1、A CPU 1 和 2 为例。

第二轮**重新启动** B owner：

```sh
./build-owner/lrpc-ub-domain-owner lrpc_ub_domain \
  --provider-numa 1 --social-shared \
  2>&1 | tee shared-owner-lse-10.log
```

随后 A 运行：

```sh
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1,2 ./build-caller/lrpc-ub-domain-shared-probe \
  --cpus 1,2 --rounds 4 --increments 10 --observe-ms 5000 \
  2>&1 | tee shared-a-lse-10.log
```

同一轮的两份日志收齐后执行：

```sh
python3 scripts/check-shared-probe.py \
  --a-log shared-a-lse-10.log --b-log shared-owner-lse-10.log
```

第二轮也通过并结束这轮 B owner 后，第三轮用新 owner 测每 worker 100 次：

B：

```sh
./build-owner/lrpc-ub-domain-owner lrpc_ub_domain \
  --provider-numa 1 --social-shared \
  2>&1 | tee shared-owner-lse-100.log
```

A：

```sh
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1,2 ./build-caller/lrpc-ub-domain-shared-probe \
  --cpus 1,2 --rounds 4 --increments 100 --observe-ms 5000 \
  2>&1 | tee shared-a-lse-100.log
```

同轮日志收齐后执行：

```sh
python3 scripts/check-shared-probe.py \
  --a-log shared-a-lse-100.log --b-log shared-owner-lse-100.log
```

这三轮的理论递增总数分别为 2、20、200。不要混用不同轮次或不同
generation 的 A/B 日志。保留 A 的 `UB_SOCIAL_CALL_FAIL`、
`UB_DOMAIN_SHARED_ATOMIC_START`、`UB_DOMAIN_SHARED_WORKER_DIAG`、
`UB_DOMAIN_SHARED_ATOMIC_DIAG`，以及 B 的 `UB_DOMAIN_SHARED_READY` 和最后几条
`UB_DOMAIN_SHARED_OBSERVED index=2`。

`UB_DOMAIN_SHARED_ATOMIC_START` 记录本轮实际解析的每 worker 次数和理论总数。
发生失败时，`UB_DOMAIN_SHARED_WORKER_DIAG` 记录每个 worker 已**返回成功**的
调用数、最后一个返回的旧值、失败请求序号及返回状态。
`UB_DOMAIN_SHARED_ATOMIC_DIAG` 汇总所有成功返回的旧值是否重复或越过理论上限。
`counter_snapshot_valid=1` 表示该 worker 在异常的 `EAGAIN` 后额外做了一次只读查询；
这可能与另一 worker 的在途调用并发，**不是最终计数**。若为 `0`，
不得使用 `counter_snapshot` 的数值。B 的 `UB_DOMAIN_SHARED_OBSERVED index=2`
仍须与同一 generation 对照；它是周期采样，也可能漏掉中间状态。
诊断不会将失败轮次判为 PASS，也不会自动重试原子写入。LSE 版本若仍出现
`EAGAIN`、异常或计数错误，应保留日志并检查实际加载的模块、B owner 镜像及
shadow 页表属性，不应直接归因为 UB 原子能力缺失。
