# UB-Tigon 实现与运行说明

> 更新记录：根据双机 YCSB 首次运行的实际排障经验补充了三处内容——
> (1) `UBSM_ERR_UBSE 6050`（region 创建失败）的根因分析与缩小 region 的推荐流程；
> (2) benchmark 运行结束但终端无输出的原因（glog 默认写 /tmp 文件）；
> (3) 运行统计中大量指标为 0 的逐项解释。
> 相应修改了「步骤 2」的推荐参数和「常见错误」清单。
>
> 更新记录（本轮）：把此前只记录在 `UB_TIGON_DESIGN_AND_PROGRAMMING_GUIDE.md` 中的测试入口补齐到
> 本文——新增「步骤 6：UB 与 spr4 双机公平对比」记录 `scripts/run_ub_two_node_compare.sh`、
> `scripts/parse/parse_two_node_compare.py`、spr4 侧 `tigon-spr4/scripts/run_two_node_compare.sh`
> 的用法与解析规则，以及一次 VM 生命周期只能跑一个 point 的约束；
> 在「主机与构建层测试」补上 `tests/test_ub_matrix_scripts.py`；按 matrix 脚本的实际实现更新
> `UB_TIGON_CPU_LIST` 与 `UB_NUMA_NODE` 的分工，以及步骤 3/5 的可覆盖环境变量清单。
>
> 更新记录（编译排障）：根据一次 openEuler（无可用 EPOL）实机编译的排障过程，在「编译」中新增
> 「仓库缺包时的依赖排查」——`dnf install` 因 `no match for argument` 中止整个事务的后果、
> `dnf provides` 反查包名、glog/gflags 源码安装（`WITH_UNWIND`、裸链接的路径要求）、Boost 仅需
> 头文件、以及 jemalloc 的 `libjemalloc.so` 与 `find_library` 缓存陷阱。
>
> 更新记录（双机运行排障）：根据一次 `run_ub_ycsb_matrix.sh` 首个 point 即 `SIGSEGV(@0x0)` 的排障
> 过程，在「双机验证流程」开头补上 `--servers` 每项必须带 `:port` 的前置约束，并在「常见错误」
> 增加对应条目；同时修正步骤 3 的两处示例 prefix base（原值合成后为 36 字节，会被脚本自己的 35
> 字节守卫拒掉，根本走不到运行），以及步骤 3 末尾「region 加大到 768/1024」与「20 万 key 实际只需
> 约 250–300 MiB」相矛盾的那处表述。

## 目标数据路径

每个 coordinator 拥有一个具名 UBS Memory region，并映射所有 peer 的 region。数据库 partition 的
所有者与物理 region 的 owner 完全一致。共享内存中不保存进程虚拟地址；`UBGlobalPtr` 通过
`region_id`、`generation` 和 `offset` 标识对象。

执行 point read/update 时，requester 解析 owner region 中的地址，直接对 tuple 执行 CAS/加锁，将
value 复制到事务本地 buffer 或从中写回，并在 commit/abort 时释放同一把共享锁。owner 不参与这条数
据路径。选择 `--shared_memory_backend=ub` 后，数据迁移以及原 CXL SCC bitmap/flush 路径均被禁用。

消息继续采用 copy-based 设计。发送方把序列化的小型 `Message` 复制到目标节点的 UB queue，接收方再
把它复制到本地 `Message`。UB queue 使用 acquire/release 完成发布，不执行 `clflush`、`clwb` 或
`_mm_sfence`。

## 已实现的基础组件

- `common/UBGlobalPtr.h`：固定 16-byte 的跨进程指针 ABI。
- `common/UBMemory.*`：UBSM 初始化、每 coordinator 一个 owner region、全 region 映射、
  generation/range 校验、root directory、CAS bump allocator、importer 优先 unmap 和 owner
  deallocate。
- `common/UBTuple.h`：requester-side 共享读写锁和 tuple header。
- `common/UBCpu.h`：AArch64/x86 通用的自旋等待 relax。
- `common/UBMPSCRingBuffer.*`：有界、copy-based UB MPSC queue。
- `core/UBBTreeCatalog.*`：root slot 8 中的 version-2 catalog/descriptor ABI；每个 table descriptor
  还发布一个稳定的 `+infinity` gap tuple。
- `core/UBBPlusTree.h`：位置无关的 UB B+ Tree、稳定 tuple、有序 leaf、requester-side insert、
  leaf/inner split 和 root replacement。
- `core/UBBPlusTreeAdapter.h`：UB B+ Tree 的 `ITable` 实现。
- `tests/ub_primitives_test.cpp`：host-only ABI 和锁竞争测试。
- `tests/ub_btree_host_test.cpp`：在进程内 region arena 上测试并发 insert/split、有界 range scan、
  successor/end-gap locking、tombstone 和同 key 复用；其中 10,000 次 delete/reinsert 用来确认同-key
  churn 不会继续分配 tuple。
- `tests/ub_btree_compile_test.cpp`、`tests/ub_adapter_compile_test.cpp`：完整模板和 `ITable` 编译
  检查。
- `tests/ub_tpcc_adapter_compile_test.cpp`：实例化全部 11 个 TPCC UB table/index adapter。
- `scripts/run_ub_two_node_compare.sh`：UB 侧的双机对比入口；把每个 point 的 `run_id` 确定性压缩
  成短 region prefix，复用步骤 3/5 的 matrix 脚本并输出 `TIGON_RUN_META`/`TIGON_RUN_END`。
- `scripts/parse/parse_two_node_compare.py`：只取 coordinator 0 的集群吞吐、做双侧配对检查、按重复实验汇总并计算 remote efficiency 的解析器。
- `tests/test_ub_matrix_scripts.py`：matrix/compare 脚本的 dry-run、两侧参数一致性和解析器回归测试

集成后的事务路径支持 YCSB `--query=rmw`、`insert`、`delete`、`scan` 和 `mixed`。数据库初始化会创建
`TableUBBPlusTree`，并把 owner partition 的 tuple 直接写入 owner region。本地和远程 point
read/update 使用同一个 requester-side `UBTupleHeader` 锁；远程路径不发送 migration request。

insert 先在 owner index 中预留 state-2 placeholder，commit 时发布为 state 1，abort 时转为 state-0
tombstone。delete 直接锁住 visible tuple，commit 时发布 state 0，abort 时只解锁。range transaction 会
锁住每个结果 tuple，以及第一个 visible successor；若范围到达末尾，则锁住稳定的 end-gap tuple。
requester-side insert 在持有 tree structural write lock 时必须获得同一个 successor 锁，从而防止
phantom 进入受保护范围。

TPCC 的全部 11 张 primary/secondary table 现在都使用 UB B+ Tree，包括 `customer_name_idx` 和
`order_customer`。共享 item table 只由 owner 初始化。payment/new-order/first-two/mixed 已提供分
阶段验证入口，但当前 workspace 只完成了模板和静态检查，尚不能视为已通过 UB 真机验证。

使用 `--use_ub_transport=true` 时，每个 coordinator 在自己 owner region 的 root slot 9 发布一个有
界 MPSC inbox。outgoing dispatcher 把序列化 `Message` 复制到目标 inbox，单个 incoming dispatcher
再复制到进程本地 `Message`。这种 fixed-entry transport 禁用 message grouping；单条序列化消息不得
超过 `--cxl_trans_entry_struct_size`。

B+ Tree leaf 保存 `{key, tuple_offset}`，而不是把 tuple 嵌入 leaf。移动 leaf entry 或执行 split 时
不会移动 `UBTupleHeader` 及其事务锁。当前实现先用一把 requester-side tree RW lock 保证 lookup、
ordered traversal、insert、split 和 root replacement 的正确性，后续再优化为 node-level OLC。
tuple transaction lock 独立存在，并一直持有到 commit/abort。

region allocator 当前保持单调增长。state-0 tombstone 可以在地址和 version 单调的前提下，原地复用
于相同逻辑 key。abort 的 placeholder 会保留为 tombstone；tuple 和 B+ Tree node 都不会复用于不同
key。跨-key reuse、leaf 物理删除、merge 和 root shrink 需要共享 epoch/QSBR，以避免 lookup-to-tuple-
lock ABA。

为支持 end-gap protection，B+ Tree descriptor ABI 已升级到 version 2。不要复用 version-1 tree 创
建的 prefix；升级后的每次运行都必须使用新的 region prefix。

## 两台 UB 主机的前置条件

本文使用下面的固定部署。示例 IP 和 provider hostname 必须替换为机器上真实注册的值：

| 机器 | Tigon ID | 示例 IP | 内存 NUMA node | 该 node 可用 CPU |
| --- | ---: | --- | ---: | ---: |
| 81 | 0 | `192.0.2.10` | 1 | 37 |
| 82 | 1 | `192.0.2.11` | 0 | 20 |

两台 AArch64 主机必须运行相同 Tigon revision 和相互兼容的 UBS Memory 软件栈。机器 82 是 worker
数量的瓶颈。

检查架构、服务、SDK 架构和动态依赖：

```bash
uname -m                         # 预期：aarch64
systemctl is-active ubse.service
systemctl is-active ubsmd
file /usr/local/ubs_mem/lib/libubsm_sdk.so
ldd /usr/local/ubs_mem/lib/libubsm_sdk.so
```

两个服务都必须为 active；`libubsm_sdk.so` 必须是 AArch64 库；`ldd` 不能出现 `not found`。在每个
用于编译或运行 Tigon 的 shell 中配置：

