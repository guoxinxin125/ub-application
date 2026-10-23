# UB 双机共享页读写与原子更新探针

这是独立于 SocialNetwork 业务数据的正确性实验，不是 ComposePost 或数据库写入。
原来的 `--social-write` 是每连接一页；这里显式选择 `--social-shared`，
所有连接把**同一块 B 持有的 4 KiB 页**映射到服务域的 `UD_WRITE`。
页中索引 1 是普通读写单元，索引 2 是原子计数器。其他 profile 不变；
B 配置页仍只读，A 本地代码仍为 RX。此实验只在双方可信的测试环境运行，
不构成对导入进程或其他服务的访问隔离。

调用顺序：先由不同连接轮流写索引 1，每轮让所有连接读回相同值；
再让所有连接同时对索引 2 执行 `LDAXR/STLXR` 的有界 fetch-add。
A 校验返回的旧值恰好覆盖 `0..总次数-1`，无重复和缺号，最后从域内读回计数器；
B owner 独立轮询并记录两个最终值。每次原子操作最多重试 256 次，
重试耗尽返回 `EAGAIN` 并使本轮失败；异常/不支持的 UB 独占访问同样不能记为通过。
**本地 mock 通过只证明流程和检查器工作；只有真机双端通过才能证明该 UB
映射上的跨连接原子更新可用。** 即使通过，也不代表跨机多对象事务或故障恢复正确。

此新增 profile 使用现有 ABI 2 结构，但 A/B 用户态和 A 内核模块都必须由同一份
更新后的源码重编译；已加载的旧模块不支持 profile 3。共享机器上先确认没有
其他人使用本项目模块/owner，只在自己的维护窗口更换自己加载的模块，不修改启动配置。
每轮先关闭本项目旧连接，再重启**本项目自己的** `--social-shared` owner：
共享计数器只在 owner 初始化时清零，A 探针也会拒绝非零初始状态。
两端同步目录后执行 `python3 scripts/manifest.py --check`。

```sh
# B：在自己的终端持续运行；NUMA 1 仅是 A NUMA 0/B NUMA 1 组合的示例。
sh scripts/build.sh owner
./build-owner/lrpc-ub-domain-owner lrpc_ub_domain --provider-numa 1 --social-shared \
  2>&1 | tee shared-owner.log

# A：另一个终端；选择两个当前允许且空闲的 CPU，下面的 1,2 仅为示例。
sh scripts/build.sh caller
sh scripts/build-module.sh --vhe   # VHE-EL2；EL1 主机不加 --vhe
modinfo -F ud_social kernel/ub-domain/ub_lrpc_domain_vhe.ko
# 停止本项目旧连接、按 README 中对应 EL1/VHE 步骤加载刚编译的模块。
sudo env LRPC_UB_DOMAIN_NAME=lrpc_ub_domain \
  taskset -c 1,2 ./build-caller/lrpc-ub-domain-shared-probe \
  --cpus 1,2 --rounds 4 --increments 100 --observe-ms 5000 \
  2>&1 | tee shared-a.log
python3 scripts/check-shared-probe.py --a-log shared-a.log --b-log shared-owner.log
```

首次通过后，可把 `--increments` 增至 1000 或 10000，最多 8 个不同 CPU/连接；
`--rounds` 范围 2..64，`--observe-ms` 范围 0..30000 毫秒。
`UB_DOMAIN_SHARED_READ_PASS` 表示 A 侧跨连接读回正确；
`UB_DOMAIN_SHARED_ATOMIC_PASS` 表示 A 侧计数器及每次返回值正确；
只有离线检查器输出 `UB_DOMAIN_SHARED_VISIBILITY_PASS` 才说明 B 也观察到了
同一 generation 的最终单元与计数器值。B 轮询可跳过中间状态，因此不要求
B 观察到每次递增。A/B 日志必须来自同一轮 owner；若 B 最终观察晚到，可等几秒
只重跑检查器，不要自动重发原子操作。

若原子阶段失败，请保留 A/B 日志、`dmesg` 中的 `ub_domain` 记录、模块版本、
内核版本、CPU/NUMA 与 UBSM 映射方式。`EAGAIN`、异常向量或计数不一致
都应先当成平台语义或实现待诊断，不能宣称原子能力已验证。
