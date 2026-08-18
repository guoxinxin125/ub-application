# eRPC UB shared-memory transport

该实现直接使用 UBSM，不依赖 `shm-lib` 或 `ubs-atomic-master`。原子操作位于
`ub_atomic.h`，消息分配、引用计数和回收由 `UBSharedAllocator` 完成。

## 数据路径

每台机器只有一个 machine region。每个 endpoint 注册后获得一个 inbox 和一个
私有分配 arena：

```text
machine region
  header + endpoint registry
  endpoint A: inbox + arena
  endpoint B: inbox + arena
```

inbox 为每个 source RPC 保留一条 SPSC 队列。队列项是 64 字节
`UBMessageDescriptor`，包含本地 eRPC header，以及发送方共享 payload 的
`machine_id/block_offset/payload_offset/length/generation`。发送端发布 descriptor，
不复制 payload；接收端把 header 放入本地 RX ring，并直接返回发送方共享内存中的
payload 指针。

receiver 不会固定扫描全部 64 条 SPSC。session 建立时 transport 为 remote `rpc_id`
增加连接引用，并把第一次出现的 source 加入紧凑 active-source 数组；session 销毁时
减少引用，最后一条连接退出后移除该 source。`rx_burst()` 仅对当前 active source 做
round-robin 轮询。

共享 SPSC 暂时满时，发送端不会终止进程。descriptor 及其 in-flight 引用会进入
对应 remote endpoint 的本地 pending 队列，并在后续 `tx_burst()`、`tx_flush()` 和
事件循环的 `rx_burst()` 中按原顺序重试。每个 remote endpoint 最多保留 64 个
pending descriptor；达到上限后的新发送尝试不会取得共享引用，并按一次普通丢包
处理，由 eRPC 超时机制重试，防止远端长期不消费时本地重试状态无限增长。

共享 block 包含 cache-line 对齐的 metadata：状态、generation、size class、引用计数、
arena offset 和 free-list 链接。发送时增加一份 in-flight 引用；请求 handler 完成、响应
continuation 消费完成或异常丢包时减少引用。最后一次 `fetch_sub` 的执行者直接把 block
归还到所属 arena 的共享 size-class free-list，因此 receiver 可以直接完成远端释放。
正常 session 销毁和 `Rpc` 析构都会释放 server request、最后一个 dynamic response、
预分配 response 以及 control MsgBuffer；尚未发布的 pending descriptor 引用由 transport
析构负责回收。

当前一个 eRPC message 只使用一个 descriptor，不按 MTU 切包；最大容量由 allocator
size class 和 `ERPC_UB_ARENA_MB` 限制。默认 endpoint arena 为 16 MiB，最大单 block
为 8 MiB。

## 构建

```bash
cd eRPC
cmake -S . -B build-ub \
  -DTRANSPORT=ub \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so
cmake --build build-ub -j --target erpc_ub_manager hello_server hello_client
```

## 单进程模式

每台机器只有一个 eRPC 工作进程时不需要 manager：

```bash
export ERPC_UB_PROCESS_MODE=single
export ERPC_UB_MEMORY_MODE=one-sided
export ERPC_UB_REGION_MB=256
export ERPC_UB_ARENA_MB=16
export ERPC_UB_MACHINE_ID=1   # 另一台设为 2
```

服务端和客户端沿用 hello_world 自身的 URI 参数。两台机器必须使用不同 machine ID，
并使用相同 region prefix、region size、arena size 和 memory mode。

## 多进程模式

每台机器先启动一个 manager，再启动本机所有 eRPC 进程：

```bash
export ERPC_UB_PROCESS_MODE=multi
export ERPC_UB_MANAGER_SOCKET=/tmp/erpc_ub_manager.sock
export ERPC_UB_MEMORY_MODE=one-sided
export ERPC_UB_REGION_MB=256
export ERPC_UB_ARENA_MB=16
export ERPC_UB_MACHINE_ID=1

numactl --cpunodebind=0 --membind=0 ./build-ub/erpc_ub_manager
```

manager 只负责 machine region 生命周期、endpoint 注册、inbox/arena 分配，不参与
RPC 数据面。manager 对每个客户端请求设置 5 秒收发超时，连接后未发送完整请求的
worker 不会永久阻塞其他 endpoint 注册或注销。停止顺序应为工作进程先退出，manager
最后退出。

## 内存模式

- `ERPC_UB_MEMORY_MODE=one-sided`：
  `UBSM_FLAG_ONLY_IMPORT_NONCACHE | UBSM_FLAG_WR_DELAY_COMP`，物理 owner 使用
  cacheable 映射，remote importer 使用 non-cacheable 映射。
- `ERPC_UB_MEMORY_MODE=nocache`：
  `UBSM_FLAG_NONCACHE | UBSM_FLAG_WR_DELAY_COMP`，owner 和 importer 都使用
  non-cacheable 映射。

其他配置包括 `ERPC_UB_REGION_PREFIX`、`ERPC_UB_PROVIDER_HOST`、
`ERPC_UB_PROVIDER_SOCKET`、`ERPC_UB_PROVIDER_NUMA` 和 `ERPC_UB_PROVIDER_PORT`。

## 当前边界

- `rpc_id` 必须位于 `[0, 63]`，并在会互相通信的机器间全局唯一，因为它也是 source
  SPSC 队列索引。
- manager 尚未实现异常退出后的 PID 存活检测和 endpoint 自动回收。
- endpoint 注销要求其消息引用已经释放；当前不支持 manager crash recovery。
- 本地已完成 C++11 静态编译验证，但 Windows 环境没有 UB 设备；双机 hello_world
  仍需在安装 `libubsm_sdk.so` 的 UB Linux 主机上执行。
