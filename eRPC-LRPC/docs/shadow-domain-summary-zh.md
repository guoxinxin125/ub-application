# eRPC-LRPC：B-owned Shadow 执行域设计与阶段总结

更新日期：2026-09-18。

本文整理当前代码、已回传的 spr0 实验结果和后续开发约束。当前主线是 **Linux remote-domain 应用后端，ABI 3**。文中的“已实跑”依据用户提供的服务器日志，不表示本文编写时重新执行了服务器实验；“静态测试通过”不等于 Linux 编译、KVM 运行或真实 UB 硬件验证。

## 1. 项目目标与当前结论

目标是在 B 提供服务执行域和持久化数据的前提下，让 A 的 CPU 直接进入该执行域执行 RPC，避免每次把请求交给 B 的 CPU 处理。核心不是请求队列上的双端收发，而是 **A 本地调用者与 B 发布的地址空间/执行上下文之间的受控切换**。

当前方案将频繁使用、与本次执行绑定的代码副本、运行时 context、参数区和执行栈放在 A 本地 WB 内存；shadow 用户页表和服务 heap 保留在 B 的共享内存中。

现阶段可以确认：

- B 初始化、发布；A 在 CPL3 执行服务；正常调用数据路径不依赖 B CPU 执行业务处理。
- Direct、upstream eRPC API 路径、Nested 及 Query 小型程序已经有 Linux/KVM 实跑结果。
- 原始 Direct 只读一次远端 heap；增大 Query 读取数量后，远端 UC 访问成本明显增加。
- 当前结果仍存在整轮执行速度变化，不能将所有 WB/NC 差异归因于内存类型。
- 最新精确 vCPU 绑定、时间预热和时钟/宿主采集已经实现，本地 81 项单元/静态测试通过，尚未收到修改后的服务器运行结果。
- 完整的宿主 EPT-UC 实验尚未实跑，当前已回传应用数据主要属于 **PAT-only、根页表仍为 WB** 的配置。

## 2. “Shadow 属于 B”具体是什么意思

### 2.1 逻辑归属、物理存储、执行 CPU 分开看

| 维度 | 当前实现 |
|---|---|
| 服务定义及初始状态由谁发布 | B 的 `remote-app-publisher` |
| shadow 用户页表由谁构造 | B，根据 A 提供的地址契约在 BAR2 中构造 |
| 服务 heap 由谁提供 | B，在共享 BAR2 中初始化 |
| 谁执行服务指令 | A CPU |
| 谁管理调用期间的切换和异常恢复 | A 的实验性 Linux 内核后端 |
| 是否存在 B 上的普通 Linux shadow PID | 不存在；`shadow_pid=0` 明确表示没有这种任务 |
| 是否迁移 B 的 `task_struct`、调度实体和整个进程 | 否 |

因此，当前准确名称是 **B-owned shadow domain（B 发布的受控执行域）**。为了延续设计讨论可以称为 shadow，但不能在汇报中将它表述成“已经支持任意 B Linux 线程跨机迁移”。

B 不需要先运行 shadow。它只需准备服务镜像、初始状态、页表和数据。在 upstream 测试中，B 还运行 eRPC 管理面服务器用于建连；这不代表 B 在处理每次业务请求，出现 `ERROR_REMOTE_CPU_HANDLER_RAN` 应判为失败。

### 2.2 与早期版本的区别

| 路线 | 执行方式 | 结果适用范围 |
|---|---|---|
| 早期 Linux scheduler-shadow | A 上有独立 Linux shadow 任务，由调度器在 caller/shadow 间切换 | 原有 LRPC/eRPC 原型，不是当前 BAR2 用户页表后端 |
| Standalone RC microbenchmark | 自包含 KVM guest，直接测量 ring-3 context 激活/返回 | 机制微测试，不是 Linux/eRPC 端到端延迟 |
| 当前 Linux remote-domain | A 的 Linux 内核在受控入口装载 B 发布的用户页表，执行固定服务，再恢复 caller | 本文主线；保留应用 API、ioctl 和必要的 Linux 进入/退出路径 |

