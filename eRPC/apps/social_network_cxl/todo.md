# UB social network 性能验证 TODO

本清单按当前 98/99 部署编写：98 是 MongoDB、PostStorage 和 Timeline 所在机器；99
运行 Client 和前端服务。示例路径为 `/home/g/src/ub-application`；若实际 checkout
不同，请在两机都替换路径。各轮只改变一个变量，并在下一轮启动前保存两机日志和 99 上的
latency 文件。

执行顺序：两机编译并核对绑核 → UBSM 裸内存微测试 → Social Network 的 `fields`/`contiguous`
分段测量 → 关闭 profiling 的端到端测量 → 可选的写路径探针和 URMA 兼容性测试。
`ERPC_SN_POST_READ_MODE` 只影响最终 Client 对远端 `PostData` 的读取；当前应用只实现
`fields` 和 `contiguous` 两种模式，没有 Client 整块 scalar copy 模式。

## 1. 编译并验证显式绑核

两台机器都执行：

```bash
cd /home/g/src/ub-application/eRPC
printf '%s\n' social_network_cxl > scripts/autorun_app_file
cmake -S . -B cmake-build-social-ub \
  -DTRANSPORT=ub -DPERF=ON \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so
cmake --build cmake-build-social-ub -j \
  --target erpc_ub_manager client load_balance nginx compose_post \
  unique_id url_shorten user_mention user_service \
  user_timeline home_timeline post_storage
git rev-parse HEAD
sha256sum build/client build/post_storage
```

先按 `README_UB.md` 第 3～7 节检查 MongoDB 的 20011/20012/20014、两份
`config.ub*.json` 中的 IP、NUMA=0、机器 ID 和服务启动顺序。两机使用相同源码与
`PostData` 布局。`PERF=ON` 是编译选项；`ERPC_SN_PROFILE=1` 才启用本应用 breakdown。

`bind_core_offset` 是服务CPU块的起点。`client_num=server_num=1` 时：

```text
client RPC thread = base + 0
server RPC thread = base + 1
worker thread     = base + 2（仅worker服务）
```

机器98的预期节点内CPU索引：

```text
user_timeline: 0,1,2
home_timeline: 5,6,7
post_storage: 10,11
```

机器99的预期节点内CPU索引：

```text
client: 0,1
load_balance: 4,5
nginx: 7,8
compose_post: 10,11,12
unique_id: 14,15,16
url_shorten: 18,19,20
user_service: 22,23,24
user_mention: 26,27,28
```

启动后检查全局Linux CPU ID和物理core：

```bash
lscpu -e=CPU,NODE,CORE,ONLINE
for service in client load_balance nginx compose_post unique_id url_shorten \
  user_mention user_service user_timeline home_timeline post_storage; do
  for pid in $(pgrep -x "$service"); do
    echo "service=$service pid=$pid"
    ps -L -p "$pid" -o pid,tid,psr,comm
  done
done
```

同时检查日志中的 `Requested binding` 和 `Launching`。节点内索引与全局CPU ID不同。
Nexus session-management线程位于NUMA节点最后一个CPU，但阻塞等待UDP控制消息，不是
数据面busy-poll线程。leader线程阻塞在join，MongoDB初始化线程在正式测量前退出。

## 2. scalar load/copy微测试

两台机器都编译：

```bash
cd /home/g/src/ub-application/tests/ubs-mem-two-node
cmake -S . -B build-ubsm \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so
cmake --build build-ubsm -j --target ubsm_remote_rw_latency_test
```

机器98作为owner：

```bash
cd /home/g/src/ub-application/tests/ubs-mem-two-node
export LD_LIBRARY_PATH="/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
numactl --cpunodebind=0 --membind=0 \
  ./build-ubsm/ubsm_remote_rw_latency_test \
    --role owner \
    --bind-ip IP98_ADDRESS \
    --provider-numa 0 \
    --name ubsm_rw_scalar_copy_98 \
    --port 18531 \
    --region-mb 4 \
    --test-bytes 2097152 \
    --iterations 10000 \
    --timeout-sec 300
```

