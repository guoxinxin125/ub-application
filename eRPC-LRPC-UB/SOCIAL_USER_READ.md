# UserTimeline read：AArch64 UB 路由域接入

这是可选应用路径，保留普通 UB-eRPC、x86/QEMU 接入和 Direct/Nested 微测试。
代码已完成本地 fixture 回归及相关用户态 AArch64 交叉编译；**未完成目标机全应用链接、
新模块构建、双机启动或性能验证**。先跑独立路由探针，再跑少量端到端请求。
不需要修改启动参数、重启机器、重启 UB 服务或使用 Docker。

## 1. 第一版范围与摆放

只支持一个 Client、一个发送 lane、并发 1、一个 nginx 域、UserTimeline read。
不包括 HomeTimeline、发帖、多 nginx、UB 业务写入，以及应用链的 fused 切换。
算术 Nested 微测试的默认路径已改为 fused；本应用 profile 在未指定路径时仍使用
独立的原路由链。启动脚本显式设置 `LRPC_UB_NESTED_PATH=legacy`；若直接使用
应用客户端并显式指定 fused，库会以 `EOPNOTSUPP` 拒绝，不会当作已支持应用级 fused。
域调用仍使用多连接池：Client 前向一个连接，Timeline 回程一个连接，同属 A 内核，
不同 slot/identity/ASID。这里的单 lane 限制只针对首版应用，不取消微测试的多连接能力。

| 机器 | Domain 模式 | 原 UB-eRPC 基线 |
| --- | --- | --- |
| A，NUMA 0 | Client、UserTimeline、A manager | 同左 |
| B，provider NUMA 1 | PostStorage、B manager；另一个 `--social` owner 发布 LB/nginx 代码、配置、页表 | 同位置的 PostStorage、B manager，以及真正的 LB/nginx eRPC 进程 |
| 数据库 | 保留原 MongoDB 所在机器和数据，仅明确应用连接地址 | 同左 |

**A/B 是实验角色，不是 98/99 的别名。** 原 `README_UB.md` 将 PostStorage 和数据库放在 98、
Client 放在 99。本实验保留 PostStorage 在 B，只把 UserTimeline 与 Client 放在 A，
以便仅 A 需要域切换模块。若保留原 PostStorage 所在物理机，则 A 对应 99、B 对应 98；
计算 NUMA、provider NUMA 和机器 ID 仍以实际分配为准，不由主机名推断。
上一版将 PostStorage 放到 A 是不必要的部署变更，现已修正；旧生成配置应重新生成。

PostData 由 B 的 PostStorage 分配在 B 的 UB arena，两种模式都由 A 消费同一布局的 handle。
使用 `one-sided` 模式，确认实际共享内存导入属性和 handle 的 machine ID；不能仅凭角色表
宣称 NC 或远端访问性能已验证。LRPC 额外的远端访问是 B 配置及 TLB miss 时的 B 页表。

逻辑调用保留 QEMU 应用适配的语义，而不是复制整个 C++ handler：

```text
前向：Client → LB 域 → nginx 域 → A 的正常应用上下文 → UserTimeline → PostStorage
回程：PostStorage → UserTimeline → nginx 域 → LB 域 → A 的正常应用上下文 → Client
```

每个前向/回程链有两个实际域入口、4 次逻辑域切换，中间恢复 A 内核执行交接。
这不是算术 Nested 微测试的 middle/leaf/resume 六次切换，也不使用其 fused 汇编。
四个入口上下文分别是 LB-forward、nginx-forward、nginx-return、LB-return；
代码 RX 在 A，参数共享一份连接私有 A-local WB mmap A-stack，各入口有独立 E-stack/ASID，
B 页表保留、RO 配置在 B。调用期间不分配/释放栈或重新复制代码，也不逐调用失效 TLB。
没有 payload 的 `copy_from_user/copy_to_user`，但仍有本地消息准备、快照校验和结果读回，
不能理解为完全没有本地内存复制。
新增 ioctl `UD_SOCIAL_CALL`；没有加入新的 Linux syscall。

原 UserTimeline 校验/查询、PostStorage 请求、消息类型 21/22/23、来源校验、
帖子 32-byte handle、正文消费与引用释放沿用当前应用。域不运行 eRPC、MongoDB、
libc 分配器、TLS，也不增加 nginx 业务校验或远端写入。
执行页在建连时从 B 发布的镜像复制到 A；A 模块中也编译了相同镜像，作用是严格的
字节白名单校验，不是直接跳到 A 编译进去的参考函数。因此不支持任意 ELF/任意函数发布。