三条路线的延迟和 breakdown 边界不同，不能混合统计，也不能把 standalone 的优化倍数直接当作当前 Linux 应用的优化倍数。

## 3. 内存和上下文布局

### 3.1 当前资源放在哪里

| 对象 | 发布/持久化位置 | 调用期间实际使用位置 | 说明 |
|---|---|---|---|
| 服务代码镜像 | B BAR2 | A 本地代码页 | 连接时复制并逐字节验证；shadow 用户映射为 RX |
| 初始寄存器上下文 | B BAR2 | 初始化 A 本地 context | 发布结构为 160 字节，不是完整 Linux 线程 |
| 运行时 context | A 内核内存 | A 本地 WB | Linux 实现使用 `struct pt_regs` 等本地状态 |
| A-stack | A 分配 | A 本地 WB | 参数/结果页，不是 CPU 的普通调用栈 |
| E-stack | A 分配 | A 本地 WB | 服务执行时由 RSP 使用；各 domain 独立 |
| shadow 用户页表 | B BAR2 | 硬件按需访问，翻译可以缓存 | 三个独立 root；支持当前 guest 的 4/5 级页表配置 |
| 服务 heap/记录 | B BAR2 | 通过 shadow 地址空间访问 B | 当前 Linux 测试的服务映射为只读 |
| caller 页表、内核栈、Linux 管理状态 | A | A | 不搬到 B |
| shadow root 的内核半区 | root 页位于 B，但条目引用 A 的内核层级 | A 内核环境 | 不表示 A 的整套内核页表也放在 B |

BAR2 是当前 QEMU `ivshmem-plain` 暴露的共享内存窗口，不是所有 B 内存的统称。宿主共享文件在 node0 首次触页，A VM 放在 node1，用跨 NUMA 访问模拟远端内存访问；它不是实际双物理机 UB 链路。

当前 BAR2 设备为 4 MiB，remote-app ABI 使用其中前 1 MiB。主要布局由 [UAPI](../include/uapi/linux/ub_lrpc_remote_app.h) 定义：

| BAR2 偏移 | 用途 |
|---|---|
| `0x00000` | 连接/发布控制区 |
| `0x01000` | A 提供的内核根页表半区条目 |
| `0x10000 + domain*0x5000 + level*0x1000` | 各 domain 的用户页表层级 |
| `0x30000` | heap：常量 100 和 Query 记录 |
| `0x40000 + domain*0x1000` | B 发布的代码镜像 |
| `0x50000 + domain*0x1000` | B 发布的初始上下文 |

shadow 固定虚拟地址约定：代码 `0x100000000`，E-stack `+0x2000`，heap `+0x4000`，A-stack `+0x6000`。未映射的间隔不是可自动增长的内存。

这里不要求 A 和 B 的普通进程拥有相同地址空间。B 构建的是 **供 A 执行的 shadow 地址空间**，其中既有映射到 A 本地物理页的条目，也有映射到 A 所见 BAR2 GPA 的条目；B 的 publisher 自己使用另一套普通 Linux 地址空间。

### 3.2 160 字节 context 的含义与边界

发布的 `ra_initial_context` 包括 15 个通用寄存器以及 RIP、CS、RFLAGS、RSP、SS，共 20 个 8 字节字段。

需要区分：

- 160 字节是当前**发布格式**，不是“页表、heap、栈都包含在这 160 字节中”。这些对象单独存放。
- Linux 运行时保存的是本地 `struct pt_regs` 和相关内核状态，不能把所有运行时状态固定描述成恰好 160 字节。
- 它没有提供通用的 SIMD/FPU/XSAVE、TLS、文件描述符或 Linux 调度状态迁移。
- 每个新 RPC 从固定服务入口开始；不是恢复任意 B 进程上次停下的位置。
- Nested 的 middle 被挂起后，其本地寄存器现场和 E-stack 确实会在 leaf 返回后恢复。
- **当前 Linux 后端没有关闭时向 B 回写最新 context 的协议**；关闭后丢弃会话本地状态。早期 standalone 中“结束后 checkpoint 到 B”不能套用于此后端。

