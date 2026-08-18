# CXL eRPC Queue Design Notes

本文档说明当前 CXL eRPC 指针传递队列的两种实现：MPSC 队列和 SPSC 轮询队列。这里的队列只传递 `cxl_ptr_queue_entry_t`，即共享内存 payload 的偏移量、队列项类型和一份 eRPC packet header；实际 RPC payload 仍位于 CXL 共享内存中。

当前实现需要同时考虑普通 cache-coherent 模式、no-cc CXL 模式、one-sided read 模式和 uncacheable memory 模式。主要困难来自 no-cc CXL：不同 host/process 对同一 cacheable CXL 地址的普通 load/store 不一定彼此可见，因此队列发布、消费和多生产者槽位分配必须显式处理可见性。

## 共同数据路径

发送端在 `CXLTransport::tx_burst()` 中构造队列项：

1. 从 `routing_info` 中取得目标 RPC ID，作为目标接收队列 ID。
2. 如果 payload 已经在 CXL 共享内存中，直接传递 payload 指针；否则先分配临时 CXL buffer 并复制 payload。
3. 构造 `cxl_ptr_queue_entry_t`，其中包含：
   - `pkthdr`：接收端或中间转发节点可先读队列项完成路由判断。
   - `data_ptr`：payload 相对 CXL base 的偏移。
   - `entry_type`：普通 RPC 消息或跨 endpoint free 消息。
4. 调用 `CXLSharedAllocator::enqueue_ptr()` 入队。

接收端在 `CXLTransport::rx_burst()` 中调用 `CXLSharedAllocator::dequeue_ptr()`：

1. 从当前 RPC ID 对应的接收队列取出队列项。
2. 如果是 free 消息，由 owner 释放对应 shared buffer。
3. 如果是 RPC 消息，将队列项中的 `pkthdr` 放入本地 RX header ring，将 payload 指针保存到 `rx_payload_ring_`。
4. eRPC 上层通过 `get_rx_payload()` 读取实际 payload。

## MPSC 队列

MPSC 队列用于多个发送方共同向同一个接收方投递消息：

```text
producer 1 \
producer 2  -> receiver queue[dst]
producer N /
```

在 `CXLSharedAllocator` 中，MPSC 模式为每个 RPC endpoint 分配一个接收队列：

```text
ptr_queues_[dst]
```

所有发送方如果要发送给 `dst`，都会 enqueue 到同一个 `ptr_queues_[dst]`。接收方 `dst` 是唯一 consumer。

### MPSC 支持并发的挑战

**1. 多 producer 必须拿到唯一 slot**

多个 producer 可能同时发送给同一个 receiver。如果 tail 是普通本地变量或普通 cacheable atomic，在 no-cc CXL 下不同 producer 可能看不到彼此对 tail 的更新，从而拿到同一个 `current_tail`，覆盖同一个 slot。

因此 no-cc MPSC 不能依赖普通 `std::atomic<size_t>` tail。当前 no-cc 分支使用 `nt<size_t> tail`，并通过：

```cpp
size_t current_tail = tail.fetch_add(1);
```

为每个 producer 分配唯一 ticket。这个 ticket 决定要写哪个 ring slot。

**2. tail 只负责分配位置，不代表 slot 已经写完**

`fetch_add()` 之后，tail 已经前进，但 producer 可能还没有写完 `slot.value`。consumer 不能只看 head/tail 判断是否可读，否则会读到半写入的 slot。

当前设计使用 per-slot `ready` 作为发布标志：

```cpp
slot.ready.store(0, relaxed);
slot.value = item;
slot.ready.store(ready_phase(current_tail), release);
```

consumer 只有看到对应 phase 的 ready 后，才认为该 slot 完整可读。

**3. no-cc 下发布必须显式写回**

在 cacheable no-cc 模式下，producer 写完 slot 后需要：

```cpp
clwb(&slot, sizeof(slot));
```

consumer 读 slot 前需要：

```cpp
clflush(&slot, sizeof(slot));
```

这样才能让远端写入对本地读取可见。`USE_ONE_SIDE_READ` 和 `ENABLE_UNCACHE_MEM` 下对应 flush 路径会减少或消失。

**4. ring 复用需要区分轮次**

`idx(current_tail)` 会对 capacity 取模，所以同一个 slot 会被复用。仅用 `ready == 1` 无法区分“当前轮次的新数据”和“上一轮残留的数据”。

当前使用 `ready_phase()` 返回 1/2 两个 phase：

```cpp
((i / capacity) & 1) + 1
```

consumer 检查：

```cpp
slot.ready == ready_phase(current_head)
```