## 2. 同步与编译

只跑微测试仍只需本目录。跑应用还需要**同一仓库布局**下的 `eRPC/`，包括其现有构建依赖
和 `apps/social_network_cxl`；不需要 `eRPC-LRPC/`、Linux 源码、Go、QEMU 或 DeathStarBench。
应用和依赖准备、MongoDB 启动/数据导入全部按
[原 README_UB.md](../eRPC/apps/social_network_cxl/README_UB.md)，不要使用 VM 脚本或覆盖现有数据库。
以下 `REPO`、SDK/OBMM 路径、IP 和 NUMA 按实际机器设置。

两端的 Protobuf 库与 `protoc` 应来自匹配的安装，版本至少 3.12。
本应用的 `.proto` 使用 proto3 `optional`；3.12–3.14 的 `protoc` 需要
`--experimental_allow_proto3_optional`，当前 `eRPC/CMakeLists.txt` 已显式传入，
兼容旧版 CMake 的 FindProtobuf。若遇到 “This file contains proto3 optional fields”
错误，同步此 CMakeLists 后重新执行对应构建目录的 `cmake -S ... -B ...` 配置命令，
再构建；不要删除 `.proto` 中的 `optional`。用 `protoc --version` 检查环境，
实际构建选择的编译器还可查看该目录 `CMakeCache.txt` 的 `Protobuf_PROTOC_EXECUTABLE`。

```sh
export REPO=/absolute/path/ub-application
export UBSM_INCLUDE_DIR=/actual/sdk/include
export UBSM_LIBRARY=/actual/sdk/lib/libubsm_sdk.so
# 仅 A 构建域客户端额外需要匹配 SDK/内核接口的 OBMM 头和库：
export OBMM_INCLUDE_DIR=/actual/obmm/include
export OBMM_LIBRARY=/usr/lib64/libobmm.so
```

A：先构建探针和模块。须先结束所有本项目连接；只替换本项目当前加载的模块。

```sh
cd "$REPO/eRPC-LRPC-UB"
sh scripts/build.sh caller
sh scripts/build-module.sh --vhe
modinfo -F ud_social kernel/ub-domain/ub_lrpc_domain_vhe.ko
# 应输出 user-read-abi1-single-nginx-legacy-chain
# 已加载 VHE 旧模块且所有连接已退出时才执行 rmmod；未加载则跳过：
sudo rmmod ub_lrpc_domain_vhe
sudo insmod kernel/ub-domain/ub_lrpc_domain_vhe.ko experimental=1 vhe_gate=1
```

EL1 机器改用不带 `--vhe` 的构建与 `ub_lrpc_domain.ko experimental=1`。
B 的域模式需要两组程序：**owner 发布服务域**，以及 **PostStorage/manager 提供普通
UB-eRPC 应用服务**。先构建 owner；B 不需要 kernel-devel 或 OBMM 开发头：

```sh
cd "$REPO/eRPC-LRPC-UB"
sh scripts/build.sh owner
```

A 构建域应用，产物隔离到新的 CMake 目录：

```sh
cmake -S "$REPO/eRPC" -B "$REPO/eRPC/cmake-build-social-ub-domain" \
  -DAPP=social_network_cxl -DTRANSPORT=ub -DPERF=ON \
  -DERPC_SN_DOMAIN_PROXY=ON -DERPC_SN_DOMAIN_BACKEND=ub \
  -DUBSM_INCLUDE_DIR="$UBSM_INCLUDE_DIR" -DUBSM_LIBRARY="$UBSM_LIBRARY" \
  -DOBMM_INCLUDE_DIR="$OBMM_INCLUDE_DIR" -DOBMM_LIBRARY="$OBMM_LIBRARY"
cmake --build "$REPO/eRPC/cmake-build-social-ub-domain" -j2 \
  --target erpc_ub_manager client user_timeline
```

**B：构建域模式所需的 PostStorage 和 manager。** 这些进程不执行域切换，使用
`PROXY=OFF` 的 `-rpc` 构建目录；这也是 `run-social.sh --side B --mode domain --config ...`
默认读取的目录。B 的 LB/nginx 域镜像已包含在上面的 owner 中，无需启动对应 eRPC 进程。