```bash
export LD_LIBRARY_PATH=/usr/local/ubs_mem/lib:/usr/lib64:${LD_LIBRARY_PATH:-}
export HCOM_CONNECTION_RETRY_TIMES=2
```

应用用户必须能够访问本机 `ubsmd` Unix socket：

```bash
id
getent group ubsmd
```

如果用户不属于该组，管理员可执行 `sudo usermod -aG ubsmd "$(id -un)"`，之后必须重新登录。共享
region 使用 `0660`，建议两台主机使用相同 UID/GID。

每个进程应显式指定自己的本地物理 provider。其值必须与 UBS Engine topology 中登记的 hostname 完全
一致，不一定等于 `hostname -f` 返回的 FQDN。

> 请把命令中的 `UB_PROVIDER_HOST` 替换成实际注册的 host name。

```bash
# 仅机器 81 / coordinator 0
export UB_PROVIDER_HOST=host-81
export UB_NUMA_NODE=1
export UB_PROVIDER_NUMA=1

# 仅机器 82 / coordinator 1
export UB_PROVIDER_HOST=host-82
export UB_NUMA_NODE=0
export UB_PROVIDER_NUMA=0
```

确认 hostname 可以解析且管理网络连通。host 0 的 TCP 端口 `18951` 用于独立 correctness test 的
barrier；两台主机的 TCP 端口 `10010` 用于 Tigon coordinator。UBS Engine/OBMM 的发现和数据通路必
须已经正常工作。

检查实际 NUMA topology：

```bash
numactl --hardware
```

`UB_PROVIDER_NUMA`、`UB_NUMA_NODE` 和 `UB_TIGON_CPU_LIST` 控制三个不同层次。
`UB_PROVIDER_NUMA` 会转换为 UBSM provider NUMA 参数，决定 coordinator-owned UB region 的位
置；`UB_NUMA_NODE` 传给 `numactl --membind=N`，决定进程本地内存的绑定位置；CPU affinity 由
`UB_TIGON_CPU_LIST` 单独控制。在当前部署中，机器 81 的前两个值均为 1，机器 82 均为 0。

matrix 脚本（`run_ub_ycsb_matrix.sh`、`run_ub_tpcc_matrix.sh` 以及调用它们的 range/compare
wrapper）按下面的组合决定实际使用的 runner：

| 设置 | runner |
| --- | --- |
| 只设 `UB_NUMA_NODE=N` | `numactl --cpunodebind=N --membind=N` |
| 同时设 `UB_NUMA_NODE=N` 与 `UB_TIGON_CPU_LIST=<list>` | `numactl --membind=N taskset -c <list>`；CPU affinity 由 CPU list 负责，`UB_NUMA_NODE` 只负责 memory bind |
| 只设 `UB_TIGON_CPU_LIST=<list>` | `taskset -c <list>` |
| 都不设 | 不调用 `numactl` 也不调用 `taskset`，线程由 OS 调度 |

`UB_TIGON_CPU_LIST` 接受 Linux CPU list 语法（如 `4-6` 或 `0,2,4`）。`run_ub_correctness.sh` 不
支持 CPU list，它只认 `UB_NUMA_NODE`（`numactl --cpunodebind=N --membind=N`）。未设置
`UB_PROVIDER_NUMA` 时由 UBS Engine 选择 provider NUMA。

`one-sided` 使用 `UBSM_FLAG_ONLY_IMPORT_NONCACHE | UBSM_FLAG_WR_DELAY_COMP`，因此平台 NC-CC/
snoop 配置必须支持该模式。`nocache` 使用 `UBSM_FLAG_NONCACHE | UBSM_FLAG_WR_DELAY_COMP`。

## 编译

编译依赖支持 C++14 的 C/C++ compiler、CMake、pthread、Boost、jemalloc、glog 和 gflags。
openEuler 默认使用 GCC/GNU ld；Clang/LLD 是可选项，不属于 ABI 要求。运行时 NUMA placement 推荐
安装 `numactl`。

Debian/Ubuntu：

```bash
sudo apt-get install -y \
  cmake clang-15 lld-15 \
  libboost-all-dev libjemalloc-dev \
  libgoogle-glog-dev libgflags-dev numactl
```

openEuler 推荐直接使用系统 GCC，不要求带版本号的 Clang：

```bash
sudo dnf makecache
sudo dnf install -y \
  cmake make gcc gcc-c++ glibc-devel \
  boost-devel jemalloc-devel \
  glog-devel gflags-devel \
  numactl
```

如果之前的 `dnf install` 包含仓库中不存在的包名（`clang15/lld15`，或后面提到的
`glog-devel/gflags-devel`），DNF 会报 `no match for argument` 并中止**整个事务**，列表里其它能装上的
包（`boost-devel`、`jemalloc-devel`）也不会被安装。请用上面的命令重新安装，并检查：

```bash
cat /etc/openEuler-release
uname -m                         # 预期：aarch64
gcc --version
g++ --version
ld --version
```

如果当前 openEuler repository 提供 Clang，也可以显式选择；较新的发行版通常使用无版本后缀的包名和
命令名：

```bash
sudo dnf install -y clang llvm lld
command -v clang clang++ ld.lld
```

不要为了获得特定 compiler version 而启用与当前 openEuler service pack 不匹配的 EPOL repository。
上述命令不负责安装 UB SDK；其 header 和 AArch64 `libubsm_sdk.so` 必须已经位于
`/usr/local/ubs_mem`，或通过 `UBSM_INCLUDE_DIR`、`UBSM_LIBRARY` 指定。

### 仓库缺包时的依赖排查

openEuler 的 base/OS 仓库不含 `glog-devel`、`gflags-devel`（它们通常在默认 `enabled=0` 的 EPOL 段里），
而这台机器没有可用 EPOL。这类环境下按下面的顺序处理。先用文件反查，确认到底缺什么：

```bash
rpm -q boost-devel jemalloc-devel jemalloc glog-devel gflags-devel
dnf provides '*/glog/logging.h' '*/gflags/gflags.h' '*/libjemalloc.so*'   # 按文件反查提供它的包
dnf repolist --all                                                       # 看 [EPOL] 是否存在且被禁用
```

如果 `openEuler.repo` 里本来就有 `[EPOL]` 段，可以用
`sudo dnf config-manager --set-enabled EPOL` 打开（需要 `dnf-plugins-core`）。启用**本机自带**的 EPOL 段
版本天然匹配；不要去添加与当前 service pack 不匹配的外部 EPOL。

#### glog / gflags

`tigon/CMakeLists.txt` 用裸链接提供这两个库（`target_link_libraries(... glog gflags)`，**没有
`find_package`**），所以 CMake 不会去找 `*Config.cmake`，只是把名字交给链接器变成 `-lglog -lgflags`。
能不能链上只取决于库和头文件是否落在编译器默认搜索路径（`/usr/include`、`/usr/lib64`）里，
装到 `/usr` 最省事：

```bash
mkdir -p ~/src

git clone --depth 1 --branch v2.2.2 https://github.com/gflags/gflags.git ~/src/gflags
cmake -S ~/src/gflags -B ~/src/gflags/build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib64
cmake --build ~/src/gflags/build -j"$(nproc)"
sudo cmake --install ~/src/gflags/build

git clone --depth 1 --branch v0.6.0 https://github.com/google/glog.git ~/src/glog
cmake -S ~/src/glog -B ~/src/glog/build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF \
  -DWITH_UNWIND=OFF -DWITH_GFLAGS=ON \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib64 -DCMAKE_PREFIX_PATH=/usr
cmake --build ~/src/glog/build -j"$(nproc)"
sudo cmake --install ~/src/glog/build
sudo ldconfig
```

`-DWITH_UNWIND=OFF` 必须加：glog 的 CMake 默认 `WITH_UNWIND=ON`，会在 configure 阶段去要求
`libunwind-devel`，在这类缺包的机器上通常也拿不到。`CMAKE_INSTALL_LIBDIR=lib64` 显式写死，因为
openEuler aarch64 的 libdir 是 `/usr/lib64`（可用 `rpm --eval '%{_libdir}'` 确认）。先装 gflags 再装
glog，后者需要前者的头文件。

装完用一段最小程序验证——它复现的正是 tigon 的裸链接方式，能过就说明 tigon 一定能链上：

```bash
cat > ~/glog_probe.cpp <<'EOF'
#include <gflags/gflags.h>
#include <glog/logging.h>
DEFINE_int32(x, 1, "probe");
int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  LOG(INFO) << "glog ok, x=" << FLAGS_x;
  return 0;
}
EOF
g++ -std=c++17 ~/glog_probe.cpp -o ~/glog_probe -lglog -lgflags -lpthread
~/glog_probe --x=7 --logtostderr=1        # 预期输出：glog ok, x=7
```

`--logtostderr=1` 不是可选的：glog 默认把日志写 `/tmp` 下的文件，不加这个参数终端没有输出
（见「运行统计解读」中的同类说明）。

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `fatal error: glog/logging.h: No such file` | 头文件不在默认 include 路径 | 确认装到 `/usr`，`ls /usr/include/glog/` |
| `cannot find -lglog` | 库不在**链接期**搜索路径 | `gcc -print-search-dirs`；改用 `/usr/lib64`，或按下面给编译器加 `-I`/`LIBRARY_PATH` |
| `error while loading shared libraries: libglog.so.1` | 运行时缓存未刷新 | `sudo ldconfig` |
| `Could NOT find Unwind` | glog 默认 `WITH_UNWIND=ON` | 加 `-DWITH_UNWIND=OFF` |