机器99作为remote：

```bash
cd /home/g/src/ub-application/tests/ubs-mem-two-node
export LD_LIBRARY_PATH="/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
numactl --cpunodebind=0 --membind=0 \
  ./build-ubsm/ubsm_remote_rw_latency_test \
    --role remote \
    --owner-ip IP98_ADDRESS \
    --name ubsm_rw_scalar_copy_98 \
    --port 18531 \
    --region-mb 4 \
    --test-bytes 2097152 \
    --iterations 10000 \
    --timeout-sec 300
```

重点保存 64/128/256/512/1024/1300/2048 B 的以下结果：

```text
remote_nc_memcpy_read
remote_nc_scalar_load_fenced
remote_nc_scalar_copy_read_fenced
remote_nc_memcpy_write
remote_nc_scalar_store_fenced
remote_nc_scalar_copy_write_fenced
```

然后交换owner/remote角色，使用新的共享内存名称和端口再测反方向。两端的 `--name`、
`--port`、`--region-mb`、`--test-bytes` 和 `--iterations` 必须一致。
98 owner → 99 remote 对应 Client 读取方向；99 owner → 98 remote 对应
PostStorage 从 Client 读取 Compose 请求的方向。此测试给出各尺寸均值，不包含 RPC、
映射引用、header 校验和应用线程调度，不能直接预测应用的 P99。

反方向运行时，先在 **99 owner** 启动：

```bash
cd /home/g/src/ub-application/tests/ubs-mem-two-node
export LD_LIBRARY_PATH="/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
numactl --cpunodebind=0 --membind=0 \
  ./build-ubsm/ubsm_remote_rw_latency_test \
    --role owner --bind-ip IP99_ADDRESS --provider-numa 0 \
    --name ubsm_rw_scalar_copy_99 --port 18532 \
    --region-mb 4 --test-bytes 2097152 --iterations 10000 --timeout-sec 300
```

再在 **98 remote** 启动：

```bash
cd /home/g/src/ub-application/tests/ubs-mem-two-node
export LD_LIBRARY_PATH="/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
numactl --cpunodebind=0 --membind=0 \
  ./build-ubsm/ubsm_remote_rw_latency_test \
    --role remote --owner-ip IP99_ADDRESS \
    --name ubsm_rw_scalar_copy_99 --port 18532 \
    --region-mb 4 --test-bytes 2097152 --iterations 10000 --timeout-sec 300
```

## 3. PostStorage远端copy探针

探针会扰动第一条写请求，只用于诊断。机器98启动前：

```bash
cd /home/g/src/ub-application/eRPC
export ERPC_SN_PROFILE=1
export ERPC_SN_STORAGE_WRITE_PROBE=1
```

机器99：

```bash
cd /home/g/src/ub-application/eRPC
export ERPC_SN_PROFILE=1
unset ERPC_SN_STORAGE_WRITE_PROBE
```

按 `README_UB.md` 的98/99命令运行，然后检查：

```bash
grep SN_UB_STORAGE_WRITE_PROBE \
  apps/social_network_cxl/logs/ub-98/post_storage.log
```

至少应看到：

```text
local_machine_id=98
source_is_local=0
request_bytes=2064
post_bytes=2048
post_offset_from_backing=32
```

正式性能测试前关闭探针：

```bash
unset ERPC_SN_STORAGE_WRITE_PROBE
```

## 4. 检查memcpy代码生成

```bash
cd /home/g/src/ub-application/eRPC
objdump -drwC build/post_storage > /tmp/post_storage.disasm
objdump -drwC build/client > /tmp/social_client.disasm
grep -n -A180 -F 'post_storage_write_req_handler' \
  /tmp/post_storage.disasm | grep -E 'memcpy|ldr|ldp|str|stp'
grep -n -E 'memcpy@|__memcpy' /tmp/post_storage.disasm
grep -n -E 'memcpy@|__memcpy' /tmp/social_client.disasm
grep -n -E 'native_post_contiguous|contiguous_scalar_checksum|native_post_fields' \
  /tmp/social_client.disasm

cd /home/g/src/ub-application/tests/ubs-mem-two-node
objdump -drwC build-ubsm/ubsm_remote_rw_latency_test \
  > /tmp/ubsm_remote_rw_latency_test.disasm
grep -n -E 'memcpy@|__memcpy|benchmark_scalar_copy' \
  /tmp/ubsm_remote_rw_latency_test.disasm
```