## 4. 一次连接及一次 RPC 的流程

### 4.1 建连：不计入稳态 RPC 延迟

1. A 显式选择 `LRPC_EXECUTION_BACKEND=remote-domain`，绑定到 guest CPU1，打开 `/dev/ub_lrpc_remote_app`。
2. A 检查运行条件、设备、缓存配置，分配三个 domain 的本地代码页、A-stack、E-stack 和运行时状态。
3. A 通过 BAR2 控制区发布本地页的 A-side GPA、A 所见 BAR2 GPA、页表级数、内核半区条目和缓存模式。
4. B 根据该契约构造 Direct/Middle/Leaf 三套用户页表，写入初始 context、代码镜像、heap 和查询记录，再发布完成序号。
5. A 验证初始 context、所有页表条目、固定代码字节及 Query fixture，复制本地代码。
6. 执行根页表别名探针和实际 shadow ring-3 heap 探针；不符合预期或结果含糊时停止，不能自动降级。
7. 应用预热后进入正式测量。

这包含初始化控制区的通信，但 **每次 RPC 不通过 B 上的请求队列/工作线程完成**。

### 4.2 Direct / Query 调用：计入端到端延迟

```text
A caller 用户态：准备请求
  -> lrpc_invoke / RA_CALL ioctl
  -> A 内核校验请求，复制到本地 A-stack
  -> 保存 caller 寄存器、CR3 和 Linux mm 状态
  -> syscall-exit hook 装载 B BAR2 中的 shadow root + PCID
  -> A CPU 进入 CPL3
       取指：A 本地代码页
       参数/结果：A 本地 A-stack
       执行栈：A 本地 E-stack
       服务数据：B BAR2 heap
  -> 固定 UD2 返回点进入受控异常处理
  -> 校验返回位置/结果，恢复 caller CR3 和寄存器
  -> caller 完成 RA_CALL 返回
  -> RA_REPORT ioctl 读取内核已保存的结果和时间戳
  -> 用户态响应复制、应用 continuation
```

当前用户态 caller **不直接 mmap 该驱动 A-stack 页**；它通过 ioctl 传入/取出数据。shadow 用户映射和内核访问都使用同一块 A 本地页面。不要与旧后端中 caller/shadow 双方 mmap A-stack 的机制混淆。

正常服务返回当前使用经过白名单验证的 `UD2` 指令位置，不是任意位置触发异常就可以返回，也不是已经实现通用 `lrpc_remote_return()` 运行库。

### 4.3 Nested 调用

```text
caller --PCID10--> middle --PCID11--> leaf --PCID10--> middle --> caller
                    41                 42               43
```

middle 和 leaf 有独立的本地 context/E-stack。middle 的第一次受控 trap 表示调用 leaf；内核保存 middle 的继续位置。leaf 返回后恢复 middle 的寄存器、栈和页表。最终恢复 caller。

每次调用有 4 次地址空间转换，其中 3 次装载 shadow root。诊断中的 `shadow_noflush=3000` 对应 1000 次调用的这 3 次装载，不包含最终 caller root 恢复，也不是硬件 TLB 命中次数。

## 5. PCID、TLB 与 WB/UC 的当前含义

### 5.1 保留的是翻译缓存，不是把远端页表整体复制回来

当前三个 domain 使用 PCID 9/10/11。首次使用或连接身份变化时刷新；同一 CPU 上、相同连接身份和不可变 root 才允许 NOFLUSH。关闭后即使物理地址复用，也不能直接复用旧连接的翻译身份。

`tlb=retain` 表示切换时尽量保留已有翻译。预热后可以减少稳态 page walk，但不保证没有 TLB miss、容量淘汰或硬件 page walk。每一轮新 VM 都重新建立状态，不继承上一轮的 TLB。

### 5.2 当前实跑策略与待验证策略