注意**链接期和运行期是两套路径**：`ldconfig` 只管运行时动态加载，替代不了 `-lglog` 的查找。如果改
装到非默认前缀，由于这些依赖在 CMakeLists 里没有 `find_package`，`CMAKE_PREFIX_PATH` 对它们无效，
必须直接传给编译器：

```bash
export CPATH=<prefix>/include:$CPATH                  # 头文件
export LIBRARY_PATH=<prefix>/lib64:$LIBRARY_PATH      # -lglog 等链接期查找
cmake ... -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath,<prefix>/lib64"
```

#### Boost

tigon 只使用 Boost 的**头文件**（`common/CCSet.h`、`common/CCHashTable.h`、
`common/atomic_offset_ptr.hpp`、`core/CXLTable.h` 用 `boost/interprocess/offset_ptr.hpp`，
`core/Coordinator.h` 用 `boost/algorithm/string.hpp`），两者都是 header-only；CMakeLists 里没有
`find_package(Boost)`，也不链接任何 `libboost_*`。所以只需要头文件位于 `/usr/include/boost/`，
**不需要编译安装 Boost**：

```bash
sudo dnf install -y boost-devel            # 优先

# 仓库没有时，直接铺头文件
curl -LO https://archives.boost.io/release/1.83.0/source/boost_1_83_0.tar.gz
tar xf boost_1_83_0.tar.gz
sudo mkdir -p /usr/include/boost
sudo cp -a boost_1_83_0/boost/. /usr/include/boost/

ls /usr/include/boost/interprocess/offset_ptr.hpp
```

用 `cp -a .../boost/. /usr/include/boost/`（结尾的点）而不是 `cp -a .../boost /usr/include/`，后者在
`/usr/include/boost` 已存在时会套成 `boost/boost/`。

版本建议选 1.74~1.83 之间。依据是项目自带的模拟机镜像：`emulation/image/mkosi.default` 用
`Distribution=ubuntu / Release=jammy`，jammy 的 `libboost-all-dev` 就是 Boost 1.74。另外
`common/atomic_offset_ptr.hpp` 直接调用了 `boost::interprocess::ipcdetail::offset_ptr_to_raw_pointer`
和 `offset_ptr_to_offset`，这些是 `ipcdetail` 命名空间下的**内部实现接口**，不保证跨大版本仍然存在，
所以不宜追新。

#### jemalloc

`CMakeLists.txt:32` 是 `find_library(jemalloc_lib jemalloc)`，它找的是 `libjemalloc.so`——开发用的
无版本号 symlink。**runtime 包 `jemalloc` 只提供 `libjemalloc.so.2`，`find_library` 不认**，必须装
`jemalloc-devel`：

```bash
sudo dnf install -y jemalloc jemalloc-devel
ls -l /usr/lib64/libjemalloc.so* && ldconfig -p | grep jemalloc
```

tigon 全仓库**不 include jemalloc 的任何头文件**（grep 只有 `CMakeLists.txt` 和本文档提到它），
链接它的目的只是替换 malloc 实现。因此只要能提供一个 `libjemalloc.so` 就够了，头文件不需要；
机器上已有别的 `libjemalloc.so*` 时可以直接用：`-Djemalloc_lib=<那个路径>`，或
`sudo ln -s <它> /usr/lib64/libjemalloc.so`。

**装完必须删掉 build 目录重新 configure**：`find_library` 的结果缓存在 `CMakeCache.txt` 里，失败时留下
的 `jemalloc_lib-NOTFOUND` 不会因为你补装了包而自动刷新，重跑 `cmake` 依旧报同一个错：

```bash
rm -rf tigon/build-host    # 或 cmake -U jemalloc_lib
```

确认缓存已刷新：

```bash
grep -n jemalloc_lib tigon/build-host/CMakeCache.txt    # 值不该再是 jemalloc_lib-NOTFOUND
```

### 主机与构建层测试

```bash
cmake -S tigon -B tigon/build-host \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DTIGON_ENABLE_UB=ON \
  -DTIGON_BUILD_UB_UNIT_TESTS=ON \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so

cmake --build tigon/build-host -j --target \
  ub_primitives_test ub_btree_host_test ub_btree_compile_test \
  ub_adapter_compile_test ub_tpcc_adapter_compile_test ub_twopl_compile_test

ctest --test-dir tigon/build-host --output-on-failure \
  -R 'ub_(primitives|btree|adapter|tpcc|twopl)'
```

AArch64 上即使是 test build 也必须指定 `TIGON_ENABLE_UB=ON`，否则会链接旧的 x86-64 `cxlalloc`
archive。如果 `build-host` 以前没有使用该选项配置，请先重新执行上述 CMake configure 命令。


### AArch64 UB 应用构建

项目使用 `-march=native`，因此应在两台 AArch64 主机上分别编译。不要复用 x86-64 或 legacy CXL
backend 产生的 CMake cache。

在每台主机的 `ub-application` repository 根目录执行：

```bash
export TIGON_REPO_ROOT="$PWD"
export TIGON_BUILD_DIR="$TIGON_REPO_ROOT/tigon/build-ub-aarch64-gcc"

cmake -S "$TIGON_REPO_ROOT/tigon" -B "$TIGON_BUILD_DIR" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DTIGON_ENABLE_UB=ON \
  -DUBSM_INCLUDE_DIR=/usr/local/ubs_mem/include \
  -DUBSM_LIBRARY=/usr/local/ubs_mem/lib/libubsm_sdk.so

cmake --build "$TIGON_BUILD_DIR" -j \
  --target bench_ycsb bench_tpcc ub_tigon_two_node_test
```

`$PWD` 并不要求 build output 必须位于任意当前目录；它只是展开为 shell 当前目录的绝对路径。上述
命令只有在 repository 根目录执行时才正确。如果 repository 位于 `/work/ub-application`，也可以写
成：

```bash
export TIGON_REPO_ROOT=/work/ub-application
export TIGON_BUILD_DIR="$TIGON_REPO_ROOT/tigon/build-ub-aarch64-gcc"
```

脚本默认使用 `tigon/build-ub`。这里只有因为手册使用了独立的 `build-ub-aarch64-gcc`，才需要设置
`TIGON_BUILD_DIR`。

UB build 不链接 `dependencies/cxlalloc/libcxlalloc_static.a`。该 archive 是为 legacy CXL backend
保留的 x86-64 artifact，与 AArch64 UB host 不兼容。UB build 使用 fail-fast stub 满足遗留 CXL 声
明；如果运行时进入 stub，说明选择了尚未适配的 CXL execution path，进程会输出对应函数名并终止。

构建前再次确认 AArch64 UB SDK 及完整依赖链：

```bash
uname -m                         # 预期：aarch64
file /usr/local/ubs_mem/lib/libubsm_sdk.so
ldd /usr/local/ubs_mem/lib/libubsm_sdk.so
```

如果旧 CMake cache 记录了 `/usr/bin/clang-15`，必须使用新 build directory。configure output 应显
示 GCC 路径，并包含：

```text
Tigon C compiler: /usr/bin/gcc
Tigon C++ compiler: /usr/bin/g++
Tigon UB backend enabled for aarch64; the legacy cxlalloc archive will not be linked
```

运行前检查生成的 executable：

```bash
file "$TIGON_BUILD_DIR/bench_ycsb"
file "$TIGON_BUILD_DIR/bench_tpcc"
file "$TIGON_BUILD_DIR/ub_tigon_two_node_test"
ldd "$TIGON_BUILD_DIR/bench_ycsb"
ldd "$TIGON_BUILD_DIR/bench_tpcc"
ldd "$TIGON_BUILD_DIR/ub_tigon_two_node_test"
```

它们必须是 AArch64 executable，且不能存在 unresolved shared library。

每次运行都要使用唯一的 `--ub_region_prefix`，且不能在 `one-sided` 与 `nocache` 之间复用。两台主
机必须使用相同的 prefix、region size、coordinator count 和 mode；`--id`、provider host 不同。

## 双机验证流程

socket address 仍用于启动/关闭 barrier，并在 `--use_ub_transport=false` 时作为控制与数据
transport。两个命令应相近时间启动，因为每个进程创建 owner region 后会等待 peer region 出现。

`--servers` 的每一项都必须写成 `IP:port`，端口不能省略；两台主机必须传**完全相同**的字符串，
顺序也要一致（host 0 在前）。脚本不会补默认端口，Tigon 侧也不校验格式——漏写端口会在
`star::Coordinator::connectToPeers()` 里以 `SIGSEGV(@0x0)` 结束，排查方法见「常见错误」。

每次运行使用新的 region prefix。不要在 `one-sided`、`nocache` 之间复用，也不要在异常退出后复用，
除非已经确认旧 UBS object 被释放。两台主机必须使用相同 prefix、mode、region size、server
ordering、key count 和 timing 参数；`--id`、`UB_PROVIDER_HOST` 不同。

### 步骤 1：独立双机正确性测试

首次 smoke test 时，在两台主机设置与本地 OBMM block 配置兼容的 region size。

机器 81：

```bash
export UB_REGION_MB=128
export UB_PROVIDER_HOST=host-81
export UB_NUMA_NODE=1
export UB_PROVIDER_NUMA=1
```

机器 82：

