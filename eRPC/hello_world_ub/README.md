# eRPC UB hello world

该示例用于验证完整的 UB eRPC 共享内存路径：共享 arena 分配、descriptor 发送、
receiver 直接读取远端 payload、request/response 引用释放以及 allocator slot 回收。
request 的全部 `msg-size` 字节都会按 client ID、request ID 和字节偏移填充。server
读取并校验全部 request，再逐字节写出同样大小的 response；client 最后校验全部
response。它只能在 `TRANSPORT=ub` 时构建。

## 构建

可以在 `eRPC/hello_world_ub` 目录中使用 Makefile。它会调用 eRPC 现有的
CMake 配置，并同时构建 server、client 和 manager：

```bash
cd eRPC/hello_world_ub
make \
  UBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  UBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so
```

单独构建某个目标时使用 `make server`、`make client`、`make multi-server`、
`make multi-client` 或 `make manager`。
默认构建目录为 `eRPC/build-ub`，可通过 `BUILD_DIR` 覆盖。

也可以从仓库根目录直接使用 CMake：

```bash
cmake -S eRPC -B eRPC/build-ub \
  -DTRANSPORT=ub \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so

cmake --build eRPC/build-ub -j \
  --target hello_ub_server hello_ub_client \
           hello_ub_multi_server hello_ub_multi_client erpc_ub_manager
```

## 双机单进程模式

两台机器必须设置不同的 `ERPC_UB_MACHINE_ID`。其他 UB 内存配置必须一致。
`ERPC_UB_NUMA_NODE` 选择 Nexus、线程绑核和 eRPC 本地分配所在节点；未设置时继承
`ERPC_UB_PROVIDER_NUMA`。后者选择 UBSM provider 节点。只有两者需要不同时才必须
同时设置；`ERPC_PROVIDER_NUMA` 不是有效变量。

机器 82（示例控制面 IP 为 `192.0.2.82`，使用 NUMA0）：

```bash
export ERPC_UB_PROCESS_MODE=single
export ERPC_UB_MEMORY_MODE=one-sided
export ERPC_UB_MACHINE_ID=82
export ERPC_UB_NUMA_NODE=0
export ERPC_UB_PROVIDER_NUMA=0
export ERPC_UB_REGION_MB=256
export ERPC_UB_ARENA_MB=16

numactl --cpunodebind=0 --membind=0 \
  ./eRPC/build-ub/hello_ub_server 192.0.2.82 31850 10100
```

机器 81（示例控制面 IP 为 `192.0.2.81`，使用 NUMA1）：

```bash
export ERPC_UB_PROCESS_MODE=single
export ERPC_UB_MEMORY_MODE=one-sided
export ERPC_UB_MACHINE_ID=81
export ERPC_UB_NUMA_NODE=1
export ERPC_UB_PROVIDER_NUMA=1
export ERPC_UB_REGION_MB=256
export ERPC_UB_ARENA_MB=16

numactl --cpunodebind=1 --membind=1 \
  ./eRPC/build-ub/hello_ub_client \
    192.0.2.82 192.0.2.81 1 4096 10000
```

server 的 `10100` 等于 client 的 100 次 warmup 加 10000 次正式请求，因此测试完成后
server 会自动退出。也可以省略该参数，让 server 一直运行直到收到 SIGINT/SIGTERM。
client 完成请求后会先执行 session disconnect；双方释放远端 machine-region 映射后，
再删除各自拥有的 UBSM region，避免退出时得到 `SHM_IN_USING`。

### 使用运行脚本

`run.sh` 会同时设置 UB 环境变量和 `numactl` 绑定。`machine-id` 和
`numa-node` 必须显式给出，防止误用跨 HLC 的 NUMA 组合。例如以
82 的 NUMA0 作为 server、81 的 NUMA1 作为 client：

```bash
# 82 机器
cd eRPC/hello_world_ub
bash run.sh server 192.0.2.82 82 0 31850 10100

# 81 机器
cd eRPC/hello_world_ub
bash run.sh client 192.0.2.82 192.0.2.81 81 1 1 4096 10000
```

另一组合应将 81 的 NUMA3 与 82 的 NUMA2 配对。示例 IP 需要替换为
实际的控制面 IP。如果 UBS Memory provider hostname 不是本机 `hostname`，
运行前还需要设置：

```bash
export ERPC_UB_PROVIDER_HOST=<provider-hostname>
```

默认使用 `single` 进程模式和 `one-sided` 内存模式。可以在命令前通过
`ERPC_UB_PROCESS_MODE`、`ERPC_UB_MEMORY_MODE`、`ERPC_UB_REGION_MB` 和
`ERPC_UB_ARENA_MB` 等环境变量覆盖。运行 `bash run.sh --help` 查看完整参数。

client 参数为：

```text
hello_ub_client <server-ip> <client-bind-ip>
  [concurrency] [msg-size] [requests] [server-port] [client-port]
```

server 参数为：

```text
hello_ub_server <bind-ip> [server-port] [max-requests]
```

`msg-size` 最小为 16 字节。client 的计时从填充整个 request 前开始，到读取并校验
整个 response 后结束；不包含 request buffer 分配和完成后的 buffer 释放。因此输出的
`average_latency` 包含 client 全消息写入、UB RPC 往返、server 全消息读取/校验与
response 写入，以及 client 全消息读取/校验。

## 多 client、单 server 测试