| 资源 | PAT-only remote-wb | PAT-only remote-nc |
|---|---|---|
| A 本地代码/context/A-stack/E-stack | WB | WB |
| B heap | WB | UC |
| 指向 B 下级用户页表的内存类型请求 | WB | UC |
| 三个 shadow 根页表的访问策略 | WB | WB |

这是当前实现中的明确限制。不能将 PAT-only NC 数据表述成“包括根在内，所有远端页表访问都已经是 UC”。缓存模式的具体结构校验和运行探针见 [Linux 应用说明](linux-remote-apps.md)。

针对根页表，已编写单独的宿主 KVM/QEMU EPT 实验扩展：对 caller VM 的指定 BAR2 GPA 窗口设置 EPT UC/WB，两种模式统一 EPT 映射粒度，其他 VM 不启用。**目前只有源代码、静态检查及服务器源树预览成功的证据，没有完成新宿主环境的运行验证。**

运行器默认要求该 EPT 扩展；继续使用现有环境时，必须显式设置 `LRPC_HOST_EPT_ENABLE=0`，保留 PAT-only 标签。不能为了运行成功而把标签改成 EPT-UC。详见 [宿主 EPT 实验说明](host-ept-experiment.md)。

共享页表只统一页表存储本身，不能自动解决多 client 的 TLB 失效、运行权转移、数据同步或缓存一致性。当前以单执行者、固定映射、发布后不修改为前提。

## 6. 当前测试各自证明什么

| 测试 | 程序/路径 | 每调用显式远端 heap 读 | 不能据此证明的内容 |
|---|---|---:|---|
| Ring-3 heap probe | 在实际 Direct shadow root 下比较热/冷读 | 单独探针，不是业务读数 | 不能测硬件 page-walk 次数 |
| Root alias probe | 通过 A 内核别名读取实际根页表页 | 单独探针 | 不是硬件页表遍历事件采样 |
| Direct | eRPC-compatible API，`20+22+heap[0]` 返回 142 | 1 次 8B | 不是大型 eRPC 业务 |
| upstream | upstream eRPC enqueue、transport、completion/continuation，复用 Direct 服务 | 1 次 8B | 不是 B CPU 处理网络请求的传统 RPC |
| Nested | root→middle→leaf→middle→root，返回 43 | 0 次 | 没有测试远端 heap 密集访问 |
| Query-1/8/32 | 沿 B 发布的记录链查找最后一条匹配记录 | 3/17/65 次 8B | 不是大 heap、锁竞争或 TLB 容量压力测试 |

Query 每访问一个节点读取 key 和 next，命中后读取 value，因此为 `2N+1` 次。32 条记录按 64B stride 排列，全部处于一个远端 4 KiB heap 页；服务不进行远端写入。增加 N 的目的，是在地址空间结构基本不变时增加远端数据依赖。

原始 Direct 的兼容层使用栈上临时请求对象；Query/Nested baseline 的 `lrpc_invoke_bytes` 包含分配、复制和释放。因此不同应用之间的总延迟差值不等于新增远端读取成本。

此前 BAR dependent-load 和 standalone read microbenchmark 是独立的内存/机制测试。它们可以辅助理解单次读取成本，但不能作为 Linux 服务的精确逐项扣除常数。

## 7. 延迟与 breakdown 的解释规范

### 7.1 公共四段时间戳

| 输出字段 | 当前 Linux 后端的边界 |
|---|---|
| `call_to_shadow_ns` | CALL 请求校验后的时间戳，到装载 shadow CR3 前的 dispatch 时间戳 |
| `shadow_user_service_ns` | dispatch 到服务返回 trap 中的 return 时间戳；包含 CR3 切换、用户态进入、服务及部分异常处理 |
| `return_to_caller_ns` | return 时间戳到 caller root/寄存器恢复后的 resume 时间戳 |
| `outside_kernel_timestamps_ns` | 用户态调用总区间减去上述内核时间跨度；包含未覆盖的进入/退出、REPORT ioctl 和库处理 |
| `total_ns` | 对应应用定义的端到端测量区间，不包含连接初始化 |