这样可以避免把上一轮的 ready 当成当前轮次的数据。

**5. 队列满检查在 no-cc 下代价高**

严格判断队列是否满需要 producer 读取 consumer 的 head：

```text
tail - head >= capacity
```

但 no-cc 下 head 由 consumer 更新，producer 读取它需要额外可见性维护，例如 flush/load 或把 head 也做成远端可见变量。这会增加 enqueue 热路径开销。

当前 no-cc MPSC 实现选择不做满队列检查，假设 benchmark 或应用层不会让 outstanding 超过队列容量。这个选择提升了热路径性能，但风险是：如果 producer 总发送量超过 consumer 处理能力并绕回 ring，可能覆盖未消费 slot。

默认 cache-coherent 分支仍保留 bounded queue 逻辑，使用 `compare_exchange_weak()` 抢占 tail，并通过 head/tail 判断满队列。

### MPSC 当前设计

no-cc cacheable 模式下，MPSC enqueue 的核心流程是：

```text
1. tail.fetch_add(1) 获取唯一 current_tail
2. 根据 current_tail 计算 slot
3. ready = 0
4. 写 slot.value
5. ready = ready_phase(current_tail)
6. clwb(slot)
```

try_dequeue 的核心流程是：

```text
1. 读取本地 head
2. 定位 slot
3. clflush(slot)
4. 检查 ready 是否等于 ready_phase(head)
5. 读取 slot.value
6. ready = 0
7. clwb(slot.ready)
8. head++
```

uncacheable 模式下，slot 读写不需要显式 `clwb/clflush`，但 uncacheable 访问本身可能有更高单次延迟。

### MPSC 的优点和问题

优点：

- 接收端只有一个队列，不需要扫描多个 per-source queue。
- 多 producer 通过 `nt tail.fetch_add()` 可以并发分配 slot。
- 对接收端而言，`dequeue_ptr()` 只需要检查自己的一个队列。

问题：

- no-cc 下 tail 必须远端可见，tail 原子操作是发送路径上的共享热点。
- 严格队列满检查代价高，当前 no-cc 分支依赖队列足够大和应用约束。
- 多 producer 同写一个接收队列，会在 tail 和接收队列 cacheline 上形成集中竞争。
- producer crash 在 tail 分配之后、ready 发布之前，会让 consumer 在对应 head 位置之后无法继续前进，需要额外 recovery 机制。

## SPSC 轮询队列

SPSC 队列将“一个接收方一个 MPSC 队列”改成“每个 sender/receiver pair 一个 SPSC 队列”：

```text
spsc_queues_[dst][src]
```

如果 `src` 发送给 `dst`，只写 `spsc_queues_[dst][src]`。该队列只有一个 producer 和一个 consumer。

### SPSC 支持并发的挑战

**1. 多 client 并发不再共享 tail**

SPSC 的最大优势是每个 producer 有自己的队列：

```text
client 1 -> queue[server][client 1]
client 2 -> queue[server][client 2]
client N -> queue[server][client N]
```

因此每个队列内部只有一个 producer，`tail_` 可以是普通本地变量，不需要跨 producer 原子 `fetch_add()`。这避免了 MPSC 的 tail 共享热点。

**2. consumer 必须发现哪些 source queue 有数据**

代价是 receiver 不再只有一个队列，而是有多个 per-source queue。server 需要轮询多个 `spsc_queues_[server][src]`，才能发现来自不同 client 的 request。

这带来空轮询问题：某个 client 的 request 被取走后，在收到 response 并发出下一个 request 之前，它的 queue 大概率为空。server 如果再次轮询到这个 queue，就会产生一次空 poll。

**3. no-cc 下每次空 poll 也可能需要 flush**

当前 `SPSCQueue::try_dequeue()` 在 cacheable no-cc 模式下会先：

```cpp
clflush(&slot, sizeof(slot));
```

然后检查 ready。即使 slot 为空，这个 flush 仍然发生。所以 SPSC 的空轮询不是纯 CPU 分支开销，而是可能包含 CXL/cacheline 可见性成本。

**4. queue matrix 占用固定空间**

当前 SPSC 在初始化时为所有 `(dst, src)` pair 建立固定地址映射：

```text
kMaxSessions * kMaxSessions * kSPSCQueuePairSpace
```

这样 producer 和 consumer 可以通过相同公式找到同一个 queue，但会占用比 MPSC 更多的队列元数据空间。

**5. active peer 信息必须正确**

如果 receiver 盲目扫描 `ERPC_CXL_WORKER_COUNT` 内所有 RPC ID，client 也会扫描其他 client 的空 response queue，开销很高。

