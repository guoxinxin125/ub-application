# UserTimeline read：AArch64 UB 路由域接入

这是可选应用路径，保留普通 UB-eRPC、x86/QEMU 接入和 Direct/Nested 微测试。
legacy 应用链已由用户在双机完成少量端到端验证；新增 fused 应用链目前仅完成本地
C 层 fixture，**尚未在目标 AArch64 内核构建、装载或双机验证**。先跑独立路由探针，
再跑少量端到端请求。
不需要修改启动参数、重启机器、重启 UB 服务或使用 Docker。

## 1. 第一版范围与摆放

只支持一个 Client、一个发送 lane、并发 1、一个 nginx 域、UserTimeline read。
不包括 HomeTimeline、发帖、多 nginx、UB 业务写入。
算术 Nested 微测试的默认路径已改为 fused；本应用 profile 在未指定路径时仍使用
独立的 legacy 路由链。显式设置 `LRPC_UB_NESTED_PATH=fused` 才选择应用 fused。
域调用仍使用多连接池：Client 前向一个连接，Timeline 回程一个连接，同属 A 内核，
不同 slot/identity/ASID。这里的单 lane 限制只针对首版应用，不取消微测试的多连接能力。

| 机器 | Domain 模式 | 原 UB-eRPC 基线 |
| --- | --- | --- |
| A，NUMA 0 | Client、UserTimeline、A manager | 同左 |
| B，provider NUMA 1 | PostStorage、B manager；另一个 `--social` owner 发布 LB/nginx 代码、配置、页表 | 同位置的 PostStorage、B manager，以及真正的 LB/nginx eRPC 进程 |
| 数据库（本轮推荐布局） | Timeline/Post 两个 MongoDB 都在 B；A 的 Timeline 通过连接或隧道加载数据 | 使用完全相同的数据库和数据 |

**A 是有 Client/UserTimeline 的机器，B 是 LB/nginx 的服务域发布方和 PostStorage 所在机器。**
这里不使用旧机器编号或旧的 `config.ub*.json`。本包提供独立 A/B 模板，见第 4 节；
NUMA 和 CPU 按实际分配调整。若现成 MongoDB 位于其他主机，保留它并显式配置可达地址即可，
数据库没有必须跟随 Client 或服务域摆放的限制。

PostData 由 B 的 PostStorage 分配在 B 的 UB arena，两种模式都由 A 消费同一布局的 handle。
使用 `one-sided` 模式，确认实际共享内存导入属性和 handle 的 machine ID；不能仅凭角色表
宣称 NC 或远端访问性能已验证。LRPC 额外的远端访问是 B 配置及 TLB miss 时的 B 页表。

逻辑调用保留 QEMU 应用适配的语义，而不是复制整个 C++ handler：

```text
前向：Client → LB 域 → nginx 域 → A 的正常应用上下文 → UserTimeline → PostStorage
回程：PostStorage → UserTimeline → nginx 域 → LB 域 → A 的正常应用上下文 → Client
```

上面是逻辑服务顺序。**域模式下整条 LB/nginx 路由链都在 A 的 CPU 上执行**；
“B 的域”表示 B 发布并持有域的页表、只读配置，不表示调用时改用 B 的 CPU。
代码在连接时从 B 复制到 A，A-stack/E-stack 也在 A。legacy 在两个域之间恢复
A 的 Linux 内核环境；fused 由 A 的 gate 直接交接到下一域：

```text
legacy：A → LB(B域，A CPU) → 恢复A → nginx(B域，A CPU) → 恢复A
fused：A → LB(B域，A CPU) → gate → nginx(B域，A CPU) → 恢复A
回程域顺序：nginx-return → LB-return，仍由 A CPU 执行
```