`shadow_user_service_ns` **不是纯用户指令或纯远端访存时间**；outside 字段也不是纯用户态时间。

upstream 另外保留 `erpc_outside_lrpc_ns` 和 `lrpc_invoke_ns`，把 API/transport/continuation 外围与 LRPC 调用区间区分开。最终应同时报告端到端统计和明确边界的诊断结果。

### 7.2 独立诊断 pass 不与 baseline 混算

- Nested DETAIL/STAGE 使用独立的 `RA_DIAG_CALL`、REPORT 和额外时间戳；它不包含 baseline 相同的库分配路径。诊断总时间比 baseline 小并不自动证明 CPU 在升频。
- Query DETAIL 在单独一轮中镜像分配、准备、invoke、结果处理和释放；不是给 baseline 的每个样本补打时间戳。
- Query 中 `total = alloc_prepare + invoke + result_free`；`invoke = outside + call_to_shadow + service + return_to_caller`。只在同一个样本集合中检查这些关系。
- 汇总表各列独立取“每轮统计值的中位数”，这些中位数不保证相加恰好等于总时间中位数。
- Query 的 p50/p99 来自单轮 1000 次调用分布；五轮汇总再对各轮统计取中位数，不是把五轮所有调用合并后算分位数。
- 软件 NOFLUSH 计数、显式 load/store 指令计数、硬件 TLB miss/page walk、远端总线事务是不同概念。

## 8. 当前进展与实验判断

### 8.1 进展清单

| 项目 | 当前状态 |
|---|---|
| 本地资源准备、私有 root 和受控 ring-3 入口 | 已有用户回传的通过日志 |
| B 初始化发布、A 使用 BAR2 用户页表执行 | Linux 应用已有实跑日志 |
| 代码/context/A-stack/E-stack 本地化 | 当前 Linux 后端已实现并用于实跑 |
| PCID/retain、Nested 独立 context/栈及 middle 恢复 | 已实现；Nested 回传诊断每轮 3000 NOFLUSH、0 显式 shadow flush |
| Direct / upstream / Nested / Query 功能及 PAT-only 对照 | 已有多轮数据，但性能稳定性尚未解决 |
| 实际 ring-3 heap 及 root alias 检查 | 已实现并用于当前 ABI 3 日志；含糊分类必须停止 |
| Nested STAGE、Query DETAIL | 已实现并有回传分段结果 |
| 精确宿主 vCPU 绑定、时间预热、时钟/宿主证据 | 已实现，本地测试通过；待服务器实跑 |
| 宿主 EPT 对指定远端页施加 UC | 源码已实现，源树预览成功；未完成宿主构建/加载/运行验证 |
| 任意服务、通用 Linux 线程、多 client、动态页表 | 尚未实现，不在现阶段通过结论中 |

### 8.2 已回传数据中的有效线索

以下为此前 PAT-only 实验，**不是最新绑核/时间预热改动后的结果**。详细数据来自对话中回传的 `comparison.txt` 和逐轮 CSV；汇报时应同时归档对应服务器目录、配置和源代码版本。

`build/query-breakdown-pat-five-2` 的分段统计：

| Query DETAIL 指标 | WB 五轮中位数 | NC 五轮中位数 | NC−WB |
|---|---:|---:|---:|
| Query-1 总时间 | 6.126 µs | 6.862 µs | +0.736 µs |
| Query-1 service 区间 | 2.973 µs | 3.668 µs | +0.695 µs |
| Query-32 总时间 | 6.270 µs | 20.641 µs | +14.371 µs |
| Query-32 service 区间 | 3.167 µs | 17.472 µs | +14.305 µs |

这些结果支持“增大受控远端读数量，会显著放大 UC 开销”。但它们不证明每次读的硬件延迟完全相同，也没有单独测出页表遍历成本。

Query-8 和 Direct/Nested 曾出现 WB 总时间高于 NC。逐轮数据表明这不能直接解释成 NC 更快：例如 Nested WB 第 2→5 轮，本地 caller 准备 354→573 ns，切换 middle 137→218 ns，middle enter/trap 2556→4045 ns，恢复 caller 300→475 ns，多个阶段一起放大约 1.6 倍。NC 也有整轮偏慢的情况。