当前设计在 session 建立时注册 active peer：

- client 创建 session 时注册 server RPC ID。
- server 处理 connect request 时注册 client RPC ID。

`dequeue_ptr()` 优先只轮询 `active_srcs_`，没有 active peer 时才 fallback 到全 worker 扫描。

这让 multi-client single-server 场景变成：

```text
server: 轮询所有已连接 client
client: 只轮询 server
```

### SPSC 当前设计

enqueue 流程：

```text
1. current_tail = tail_++
2. 定位 spsc_queues_[dst][src] 中的 slot
3. ready = 0
4. 写 slot.value
5. ready = ready_phase(current_tail)
6. no-cc cacheable 下 clwb(slot)
```

try_dequeue 流程：

```text
1. current_head = head_
2. 定位 slot
3. no-cc cacheable 下 clflush(slot)
4. 检查 ready 是否等于 ready_phase(head)
5. 成功则读取 slot.value
6. ready = 0
7. no-cc cacheable 下 clwb(slot.ready)
8. head_++
```

allocator 层轮询流程：

```text
1. 根据 active_srcs_ 得到要轮询的 source list
2. 从 next_poll_src_ 开始 round-robin
3. 对每个 src 调用 spsc_queues_[dst][src]->try_dequeue()
4. 成功后 next_poll_src_ 指向下一个 peer，并返回一个队列项
5. 如果所有 active peer 都为空，则返回 false
```

### SPSC 的优点和问题

优点：

- producer 之间不共享 tail，避免 MPSC tail 热点。
- 每个 queue 内部是单 producer 单 consumer，队列协议更简单。
- client 端注册 active peer 后，只需要轮询 server response queue。
- 在 multi-client single-server 场景中，SPSC 可减少 MPSC 多 producer 抢 tail 的开销。

问题：

- server 端需要轮询多个 active client queue。
- active queue 也可能为空，尤其是 client concurrency 较低时。
- 空 poll 在 no-cc cacheable 模式下仍可能触发 `clflush(slot)`。
- 当前 SPSC 也没有严格满队列检查，依赖容量和 outstanding 控制。
- 如果存在没有 eRPC session 的跨 endpoint free 消息，严格 active peer 轮询可能漏掉该 source，需要保留 fallback、专用 free 队列或额外注册机制。

## MPSC 与 SPSC 的取舍

MPSC 将并发压力集中在接收方的一个队列上：

```text
优点：receiver 不需要扫描多个队列。
缺点：producer 共享 tail，no-cc 下 tail 必须远端可见且可能成为热点。
```

SPSC 将并发压力拆分到多个 per-source 队列上：

```text
优点：producer 不共享 tail，每条队列协议简单。
缺点：receiver 需要轮询多个队列，空 poll 成本可能较高。
```

因此两者适合回答不同问题：

- MPSC 更适合验证“多个 producer 写同一个接收队列”的正确性和共享 tail 成本。
- SPSC 更适合验证“去掉多 producer 共享 tail 后，系统是否受 receiver polling 成本限制”。

当前测量已经显示，SPSC active peer 轮询能显著减少 client 端无效扫描；server 端正式测量阶段仍有空 poll，但不是启动/析构阶段统计显示的极端情况。后续是否引入 bitmap/doorbell，应取决于空 poll 成本是否高于额外通知变量的写入、刷新和读取成本。

## 后续可选优化

**1. Batch dequeue**

server 从某个 source queue 成功取到一个消息后，可以继续从同一个 queue 连续取若干个消息，再切换到下一个 peer。这样可能减少 round-robin 调度开销，但会影响公平性。

**2. Doorbell 或 bitmap**

producer enqueue 后写一个额外通知变量，consumer 先检查通知，再决定是否访问 queue slot。它可以降低空 poll 访问 queue slot 的成本，但会增加 producer 成功路径的通知写入和可见性成本。

在 no-cc CXL 中，packed bitmap 需要特别小心多 producer 原子更新和 cacheline 覆盖问题。更稳妥的实验方案是 per-source doorbell counter，让每个 `(dst, src)` 只有一个 producer 写。

**3. 更严格的 bounded queue**

如果需要在生产环境中防止 ring 覆盖，需要恢复或重做满队列检测。no-cc 下这通常意味着 producer 要读取远端 head，或者引入 credit/backpressure 协议。

**4. Failure recovery**

MPSC 和 SPSC 都需要考虑 producer 在占用 slot 后崩溃的情况。仅靠 ready 标志可以避免读半写数据，但不能自动跳过永久未 ready 的 head slot。需要心跳、epoch、owner recovery 或上层重建队列。