因此改用 fused 不需要把 LB/nginx 的 owner 搬到 A。A→B域是访问远端服务地址空间，
B域→B域是 A CPU 上两个 B 发布域之间的切换；它们都可能访问 B 配置及 TLB miss 后的
B 页表，不发生 CPU 跨机迁移，也不保证每次切换都会触发远端 page walk。
域调用完成后，Client→UserTimeline 是 A 上的普通进程间 UB-eRPC；
UserTimeline(A)→PostStorage(B) 才是由两台机器各自 CPU 处理的跨机 UB-eRPC。
RPC 基线则是在 B 真正运行 LB/nginx 进程，与域模式的“B 发布、A 执行”有所区别。

每个前向/回程链有两个实际域入口：legacy 有 4 次逻辑域切换，fused 有 3 次。
fused 复用 A 的 nested gate 做两域交接，第二域直接返回 A；没有算术微测试的
middle→leaf→middle 恢复阶段。对每次调用，gate 在进入第二域前验证第一域的
BRK/PC、action/target；A 内核返回后仍验证最终消息及切换计数。
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
# 新模块应输出 user-read-abi1-single-nginx-legacy,fused
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
若接着使用第 5 节单终端启动器，先退出此探针 owner；启动器会创建新的同名 owner。
旧微测试 owner 不兼容，错误模式会在建连时拒绝，
不会悄悄退回原 RPC，也不要对 `--social` owner 运行算术 Direct/Nested。

## 4. 生成私有应用配置

本轮使用两份独立模板，不修改原应用配置：

- `config/social-user-read-a.json`：A 的 Client/Timeline，计算 NUMA 默认 0。
- `config/social-user-read-b.json`：B 的 PostStorage/manager，以及 RPC 基线 LB/nginx，计算 NUMA 默认 1。

模板中的 `A_IP/B_IP` 必须替换成实际 IPv4 地址后才能用于运行。推荐通过下面的生成脚本
生成私有 `config-a.json/config-b.json`，它默认分别读取本包独立 A/B 模板，并应用
`--numa-a/b`。原模板和旧应用 JSON 都不修改，已有输出目录也不会覆盖。
保留未运行服务的地址项是为了现有应用的配置解析；启动器只启动本轮所需的服务。

### MongoDB 的放置和用途

这轮 UserTimeline read 只需要两组已有数据，**推荐都放 B**：

| 数据 | B 上 MongoDB 端口 | 实际读取它的应用 | 模板中的连接地址 |
| --- | --- | --- | --- |
| `user-timeline.user-timeline`，用户到帖子 ID 的关系 | 20012 | A 的 UserTimeline，启动时加载到 A 内存 | A 的 `127.0.0.1:22012`，通过 SSH 隧道转到 B:20012 |
| `post.post`，帖子内容 | 20014 | B 的 PostStorage，启动时加载到 B 的 UB arena | B 的 `127.0.0.1:20014` |

**MongoDB 存放位置与 UB 远端内存位置是两件事。** 当前只读路径启动时加载 MongoDB，
每次读请求主要查询已加载的内存索引和 PostData，不是每次请求都查询数据库。
本轮不启动 UserMention 等服务，不要求额外启动 user/social-graph 数据库。
两份 JSON 保留相同的 DB 键，但 A 只使用 `user_timeline_mongodb`，B 只使用
`post_storage_mongodb`；B 模板的 Timeline 项指向 B 本地 20012，但本轮不在 B 启动 Timeline。
两个集合应来自匹配数据集，Timeline 引用的帖子必须在 Post 数据中。

