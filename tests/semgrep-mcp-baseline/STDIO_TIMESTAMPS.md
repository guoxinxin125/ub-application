# 单机 stdio：SDK 与管道收发细分

只在 A 运行；B 不用修改。它沿用真正的 MCP stdio transport 和 Semgrep 工具，**不插入转发进程、不改 SDK 文件、不改内核**。保持原来的扫描打点，在同机两个 Python 进程间用 `perf_counter_ns` 配对收发。每次请求的九段之和必须等于客户端端到端时长，否则报错，不裁剪负值。

## 1. 准备

将完整测试目录同步到 A。使用之前已安装扫描打点的同一 Semgrep 私有虚拟环境：

```bash
source ~/venvs/semgrep-1.182/bin/activate
cd /你的路径/semgrep-mcp-baseline
python3 -c 'import importlib.metadata; print(importlib.metadata.version("mcp"))'
```

当前适配 MCP SDK **1.29.0**，对应这个 Semgrep 版本的依赖；其它版本会明确拒绝，不要在共享环境里盲目升级依赖。之前 `--profile` 已成功的机器不用重新运行 `install_profile.py --apply`。环境仍保留关闭 tracing、metrics、`--no-auth` 的本地测试配置。

## 2. 一个样本确认

```bash
RUN="results/stdio-boundary-check-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"
python3 scripts/run_mcp.py --transport stdio \
  --source fixtures/cases/comments_1k.py \
  --profile --engine-time --stdio-timestamps --warmup 0 --samples 1 \
  --output "$RUN/stdio-comments_1k.json"
python3 scripts/check_findings.py "$RUN/stdio-comments_1k.json" \
  --expected fixtures/cases/comments_1k.py.expected.json
python3 scripts/summarize_profile.py "$RUN" --output "$RUN/summary.json"
```

应看到 `FINDINGS_PASS`，汇总出现 `STDIO/request_ipc_ns`、`STDIO/response_ipc_ns` 等项。客户端自动启动自己的带读写观察的 Semgrep 子进程；不需要你手动另起 stdio 服务。未加 `--stdio-timestamps` 时仍走原路径。

## 3. 六个用例整批

```bash
RUN="results/stdio-boundary-batch-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"
for f in fixtures/cases/*.py; do
  name=$(basename "$f" .py)
  python3 scripts/run_mcp.py --transport stdio --source "$f" \
    --profile --engine-time --stdio-timestamps --warmup 10 --samples 100 \
    --output "$RUN/stdio-$name.json" || break
  python3 scripts/check_findings.py "$RUN/stdio-$name.json" \
    --expected "$f.expected.json" || break
done
python3 scripts/summarize_profile.py "$RUN" --output "$RUN/summary.json"
```

每个文件内连接复用，原扫描后端仍每次走 CLI。保留旧结果目录，不覆盖。进程启动/初始化在测量区间之外。

## 九段时间线

| 字段 | 边界与含义 |
|---|---|
| `client_before_queue_ns` | call_tool 开始 → 客户端向 SDK transport 排队；含请求对象准备 |
| `client_encode_queue_ns` | SDK 排队 → 向管道发送已编码帧；含 SDK 序列化、排队及观察开销 |
| `request_ipc_ns` | 客户端开始写请求帧 → 服务进程读完该帧；含写入、唤醒、调度和缓冲区读取 |
| `server_decode_queue_ns` | 服务端读完帧 → 解析后消息交给 session；含 JSON 校验、内部队列和观察开销 |
| `server_processing_ns` | session 收到请求 → 响应交给 transport；包含工具扫描、外层 wrapper、指标与 dispatch |
| `server_encode_queue_ns` | 响应进入 transport → 已编码帧开始写回；包含序列化、排队和观察开销 |
| `response_ipc_ns` | 服务端开始写响应帧 → 客户端读完；含写入、唤醒、调度和缓冲区读取 |
| `client_decode_queue_ns` | 客户端读完 → 解析后消息交给 session |
| `client_dispatch_ns` | 客户端 session 收到响应 → call_tool 返回；含结果模型校验等 |

`server_framework_nested` 是 `server_processing_ns - tool_total_ns`，能进一步观察原本混在“工具外”的 MCP wrapper/指标等处理。它是嵌套项，不要重复相加。

原 `phases` 中的真实扫描细分及 `native_engine_diagnostics` 均保留。新的 `stdio_partition` 是同一次调用的另一份完整分解，不应和旧 `phases` 再相加。占比按配对时长总和计算，不做 p50 相减。

## 测量边界与开销

- 同机 Linux 的 boot ID、time namespace 和 perf_counter 实现由父子进程核对，不使用跨机 UTC 相减。若不一致会报错。
- 同步写的完成时间与对方读完时间可能互相交错，所以分段统一使用**开始写 → 对方读完**，额外保留 write_done 仅供诊断，避免重叠相加。
- SDK JSON 编码/解析与内部排队并未强行拆开；观察代码为按 RPC ID 关联帧会额外解析少量 JSON，这项开销留在 encode/decode/排队项里。结果不是无扰动的纯 JSON 编解码成本。
- 服务端只把 ID 和时间戳写到私有临时侧日志，不在 MCP stdout 中混入日志。每次响应 flush 后记录一次，可能影响后续调度；属于打点开销。客户端在自己的子进程退出后合并，再删除临时目录，最终原始时间戳保存在结果 JSON。
- SDK 创建子进程时，客户端临时替换该 SDK 的进程工厂，为新进程自身的 stdin/stdout 方法加观察；向调用方交出会话前恢复工厂。不支持同一 Python 进程同时建立其它 stdio 会话。进程本体和原 SDK 的退出清理逻辑保持不变。

本地自动测试覆盖真实子进程管道、侧日志配对及时间段求和，尚不代表 AArch64 Semgrep + MCP 的完整真机验收。请先跑上面的单样本命令；若缺帧或打点缺失，保留报错，不用猜测补值。