目标是确认 PostStorage 固定 2048 B copy 是否内联、微测试是否调用通用 libc
`memcpy`，并观察 Client 两种读取模式的目标机指令。反汇编不能证明 UB 总线事务次数。

## 5. Social Network：分段测量

先验证第 1 节的 CPU 绑定，然后两机启动前设置：

```bash
export ERPC_SN_PROFILE=1
unset ERPC_UB_PROFILE
unset ERPC_SN_STORAGE_WRITE_PROBE
```

98 先启动 `run_ub.sh`，99 后启动；完整环境变量、`SN_SERVICES` 与日志路径见
`README_UB.md` 第 5、6 节。保持 `generate_num`、`common.concurrency`、
`common.test_loop`、NUMA/CPU 映射一致。只修改 99 的 Client workload 配置：

| 场景 | `user_num` | `home_num` | `write_num` | Client 读取模式 |
| --- | ---: | ---: | ---: | --- |
| User Timeline | 1 | 0 | 0 | `fields`、`contiguous` |
| Home Timeline | 0 | 1 | 0 | `fields`、`contiguous` |
| Compose Post | 0 | 0 | 1 | 固定 `fields` 即可 |
| 混合负载（可选） | 3 | 4 | 3 | 两种模式分别测 |

三种 `*_num` 是请求类型权重，不是请求总数；总数由 `generate_num` 控制。
在 99 启动 launcher 的终端分别设置：

```bash
export ERPC_SN_POST_READ_MODE=fields
# 下一轮改为：export ERPC_SN_POST_READ_MODE=contiguous
```

每次修改 JSON 或读取模式后重启进程；建议按 README 的顺序重启两侧 launcher。
99 的 `client.log` 必须出现对应的 `SN_UB_POST_READ_MODE=...`，否则本轮作废。
写请求会改变 PostStorage 内存中的数据；先完成纯读两模式对比，再做写入。
若比较两轮混合负载，必须让 PostStorage 初始数据状态相同。

每轮正常退出后、下一轮启动前，在 **99** 保存结果：

```bash
cd /home/g/src/ub-application/eRPC
RUN_TAG=home-fields-profile  # 按场景、读取模式和 profiling 状态修改
mkdir -p "measurements/$RUN_TAG"
cp latency1-home latency1-user latency1-write "measurements/$RUN_TAG/"
cp -a apps/social_network_cxl/logs/ub-99 "measurements/$RUN_TAG/"
cp apps/social_network_cxl/config/config.ub99.json "measurements/$RUN_TAG/"
```

在 **98** 保存该轮服务日志与配置：

```bash
cd /home/g/src/ub-application/eRPC
RUN_TAG=home-fields-profile  # 必须与 99 相同
mkdir -p "measurements/$RUN_TAG"
cp -a apps/social_network_cxl/logs/ub-98 "measurements/$RUN_TAG/"
cp apps/social_network_cxl/config/config.ub98.json "measurements/$RUN_TAG/"
```

`latency1-*` 来自当前 `common.latency_file=./latency`、`common.concurrency=1`，
写在 launcher 的工作目录 `eRPC/`。下一轮会覆盖日志和 latency 文件。短 profiling
运行要正常退出，避免 `kill -9` 丢失线程退出时的最后一批统计。Breakdown 包含
`calls`、`avg_ns`、`p50_upper_ns`、`p90_upper_ns`、`p95_upper_ns`、
`p99_upper_ns`、`max_ns`；分位数是 2 次幂直方图桶上界，不是精确纳秒分位数。

## 6. 读取方式决策与正式延迟

在两轮纯读 profiling 运行中，重点查看 99 的 Client：