```sh
# B，沿用本节开头的 REPO、UBSM 和应用依赖环境变量：
cmake -S "$REPO/eRPC" -B "$REPO/eRPC/cmake-build-social-ub-rpc" \
  -DAPP=social_network_cxl -DTRANSPORT=ub -DPERF=ON \
  -DERPC_SN_DOMAIN_PROXY=OFF -DERPC_SN_DOMAIN_BACKEND=ub \
  -DUBSM_INCLUDE_DIR="$UBSM_INCLUDE_DIR" -DUBSM_LIBRARY="$UBSM_LIBRARY"
cmake --build "$REPO/eRPC/cmake-build-social-ub-rpc" -j2 \
  --target erpc_ub_manager post_storage
```

已有应用依赖装在私有 prefix 时继续设置 `CMAKE_PREFIX_PATH`、`PKG_CONFIG_PATH`、
`LD_LIBRARY_PATH`，参考原 README。**准备严格 RPC 基线时，A 另建 RPC-only 目录，
B 复用上面的 `-rpc` 目录并补建 nginx/load_balance**：

```sh
# A：配置并构建 RPC 基线。
cmake -S "$REPO/eRPC" -B "$REPO/eRPC/cmake-build-social-ub-rpc" \
  -DAPP=social_network_cxl -DTRANSPORT=ub -DPERF=ON \
  -DERPC_SN_DOMAIN_PROXY=OFF -DERPC_SN_DOMAIN_BACKEND=ub \
  -DUBSM_INCLUDE_DIR="$UBSM_INCLUDE_DIR" -DUBSM_LIBRARY="$UBSM_LIBRARY"
cmake --build "$REPO/eRPC/cmake-build-social-ub-rpc" -j2 \
  --target erpc_ub_manager client user_timeline
# B：基线才需要真正的 nginx/load_balance 进程。
cmake --build "$REPO/eRPC/cmake-build-social-ub-rpc" -j2 \
  --target erpc_ub_manager post_storage nginx load_balance
```

`BACKEND=ub` 让 manager 和应用都输出到上述构建目录，不覆盖 `eRPC/build`。
A/B 启动前用 `ldd` 检查各自二进制，不能有 `not found`。B 不需要内核模块或 OBMM 开发头。

## 3. 不依赖数据库的路由探针

两端使用同一个**未被其他实验使用**的区域名称，例如 `lrpc_social_lab_1017`。
名称只供本轮示例，下一轮并行实验须换名。B 先运行并保持存活：

```sh
cd "$REPO/eRPC-LRPC-UB"
export LRPC_UB_DOMAIN_NAME=lrpc_social_lab_1017
export LRPC_UB_PROVIDER_NUMA=1
sh scripts/run-social.sh --side B --mode domain
```

等到 `UB_DOMAIN_OWNER_READY ... profile=social-user-read`。A 在允许的 NUMA0 CPU 上：

```sh
cd "$REPO/eRPC-LRPC-UB"
sudo env LRPC_UB_DOMAIN_NAME=lrpc_social_lab_1017 \
  LRPC_UBSM_SDK_MODE=owned LRPC_UB_NESTED_PATH=legacy \
  taskset -c 1 ./build-caller/lrpc-ub-social-probe --samples 1
```

自定义动态库目录时给 `sudo env` 显式传入实际 `LD_LIBRARY_PATH`。
应有 `UB_SOCIAL_ROUTE_PASS ... entries_per_chain=2 transitions_per_chain=4`。
探针仅验证不透明合成 handle 在路由中保持原样，不读取帖子或数据库。
通过后可保留 B owner，再运行应用。旧微测试 owner 不兼容，错误模式会在建连时拒绝，
不会悄悄退回原 RPC，也不要对 `--social` owner 运行算术 Direct/Nested。

## 4. 生成私有应用配置

以 A 上已经跑通 UB 的 JSON 为输入。脚本复制数据，不覆盖输入/已有输出目录，
将 Client/Timeline 地址指向 A，PostStorage/LB/nginx 指向 B。
所有 `*_mongodb` 默认原样保留，仅指定 `--timeline-db-host/port` 或 `--storage-db-host/port`
时修改对应的应用连接配置；不会迁移数据、修改 mongod 配置或重启数据库。

