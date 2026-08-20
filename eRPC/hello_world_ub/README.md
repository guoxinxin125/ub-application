# eRPC UB hello world

该示例用于验证完整的 UB eRPC 共享内存路径：共享 arena 分配、descriptor 发送、
receiver 直接读取远端 payload、request/response 引用释放以及 allocator slot 回收。
它只能在 `TRANSPORT=ub` 时构建。

## 构建

可以在 `eRPC/hello_world_ub` 目录中使用 Makefile。它会调用 eRPC 现有的
CMake 配置，并同时构建 server、client 和 manager：

```bash
cd eRPC/hello_world_ub
make \
  UBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  UBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so
```

单独构建某个目标时使用 `make server`、`make client` 或 `make manager`。
默认构建目录为 `eRPC/build-ub`，可通过 `BUILD_DIR` 覆盖。

也可以从仓库根目录直接使用 CMake：

```bash
cmake -S eRPC -B eRPC/build-ub \
  -DTRANSPORT=ub \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so

cmake --build eRPC/build-ub -j \
  --target hello_ub_server hello_ub_client erpc_ub_manager
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