```bash
export UB_REGION_MB=128
export UB_PROVIDER_HOST=host-82
export UB_NUMA_NODE=0
export UB_PROVIDER_NUMA=0
```

先启动机器 81 / coordinator 0。示例中的 `192.0.2.10` 是其 TCP barrier 监听地址：

```bash
bash tigon/scripts/run_ub_correctness.sh \
  0 192.0.2.10 one-sided correctness_001_os 10000 18951
```

然后启动机器 82 / coordinator 1：

```bash
bash tigon/scripts/run_ub_correctness.sh \
  1 192.0.2.10 one-sided correctness_001_os 10000 18951
```

两个进程都必须以 `PASS` 结束。该测试覆盖跨节点 writer-lock increment、placeholder
conflict/publication、delete visibility、同-key tombstone reuse、abort placeholder tombstone、并发
requester insert、leaf/inner/root split、ordered scan、queue-full backpressure、双向 queue
payload、range successor/end-gap exclusion 和 importer-first cleanup。

换用新 prefix，在两台主机重复 `nocache`：

```bash
# Host 0
bash tigon/scripts/run_ub_correctness.sh \
  0 192.0.2.10 nocache correctness_001_nc 10000 18951

# Host 1
bash tigon/scripts/run_ub_correctness.sh \
  1 192.0.2.10 nocache correctness_001_nc 10000 18951
```

> 经验提示：correctness test 默认使用 128 MiB region。如果后续 YCSB 在 4 GiB region 上创建失败
>（`UBSM_ERR_UBSE 6050`），说明 128 MiB 与 4 GiB 之间的创建能力是断层的，步骤 2 应按缩小后的
> region 起步。

### 步骤 2：单组 YCSB 点操作测试

> 本节已按首次双机运行的实际经验更新：**不要直接用 4 GiB region 起步**。4 GiB 创建失败是首次
> 运行最常见的错误（表现为 `UBSM_ERR_UBSE 6050`），而 YCSB 20 万 key 实际只需约 250–300 MiB。
> 推荐按「先小 region + TCP 消息 → 开 UB queue → 恢复 key 数」三步推进，并始终添加
> `--logtostderr=1` 以便在终端直接看到运行统计。

下面手动运行 `one-sided` RMW。所有 benchmark 命令都建议添加 `--logtostderr=1`，否则 glog 默认把
INFO 日志写入 `/tmp` 下的文件，终端看起来就像"运行结束但什么都没有打印"。

#### 第一步：缩小数据集，先关闭 UB 消息队列

两台机器使用 256 MiB region、5 万 key、`--use_ub_transport=false`（Tigon 消息走 TCP，tuple、
锁和 B+ Tree 仍全部使用 UB，可排除 UB queue 集成问题）：

机器 81 / coordinator 0：

```bash
"$TIGON_BUILD_DIR/bench_ycsb" \
  --logtostderr=1 \
  --id=0 --servers="192.0.2.10:10010;192.0.2.11:10010" \
  --protocol=TwoPLPasha --partition_num=2 --threads=1 --io=1 \
  --shared_memory_backend=ub --ub_memory_mode=one-sided \
  --ub_region_prefix=tigon_smoke_001 --ub_region_mb=256 \
  --ub_provider_host=host-81 --ub_provider_numa=1 \
  --use_ub_transport=false --query=rmw --keys=50000 \
  --cross_ratio=100 --time_to_warmup=5 --time_to_run=20 \
  --lotus_checkpoint=0 2>&1 | tee ycsb-node0.log
echo "bench exit code=${PIPESTATUS[0]}"
```

机器 82 / coordinator 1（`--id`、provider host/numa 不同，其余参数必须与机器 81 完全一致）：

```bash
"$TIGON_BUILD_DIR/bench_ycsb" \
  --logtostderr=1 \
  --id=1 --servers="192.0.2.10:10010;192.0.2.11:10010" \
  --protocol=TwoPLPasha --partition_num=2 --threads=1 --io=1 \
  --shared_memory_backend=ub --ub_memory_mode=one-sided \
  --ub_region_prefix=tigon_smoke_001 --ub_region_mb=256 \
  --ub_provider_host=host-82 --ub_provider_numa=0 \
  --use_ub_transport=false --query=rmw --keys=50000 \
  --cross_ratio=100 --time_to_warmup=5 --time_to_run=20 \
  --lotus_checkpoint=0 2>&1 | tee ycsb-node1.log
echo "bench exit code=${PIPESTATUS[0]}"
```

两个命令应相近时间启动。若此前 4 GiB 创建失败，先用这一步确认 256 MiB 能正常创建并跑完 25 秒以上。

#### 第二步：打开 UB transport

第一步成功后换新 prefix，验证 UB queue：

```bash
--ub_region_prefix=tigon_smoke_002
--ub_region_mb=256
--keys=50000
--use_ub_transport=true
```

#### 第三步：恢复 20 万 key

前两步成功后恢复原始规模：

```bash
--ub_region_prefix=tigon_ycsb_200k_001
--ub_region_mb=512
--keys=200000
--use_ub_transport=true
```

512 MiB 对 20 万 key 已有余量（实测占用约 250–300 MiB，见「常见错误」里 6050 那条的第 3 步）。
只有数据库初始化阶段真的抛出 `std::bad_alloc` 时才需要加大 —— 这与"UBSE 连 region 都
创建不出来"的 6050 是不同错误，加的时候优先在 384 → 512 这一档内微调、每次加 128 MiB，
不要直接跳到 768/1024：region 越大，`UBSM_ERR_UBSE 6050` 的创建失败风险越高。

#### 原始参考命令（确认 4 GiB 可创建后再使用）

```bash
"$TIGON_BUILD_DIR/bench_ycsb" \
  --logtostderr=1 \
  --id=0 --servers="192.0.2.10:10010;192.0.2.11:10010" \
  --protocol=TwoPLPasha --partition_num=2 --threads=1 --io=1 \
  --shared_memory_backend=ub --ub_memory_mode=one-sided \
  --ub_region_prefix=tigon_run_001 --ub_region_mb=4096 \
  --ub_provider_host=host-81 --ub_provider_numa=1 \
  --use_ub_transport=true --query=rmw --keys=200000 \
  --cross_ratio=100 --time_to_warmup=5 --time_to_run=20 \
  --lotus_checkpoint=0
```

（机器 82 相应使用 `--id=1`、`--ub_provider_host=host-82`、`--ub_provider_numa=0`。）

#### 如何确认运行结果正常

1. `echo $?` 应为 0（134 通常是 abort，139 通常是段错误）；运行时长应明显超过
   初始化时间 + warmup + time_to_run。
2. 若未加 `--logtostderr`，从 `/tmp` 找日志：
   `ls -lt /tmp/*bench_ycsb* | head`，再 `tail -n 100` 查看。
3. 每秒窗口会输出 `commit: N / abort: M / persistence latency / txn latency / lock latency /
   network size / local / remote_access` 等；结束时输出 `average commit`、`Worker 0 latency`
   分位数、`dist txn latency` 等，见下文「运行统计解读」。

### 步骤 3：分阶段 YCSB 测试矩阵

matrix 脚本默认运行 12 个组合：两种 memory mode、三种 point operation、两种 message
transport。首次运行同样建议把 `UB_REGION_MB` 从 4096 降到 256/512，并按上节先只跑一个组合：

```bash
export UB_REGION_MB=512
export UB_TIGON_THREADS=1
export UB_TIGON_KEYS=200000
export UB_TIGON_RUN_SECONDS=20
export UB_TIGON_WARMUP_SECONDS=5
export UB_TIGON_MODES="one-sided"
export UB_TIGON_QUERIES="rmw"
export UB_TIGON_TRANSPORTS="false"
```

保留步骤 1 中机器各自的 `UB_PROVIDER_HOST`、`UB_PROVIDER_NUMA` 和 `UB_NUMA_NODE`。以下两个命令
必须并发运行。

Host 0：

```bash
bash tigon/scripts/run_ub_ycsb_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' ycsb_tcp_001
```

Host 1：

```bash
bash tigon/scripts/run_ub_ycsb_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' ycsb_tcp_001
```

通过后设置 `UB_TIGON_TRANSPORTS=true`，并使用新的 prefix base 测试 UB queue。最后运行完整
matrix：

```bash
export UB_TIGON_MODES="one-sided nocache"
export UB_TIGON_QUERIES="rmw insert delete"
export UB_TIGON_TRANSPORTS="false true"

# Host 0
bash tigon/scripts/run_ub_ycsb_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' ycsb_mat_001

# Host 1
bash tigon/scripts/run_ub_ycsb_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' ycsb_mat_001
```

脚本生成的 suffix 会让每个 mode/query/transport 组合使用不同 region prefix。**合成后的 prefix 必须
≤ 35 字节**（脚本会拒绝超长并 `exit 2`）：最长后缀 `_one-sided_insert_tcp` 占 21 字节，所以
prefix base 要 **≤ 14 字符**，上例的 `ycsb_tcp_001`（12）和 `ycsb_mat_001`（12）都留了余量。
base 取长了不会跑起来而是先报 `UB region prefix exceeds 35 bytes: <prefix>`（退出码 2），容易
被误当成运行失败。可覆盖的环境变量按用途分组：

