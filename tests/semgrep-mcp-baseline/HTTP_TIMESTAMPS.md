# 更细 HTTP 打点：上传/返回粗估与时钟误差

本功能按“先假设 A/B 时钟接近”收集粗估数据，默认假定 `B时间-A时间=0`，**不是确认同步，也不是测得纯网络时间**。保留原始 UTC 和 monotonic 时间戳，可检查误差、重算；负数保留并报警，不裁为零。

## 1. B：换用带 HTTP 边界的启动入口

同步整个测试目录到 A/B。仍使用之前安装了 `install_profile.py --apply` 的同一 Semgrep 私有环境；如果已经安装过，无需重复安装。本功能不再改 site-packages，仅用一个启动脚本给原 Semgrep MCP 应用加 ASGI middleware。

先停止**自己启动的**旧 HTTP MCP 进程，不要杀其它用户的进程。B 上从本测试目录执行：

```bash
source ~/venvs/semgrep-1.182/bin/activate
export SEMGREP_MCP_PROFILE=1
export SEMGREP_MCP_PROFILE_ENGINE_TIME=1
export SEMGREP_MCP_DISABLE_TRACING=true
B_BIND='填写B原来使用的受控监听地址'
python3 scripts/run_profiled_http.py --host "$B_BIND" --port 8000 --no-auth
```

`--no-auth` 仅用于已经限制访问的测试环境，不能把无认证扫描服务暴露到公共网络；需要认证就不传此参数。原 tmpfs helper 和工具实现照常使用。监听地址、端口、metrics 环境等保持与原实验一致并记录。若通过 SSH 隧道访问，可让 B 监听 `127.0.0.1`，但测得的边界耗时也包含隧道。

脚本使用你们修改版 `setup_mcp_server(host, port, no_auth=...)`，保留原工具注册、生命周期及认证设置；默认 JSON 响应。不涉及内核模块或系统时钟调整。

## 2. A：先测一个样本，再测整批

在 A 激活同一私有环境，从本测试目录执行：

```bash
MCP_URL='http://B的实际受控地址:8000/mcp'
RUN="results/http-boundary-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"
python3 scripts/run_mcp.py --transport http --url "$MCP_URL" \
  --source fixtures/cases/comments_1k.py --profile --engine-time --http-timestamps \
  --warmup 0 --samples 1 --output "$RUN/check.json"
python3 scripts/summarize_profile.py "$RUN"
```

默认假设偏差为 0；汇总出现 `clock_error_bound_unknown` 是刻意的提醒，不是扫描失败。确认没有“服务端时间戳缺失”等错误后，新建一个目录测六个文件：

```bash
RUN="results/http-boundary-batch-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"
for f in fixtures/cases/*.py; do
  name=$(basename "$f" .py)
  python3 scripts/run_mcp.py --transport http --url "$MCP_URL" --source "$f" \
    --profile --engine-time --http-timestamps --warmup 10 --samples 100 \
    --output "$RUN/http-$name.json" || break
  python3 scripts/check_findings.py "$RUN/http-$name.json" \
    --expected "$f.expected.json" || break
done
python3 scripts/summarize_profile.py "$RUN" --output "$RUN/summary.json"
```

只测 HTTP 支持此模式，stdio 不支持。服务器必须是上面的启动入口，否则客户端会报“HTTP server timestamps missing”。自动重试/重定向出现多个 tools/call POST 时拒绝盲目配对。SSE 或响应未读完也拒绝生成单向估计。

## 3. 时间线和结果解释

```
A a_send：HTTPX 请求 hook，JSON 已编码，尚未调用底层发送
         ↓ socket/连接池/协议栈、请求体传送、B 调度和读取
B b_body：ASGI 最后一个请求体分块交给 MCP 应用
         ↓ MCP 解码/框架/扫描/编码，可能还有 list_roots 等内部往返
B b_send：ASGI 响应头即将交给服务器发送
         ↓ B 写入、协议栈、响应传送、A 调度与读取
A a_body：HTTP 响应体读完，尚未交给 MCP 解析
```

| 字段 | 意义 |
|---|---|
| `upstream_estimate_ns` | `b_body - a_send - offset`：整个 MCP 请求到达应用的单向粗估，包含源码、规则、JSON 和 HTTP 开销，不只是源码字节 |
| `downstream_estimate_ns` | `a_body - b_send + offset`：整个响应返回粗估，不含 B 发送前已完成的 JSON 编码，但包括发送/读取/调度 |
| `combined_http_boundary_ns` | A 的 HTTP 往返 monotonic 时长减 B 从 body 到 headers 的 monotonic 时长；固定时钟偏差基本抵消，优先看这个 |
| `server_body_to_headers_ns` | B 的应用处理间隔；与原 `tool_total_ns` 不同，还包含框架/编码和可能的内部 RPC |
| `server_request_body_read_ns` | B 应用入口到 body 读完，本机间隔；可能只反映缓冲区读取，不等于上传时间 |
| `client_response_body_read_ns` | A 从响应头到响应体读完；小响应可能早已缓存，不能把它当完整下载时间 |
| `client_sdk_outside_http_ns` | MCP call_tool 总耗时减 HTTP hook 到 body 完整读取的间隔，主要为 hook 外的 SDK/编码/解析/排队 |

汇总显示各项均值、p50/p99及其相对同组端到端总时长的占比。**HTTP 边界是一组新的、与旧 phases 重叠的观察视角，不可把这些诊断项再次加入原 phases 求和。** 真正单向“在网线上飞行”的时间依旧不在此测量范围。

## 4. 偏差有多大，结果就有多不确定

若 B 时钟比 A 快 1 ms，未校正时上传会多算约 1 ms，返回会少算约 1 ms。两台显示秒数接近，仍可能不足以估计一次微秒级上传。只要 B-A 偏差近似恒定，双向合计基本不受这个偏差影响。

知道偏差后，在客户端添加 `--clock-offset-us <B减A的微秒值>`；只有有外部同步证据时才添加 `--clock-uncertainty-us <误差上界>`，不要随意填一个很小的数。原始每样本记录还提供单向上下界及 wall/monotonic 的一致性差异。

出现 `negative_interval` 或 `wall_monotonic_disagreement` 时，单向百分比不可作为可靠结论。即使没有负数，也不能证明同步准确；误差上界未知时始终保留 `clock_error_bound_unknown`。第一轮建议汇报“按零偏差假设估算的上传/返回”及双向合计，不写成精确网络百分比。

本地自动测试覆盖了真实 HTTPX hooks + ASGI middleware 的配对、偏差敏感性与负值处理，不代表已在真实 UB 网络完成验收。