```bash
grep -E 'SN_UB_POST_READ_MODE|SN_UB_PROFILE.*stage=client_(import|metadata|fields|release)' \
  measurements/home-fields-profile/ub-99/client.log
grep -E 'SN_UB_POST_READ_MODE|SN_UB_PROFILE.*stage=client_(import|metadata|fields|release)' \
  measurements/home-contiguous-profile/ub-99/client.log
```

比较 `client_metadata + client_fields` 以及端到端 `latency1-home`、
`latency1-user`。`client_fields_contiguous` 只覆盖连续模式 header 后的 1944 B
扫描；`client_fields` 还包含本地长度校验和 checksum 汇总。`fields` 只读有效字段，
`contiguous` 读完整 2048 B；两者 checksum 数值语义不同，都只作为消费结果的 sink。
`scalar_copy_read` 微测试如果接近 `scalar_load`，再决定是否增加 Client 的第三种模式；
不能把当前微测试结果说成应用的第三种实测模式。

LB/Nginx 小消息转发 A/B：在 99 启动 `run_ub.sh` 前分别设置
`ERPC_SN_PROXY_INLINE=0` 和 `ERPC_SN_PROXY_INLINE=1`，每轮重启 LB/Nginx，
保持 `ERPC_SN_POST_READ_MODE=contiguous` 与纯 Home/User 负载一致。
开关仅改变 Timeline 读请求及其引用响应在代理内部跨线程交接的方式；
Ping/Compose 和服务跳数不变。先在两侧 `ERPC_SN_PROFILE=1` 下比较代理
`proxy_*_rx_pin`、`proxy_*_queue_handoff`、`proxy_*_tx_prepare`，再关闭
profiling 比较端到端 P50/P99。关闭是默认路径，`0` 和 unset 等价。

两机都 `unset ERPC_SN_PROFILE`、`unset ERPC_UB_PROFILE`，保持其他参数相同，
分别重跑 User/Home 两种纯读和 Compose 写入，保存无 profiling 的
`latency1-*` P50/P90/P99。若要比较两个读取模式的端到端差异，两种模式都要各跑一轮
无 profiling 测试。记录每轮的 Git revision/二进制 hash、机器角色、读取模式、
NUMA/CPU、workload、有效请求数和错误信息。若 P90/P99 仍有长尾，再检查 98 的
Timeline/PostStorage 和 99 的 LB/Nginx/Client breakdown；不要把不同线程或
interval 的均值直接相加。

现有 `logs/ub-99/client.log` 可能包含旧的 `client_copy` 阶段；以新运行日志里的
`SN_UB_POST_READ_MODE` 和 `client_fields*` 确认已使用这次编译的代码。

## 6.1 复机后验证 Proxy Inline 的 P90～P99（可直接执行）

本轮专门验证 LB/Nginx inline 队列缩为 64 B、每队列每轮最多处理 8 条之后，
尾延迟是否改善。先在**两台机器**拉取同一代码版本并按第 1 节重新编译；不要把上次
`cmp.txt` 与新二进制直接当作严格 A/B。以下使用 `/home/g/src/ub-application`、
`/home/g/.local` 和 `eRPC/build`，与第 1 节、`run_ub.sh` 的实际输出目录一致。

先检查地址、CPU 和 NUMA；两份 JSON 中的 `IP98`/`IP99` 必须换成可达的实际 IP：

```bash
cd /home/g/src/ub-application/eRPC
git rev-parse HEAD
sha256sum build/{client,load_balance,nginx,home_timeline,user_timeline,post_storage,erpc_ub_manager}
grep -nE 'IP98|IP99' apps/social_network_cxl/config/config.ub{98,99}.json
numactl -H
lscpu -e=CPU,NODE,CORE,ONLINE
test -x build/erpc_ub_manager && test -x build/client && test -x build/post_storage
```

