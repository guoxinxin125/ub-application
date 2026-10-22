# UB 服务域远端写入探针

这是 ComposePost 接入前的底层验证，不是发帖实现。现有 `--social` owner 和
`us_client_open()` 仍为只读；只有显式选择 `--social-write` 与
`us_client_open_write()` 才启用 B 持有的可写数据页。每个连接槽有独立的
4 KiB 页；服务域页表里的 B 配置仍是只读，代码仍在 A 本地 RX，
这不是对 A 导入进程本身的安全隔离。A 内核验证导入映射的
NC/可写 PTE 与固定偏移的物理页，并校验 B 发布的精确页表与代码镜像。
域内 nginx 测试服务把 `value` 写到 `request & 511` 对应的 64 位槽，
然后读回到 `ticket`。B owner 只作为观察者打印实际看到的变化，
不参与服务执行。该探针不保证持久化、跨连接的原子事务或数据库写入。

本次页表契约和连接池 ABI 均升为 2。A 的用户库/模块与 B 的 owner
必须来自同一份源码；旧二进制不可混用。先确认共享机器上没有其他用户
正在使用本项目的 `/dev/ub_lrpc_domain` 或同名 owner，再在维护窗口内
替换自己加载的模块；不要卸载别人的模块，也不需要修改启动配置。

两端先执行 `python3 scripts/manifest.py --check`，再按各自已有的
`UBSM_*`/`OBMM_*` 环境变量编译：

```sh
# B：构建并在一个终端保持运行；NUMA 1 仅是 A NUMA 0/B NUMA 1 的示例。
sh scripts/build.sh owner
./build-owner/lrpc-ub-domain-owner lrpc_ub_domain --provider-numa 1 --social-write

# A：独立终端，使用与正在运行的内核匹配的 kernel-devel。
sh scripts/build.sh caller
sh scripts/build-module.sh --vhe   # VHE-EL2 主机；EL1 主机去掉 --vhe
modinfo -F ud_social kernel/ub-domain/ub_lrpc_domain_vhe.ko
# 停止本项目所有旧连接后，按 README 的 EL1/VHE 模块更换步骤加载新模块。
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1 ./build-caller/lrpc-ub-domain-write-probe
```

A 输出 `UB_DOMAIN_WRITE_A_PASS slot=S index=1 value=42 ticket=42` 只证明
域内写入和 A 的读回。还必须在 B 终端看到同一 `slot=S index=1 value=42` 的
`UB_DOMAIN_WRITE_OBSERVED`，才能认为这次双机写入可见性测试通过。
若 B 未观察到，不要直接用于业务写入；保留两侧日志、模块版本、NUMA、
CPU、内核版本以及 `dmesg` 中的 `ub_domain` 记录排查。首次验证只跑
一个连接；之后再测不同槽位并发、重复写、失败/重连与写后读。

下一步 ComposePost 需要另外定义 B 侧业务状态、PostStorage 与两种
Timeline 的更新协议及失败恢复。不能把这个 64 位槽探针直接当成
PostStorage 写接口，更不能用 B owner 观察日志代替数据库确认。