**注意原配置的 Timeline MongoDB 是 `127.0.0.1:20012`。** 若它仍在 B，A 的 localhost
不是 B！可以使用已经可达的 DB 地址；若现有 mongod 只监听 B 本机，可在 A 单独终端建立
仅本机监听的 SSH 隧道（保持该终端存活，两种模式复用同一隧道）：

```sh
# A：UB_B_SSH 替换为有权限的 B 用户@主机；无需更改 B 的 mongod bind 地址。
ssh -N -T -o ExitOnForwardFailure=yes \
  -L 127.0.0.1:22012:127.0.0.1:20012 UB_B_SSH
```

下面的配置生成命令采用此隧道。若 A 已有正确可达的 DB，则改用对应 host/port。
PostStorage 仍在 B，原 B 本机 `127.0.0.1:20014` 无需改变。

```sh
cd "$REPO/eRPC-LRPC-UB"
python3 scripts/prepare-social.py \
  --input "$REPO/eRPC/apps/social_network_cxl/config/config.ub98.json" \
  --output "$REPO/eRPC-LRPC-UB/campaign-user-read" \
  --host-a REAL_A_IPV4 --host-b REAL_B_IPV4 --numa-a 0 --numa-b 1 --samples 10 \
  --timeline-db-host 127.0.0.1 --timeline-db-port 22012
```

在两端使用同一次生成的文件：A 用 `config-a.json`，B 两种模式都用 `config-b.json`。
如样本配置仍有 `IP98` 等数据库占位符，先准备一份填好真实数据库设置的私有输入文件。
默认单 lane 时 A Client/Timeline 的 NUMA 内 CPU 偏移分别为 `0..2`、`3..5`；
B nginx/LB/PostStorage 偏移分别为 `0..2`、`3..5`、`6..8`。
PostStorage 在两种模式都保留 `6..8`，不因域模式少跑两个服务而移动 CPU。
检查这些 CPU 都在进程允许的 affinity 内且未占用。
启动器会在创建任何应用 region 前，读取实际 NUMA CPU 列表和当前 affinity 做检查；
若某个偏移对应被排除的 CPU，会拒绝启动，按报错修改私有 JSON 的该服务 `bind_core_offset`，
保持不同服务的三 CPU 预留块不重叠。两种模式使用同一组调整后的配置。
`--numa-a/b` 是计算 NUMA；B owner provider 使用单独的 `LRPC_UB_PROVIDER_NUMA`。
日志中的源 `rpc_id` 不等于 Linux CPU 号。

## 5. 双机运行少量端到端请求

先确认原 MongoDB 和 A 的 DB 连接/隧道可用，保持既有数据，不使用 VM fixture，不重置数据库。
B 的 `--social` owner 继续存活。**另开 B 终端**运行 PostStorage 和 manager：

```sh
export REPO=/absolute/path/ub-application
export ERPC_UB_MACHINE_ID=98
export ERPC_UB_PROVIDER_NUMA=1 ERPC_UB_NUMA_NODE=1
export ERPC_UB_MEMORY_MODE=one-sided ERPC_UB_REGION_MB=2048 ERPC_UB_ARENA_MB=64
export ERPC_UB_REGION_PREFIX=sn_lrpc_lab_1017
export ERPC_UB_MANAGER_SOCKET=/tmp/sn_lrpc_lab_1017_98.sock
export LRPC_UB_DOMAIN_NAME=lrpc_social_lab_1017
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-domain-b"
# 按实际依赖设置 LD_LIBRARY_PATH；默认使用 cmake-build-social-ub-rpc。
cd "$REPO/eRPC-LRPC-UB"
sh scripts/run-social.sh --side B --mode domain \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-read/config-b.json"
```

紧接着启动 A，避免 PostStorage 建连等待超时。A 以具有 CAP_SYS_RAWIO 的身份执行；
下例在 A 的 root 实验 shell 中设置环境，不依赖 sudo 是否保留变量。
98/99 仅为不同的非零 ID 示例，按实际部署保持一致；不能让两端使用同一 ID。