下面按 **98=NUMA 0、99=NUMA 2** 写命令：99 上次换到 NUMA 2 后才解决
manager 分配内存失败，但这不是当前机器状态的实时证明。99 必须先确认 NUMA 2
至少容纳 README 预留的节点内 CPU 索引 0～30，且有足够空闲内存；否则停止并根据
`numactl -H` 选择实际可用节点，同步修改下方 `SN_NUMA_99`，不要仅修改
`numactl` 而保留 JSON 中的 NUMA 0。98 的 MongoDB 及 IP/端口准备仍按
`README_UB.md` 第 3～4 节执行。若需要额外的 `ERPC_UB_PROVIDER_HOST`、
`ERPC_UB_PROVIDER_SOCKET` 或 `ERPC_UB_PROVIDER_PORT`，沿用该机器上次成功运行的值。

每轮在两侧使用同一个、尚未使用过的 `RUN_TAG`。先在 **98** 的终端设置：

```bash
cd /home/g/src/ub-application/eRPC
export RUN_TAG=mixed-inline1-profile0-r1  # 按下表逐轮修改，98/99 必须相同
export SN_NUMA_98=0
export SN_DEPS=/home/g/.local
export PATH="$SN_DEPS/bin:$PATH"
export LD_LIBRARY_PATH="$SN_DEPS/lib:$SN_DEPS/lib64:/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
export PKG_CONFIG_PATH="$SN_DEPS/lib/pkgconfig:$SN_DEPS/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
export CMAKE_PREFIX_PATH="$SN_DEPS:${CMAKE_PREFIX_PATH:-}"
export ERPC_UB_MACHINE_ID=98
export ERPC_UB_PROCESS_MODE=multi
export ERPC_UB_MEMORY_MODE=one-sided
export ERPC_UB_PROVIDER_NUMA="$SN_NUMA_98"
export ERPC_UB_REGION_MB=2048
export ERPC_UB_ARENA_MB=64
export ERPC_UB_MANAGER_SOCKET=/tmp/erpc_ub_social_network.sock
export ERPC_UB_SHUTDOWN_TIMEOUT_MS=30000
export ERPC_UB_SHUTDOWN_RETRY_MS=500
export SN_BUILD_DIR="$(pwd)/build"
export SN_CONFIG="$(pwd)/apps/social_network_cxl/config/config.ub98.json"
export SN_LOG_DIR="$(pwd)/measurements/$RUN_TAG/ub-98"
export SN_SERVICES='post_storage user_timeline home_timeline'
export ERPC_SN_POST_READ_MODE=contiguous
export ERPC_SN_PROXY_INLINE=1  # 每轮按下表改为 0 或 1；98 无代理，但保持环境一致
unset ERPC_UB_PROFILE ERPC_SN_STORAGE_WRITE_PROBE
unset ERPC_SN_PROFILE  # profiling 轮改为：export ERPC_SN_PROFILE=1
test ! -e "$SN_LOG_DIR/erpc_ub_manager.log" || { echo 'RUN_TAG 已使用'; false; }
numactl --cpunodebind="$SN_NUMA_98" --membind="$SN_NUMA_98" \
  ./apps/social_network_cxl/run_ub.sh
```

等 98 的服务启动后，在 **99** 的另一个终端设置并运行。99 的 `SN_CONFIG`
是每轮新生成的副本，不修改仓库中的 `config.ub99.json`。`USER_NUM`、
`HOME_NUM`、`WRITE_NUM` 是权重而不是请求总数；`generate_num` 保持 5000：