当前判断应写成：**观察到跨轮整条路径的速度变化；缓存模式效应与运行状态变化尚未完全分离。** CPU 频率、宿主调度、时钟路径等是待验证因素，不能提前宣布其中一个就是根因，也不能删除慢轮来获得预期结论。

### 8.3 最新运行控制

- QEMU 先暂停启动，通过 QMP 获取各 vCPU 的宿主线程 ID，设置并验证亲和性后才运行 guest。
- 默认 publisher vCPU0/1 固定到 CPU40/41，其余线程使用 42–43；caller vCPU0/1 固定到 88/89，其余线程使用 90–91。RPC 线程选择 guest CPU1，即默认宿主 CPU89。
- B/node0、A/node1、共享文件首次触页 node0。亲和性不是独占 CPU，不能排除其他任务/中断。
- Direct/upstream/Nested/Query 在原有 100 次预热和 1000 次测量之前增加默认 1000 ms 同路径预热；`LRPC_REMOTE_WARMUP_MS=0` 可作为无额外预热对照。upstream 的时间预热经过完整 eRPC API、fake transport、LRPC invoke 和 continuation 路径。
- 时钟审计位于测量循环边界，不给每次调用额外增加内核打点；TSC ticks 不能直接当作 CPU 实际执行频率的周期数。
- 宿主证据以 250 ms 采样；频率文件为辅助读数，可能不存在，短测量窗口可能落在两次采样之间。不能把这些记录描述成逐 RPC 的精确频率/调度追踪。

## 9. Shadow 服务编程规范

以下是**当前实现的硬性约束**，不是对未来完整系统能力的定义。

### 9.1 地址与内存

1. 不得持有指向 B 普通进程 DRAM、栈、TLS、动态链接器内部状态的地址。这些地址不属于 A 正在执行的 shadow 映射。
2. 持久化服务数据放在协议声明的 BAR2 区域；结构中的引用使用约定的 shadow VA 或有边界校验的 offset/index。当前 Query 的 next 是记录索引，不是 B 进程指针。
3. 不得把 A 本地临时指针写成可由未来 client 复用的持久化 B 指针。A-stack/E-stack 的生命周期和归属必须明确。
4. 代码、A-stack、E-stack 保持 A 本地 WB；服务代码映射 RX，数据/栈 NX，不得引入 W+X 用户映射或同一物理页的冲突缓存别名。
5. 当前 Linux heap 用户映射为只读。加入远端写操作必须同步设计权限、可见性、排序和结果校验，不能只向现有 stub 增加 store。
6. 页表在连接期间不可由 B 修改。加入动态映射前必须设计 root 生命周期、PCID 身份和 TLB 失效协议；共享页表不等于自动失效。

### 9.2 代码、栈与寄存器

1. 当前接受经过审计的固定代码字节，不接受任意上传的函数指针或 ELF。当前 stub 含固定 shadow VA 常量，不应称为“可在任意地址运行的通用 PIC 装载器”。
2. 如果扩展为编译生成的 PIC 服务，必须审查重定位、GOT/PLT、全局变量、库函数和 TLS 依赖；不能把“代码复制成功”等同于“运行环境完整”。
3. 当前只支持受限整数指令和有界工作量。不要使用 SIMD/FPU/AMX、FS/GS/TLS 操作，尚无完整扩展状态切换契约。
4. 不得调用普通 Linux syscall、glibc malloc/free、文件描述符操作、futex/pthread、睡眠/阻塞接口或依赖 signal 的逻辑。
5. E-stack 固定预分配，当前每 domain 一页；不得依赖缺页自动扩栈、大递归或不受控的大栈对象。
6. shadow 执行期间 IF=0，必须快速、有限完成。Query 当前最多 32 次访问；不能通过增加无界循环来模拟更大应用。
7. 返回/嵌套调用只能使用协议指定的受控 trap 位置。修改指令布局必须同步更新代码白名单、trap 偏移和反汇编测试。