- 规模与时序：`UB_TIGON_PARTITIONS`（默认 2）、`UB_TIGON_THREADS`（1）、`UB_TIGON_KEYS`（200000）、
  `UB_TIGON_RUN_SECONDS`（20）、`UB_TIGON_WARMUP_SECONDS`（5）。`UB_TIGON_RUN_SECONDS` 是**传给
  `--time_to_run` 的总时长**，必须大于 warmup；真正测量的窗口是两者之差（脚本按
  `measurement = RUN_SECONDS - WARMUP_SECONDS` 记录到运行标记里）。
- workload 形状：`UB_TIGON_RW_RATIO`（80，映射到 `--read_write_ratio`）、`UB_TIGON_ZIPF`（0）、
  `UB_TIGON_CROSS_RATIO`（100）。
- 组合扫描：`UB_TIGON_MODES`（`one-sided nocache`）、`UB_TIGON_QUERIES`（`rmw insert delete`）、
  `UB_TIGON_TRANSPORTS`（`false true`）。
- 运行环境：`TIGON_BUILD_DIR`、`UB_REGION_MB`、`UB_PROVIDER_HOST`、`UB_PROVIDER_NUMA`、
  `UB_NUMA_NODE`、`UB_TIGON_CPU_LIST`、`UB_RESULT_DIR`。
- 结果标记：`UB_TIGON_RUN_ID`（默认取 prefix base，写入 `TIGON_RUN_META`）、
  `UB_TIGON_REPETITION`、`UB_TIGON_DRY_RUN`（1 = 只打印不运行）。

设置 `UB_RESULT_DIR` 后，每个 point 的**终端输出**（脚本自己的两行标记，加上子进程写到
stdout/stderr 的内容）会写入
`$UB_RESULT_DIR/ub_ycsb_<run_id>_host<id>_<mode>_<query>_<transport>.log`（文件名中的
`<id>`/`<mode>`/`<query>`/`<transport>` 都是实际取值，如
`ub_ycsb_ycsb_mat_001_host0_one-sided_rmw_ubq.log`），这是步骤 6 解析器的输入。路径建议写绝对
路径，相对路径会相对当前工作目录展开（从别的目录启动时容易多出一层 `results/results/`）。

**注意：这个文件默认不含测量数据。** benchmark 的吞吐和延迟统计全部是 glog 的 `LOG(INFO)`
（`core/Coordinator.h:348` 的 `average commit`、`:639` 的 `Global Stats: total_commit`），而 matrix
脚本不传 `--logtostderr=1`（`TIGON_RUN_META` 里的 `logging=off`），glog 默认只把 INFO 写进**每台
机器自己**的 `/tmp/bench_ycsb.<host>.<user>.log.INFO.<date>-<time>.<pid>`。所以只设置
`UB_RESULT_DIR` 的话，看到的通常就是两行标记加 libubsm 的 stderr 报错（例如 region 映射重试时的
`Failed to mmap ... ret=606`，见「常见错误」）。要让统计进 `UB_RESULT_DIR`，二选一：

- 跑之前先从环境变量打开 stderr 输出（脚本本身不接受额外参数）：`export GLOG_logtostderr=1`。
  这只在 glog 支持 `GLOG_<flag>` 环境变量时有效，所以先单独跑一个点确认终端开始出现
  `I... Coordinator.h:...` 之类的行，再跑整个矩阵；
- 或者在跑完之后，在**每台机器上**把 glog 文件收进同一个目录再解析（host 0 那份才有
  `Global Stats: total_commit`，host 1 那份只用于配对检查）：
  ```bash
  cp -n "$(ls -t /tmp/bench_ycsb.*.log.INFO.* | head -1)" \
        "${UB_RESULT_DIR:-tigon/results}/"
  ```

只想快速看吞吐时，也可以直接 `grep 'Global Stats' /tmp/bench_ycsb.* | tail`。

### 步骤 4：YCSB 范围查询与幻读保护

point case 全部通过后，单独运行 range transaction，以免较大 matrix 掩盖故障。wrapper 默认选择
`scan mixed`，其他参数和环境变量与 Step 3 相同：

```bash
export UB_TIGON_MODES="one-sided"
export UB_TIGON_TRANSPORTS="false"
export UB_TIGON_RANGE_QUERIES="scan"

# 分别在 Host 0 和 Host 1 并发运行
bash tigon/scripts/run_ub_ycsb_range_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' ycsb_scan_tcp_001
bash tigon/scripts/run_ub_ycsb_range_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' ycsb_scan_tcp_001
```

`scan` 通过后按下面的顺序推进。每段必须换一个新的 prefix base：region 名字是
`<prefix>_<i>`（`UBMemory.cpp:105`），而 `initialize_local_header()` 会拒绝已经带 Tigon header 的
owner region（`UBMemory.cpp:208-210`），复用旧 base 会直接抛错而不是复用数据。range 组合的最长
后缀是 `_one-sided_mixed_tcp` / `_one-sided_mixed_ubq`（各 20 字节），所以 base ≤ 15；下面各段都用
≤ 12 字节的 base（上一段的 `ycsb_scan_tcp_001` 是 16 字节，只对 `scan` 那个组合刚好 35，换 query 就
会超，所以后面每段都另取更短的 base）。每段的两个命令仍然必须并发运行。

**阶段 2 —— 同一 transport 下测 `mixed`**

```bash
export UB_TIGON_RANGE_QUERIES="mixed"
export UB_TIGON_MODES="one-sided"
export UB_TIGON_TRANSPORTS="false"

# Host 0
bash tigon/scripts/run_ub_ycsb_range_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' ycsb_mix_001

# Host 1
bash tigon/scripts/run_ub_ycsb_range_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' ycsb_mix_001
```

**阶段 3 —— 换成 UBS queue transport（`scan` + `mixed` 两种 range 查询）**

```bash
export UB_TIGON_RANGE_QUERIES="scan mixed"
export UB_TIGON_MODES="one-sided"
export UB_TIGON_TRANSPORTS="true"

# Host 0
bash tigon/scripts/run_ub_ycsb_range_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' ycsb_ubq_001

# Host 1
bash tigon/scripts/run_ub_ycsb_range_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' ycsb_ubq_001
```

**阶段 4 —— 换 `nocache` mode（先 TCP，再用新 base 跑 UB queue）**

```bash
export UB_TIGON_RANGE_QUERIES="scan mixed"
export UB_TIGON_MODES="nocache"
export UB_TIGON_TRANSPORTS="false"

# Host 0
bash tigon/scripts/run_ub_ycsb_range_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' ycsb_nc_001

# Host 1
bash tigon/scripts/run_ub_ycsb_range_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' ycsb_nc_001
```

再把 `UB_TIGON_TRANSPORTS` 设为 `true` 并用 `ycsb_nc_002` 重跑一遍，range 部分就覆盖完了。

正确运行时不应出现未锁定 next row、重复/乱序 scan key，或在受并发保护的 gap 中成功 commit
insert。

### 步骤 5：TPCC 主索引与二级索引

TPCC 消耗的 region 空间明显更多，launcher 默认每个 owner region 使用 8 GiB。**注意：如果
YCSB 在 4 GiB 上已出现 6050，TPCC 默认的 8 GiB 同样可能创建失败，应先用缩小后的 region（如
1024–2048 MiB）验证，再逐步上调。**

TPCC 的初始化要往 UBS region 里**逐行**插入大量数据：`item` 一张表就是 100,000 行
（`ITEM_NUM`，`benchmark/tpcc/Schema.h:33`），每个 warehouse 还有 `stock` 100,000 行、`customer`
30,000 行、`order` 30,000 行、`order_line` 约 300,000 行；`--partition_num` 在 TPCC 下默认是
`2 × threads`，所以首发配置大约 1.2M 行，且 `item` 由单个 partition 单线程写入。全程只写 glog；
matrix 脚本默认**不传** `--logtostderr=1`，所以终端在 `TIGON_RUN_META` 那一行之后会完全静默
**几分钟**——这是正常现象，不等于卡死（判断和定位方法见「常见错误」里"终端长时间无输出"那条）。

先隔离一种 transaction type 并使用 TCP message。TPCC 的 prefix base 比 YCSB 更紧：最长后缀是
`_one-sided_first_two_tcp`（24 字节），所以 base 必须 **≤ 11 字节**（`UBMemory.cpp:68` 要求
`prefix.size() + 12 < 48`，也就是合成后的 prefix ≤ 35 字节；YCSB 那条 ≤ 14 的规则来自它自己的最长
后缀 `_one-sided_insert_tcp` = 21 字节）。下面各段都用 8–10 字节的 base，两个命令并发运行：

```bash
export UB_REGION_MB=2048
export UB_TIGON_THREADS=1
export UB_TIGON_MODES="one-sided"
export UB_TIGON_TRANSPORTS="false"
export UB_TPCC_QUERIES="payment"
export UB_TPCC_PAYMENT_DIST=100
export UB_TPCC_NEWORDER_DIST=100
```

Host 0：

```bash
bash tigon/scripts/run_ub_tpcc_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' tpcc_iso_01
```

Host 1：

```bash
bash tigon/scripts/run_ub_tpcc_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' tpcc_iso_01
```

`payment` 通过后，按「先 TCP 后 UB queue」×「先 one-sided 后 nocache」推进。每个阶段通过环境变量
把 4 种 transaction 一次性交给脚本的循环（顺序与脚本默认一致：`payment neworder first_two
mixed`），每段换一个新 base，两个命令并发运行。

**阶段 A —— TCP + one-sided，4 种 transaction**