```sh
# 在 A 的实验终端中；REPO 和所有路径使用真实值。
export REPO=/absolute/path/ub-application
export ERPC_UB_MACHINE_ID=99
export ERPC_UB_PROVIDER_NUMA=0
export ERPC_UB_NUMA_NODE=0
export ERPC_UB_REGION_PREFIX=sn_lrpc_lab_1017
export ERPC_UB_MANAGER_SOCKET=/tmp/sn_lrpc_lab_1017_99.sock
export ERPC_UB_MEMORY_MODE=one-sided ERPC_UB_REGION_MB=2048 ERPC_UB_ARENA_MB=64
export LRPC_UB_DOMAIN_NAME=lrpc_social_lab_1017
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-domain-a"
# 非标准动态库路径时，另外 export LD_LIBRARY_PATH=实际的 SDK/OBMM/应用依赖目录
cd "$REPO/eRPC-LRPC-UB"
sh scripts/run-social.sh --side A --mode domain \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-read/config-a.json"
```

脚本复用原 `run_ub.sh`，启动 A 的 manager、UserTimeline、Client；B 启动 PostStorage/manager，
但不启动普通 LB/nginx。B 无 `--config` 的命令仅启动 owner，有 `--config` 才启动应用，
这两个 B 终端缺一不可。B PostStorage 在两种模式使用相同 RPC-only 二进制和就绪握手，
不创建域连接。不要在整个 A 启动命令外加 `taskset -c 1`，各线程已有不同绑核。
SDK 在 `AppRpc` 构造时由 eRPC 初始化，LRPC 使用 `external` 模式；域连接在线程退出、
`AppRpc` 析构前关闭，避免重复 initialize/finalize。

另开 A 终端查看：

```sh
grep -E 'SN_DOMAIN_READY|SN_DOMAIN_ERROR|SN_DOMAIN_E2E|SN_DOMAIN_CLASS' \
  "$REPO/eRPC-LRPC-UB/results/social-domain-a/client.log" \
  "$REPO/eRPC-LRPC-UB/results/social-domain-a/user_timeline.log"
```

应有两个 `SN_DOMAIN_READY ... backend=ub`，slot 和 identity 不同。
Client 最终应有 `SN_DOMAIN_E2E ... completed=10 expected=10 clock=steady_ns scope=call-to-consume`。
原程序没有名为 `SN_DOMAIN_E2E_PASS` 的输出。Client 完成后 A 启动终端可能仍在等待长期服务，
在 **A** 按 Ctrl-C 让 Timeline/manager 正常结束，再停止 B 的 PostStorage/manager，
最后才停止 B owner。不要在 A 连接仍存活时结束 owner。
将本轮 B `social-domain-b/` 日志复制到 A 的同名本地目录（或使用已有只读共享目录），
下例 `--peer-logs` 是 **A 可读的路径**，并不自动访问 B：

```sh
python3 scripts/check-social.py --logs "$REPO/eRPC-LRPC-UB/results/social-domain-a" \
  --peer-logs "$REPO/eRPC-LRPC-UB/results/social-domain-b" --mode domain --samples 10
sudo dmesg | grep -iE 'ub_domain|BUG:|Oops|panic|lockup|RCU.*stall|SError|Unhandled' | tail -80
```

验收脚本检查完成数量、测量口径、两个域连接、每端调用次数和关闭标志，
只在这些检查后打印 `UB_SOCIAL_E2E_PASS ... kernel_health_checked=0`。
它不代替 dmesg 或真实数据正确性校验；日志错误需结合本次时间窗口判断是否属于本实验。
`SN_DOMAIN_E2E` 中 nonempty/empty 是历史 fixture 计数，`fixture=0` 时不能用它判断真实结果；
实际空/非空数量看 `SN_DOMAIN_CLASS phase=measured class=nonempty|empty count=...`。
全为空可以验证空结果返回，但不能宣称完整帖子消费已验证；使用既有非空数据再测。

真实 UB 数据模式固定 `ERPC_SN_DOMAIN_FIXTURE=0`，允许测量合法空结果和普通帖子；
合成帖子的逐字段 fixture 校验只在显式开启该开关时执行，RPC 与 domain 路径一致。
`SN_DOMAIN_CLASS` 的 `fixture_validation_included` 随实际开关报告，不再固定为 1。
MongoDB 游标读取失败会输出 `SN_MONGODB_ERROR` 并终止对应服务，不能把连接失败当成空库；
成功读取的空集合仍合法。确认没有此错误，并检查两端的加载数量，再判断空结果是否合理。
NUMA 2/3 也使用按节点独立的辅助绑核计数器和实际 CPU 数，仍需通过 affinity 检查；
辅助 leader/数据库线程不属于数据面三 CPU 预留块，启动时也要检查它们的实际绑核。