若已经有可用数据库，不搬迁或重新导入，只通过下面的 override 参数指向它。
若在新机器上还没有数据，按原 [README 的 MongoDB 数据准备步骤](../eRPC/apps/social_network_cxl/README_UB.md#3-start-mongodb-on-machine-98)
由操作者将所需两组数据准备到选定数据库主机；该旧文中的物理机器编号不适用于本轮角色。
配置脚本不会启动 MongoDB、恢复数据、修改监听地址或重启服务。

B 的数据库若只监听 localhost，A 需要本地 22012→B:20012 的 SSH 隧道。
若先前的登录终端断线，**不要猜测隧道是否仍在运行**，在 A 检查：

```sh
ss -ltnp '( sport = :22012 )'
# 若 A 装有 mongosh，再做数据库端到端检查：
mongosh --quiet --host 127.0.0.1 --port 22012 --eval 'db.adminCommand({ping:1}).ok'
```

`ss` 只证明本地有监听者；`mongosh` 成功才说明当前能经过它到达数据库。
第 5 节单终端启动器可用 `--ssh-b B用户@B主机` 自动建立并在退出时关闭其**自己创建的**
隧道。如果已有可用隧道，不加 `--ssh-b`，启动器不会碰现有进程。默认模板已采用
本地隧道地址，不需要更改 B 的 mongod bind 地址。然后在 A 生成配置：

```sh
cd "$REPO/eRPC-LRPC-UB"
python3 scripts/prepare-social.py \
  --output "$REPO/eRPC-LRPC-UB/campaign-user-read" \
  --host-a REAL_A_IPV4 --host-b REAL_B_IPV4 --numa-a 0 --numa-b 1 --samples 10
```

在两端使用同一次生成的文件：A 用 `config-a.json`，B 两种模式都用 `config-b.json`。
如果 Timeline 数据库可从 A 直接访问，不使用隧道，生成时加
`--timeline-db-host ACTUAL_DB_HOST --timeline-db-port 20012`；如果 Post 数据库不在 B 本地，
再加 `--storage-db-host ACTUAL_DB_HOST --storage-db-port 20014`。这些是应用视角的 DB 地址，
不是 LB/nginx 域地址。需要继承其他已有私有数据配置时仍可显式指定 `--input /path/to/private.json`。
默认单 lane 时 A Client/Timeline 的 NUMA 内 CPU 偏移分别为 `0..2`、`3..5`；
B nginx/LB/PostStorage 偏移分别为 `0..2`、`3..5`、`6..8`。
PostStorage 在两种模式都保留 `6..8`，不因域模式少跑两个服务而移动 CPU。
检查这些 CPU 都在进程允许的 affinity 内且服务预留块互不重叠。
启动器会在创建任何应用 region 前，读取实际 NUMA CPU 列表和当前 affinity 做检查；
若某个偏移对应被排除的 CPU，会拒绝启动，按报错修改私有 JSON 的该服务 `bind_core_offset`；
例如 B NUMA1 的可用索引从 13 开始时，可在私有 B 配置中给 nginx/LB/PostStorage 设
`13/16/19`（各占三项），
保持不同服务的三 CPU 预留块不重叠。两种模式使用同一组调整后的配置。
`--numa-a/b` 是计算 NUMA；B owner provider 使用单独的 `LRPC_UB_PROVIDER_NUMA`。
日志中的源 `rpc_id` 不等于 Linux CPU 号。

## 5. 双机运行少量端到端请求

先确认原 MongoDB 和 A 的 DB 连接/隧道可用，保持既有数据，不使用 VM fixture，不重置数据库。
**每台机器只需一个实验终端**。B 的新会话脚本先启动 `--social` owner、等 READY，
再启动 PostStorage/manager；结束时反序关闭。若之前手动启动了同名 owner，先按正常
退出顺序结束旧实验，避免重复发布同名共享域：

```sh
export REPO=/absolute/path/ub-application
export ERPC_UB_MACHINE_ID=2
export ERPC_UB_PROVIDER_NUMA=1 ERPC_UB_NUMA_NODE=1
export ERPC_UB_MEMORY_MODE=one-sided ERPC_UB_REGION_MB=2048 ERPC_UB_ARENA_MB=64
export ERPC_UB_REGION_PREFIX=sn_lrpc_lab_1017
export ERPC_UB_MANAGER_SOCKET=/tmp/sn_lrpc_lab_1017_b.sock
export LRPC_UB_DOMAIN_NAME=lrpc_social_lab_1017
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-domain-b"
# 按实际依赖设置 LD_LIBRARY_PATH；默认使用 cmake-build-social-ub-rpc。
cd "$REPO/eRPC-LRPC-UB"
bash scripts/run-social-session.sh --side B --mode domain \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-read/config-b.json"
```

看到 `UB_SOCIAL_OWNER_READY` 后紧接着启动 A，避免 PostStorage 建连等待超时。
A 以具有 CAP_SYS_RAWIO 的身份执行；
下例在 A 的 root 实验 shell 中设置环境，不依赖 sudo 是否保留变量。
这里 A/B 使用逻辑 ID 1/2，和实际主机编号无关；按既有部署需要调整，但两端必须不同且非零。

```sh
# 在 A 的实验终端中；REPO 和所有路径使用真实值。
export REPO=/absolute/path/ub-application
export ERPC_UB_MACHINE_ID=1
export ERPC_UB_PROVIDER_NUMA=0
export ERPC_UB_NUMA_NODE=0
export ERPC_UB_REGION_PREFIX=sn_lrpc_lab_1017
export ERPC_UB_MANAGER_SOCKET=/tmp/sn_lrpc_lab_1017_a.sock
export ERPC_UB_MEMORY_MODE=one-sided ERPC_UB_REGION_MB=2048 ERPC_UB_ARENA_MB=64
export LRPC_UB_DOMAIN_NAME=lrpc_social_lab_1017
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-domain-a"
# 非标准动态库路径时，另外 export LD_LIBRARY_PATH=实际的 SDK/OBMM/应用依赖目录
cd "$REPO/eRPC-LRPC-UB"
bash scripts/run-social-session.sh --side A --mode domain \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-read/config-a.json" \
  --ssh-b UB_B_SSH
```

`UB_B_SSH` 替换成有权限的 B 用户@主机。`--ssh-b` 会在当前 A 执行身份下进行 SSH 认证，
建立私有控制 socket 和本机 22012 转发；若该身份没有 B 的 SSH 凭据，可由普通用户
预先建立隧道，确认 22012 可用后**省略** `--ssh-b`。若已有隧道占用 22012，
不要同时指定 `--ssh-b`。它仅清理自己创建的隧道，不会终止既有 SSH 进程。
脚本复用原 `run_ub.sh`，启动 A 的 manager、UserTimeline、Client；B 同终端管理
owner 和 PostStorage/manager，但不启动普通 LB/nginx。B PostStorage 在两种模式
使用相同 RPC-only 二进制和就绪握手，
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
原程序没有名为 `SN_DOMAIN_E2E_PASS` 的输出。`check-social.py` 的 PASS 行只验收计数，
延迟在 A 的 `client.log`：`SN_DOMAIN_E2E` 和 `SN_DOMAIN_CLASS phase=measured class=all` 的
`p50_ns/p99_ns/avg_ns`。Client 完成后 A 启动终端可能仍在等待长期服务；
先在 **A** 按 Ctrl-C，等 Timeline/manager 及本会话创建的隧道退出；再在 **B** 按 Ctrl-C，
脚本会先结束 PostStorage/manager，最后结束 owner。不要在 A 连接仍存活时结束 B。
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

### 应用 fused 路由：先小样本验证

只有 A 的新模块和域客户端/应用需要重新构建；B 的 `--social` owner 发布格式和
PostStorage 二进制没有变化。按第 2 节先结束旧连接，再在 A 构建、检查并换成本次
`ud_social=user-read-abi1-single-nginx-legacy,fused` 模块；VHE/EL1 仍按各自原有步骤，
不要更改机器启动配置。B 按第 5 节同一命令启动新会话。A 显式选择 fused：

```sh
export LRPC_UB_NESTED_PATH=fused
bash scripts/run-social-session.sh --side A --mode domain \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-read/config-a.json" \
  --ssh-b UB_B_SSH
```

若 A 已有可用的 22012 隧道，省略 `--ssh-b`。先确认 Client 和 Timeline 都报告
`SN_DOMAIN_READY ... chain=fused entries=2 transitions=3`；结束顺序仍是 A 后 B。
两端日志收齐后在 A 验收：

```sh
python3 scripts/check-social.py \
  --logs "$REPO/eRPC-LRPC-UB/results/social-domain-a" \
  --peer-logs "$REPO/eRPC-LRPC-UB/results/social-domain-b" \
  --mode domain --chain fused --samples 10
```

若先做无数据库的 fused 路由探针，B 可单独启动第 3 节不带 `--config` 的 owner，
A 在第 3 节探针命令中把 `LRPC_UB_NESTED_PATH=legacy` 改为 `fused`，确认
`UB_SOCIAL_ROUTE_PASS ... chain=fused transitions_per_chain=3`。结束探针后先停止
探针 owner，再用上面的单终端启动器重新发布同名域。

对照延迟时，在**同一新版 A 模块**上分别用显式 `legacy` 和 `fused` 跑独立轮次，
相同 A/B 配置、数据、CPU、warmup 和样本数，并分别保存日志。legacy 每链 4 次切换，
fused 每链 3 次；这不等于真实延迟必然降低。新增汇编 gate 尚待 AArch64 编译和双机验证，
出现 fault/内核异常时停止实验并保留本轮日志，不自动退回 legacy。

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
export ERPC_UB_MACHINE_ID=2
export ERPC_UB_PROVIDER_NUMA=1
export ERPC_UB_NUMA_NODE=1
export ERPC_UB_REGION_PREFIX=sn_lrpc_lab_1017
export ERPC_UB_MANAGER_SOCKET=/tmp/sn_lrpc_lab_1017_b.sock
export ERPC_UB_MEMORY_MODE=one-sided ERPC_UB_REGION_MB=2048 ERPC_UB_ARENA_MB=64
export SN_BUILD_DIR="$REPO/eRPC/cmake-build-social-ub-rpc"
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-rpc-b"
cd "$REPO/eRPC-LRPC-UB"
bash scripts/run-social-session.sh --side B --mode rpc \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-read/config-b.json"

# A（另一个终端，保留前述 A 的 machine ID/provider/prefix）
export SN_BUILD_DIR="$REPO/eRPC/cmake-build-social-ub-rpc"
export SN_LOG_DIR="$REPO/eRPC-LRPC-UB/results/social-rpc-a"
bash scripts/run-social-session.sh --side A --mode rpc \
  --config "$REPO/eRPC-LRPC-UB/campaign-user-read/config-a.json" \
  --ssh-b UB_B_SSH
```

B 启动后紧接着启动 A；基线 B 会启动 PostStorage、nginx、LB 和 manager，
A 启动 Timeline、Client、manager 和可选隧道。已有可用隧道时省略 `--ssh-b`；
Client 完成后正常结束 A，再结束 B。RPC 模式不启动 domain owner。
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
此前的审查清理删除了 UB 客户端无读取者的约 60 KiB scratch、旧 UBSM 服务发布/用户态
代码本地化实现和未实现的公开函数声明；仍不要混用两套目录的 `lrpc.h` 或旧用户态静态库。
**这次 fused 接入修改了 A 的 EL1/VHE nested gate 和 social 分发代码**，因此运行 fused
前必须在 A 重新编译并更换匹配模块及域应用（第 2 节命令）；只修改脚本或继续跑已验证的
legacy/RPC 时不需要因此重启机器、MongoDB 或 UB 服务。B 的 owner 发布格式未变。

本轮源码范围：

- `eRPC-LRPC-UB/include/`：`lrpc/ub_social.h`、`lrpc/ub_social_client.h`、
  `lrpc/ub_domain_services.h`、`uapi/linux/ub_lrpc_ub_domain.h`。
- `eRPC-LRPC-UB/kernel/ub-domain/`：`domain.c`、`Makefile`、`social_services.S`。
- `eRPC-LRPC-UB/lib/`：`ub_domain_client.c`、`ub_domain_pool.c`；
  `include/lrpc/ub_domain_pool.h`；`demo/ub/domain_owner.c`、`social_probe.c`；`CMakeLists.txt`。
- `eRPC-LRPC-UB/scripts/`：`prepare-social.py`、`run-social.sh`、`check-social.py`、`check-social-cpus.py`。
- 单终端会话脚本：`eRPC-LRPC-UB/scripts/run-social-session.sh`。
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