```bash
export UB_TPCC_QUERIES="payment neworder first_two mixed"
export UB_TIGON_TRANSPORTS="false"
export UB_TIGON_MODES="one-sided"

# Host 0
bash tigon/scripts/run_ub_tpcc_matrix.sh \
  0 '192.0.2.10:10010;192.0.2.11:10010' tpcc_001

# Host 1
bash tigon/scripts/run_ub_tpcc_matrix.sh \
  1 '192.0.2.10:10010;192.0.2.11:10010' tpcc_001
```

**阶段 B —— 换成 UB queue**：`export UB_TIGON_TRANSPORTS="true"`，base `tpcc_002`。

**阶段 C —— 换 `nocache` + TCP**：`export UB_TIGON_MODES="nocache"` 和
`UB_TIGON_TRANSPORTS="false"`，base `tpcc_003`。

**阶段 D —— `nocache` + UB queue**：`UB_TIGON_TRANSPORTS="true"`，base `tpcc_004`。

各阶段除上面这几行 export 和 base 之外，命令与阶段 A 完全相同；每一段都必须两端并发、完整跑完，
出现非零退出码就停下来查。

首次运行建议直接跑二进制并加 `--logtostderr=1`，这样终端能看到初始化和每秒统计（两台主机同时
启动）：

```bash
"$TIGON_BUILD_DIR/bench_tpcc" \
  --logtostderr=1 \
  --id=0 --servers="192.0.2.10:10010;192.0.2.11:10010" \
  --protocol=TwoPLPasha --partition_num=2 --threads=1 --io=1 \
  --shared_memory_backend=ub --ub_memory_mode=one-sided \
  --ub_region_prefix=tpcc_raw_001 --ub_region_mb=2048 \
  --ub_provider_host=host-81 --ub_provider_numa=1 \
  --use_ub_transport=false --query=payment \
  --neworder_dist=100 --payment_dist=100 \
  --time_to_warmup=5 --time_to_run=20 --lotus_checkpoint=0
```

机器 82 用 `--id=1`、`--ub_provider_host=host-82`、`--ub_provider_numa=0`。正常会看到
`creating hash tables for database...` 和每张表的 `<name> initialization finished in N ms`；
如果停在 region 映射阶段想放宽等待，加 `--ub_map_timeout=300`。

两端的启动和关闭 consistency check 都必须通过。除步骤 3 的通用 matrix 变量外，脚本还接受
`UB_TPCC_QUERIES`（默认 `payment neworder first_two mixed`）、
`UB_TPCC_PAYMENT_DIST` 和 `UB_TPCC_NEWORDER_DIST`；`UB_TIGON_PARTITIONS` 的 TPCC 默认值不是 2，
而是 `2 × UB_TIGON_THREADS`，终端输出（脚本标记 + stderr）写入
`$UB_RESULT_DIR/ub_tpcc_<run_id>_host<id>_<mode>_<query>_<transport>.log`。和步骤 3 一样，这个文件
默认**不含** glog 统计与 `TPC-C consistency check passed!`，收集方法见步骤 3 的说明。TPCC 没有
keys/rw_ratio/zipf/cross_ratio 概念，这些字段在 `TIGON_RUN_META` 中为空。

设置 `--threads=N` 时，每个进程还包含 manager、incoming dispatcher、outgoing dispatcher 和
coordinator main thread，因此 Tigon 主要线程数约为 `N + 4`。机器 82 理论上限接近 `N=16`，但
`N=14` 或 `N=15` 更安全，可为 UBS 服务与 OS 留出 CPU。建议按 1、4、8、12、14 逐步扩展；两个
coordinator 使用相同 `UB_TIGON_THREADS`。

### 步骤 6：UB 与 spr4 双机公平对比

步骤 1–5 验证的是 UB 路径本身是否成立。若要做 UB 与 spr4（原 CXL/emulation 分支）的对比，使用
`tigon/scripts/run_ub_two_node_compare.sh`。它在两台 UB 主机上使用**完全相同的 workload 顺序
和 campaign**，两端之间只有 `--id` 与本机 provider 配置不同：

```bash
# Host 0 / coordinator 0
bash tigon/scripts/run_ub_two_node_compare.sh \
  ycsb 0 '192.0.2.10:10010;192.0.2.11:10010' cmp_001

# Host 1 / coordinator 1
bash tigon/scripts/run_ub_two_node_compare.sh \
  ycsb 1 '192.0.2.10:10010;192.0.2.11:10010' cmp_001
```

第二个参数是 `ycsb` 或 `tpcc`。campaign 字符串两端必须一致，且只能包含字母、数字、点、下划线和
短横线。wrapper 不自己拼 benchmark 命令，而是把参数导出成 `UB_TIGON_*` 后调用步骤 3/5 的 matrix
脚本，因此 region prefix、mode、query、transport 和 point 顺序在两端自动一致。两个 wrapper 都会传
`--lotus_checkpoint=0` 关闭 checkpoint（也就是运行标记里的 `logging=off`），避免持久化开销污染
吞吐对比。

默认值（全部可用 `UB_COMPARE_*` 覆盖）：

| 变量 | 默认 | 含义 |
| --- | --- | --- |
| `UB_COMPARE_REPETITIONS` | 3 | 每个 point 重复次数 |
| `UB_COMPARE_THREADS` | 3 | 每进程 worker 数 |
| `UB_COMPARE_WARMUP_SECONDS` / `UB_COMPARE_RUN_SECONDS` | 30 / 30 | 预热与测量窗口；`time_to_run` 收到两者之和（60 秒） |
| `UB_COMPARE_MODES` | `one-sided` | 对应 `UB_TIGON_MODES` |
| `UB_COMPARE_TRANSPORTS` | `true` | 默认使用 UB queue |
| `UB_COMPARE_REGION_MB` | YCSB 4096 / TPCC 8192 | 每个 owner region 大小；同样受 6050 影响，smoke test 应先调小 |
| `UB_COMPARE_PARTITIONS` | YCSB 2 / TPCC `2 × threads` | partition 数 |
| `UB_COMPARE_KEYS` / `UB_COMPARE_ZIPF` | 300000 / 0.7 | YCSB 每 partition key 数与 skew |
| `UB_COMPARE_YCSB_QUERIES` | `rmw` | YCSB query |
| `UB_COMPARE_RW_RATIOS` | `95 50` | 扫描的 read/write 比 |
| `UB_COMPARE_CROSS_RATIOS` | `0 25 50 75 100` | 扫描的跨 partition 比例 |
| `UB_COMPARE_TPCC_QUERIES` | `mixed` | TPCC query |
| `UB_COMPARE_TPCC_REMOTE_PAIRS` | `0:0 60:90` | 扫描的 `neworder:payment` 远程比例 |

即 YCSB 默认扫 `2 × 5 × 3 = 30` 个 point，TPCC 默认扫 `2 × 3 = 6` 个。先缩小再做正式矩阵，例如：

```bash
export UB_COMPARE_REPETITIONS=1
export UB_COMPARE_RW_RATIOS="95"
export UB_COMPARE_CROSS_RATIOS="0 100"
export UB_COMPARE_WARMUP_SECONDS=5
export UB_COMPARE_RUN_SECONDS=10
```

每个 point 的描述性 `run_id`（形如 `cmp_001_y_rw95_c100_r1`）会被 `cksum` 确定性地压缩成短
region prefix（`u%08x`），因此两端得到同一个 prefix，也不会撞上 UBSM 48-byte 名称 ABI；matrix
脚本仍会检查合成后的 prefix 不超过 35 字节。每个 point 都换 prefix，所以对比运行不需要手工清理
region。

wrapper 为每个 point 打印两行可解析标记：

```text
TIGON_RUN_META system=ub workload=ycsb run_id=cmp_001_y_rw95_c100_r1 host_id=0 repetition=1 mode=one-sided query=rmw transport=ubq partitions=2 workers=3 keys=300000 rw_ratio=95 zipf=0.7 cross_ratio=100 neworder_dist= payment_dist= warmup_seconds=30 run_seconds=30 total_seconds=60 logging=off
TIGON_RUN_END run_id=cmp_001_y_rw95_c100_r1 host_id=0 status=ok exit_code=0
```

`TIGON_RUN_END` 的 `status` 在正常结束时为 `ok`、benchmark 非零退出时为 `failed`、dry-run 时为
`dry-run`。设置 `UB_RESULT_DIR` 后每个 point 的终端输出（含这两行）写入步骤 3/5 所述的日志文件，
解析器就靠这些日志工作。**但解析器要的是 glog 输出**：YCSB 需要 host 0 日志里出现
`Global Stats: total_commit`，TPCC 需要 `TPC-C consistency check passed!`，这两行都是 `LOG(INFO)`，
默认只落在各主机自己的 `/tmp`。跑之前 `export GLOG_logtostderr=1`，或跑完后按步骤 3 的方法把
`/tmp/bench_*.log.INFO.*` 收进 `UB_RESULT_DIR`，否则解析器会报"解析到 0 行"。

解析器递归读取 UB 的日志文件或目录：

```bash
python3 tigon/scripts/parse/parse_two_node_compare.py \
  "$UB_RESULT_DIR" tigon/results/two-node \
  --raw-output two_node_raw.csv --summary-output two_node_summary.csv
```

解析规则与约束：

- 吞吐只取 coordinator 0（`host_id=0`）打印的 `Global Stats total_commit`，该值已经聚合两个
  coordinator；**不得把 host 1 的吞吐再加一次**。