## 6. 相同服务位置的原 UB-eRPC 对照

先按上一节顺序退出域实验。两端使用第 2 节 `PROXY=OFF, BACKEND=ub` 的
`cmake-build-social-ub-rpc` 产物，不使用域二进制冒充独立构建基线。
**原样复用同一对 config-a/b.json**：A 仍是 Client/Timeline，B 仍是 PostStorage，
只有 LB/nginx 从 B 发布的路由域改为 B 上真实 eRPC 进程。没有重新搬动数据库或帖子。
同一 prefix、machine ID、NUMA、region/arena 和动态库设置均沿用上一节。
两端不同时运行两种模式，使用新日志目录：

```sh
# B（其应用依赖和 ldd 已检查）
export REPO=/absolute/path/ub-application
export ERPC_UB_MACHINE_ID=98
export ERPC_UB_PROVIDER_NUMA=1
export ERPC_UB_NUMA_NODE=1
export ERPC_UB_REGION_PREFIX=sn_lrpc_lab_1017
export ERPC_UB_MANAGER_SOCKET=/tmp/sn_lrpc_lab_1017_98.sock
export ERPC_UB_MEMORY_MODE=one-sided ERPC_UB_REGION_MB=2048 ERPC_UB_ARENA_MB=64
export SN_BUILD_DIR="$REPO/eRPC/cmake-build-social-ub-rpc"
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-rpc-b"
cd "$REPO/eRPC-LRPC-UB"
sh scripts/run-social.sh --side B --mode rpc \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-read/config-b.json"

# A（另一个终端，保留前述 A 的 machine ID/provider/prefix）
export SN_BUILD_DIR="$REPO/eRPC/cmake-build-social-ub-rpc"
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-rpc-a"
sh scripts/run-social.sh --side A --mode rpc \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-read/config-a.json"
```

B 启动后紧接着启动 A；基线 B 会启动 PostStorage、nginx、LB 和 manager，
A 只启动 Timeline、Client 和 manager。Client 完成后正常结束 A，再结束 B。
把本轮 B `social-rpc-b/` 日志复制到 A 可读目录，再在 A 验收：

```sh
python3 scripts/check-social.py --logs "$REPO/eRPC-LRPC-UB/results/social-rpc-a" \
  --peer-logs "$REPO/eRPC-LRPC-UB/results/social-rpc-b" --mode rpc --samples 10
```

基线关闭 `ERPC_SN_DOMAIN_PROXY`，启用已有的 `ERPC_SN_RPC_BASELINE` 测量路径；
仍经过真实 LB/nginx、相同位置的 Timeline/Storage、远端 handle 消费、数据和计算侧时钟。
两条路径都计入 Timeline→B PostStorage 请求和 A 的帖子正文消费；引用释放都在计时外。
先确认两边请求数/空非空分类合理，再比较 `SN_DOMAIN_CLASS phase=measured class=all`
以及 `SN_DOMAIN_E2E` 的延迟。10 个样本只是启动验证，不是性能结论；后续重新生成
`--samples 1000` 的私有配置，可在两条路径都设置 `ERPC_SN_DOMAIN_WARMUP=10`。
固定 CPU/NUMA、数据库状态、数据量、内存模式、region/arena 容量和 profile 开关，交替多轮运行。
当前两条路径都按 `i % 962` 生成相同的用户读取序列；同样的 `generate_num`、预热和
不变的数据集应保持结果分类一致。若空/非空比例不同，先查数据或运行条件，不把差异当成域切换收益。

```sh
grep -E 'SN_DOMAIN_E2E |SN_DOMAIN_MEASUREMENT |SN_DOMAIN_CLASS ' \
  "$REPO/eRPC-LRPC-UB/results/social-domain-a/client.log" \
  "$REPO/eRPC-LRPC-UB/results/social-rpc-a/client.log"
```

这仍是受控、相同应用算法的 UB-eRPC 对照，不是原生 DeathStarBench gRPC 基线。
下一轮回到 domain 模式时，取消基线留下的 `SN_BUILD_DIR` 或显式设回 A 的 `-domain` 目录；
启动新的 B owner，重新建立域连接，不复用已经退出的连接。

## 7. 安全边界与本地验证