这些约束作用于 shadow 中执行的代码。A 的普通用户态封装在进入 domain 之前/之后可以进行现有的分配、复制等工作，不能把两者混为一谈。

### 9.3 正确性与生命周期

- 单连接、单执行者；不得把当前 PCID 固定编号方案直接扩大为任意并发 client。
- A 必须校验 B 的发布内容后再启用 root；当前依赖可信 B 发布后不再恶意修改，校验不是完整敌对隔离机制。
- 同时维护 caller 的 CR3、寄存器、Linux `loaded_mm`、pending/active 和引用生命周期；不可只修改 CR3 而遗漏恢复路径。
- 正常返回与异常恢复是不同路径，都要检查。代码已有受控异常处理，但不代表任意故障、NMI 压力、调试器和热插拔场景已验证。
- IRQ-off 路径不能执行可睡眠释放；资源拆除按现有 deferred work 生命周期处理。
- 失败时输出明确失败标志，不允许回退旧 scheduler backend、把 UC 改成 WB 或放宽探针阈值来继续生成“有效结果”。

## 10. 开发、测试与结果归档规范

### 10.1 修改入口

| 内容 | 主要文件 |
|---|---|
| 发布格式、VA/偏移、固定字节码 | [ub_lrpc_remote_app.h](../include/uapi/linux/ub_lrpc_remote_app.h) |
| B 构造 root、初始 context、heap | [remote_app_publisher.c](../demo/remote_app_publisher.c) |
| A 激活、异常返回、Nested/PCID、页表校验 | [remote_app_arch.inc](../kernel/remote-domain/remote_app_arch.inc) |
| 用户 API 到 ioctl 的适配 | [remote_app_backend.h](../lib/remote_app_backend.h)、[lrpc.c](../lib/lrpc.c) |
| Direct / Nested / Query 测量 | [erpc_client.cc](../demo/erpc_client.cc)、[remote_app_nested.c](../demo/remote_app_nested.c)、[remote_app_query.c](../demo/remote_app_query.c) |
| Query 指令源 | [remote_app_query.S](../kernel/remote-domain/remote_app_query.S) |
| upstream 时间统计与预热补丁 | [erpc-lrpc-timing.patch](../patches/erpc-lrpc-timing.patch)、[erpc-lrpc-precondition.patch](../patches/erpc-lrpc-precondition.patch) |
| 时间预热、时钟审计 | [remote_app_bench.h](../include/lrpc/remote_app_bench.h) |
| Guest 源码集成/构建 | [install-linux-remote-app.py](../scripts/install-linux-remote-app.py)、[build-linux-remote-app.sh](../scripts/build-linux-remote-app.sh) |
| 启动、精确绑核、统计 | [run-linux-remote-apps.sh](../scripts/run-linux-remote-apps.sh)、[remote-app-vm.py](../scripts/remote-app-vm.py)、[summarize-linux-remote-apps.py](../scripts/summarize-linux-remote-apps.py) |
| Guest init | [remote-app-init](../vm/remote-app-init) |

扩展一个服务时，同步修改发布代码、字节码/汇编、内核参数及结果验证、trap 偏移、用户调用程序、统计解析和测试。不能只在 B 发布新字节码而保留 A 的旧白名单。

修改 UAPI 或共享布局时，显式处理 ABI 兼容性并重建 A/B；禁止混用旧 kernel、新用户程序、旧 initramfs。Linux 源码注入优先修改仓库中的源资产并通过 installer 更新，保留哈希备份，不手改已生成副本绕过检查。

### 10.2 验证层级

1. 本地：UAPI/字节码一致性、installer 幂等及拒绝异常修改、统计解析、QMP/绑核逻辑、shell 语法等测试。
2. Linux：编译 guest kernel、所有用户程序及 upstream eRPC，并重新打包 initramfs。
3. KVM：检查发布、探针、结果、返回/恢复、完成标志和进程退出码。
4. 性能：同版本、同放置、同时间策略下比较 WB/NC，保留所有逐轮数据和失败日志。
5. 真实 UB：另行验证硬件映射、排序/一致性、指令集和访问延迟；不能由 QEMU/KVM PASS 推导出来。