`hello_ub_multi_server` 使用一个 server endpoint。它的 inbox 已为每个 source
`rpc_id` 包含一条独立 SPSC；client session 建立时，相应 source 会加入 transport 的
active-source 数组，接收路径只对这些 source 做 round-robin 轮询。

每个 client 必须使用全局唯一的 `rpc-id`。当前 multi server 要求 N 个 client 使用连续
ID `[1, N]`，并分别检查每个 client 的请求数、checksum 和全 payload 错误数。同一台
机器运行多个 client 进程时必须使用 `ERPC_UB_PROCESS_MODE=multi`，先启动该机器的
`erpc_ub_manager`；不能让多个 single-mode 进程分别创建同一个 machine region。

下面示例在机器 82 上运行一个 server，在机器 81 上并发运行两个 client。两端的
memory mode、region prefix、region/arena size 必须一致。

机器 82：

```bash
export ERPC_UB_PROCESS_MODE=single
export ERPC_UB_MEMORY_MODE=one-sided
export ERPC_UB_MACHINE_ID=82
export ERPC_UB_NUMA_NODE=0
export ERPC_UB_PROVIDER_NUMA=0
export ERPC_UB_REGION_MB=256
export ERPC_UB_ARENA_MB=16

numactl --cpunodebind=0 --membind=0 \
  ./eRPC/build-ub/hello_ub_multi_server 192.0.2.82 2 10000 31850
```

机器 81 先启动 manager：

```bash
export ERPC_UB_PROCESS_MODE=multi
export ERPC_UB_MANAGER_SOCKET=/tmp/erpc_ub_manager.sock
export ERPC_UB_MEMORY_MODE=one-sided
export ERPC_UB_MACHINE_ID=81
export ERPC_UB_NUMA_NODE=1
export ERPC_UB_PROVIDER_NUMA=1
export ERPC_UB_REGION_MB=256
export ERPC_UB_ARENA_MB=16

numactl --cpunodebind=1 --membind=1 \
  ./eRPC/build-ub/erpc_ub_manager
```

manager 就绪后，在机器 81 的另一个 shell 中继承同一组环境变量，并并发启动两个
client。不同进程必须使用不同 `rpc-id` 和 `client-port`：

```bash
numactl --cpunodebind=1 --membind=1 \
  ./eRPC/build-ub/hello_ub_multi_client \
    192.0.2.82 192.0.2.81 1 8 4096 10000 31850 31851 \
    >client-1.log 2>&1 &
client_1_pid=$!

numactl --cpunodebind=1 --membind=1 \
  ./eRPC/build-ub/hello_ub_multi_client \
    192.0.2.82 192.0.2.81 2 8 4096 10000 31850 31852 \
    >client-2.log 2>&1 &
client_2_pid=$!

wait "${client_1_pid}"
client_1_status=$?
wait "${client_2_pid}"
client_2_status=$?
echo "client statuses: ${client_1_status} ${client_2_status}"
```

也可以用批量启动脚本完成同样的操作。参数中的 `2` 是 client 数量；脚本自动使用
RPC ID `[1, 2]` 和从 31851 开始的不同 client port，并等待、汇总全部 client：

```bash
bash eRPC/hello_world_ub/run_multi_clients.sh \
  192.0.2.82 192.0.2.81 81 1 2 8 4096 10000 31850 31851
```

完整参数为：

```text
run_multi_clients.sh \
  <server-ip> <client-bind-ip> <machine-id> <numa-node> <client-count> \
  [concurrency] [msg-size] [requests] [server-port] [first-client-port]
```

脚本不会启动或停止 manager。运行脚本前，client 机器必须已经用相同 UB 环境启动
`erpc_ub_manager`。每个 client 的输出保存在独立日志中；默认日志目录为
`${TMPDIR:-/tmp}/erpc-ub-multi-<时间>-<脚本PID>`，可用 `ERPC_UB_LOG_DIR` 覆盖。

两个 client 都退出后再停止 client 机器上的 manager。成功时两个 client 和 server
都输出 `PASS`。multi 程序参数为：

```text
hello_ub_multi_server <bind-ip> <clients> <requests-per-client>
  [server-port]

hello_ub_multi_client <server-ip> <client-bind-ip> <rpc-id>
  [concurrency] [msg-size] [requests] [server-port] [client-port]
```

## UB transport profiling

Set `ERPC_UB_PROFILE=1` on both the client and server to collect aggregated
transport-stage timings. Each process prints `UB_PROFILE` lines when its
`Rpc` is destroyed. The reported stages include shared-buffer allocation and
free, endpoint lookup, TX reference acquisition, queue publish/poll, remote
payload resolution, and total TX/RX burst time. Payload resolution is further
split into bounds, state, metadata-load, and final-check stages.
`adjusted_avg_ns` subtracts the measured timestamp-read overhead.

Remote machine mappings are acquired and reference-counted when a session
route is resolved. The RX data path uses the `machine_base` cached in the
remote endpoint without taking the machine-context mutex. Disconnect releases
that endpoint's mapping reference, and the last reference performs the unmap.

Profiling adds timestamp and counter overhead. Use it to locate expensive
stages, then unset `ERPC_UB_PROFILE` when measuring final end-to-end latency:

```bash
export ERPC_UB_PROFILE=1
```

成功时 client 输出 `PASS`、实际/预期 checksum、校验错误数和平均端到端延迟。该延迟
包含完整 RPC，而不是单独的 UB load 延迟。