```bash
cd /home/g/src/ub-application/eRPC
export RUN_TAG=mixed-inline1-profile0-r1  # 与 98 一致
export SN_NUMA_99=2
export USER_NUM=0 HOME_NUM=7 WRITE_NUM=3  # 按下表逐轮修改
export SN_DEPS=/home/g/.local
export PATH="$SN_DEPS/bin:$PATH"
export LD_LIBRARY_PATH="$SN_DEPS/lib:$SN_DEPS/lib64:/usr/local/ubs_mem/lib:${LD_LIBRARY_PATH:-}"
export PKG_CONFIG_PATH="$SN_DEPS/lib/pkgconfig:$SN_DEPS/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
export CMAKE_PREFIX_PATH="$SN_DEPS:${CMAKE_PREFIX_PATH:-}"
export ERPC_UB_MACHINE_ID=99
export ERPC_UB_PROCESS_MODE=multi
export ERPC_UB_MEMORY_MODE=one-sided
export ERPC_UB_PROVIDER_NUMA="$SN_NUMA_99"
export ERPC_UB_REGION_MB=2048
export ERPC_UB_ARENA_MB=64
export ERPC_UB_MANAGER_SOCKET=/tmp/erpc_ub_social_network.sock
export ERPC_UB_SHUTDOWN_TIMEOUT_MS=30000
export ERPC_UB_SHUTDOWN_RETRY_MS=500
export SN_BUILD_DIR="$(pwd)/build"
export SN_LOG_DIR="$(pwd)/measurements/$RUN_TAG/ub-99"
export SN_SERVICES='client load_balance nginx compose_post unique_id url_shorten user_service user_mention'
export ERPC_SN_POST_READ_MODE=contiguous
export ERPC_SN_PROXY_INLINE=1  # 按下表改为 0 或 1
unset ERPC_UB_PROFILE ERPC_SN_STORAGE_WRITE_PROBE
unset ERPC_SN_PROFILE  # profiling 轮改为：export ERPC_SN_PROFILE=1
test ! -e "measurements/$RUN_TAG" || { echo 'RUN_TAG 已使用'; false; }
mkdir -p "measurements/$RUN_TAG"
python3 - "$RUN_TAG" "$SN_NUMA_99" "$USER_NUM" "$HOME_NUM" "$WRITE_NUM" <<'PY'
import json, sys
tag, numa, user, home, write = sys.argv[1:]
with open('apps/social_network_cxl/config/config.ub99.json') as f:
    config = json.load(f)
config['common']['numa_client_node'] = int(numa)
config['common']['numa_server_node'] = int(numa)
config['client'].update(user_num=int(user), home_num=int(home), write_num=int(write))
with open('measurements/{}/config.ub99.json'.format(tag), 'w') as f:
    json.dump(config, f, indent=2)
PY
export SN_CONFIG="$(pwd)/measurements/$RUN_TAG/config.ub99.json"
grep -nE 'IP98|IP99' "$SN_CONFIG"  # 有输出就先修正地址，不要启动
test ! -e latency1-home && test ! -e latency1-user && test ! -e latency1-write
numactl --cpunodebind="$SN_NUMA_99" --membind="$SN_NUMA_99" \
  ./apps/social_network_cxl/run_ub.sh
```

上述 `test`/`grep` 是检查命令，不会自动中止后续复制粘贴；发现占位 IP、旧
`latency1-*` 或节点 CPU/内存不足时必须先处理，不能继续启动。99 如果确实要
继续用 NUMA 0，只有确认内存容量问题已解决后才把 `SN_NUMA_99=0`。

建议按以下顺序做，先复现原来的混合负载，再用纯读隔离。每轮必须**两侧都正常
Ctrl-C 结束、保存日志，再启动下一轮**；无 profiling 的每种配置至少重复 3 轮，
不要只凭 5000 次请求的一轮 P99 下结论。profile 轮只用于定位阶段，不与
无 profile 轮直接比较绝对延迟。

| 场景/`RUN_TAG` 前缀 | `USER_NUM HOME_NUM WRITE_NUM` | `ERPC_SN_PROXY_INLINE` | `ERPC_SN_PROFILE` | 重复 |
| --- | --- | ---: | ---: | ---: |
| `mixed-inline0-profile0` | `0 7 3` | 0 | unset | 3 |
| `mixed-inline1-profile0` | `0 7 3` | 1 | unset | 3 |
| `home-inline0-profile0` / `home-inline1-profile0` | `0 1 0` | 0 / 1 | unset | 各 3 |
| `user-inline0-profile0` / `user-inline1-profile0` | `1 0 0` | 0 / 1 | unset | 各 3 |
| `home-inline0-profile1` / `home-inline1-profile1` | `0 1 0` | 0 / 1 | 1 | 各 1 |
| `user-inline0-profile1` / `user-inline1-profile1` | `1 0 0` | 0 / 1 | 1 | 各 1 |

