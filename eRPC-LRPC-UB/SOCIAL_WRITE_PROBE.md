# UB 服务域远端写入探针

这是 ComposePost 接入前的底层验证，不是发帖实现。现有 `--social` owner 和
`us_client_open()` 仍为只读；只有显式选择 `--social-write` 与
`us_client_open_write()` 才启用 B 持有的可写数据页。每个连接槽有独立的
4 KiB 页；服务域页表里的 B 配置仍是只读，代码仍在 A 本地 RX，
这不是对 A 导入进程本身的安全隔离。A 内核验证导入映射的
NC/可写 PTE 与固定偏移的物理页，并校验 B 发布的精确页表与代码镜像。
域内 nginx 测试服务把 `value` 写到 `request & 511` 对应的 64 位槽，
然后读回到 `ticket`。A 探针可做单次写、多个不同槽位写、同槽位重复写，
也可用多个固定在不同 CPU 的线程同时持有独立连接；每个连接仍只写自己的页。
B owner 只作为观察者打印实际看到的变化，不参与服务执行。该探针
不保证持久化、跨连接的原子事务或数据库写入。重复写的中间值可能被
B 的轮询跳过，验收只比较每个槽位的最终值。

本次页表契约和连接池 ABI 均升为 2。A 的用户库/模块与 B 的 owner
必须来自同一份源码；旧二进制不可混用。先确认共享机器上没有其他用户
正在使用本项目的 `/dev/ub_lrpc_domain` 或同名 owner，再在维护窗口内
替换自己加载的模块；不要卸载别人的模块，也不需要修改启动配置。
本次探针扩展只改了 A 的用户态探针、B 的 owner 和校验脚本，未更改
内核模块或 ABI；如果当前已加载模块的 `ud_social` 是文档要求的 ABI 2
版本，不必为了本次扩展重新 `insmod`。首次部署或模块版本不匹配时才按
下面的构建/替换步骤操作。
这段只适用于原有 `--social-write` 私有页探针；新增的 `--social-shared`
是另一种 profile，需要按其独立文档重新编译并替换本项目模块。

两端先执行 `python3 scripts/manifest.py --check`，再按各自已有的
`UBSM_*`/`OBMM_*` 环境变量编译：

```sh
# B：构建并在一个终端保持运行；NUMA 1 仅是 A NUMA 0/B NUMA 1 的示例。
sh scripts/build.sh owner
./build-owner/lrpc-ub-domain-owner lrpc_ub_domain --provider-numa 1 --social-write \
  2>&1 | tee write-owner.log

# A：独立终端，使用与正在运行的内核匹配的 kernel-devel。
sh scripts/build.sh caller
sh scripts/build-module.sh --vhe   # VHE-EL2 主机；EL1 主机去掉 --vhe
modinfo -F ud_social kernel/ub-domain/ub_lrpc_domain_vhe.ko
# 停止本项目所有旧连接后，按 README 的 EL1/VHE 模块更换步骤加载新模块。
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1 ./build-caller/lrpc-ub-domain-write-probe \
  2>&1 | tee write-a.log
python3 scripts/check-write-probe.py --a-log write-a.log --b-log write-owner.log
```

A 输出 `UB_DOMAIN_WRITE_A_PASS slot=S identity=I index=1 value=42 ticket=42`
只证明域内写入和 A 的读回。B owner 必须先打印同一个 `slot=S identity=I` 的
`UB_DOMAIN_WRITE_SLOT_READY`，再打印 `UB_DOMAIN_WRITE_OBSERVED`。
离线校验脚本逐个比较所有 A 最终值与 B 最后观察值，成功时输出
`UB_DOMAIN_WRITE_VISIBILITY_PASS`；`UB_DOMAIN_WRITE_CAMPAIGN_PASS` 本身
不代表 B 已观察。A/B 两个日志应来自**同一轮运行**，且每轮使用单独的
owner 日志，避免旧轮次混入。`tee` 的目录应是本项目的私有工作目录。
如果只看到 A 的 PASS 而 B 未观察到，不要直接用于业务写入；保留两侧日志、
模块版本、NUMA、CPU、内核版本及 `dmesg` 中的 `ub_domain` 记录排查。
若只是 B 日志稍晚到达，可等待几秒后仅重跑离线校验脚本，不要自动重发写请求。

单次写通过后，在 A 依次进行以下两轮。每轮建议重启**本项目自己的**
`--social-write` owner 并使用新的日志文件；不要影响共享机器上的其他进程。
`--calls N` 写索引 `1..N` 的不同槽位，最多 64；`--rewrites N` 随后
重写索引 1，最多 10000 次。`--observe-ms` 默认保持连接 2 秒供 B 轮询；
繁忙机器可调高到最多 30 秒。该等待不计入性能数据。

```sh
# B：每轮分别启动，先结束上一轮本项目 owner；命令在 B 的终端执行。
./build-owner/lrpc-ub-domain-owner lrpc_ub_domain --provider-numa 1 --social-write \
  2>&1 | tee write-owner-repeat.log
# 并发轮重新启动时把日志文件改为 write-owner-concurrent.log。

# A：同一连接的独立槽位和重复写。B 日志例如 write-owner-repeat.log。
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1 ./build-caller/lrpc-ub-domain-write-probe \
  --calls 8 --rewrites 100 --observe-ms 5000 2>&1 | tee write-a-repeat.log
python3 scripts/check-write-probe.py \
  --a-log write-a-repeat.log --b-log write-owner-repeat.log

# A：两个连接/两个不同 CPU/各自独立写页。B 日志例如 write-owner-concurrent.log。
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1,2 ./build-caller/lrpc-ub-domain-write-probe \
  --cpus 1,2 --calls 8 --rewrites 100 --observe-ms 5000 \
  2>&1 | tee write-a-concurrent.log
python3 scripts/check-write-probe.py \
  --a-log write-a-concurrent.log --b-log write-owner-concurrent.log
```

`1,2` 只是示例，必须换成 A 机器当前允许且空闲的两个不同 CPU。
先确认单连接结果，再增加连接；最多 8 个连接。这个并发实验检测的是
不同连接私有页的写入隔离，**尚未测试多个连接争用同一个 B 状态字的原子性**。
故障/重连和真正共享状态竞争留到后续单独设计，不用本探针的 PASS 代替。
共享页跨连接读写与原子计数的独立实验见
[SOCIAL_SHARED_PROBE.md](SOCIAL_SHARED_PROBE.md)。

下一步 ComposePost 需要另外定义 B 侧业务状态、PostStorage 与两种
Timeline 的更新协议及失败恢复。不能把这个 64 位槽探针直接当成
PostStorage 写接口，更不能用 B owner 观察日志代替数据库确认。