### 10.3 当前建议运行命令

在服务器 `eRPC-LRPC` 目录，使用已有支持 `honor-guest-pat` 的 `$QEMU`。本次运行控制改动不需要更换宿主内核，也不需要重新安装 guest 源码 hook；先重建用户程序和镜像：

```bash
bash scripts/build-linux-remote-app.sh

set -o pipefail
QEMU="$QEMU" \
LRPC_HOST_EPT_ENABLE=0 \
LRPC_RUNS=5 \
LRPC_REMOTE_TESTS='direct nested' \
LRPC_REMOTE_WARMUP_MS=1000 \
LRPC_BASE_PORT=23900 \
LRPC_REMOTE_OUTPUT=build/direct-nested-pinned-warm-five \
bash scripts/run-linux-remote-apps.sh 2>&1 |
    tee build/direct-nested-pinned-warm-five.log
run_rc=${PIPESTATUS[0]}
echo "RUN_EXIT_CODE=$run_rc"
```

输出目录必须不存在。若运行完整六项测试，将 tests 改为 `direct upstream nested query-1 query-8 query-32`，并使用新的输出目录和空闲端口。宿主 EPT 路线必须另按专门文档准备，不能省略其构建和验证。

### 10.4 每次归档

- `cases.txt`、`ept-policy.txt`、`run-policy.txt` 和完整启动命令。
- `results.csv`、`summary.csv`、`comparison.txt/csv`、`cache-probes.csv`、`nested-switch-counts.csv`、`clock-audits.csv`。
- 每轮 caller/publisher 日志，以及 `caller-host.jsonl`、`publisher-host.jsonl`。
- 源码提交/差异或同步文件版本，QEMU 路径/版本，宿主和 guest 内核版本，构建产物哈希；这些信息应一并保存，不能假定现有脚本已自动完整归档。
- WB/NC、PAT-only/EPT、应用名字、预热参数、CPU/NUMA、PCID/TLB 策略必须随结果一起报告。

报告时保留单位、样本数、统计口径及 range；不要把不同版本、不同 pass 的数相减构造“精确硬件开销”。修改文件后在交付说明末尾列出同步清单，方便服务器保持同版。

## 11. 后续工作顺序

1. 实跑最新绑核/时间预热版本，先判断 Direct/Nested 的整轮速度变化是否收敛；仍异常时结合时钟审计和宿主证据定位，不先调整服务以迎合结果。
2. 在同一稳定配置下重测 Query-1/8/32 和 upstream，确认远端读取数量与延迟增长关系。
3. 在维护窗口构建并验证独立宿主 EPT 实验，保留现有可启动内核；对根别名、heap 和 EPT 接受标志严格验收。
4. 必要时继续细分 Linux 用户态进入/异常返回区间；不能仅用 composite 区间推断硬件 page walk 成本。
5. 获得可复现基线后，再逐项扩展远端写、更多 heap 页、翻译压力及更真实的服务逻辑。
6. 多 client、运行权/上下文交接、动态映射和失效协议、扩展寄存器、通用运行库属于独立设计工作，不预先计为已经完成。

## 12. 可直接用于汇报的简述

> 当前实现了一个 B 发布、A 执行的受控 shadow domain：B 提供用户页表、初始上下文和服务数据，A 在本地保存代码副本、运行时 context、参数区与执行栈，通过 PCID 保留翻译，在 caller 与远端 domain 间切换。Direct、upstream eRPC、Nested 和有界 Query 已有 Linux/KVM 功能与性能日志。Query 随远端读取增多表现出明显 UC 开销，但部分轮次存在整体执行速度变化，最新精确绑核和时间预热方案仍待实跑。现有应用结果属于 PAT-only 对照，根页表仍 WB；完整宿主 EPT-UC 和真实 UB 硬件行为尚未验证。该原型不是任意 Linux 线程迁移或通用多 client RPC 运行时。