- host 1 的日志只用于两件事：检查 point 是否配对（`peer_log_found`/`peer_status`）和检查退出
  状态。缺 peer、`status != ok`、YCSB 缺少 `total_commit`、TPCC 未打印
  `TPC-C consistency check passed!` 的样本都不会进入 summary 统计。
- summary 按重复实验给出 mean/median/min/max 和 `abort_rate_mean`；
  `remote_efficiency_vs_local` 以本系统本配置的本地基线（YCSB `cross_ratio=0`，或 TPCC
  `neworder_dist=0` 且 `payment_dist=0`）用 median 计算。
- 返回码 0 表示至少解析到一行 cluster row，否则返回 1；`parsed_logs` 与 `cluster_rows` 会打印
  出来，便于判断两侧日志是否齐全。
- 不同 ISA、虚拟化和物理拓扑下的绝对吞吐只能标为 cross-platform comparison，不能解释成单一
  CXL/UB 机制差异。

#### spr4 侧对比与结果配对

对比的另一侧是 `tigon-spr4/scripts/run_two_node_compare.sh`，它的参数与 UB 侧不同——没有 host id，
只有 workload 和 campaign：

```bash
bash tigon-spr4/scripts/run_two_node_compare.sh ycsb cmp_001
bash tigon-spr4/scripts/run_two_node_compare.sh tpcc cmp_001
```

它通过 `emulation/shared_run.sh` 驱动模拟机，所以运行前必须先 source 共享主机环境
（`emulation/shared-host.env`，它提供脚本要求的 `TIGON_RUNTIME_DIR`），否则脚本直接报错退出；只有
`SPR4_COMPARE_DRY_RUN=1` 时可以跳过这一步。结果目录默认为 `tigon-spr4/results/two-node`，可用
`SPR4_RESULT_DIR` 覆盖，日志名形如 `spr4_${workload}_${run_id}.log`。它打印的运行标记是
`system=spr4 ... mode=native transport=cxlq`。

spr4 侧默认值比 UB 侧保守，要跟 UB 的扫描矩阵对上，必须显式把两侧设成一致：

| 变量 | spr4 默认 | 对应的 UB 侧变量 |
| --- | --- | --- |
| `SPR4_COMPARE_REPETITIONS` | 1 | `UB_COMPARE_REPETITIONS` |
| `SPR4_COMPARE_THREADS` | 3 | `UB_COMPARE_THREADS` |
| `SPR4_COMPARE_RW_RATIOS` | `95` | `UB_COMPARE_RW_RATIOS` |
| `SPR4_COMPARE_CROSS_RATIOS` | `0` | `UB_COMPARE_CROSS_RATIOS` |
| `SPR4_COMPARE_TPCC_REMOTE_PAIRS` | `0:0` | `UB_COMPARE_TPCC_REMOTE_PAIRS` |
| `SPR4_COMPARE_KEYS` / `SPR4_COMPARE_ZIPF` | 300000 / 0.7 | `UB_COMPARE_KEYS` / `UB_COMPARE_ZIPF` |

**一次 VM 生命周期只能跑一个 point。** 脚本会检查「点数 × repetitions」，只要不是「恰好一个 point、
恰好一次重复」就拒绝执行并返回 2：

```text
refusing to run 2 point(s) x 1 repetition(s) in one VM lifecycle
current spr4 CXL metadata is not reusable
```

这不是故障，而是保护性拒绝：spr4 侧的 CXL metadata 在同一个 VM 里不可复用，换 point 必须重建或重启
VM。`SPR4_ALLOW_VM_REUSE=1` 只用于诊断，不要用它产出正式对比数据。

两侧靠 `run_id` 与配置字段对齐：UB 侧是 `${campaign}_y_rw${rw}_c${cross}_r${rep}`（TPCC 为
`${campaign}_t_n${neworder}_p${payment}_r${rep}`），spr4 侧用同样的命名，因此**同一个 campaign
字符串两端必须一致**。解析器按配置字段分组，而 `mode`（`one-sided` / `native`）和 `transport`
（`ubq` / `cxlq`）不同，所以 UB 与 spr4 各自成为独立的 summary 行，正好用于并排比较。

`tigon/tests/test_ub_matrix_scripts.py` 中的 `test_compare_wrappers_have_matching_ycsb_command`、
`test_compare_wrappers_have_matching_tpcc_command` 和
`test_spr4_refuses_multiple_points_without_reuse_override` 会用 dry-run 断言两侧拼出的 workload 参数
逐项一致、以及上面那条拒绝逻辑；改这两个脚本时先跑它。

## 运行统计解读

按本文档参数（`--partition_num=2 --cross_ratio=100 --query=rmw`）首次运行 YCSB 时，每秒统计中
大量字段为 0 是预期现象，不代表 benchmark 空跑。参考数据：单线程下 `commit: 4280` 对应
`local_access` 与 `remote_access` 各约 21422。

### commit 与 access 的换算关系

YCSB 默认每个事务包含 10 个 key 操作（`tigon/benchmark/ycsb/Context.h`）。在
`cross_ratio=100` 且 `partition_num=2` 时，每个事务的 10 个操作轮流分配到两个 partition，即每
事务约访问 5 个本地 tuple、5 个远程 tuple。因此：

```text
local_access  ≈ commit × 5
remote_access ≈ commit × 5
```

`commit: 4280` 时理论值约为 21400，实际 21422 非常接近；少量偏差来自当秒发生的 1–2 次
abort/retry，以及 commit 与 access 计数器在一秒窗口边缘的采样时间差。这一组数据自洽，说明两台机
器的 UB 映射、远程 tuple 查找、requester-side 加锁和提交主路径都已跑通。

### 为 0 指标的分类

按当前 UB requester-side 直访设计，下列指标为 0 属于**正常**：

| 指标 | 为 0 的原因 |
| --- | --- |
| `persistence latency` | 使用了 `--lotus_checkpoint=0`，没有执行持久化/checkpoint |
| `local` | 表示纯本地事务比例，不是本地 tuple 访问次数；`cross_ratio=100` 时应为 0% |
| `local_cxl_access` | 原 CXL/migration 路径的旧统计；UB 路径使用 `local_access` |
| `remote_access_with_req` | UB requester 直接锁定并访问远程 tuple，不向 owner 发数据迁移请求 |
| `data_move_in/out` | UB 路径已移除 migration/SCC |
| `network size` | 该字段统计事务协议的消息字节；点操作直接访问 UB 内存，无 owner-side tuple 请求 |
| `n_failed_read/write_lock` | 当前冲突较少 |
| `n_failed_no_cmd`、`cmd_not_ready` | 无消息队列命令缺失或未就绪 |
| `abort` 偶尔 1–2 | 两台机器可能并发竞争相同 key 的写锁 |

下列指标为 0 是**统计链路尚未接入**，不代表实际耗时为 0：

| 指标 | 说明 |
| --- | --- |
| `txn latency` | TwoPLPasha Executor 未更新该每秒窗口字段（读取的是 Worker 中默认初始化为 0 的字段） |
| `lock` / `queued lock latency` | 遗留监控字段，UB requester-side 锁路径尚未接入 |
| `active_txns` | 当前执行器未维护该窗口统计 |

### 应查看的真实延迟结果

事务真实延迟由通用 Executor 记录：每次事务完成时写入 `commit_latency`，commit 成功时写入总延
迟、分布式事务延迟和本地事务延迟，Worker 退出时输出 P50/P75/P95/P99。日志中应能看到：

```text
average commit: ...
Worker 0 latency: ... us (50%) ... us (99%)
dist txn latency: ...
txn commit latency: ...
Executor 0 exits.
```

其中 `average commit` 是去除 warmup/cooldown 后的平均 txn/s；`dist txn latency` 在
`cross_ratio=100` 时最值得关注。

```bash
grep -E 'average commit|Worker [0-9]+ latency|txn commit latency|Executor .* exits|Dispatcher exits' \
  /tmp/<实际日志文件> | tail -n 50
```

## 当前支持范围与故障排查

在两台 UB 主机实际完成步骤 4、5 前，已完成真机验证的基线仍是此前的点操作范围。当前源码已经包含事
务型 YCSB range/mixed、requester-side next-key/end-gap protection，以及全部 TPCC 主表/二级索引
表的 UB B+ Tree adapter。这些新增部分通过了仅主机 B+ Tree 测试和模板检查，但尚未在本工作区完成
真实 UBS Memory 运行。

跨-key tuple/node reclamation、通用 N-node shutdown 和 crash recovery/WAL replay 仍未实现。

### 常见错误

- `ubs_mem.h was not found`：修正 `UBSM_INCLUDE_DIR`。
- `libubsm_sdk.so: cannot open shared object file`：检查 `LD_LIBRARY_PATH` 和 `ldd`。
- AArch64 上出现 `immintrin.h: No such file or directory`：更新到 portable source，其中 legacy
  B+ Tree spin loop 使用 `UBCpu.h`，x86 CXL cache intrinsic 有架构保护。不要把 x86 intrinsic
  header 安装或复制到 ARM 主机；更新后用 `TIGON_ENABLE_UB=ON` 重新运行 CMake。
- `UBSM_ERR_IN_USING`（`6024`）：旧进程或 mapping 仍引用 object。不要强制删除 live region；停止两
  端旧进程，或在排查时使用新 prefix。
- `UBSM_ERR_NOT_SUPPORTED`（`6025`，one-sided）：检查 BIOS snoop/NC-CC 及
  `/sys/bus/ub/ub_feature` compatibility。