不重启共享机器、UB 服务、数据库；不自动加载/卸载模块，不自动清理别人的共享区域。
启动脚本只管理自己启动的进程。私有 manager socket/prefix/log 目录必须与其他实验隔离。
原应用默认的 2 GiB region / 64 MiB endpoint arena 仍然有效，少量请求不代表 MongoDB
加载全部 PostData 的容量变小；按原 README 确认容量。性能模式不要启用 trace/profile。
连接准备失败的槽位仍遵循隔离规则，所有 A 连接退出后再处理模块/owner，不能在线清池。

```sh
python3 -m unittest discover -s tests -p 'test_social.py' -v
python3 -m unittest discover -s tests -p 'test_social_application.py' -v
python3 -m unittest discover -s tests -p 'test_multi.py' -v
python3 -m unittest discover -s tests -p 'test_nested_client.py' -v
python3 -m unittest discover -s tests -p 'test_nested_runtime.py' -v
# 可选 Unicorn 2.x 指令测试，用 AArch64 编译器生成临时对象：
TEST_OBJECTS=$(mktemp -d)
cc -c kernel/ub-domain/social_services.S -o "$TEST_OBJECTS/social_services.o"
python3 tests/test_social_services.py --object "$TEST_OBJECTS/social_services.o"
```

本地测试使用实际适配层、内核路由分发函数、publish/prepare/commit 代码或服务指令，
但 SDK、异常入口、页表/ASID、调度和业务网络存在 fixture，不能替代 UB 端到端实测。
x86 的细粒度 kernel profile 和 fused 算术 Nested 不会被冒充成应用 profile；
首版 UB 应用拒绝这些不兼容选项，只支持已有应用级 profile。

`test_social_application.py` 编译实际应用中的测量条件、RPC 返回校验、数据库失败检查和
NUMA 分配函数，补足 adapter 测试未覆盖的入口策略。DB/网络仍是 fixture，不代表完整应用启动。
本次清理删除了 UB 客户端无读取者的约 60 KiB scratch、旧 UBSM 服务发布/用户态代码本地化
实现和未实现的公开函数声明；当前 domain 的 `UD_COMMIT` 代码复制/白名单、页表发布、
EL1/VHE gate、mmap A-stack 均未改变。不要混用两套目录的 `lrpc.h` 或旧用户态静态库。
更新后重新编译 A 的 caller、domain/RPC 应用和 B 的 owner/RPC 应用（第 2 节命令）；
**本次审查修复未修改内核模块**，若已加载带 social 支持的匹配模块，无需为这些用户态修复重载。
不需要重启 MongoDB、UB 服务或机器。

本轮源码范围：

- `eRPC-LRPC-UB/include/`：`lrpc/ub_social.h`、`lrpc/ub_social_client.h`、
  `lrpc/ub_domain_services.h`、`uapi/linux/ub_lrpc_ub_domain.h`。
- `eRPC-LRPC-UB/kernel/ub-domain/`：`domain.c`、`Makefile`、`social_services.S`。
- `eRPC-LRPC-UB/lib/`：`ub_domain_client.c`、`ub_domain_pool.c`；
  `include/lrpc/ub_domain_pool.h`；`demo/ub/domain_owner.c`、`social_probe.c`；`CMakeLists.txt`。
- `eRPC-LRPC-UB/scripts/`：`prepare-social.py`、`run-social.sh`、`check-social.py`、`check-social-cpus.py`。
- `eRPC-LRPC-UB/tests/`：`test_social.py`、`test_social_services.py`、`test_multi.py`、
  `test_nested_runtime.py`。
- `eRPC-LRPC-UB/README.md`、本说明、`SOURCE_MANIFEST.sha256`。
- `eRPC/CMakeLists.txt`、`eRPC/apps/social_network_cxl/domain_proxy_runtime.h`、
  `domain_proxy.h`、`README_UB.md`。未修改 eRPC transport、数据库内容或帖子 ownership 规则。

审查修复额外涉及：本目录 `include/lrpc/lrpc.h`、`include/lrpc/lrpc_ubsm.h`、
`lib/backend_ubsm.c`、`lib/ub_domain_client.c`、`scripts/check-social.py`、
`tests/test_multi.py`、`tests/test_social.py`、`tests/test_social_application.py`、源码清单；
应用目录 `client/client.h`、`client/client.cpp`、`domain_measure.h`、`social_network_commons.h`、
`utils_mongodb.h`、`user_timeline/user_timeline.cpp`、`post_storage/post_storage.cpp`。