例：`RUN_TAG=home-inline1-profile0-r2` 时，两台机器都设置
`ERPC_SN_PROXY_INLINE=1`、`unset ERPC_SN_PROFILE`；99 再设置
`USER_NUM=0 HOME_NUM=1 WRITE_NUM=0`。为了控制运行顺序/预热的影响，
可以交错运行 inline0/inline1，而不是连续跑完一种模式。

每轮确认 99 的 `latency1-*` **新文件已经生成**后，再在 99、98 两侧
分别对 launcher 发送 Ctrl-C，让 worker 先于 manager 退出。随后在 99 归档：

```bash
cd /home/g/src/ub-application/eRPC
mv latency1-home latency1-user latency1-write "measurements/$RUN_TAG/"
sha256sum build/{client,load_balance,nginx} > "measurements/$RUN_TAG/sha256-99.txt"
git rev-parse HEAD > "measurements/$RUN_TAG/git-revision-99.txt"
grep -E 'SN_UB_PROXY_INLINE|SN_UB_POST_READ_MODE' \
  "measurements/$RUN_TAG/ub-99/"{client,load_balance,nginx}.log
grep -RniE 'error|failed|assert|6050|603|reference underflow' \
  "measurements/$RUN_TAG/ub-99" || true
```

98 归档其配置与构建标识，并检查 manager 是否干净退出：

```bash
cd /home/g/src/ub-application/eRPC
cp apps/social_network_cxl/config/config.ub98.json "measurements/$RUN_TAG/config.ub98.json"
sha256sum build/{home_timeline,user_timeline,post_storage} > "measurements/$RUN_TAG/sha256-98.txt"
git rev-parse HEAD > "measurements/$RUN_TAG/git-revision-98.txt"
grep -RniE 'error|failed|assert|6050|603|reference underflow' \
  "measurements/$RUN_TAG/ub-98" || true
```

profile 轮重点提取这些阶段，分别保存 **LB 和 Nginx** 的正反向队列交接、
接收端 copy/pin、发送端 materialize/准备与回调释放，以及 Client 远端读取：

```bash
cd /home/g/src/ub-application/eRPC
grep -E 'stage=proxy_(forward|reverse)_(rx_pin|rx_queue|queue_handoff|tx_prepare|tx_enqueue|callback_release)' \
  "measurements/$RUN_TAG/ub-99/"{load_balance,nginx}.log
grep -E 'stage=client_(import|metadata|fields|fields_contiguous|release)' \
  "measurements/$RUN_TAG/ub-99/client.log"
grep -E 'stage=(timeline_|forward_|storage_|rx_)' \
  "measurements/$RUN_TAG/ub-98/"{home_timeline,user_timeline,post_storage}.log
```

判断顺序：先看同场景无 profile 的 P50/P90/P95/P99 和每轮波动；若
inline1 的 P99 仍变差，检查 `proxy_*_queue_handoff` 的 P99 是否同步抬高。
若队列交接没有抬高，再查 `proxy_*_rx_pin`、`proxy_*_tx_prepare`，最后
对照 Client 的 `client_import`/`client_fields*` 与 98 的 Timeline/PostStorage。
这次修复只消除了代码中可能的长时间队列 drain，并未在真机证明 P99 原因。

## 7. 可选：URMA 与 UBSM 兼容性

如果这次也要评估 UB shared memory + URMA 混合路径，按
`tests/ubsm-urma-two-node/README.md` 在两台机器编译、运行并保存原始输出：

1. `urma_rw_latency_test`：98 server、99 client，测 64～4096 B 的 URMA
   READ/WRITE post-to-completion，再交换方向。
2. `ubsm_urma_compat_test`：98 owner、99 remote，验证同一 UBSM owner 映射可否
   注册为 URMA segment、CPU/URMA 双向写入是否可见，再交换方向。

先用 `urma_admin show --all` 核对设备、EID、`tp_type`。兼容性测试全部通过后
才考虑混合路径；这些独立程序不会改变 eRPC 数据路径。重点记录 1300/2048 B，
并注明 URMA post-to-completion 与 UBSM CPU load/copy 的计时边界不同。