- `UBSM_ERR_NET`（`6040`）：检查 UBS node discovery 和 control-plane connectivity。
- `UBSM_ERR_UBSE`（`6050`）：检查 `ubse.service`、OBMM resource pool 和 UBS Engine log。若
  出现在 **region 创建阶段**（堆栈落在 `star::UBMemory::initialize()`），最常见的实际原因是
  **申请的 region 太大**，见下一条。
- `ipc call create with provider failed ret=800` / `UBS memory error=6050`，堆栈为
  `star::UBMemory::initialize()` → `__cxa_rethrow` → `std::terminate` → `abort`：provider
  定位已成功，失败发生在 UBSE 实际创建 owner region 时。`ret=800`（`MXM_ERR_UBSE_INNER`）是通
  用包装错误，底层真实原因要看 `ubsmd` 日志中 `pUbseMemShmCreateWithLender failed, ret=<底层
  错误码>` 那一行（可能是 provider NUMA 的 OBMM pool 不足、hugepage 不足、单对象大小限制或
  mem-id/FD 数量限制）。典型场景：correctness test 的 128 MiB 成功，YCSB 的 4096 MiB 失败。
  排障步骤：
  1. 报错后立即查日志：
     ```bash
     journalctl -u ubsmd -b --no-pager | \
       grep -iE '<region_prefix>|ShmCreateWithProvider|failed' | tail -n 100
     journalctl -u ubse.service -b --no-pager | \
       grep -iE '<region_prefix>|memory|mempool|obmm|alloc|failed|error' | tail -n 200
     dmesg | grep -iE 'obmm|ubse|alloc|huge|memory|mempool' | tail -n 200
     ```
  2. 用基础程序绕过 Tigon 直接验证（每次用唯一 `--name`）：
     ```bash
     for size in 128 256 512 1024 2048 4096; do
       echo "testing ${size} MiB"
       tests/ubs-mem-two-node/build-ubsm/ubsm_local_one_sided_test \
         --provider-host "$UB_PROVIDER_HOST" --provider-numa "$UB_PROVIDER_NUMA" \
         --name "ubsm_size_${size}_001" --region-mb "$size" --test-bytes 4096 || break
     done
     ```
  3. 按「步骤 2」的三步流程用 256 MiB 起步即可，YCSB 20 万 key 实际只需约 250–300 MiB（含
     tuple header、key/value、对齐、B+ Tree node、catalog，以及 UB transport 的约 32 MiB
     inbox：4096 entries × 约 8 KiB/entry）。
- `IpcCallShmMap response error : 606` / `Failed to mmap, name=<region>, mapSize=..., ret=606`（在
  `UB_RESULT_DIR` 或终端里出现，通常连着几行）：这是对端 region 此刻还不可映射（常见于两端启动
  有时间差），`map_region_with_retry()` 会每 100 ms 重试（`UBMemory.cpp:178-195`）；**后续重试成功
  就完全无影响**，日志里紧接着仍是正常跑完的 `TIGON_RUN_END ... exit_code=0`。只有一直失败到
  `--ub_map_timeout`（默认 120 秒）才会抛 `map region ...`，那时按下面 peer timeout 那条排查。
- 运行结束但终端没有任何输出：不是故障。`bench_ycsb` 的运行信息和吞吐量都用 glog `LOG(INFO)`
  输出，默认写入 `/tmp` 下的日志文件；它也不会像 `ub_tigon_two_node_test` 那样打印 `PASS`。
  确认方法：`echo $?`（0 为正常退出；134 通常为 abort，139 通常为段错误）；运行时长应明显超过
  初始化 + warmup + run 时间；`ls -lt /tmp/*bench_ycsb* | head` 找到日志后查看。建议所有
  benchmark 命令都加 `--logtostderr=1`，需要留档时再用 `2>&1 | tee <log>` 同时保存。
- 每秒统计中大量字段为 0：先对照上文「运行统计解读」。`commit`/`local_access`/`remote_access`
  非零且三者比例自洽（`access ≈ commit × 每事务本地/远程操作数`）即说明主路径正常；真正反映延迟
  的是结束时的 `average commit`、`Worker N latency` 分位数和 `dist txn latency`。
- 运行中终端长时间无输出、看起来卡住（TPCC 首次运行尤其常见）：先按"不是故障"处理。TPCC 的
  初始化是**单行插入、每张表各自串行、全程只写 glog**，规模按 warehouse 计：`item` 100,000 行
  （只有一个 partition，固定由一台主机写入），每个 warehouse 另有 `stock` 100,000、`customer`
  30,000、`order` 30,000、`order_line` 约 300,000（10 district × 3,000 order × O_OL_CNT，常量见
  `benchmark/tpcc/Schema.h`）；`--partition_num` 默认 `2 × threads`，所以首发配置约 1.2M 行，
  每行都要走 UB B+ Tree 的加锁与可能的分裂。**几分钟量级是正常的，30 秒远远不够**。而 matrix
  脚本不传 `--logtostderr=1`，终端从 `TIGON_RUN_META` 之后就完全静默。三步定位：
  1. 别看终端，看日志：`ls -lt /tmp/*bench_tpcc* | head -3`，再 `tail -f` 最新那个。正常会依次
     出现 `creating hash tables for database...` 和每张表的 `<name> initialization finished in
     N ms`，卡在哪张表一目了然；
  2. 判断进程是忙还是堵：`ps -o pid,stat,%cpu,etime,cmd -p <pid>` —— `R` 且接近 100% = 在加载或
     自旋，`S` 且 0% = 阻塞在 UBS 调用；
  3. 要精确到栈：`gdb -p <pid> -batch -ex 'thread apply all bt' | head -80`。常见自旋点是
     `UBBPlusTree::lock_shared()` / `lock_exclusive()`（`UBBPlusTree.h:424-442`，对 `tree_lock`
     无限重试）和 `TableUBBPlusTree::native_tree()`（`UBBPlusTreeAdapter.h:225-235`，等对端创建
     descriptor）。
  另有一段静默在初始化**之后**：`bench_tpcc` 在 `connectToPeers()` 之前会先调用一次
  `db.check_consistency()`（`bench_tpcc.cpp:66`），它会完整扫描本机每张表。所以"初始化日志已经打完
  但终端仍然没有输出"也可能停在这里，同样不是故障。
  还有一段"卡住"是**有上界、不要提前 Ctrl-C** 的：region bootstrap 里
  `map_region_with_retry()` 每 100 ms 重试一次，直到 `--ub_map_timeout`（默认 120 秒）才抛
  `map region ...`；`validate_remote_header()` 等对端把 `header->ready` 置 1 也是同一个 deadline
  （`UBMemory.cpp:178-256`）。两段都静默，所以前两分钟没有任何输出是正常的。
- peer mapping/header timeout：真等到超时了，就对比两台主机的 prefix、mode、region size、
  coordinator count、provider hostname 和启动顺序（同一个 `--ub_map_timeout` deadline 同时约束
  SDK mapping 和等待 peer 发布 region header），并考虑把 `--ub_region_mb` 降一档。
- 出现 `cxlalloc_* was called while Tigon is using the UB backend` fatal message：进入了尚未
  适配的 legacy CXL path，例如选择了不支持的 workload。
- `refusing to run N point(s) x M repetition(s) in one VM lifecycle`（spr4 侧 compare，返回码 2）：
  这不是故障，而是脚本的保护性拒绝。一次 VM 生命周期只能跑一个 point，跑下一个前先重建或重启 VM；
  只有 `SPR4_ALLOW_VM_REUSE=1` 能绕过，且仅限诊断用途。详见「步骤 6」的「spr4 侧对比与结果配对」。
- `SIGSEGV(@0x0)`，栈为 `google::(anonymous namespace)::FailureSignalHandler` →
  `star::Coordinator::connectToPeers` → `main`（退出码 139）：`--servers` 里有一项不是 `IP:port`。
  `Coordinator.h` 的 `getAddressPort()` 按 `:` 切分后不检查段数，调用点直接取 `addressPort[1]`
  （listener 线程在 `:504`，主线程在 `:542`）；缺端口时读到的是缓冲区尾后的堆内存，
  `std::string::c_str()` 返回空指针，`atoi(nullptr)` 读地址 0，于是崩在 `connectToPeers`。
  检查方法（期望 2 项、每项 1 个冒号）：
     ```bash
     servers='192.0.2.10:10010;192.0.2.11:10010'
     IFS=';' read -ra e <<< "$servers"
     for x in "${e[@]}"; do echo "entry=[$x] colons=$(tr -cd ':' <<<"$x" | wc -c)"; done
     ```
  注意**端口写错（而非漏写）不会 segv**：那是重试 50 次后 `LOG(FATAL) failed to connect to peers`，
  退出码 134。两种症状要分开判断。此路径对 `--servers` 的校验目前只存在于脚本使用说明层面，
  漏写端口在任何 `bench_*` 手动命令上都是同一个 `SIGSEGV(@0x0)`。

## 生命周期约定

正常关闭顺序如下：停止 worker、message producer 和 dispatcher；unmap 全部 imported region；执行
最终 TCP peer barrier；unmap/deallocate 本地 owner region；调用 `ubsmem_finalize()`。当前
coordinator 对支持的单节点/双节点 YCSB 路径执行该顺序。

单节点没有 peer socket 或 imported region，因此跳过 TCP peer barrier。通用 N-node shutdown、crash
cleanup 和 replay 不属于当前 milestone。
