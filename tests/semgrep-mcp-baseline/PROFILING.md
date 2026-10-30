# MCP 扫描耗时拆分（可选开关）

新增 [HTTP 收发边界打点](HTTP_TIMESTAMPS.md)：可在假设时钟接近的条件下估算上传/返回，保留误差警告，并计算固定偏差基本抵消的双向合计。它使用新的服务启动入口，不需要重装已有 Python 打点。

单机 stdio 的更细管道与 SDK 打点见 [STDIO_TIMESTAMPS.md](STDIO_TIMESTAMPS.md)，通过 `--stdio-timestamps` 启用，默认路径保持不变。

目标：分析这六个真实 Semgrep 扫描请求中，输入准备、扫描后端、结果处理和工具外开销各占多少。使用现有 `semgrep_scan_with_custom_rule`，**不改变规则或扫描引擎，不是 UB 域内扫描，也不是 daemon 基线**。

## 1. A/B 安装 Python 打点

同步本测试目录到两台机器，先结束自己启动的 MCP 服务，再在各自的 Semgrep 私有虚拟环境中执行（路径按实际部署调整）：

```bash
source ~/venvs/semgrep-1.182/bin/activate
cd /你的路径/semgrep-mcp-baseline
python3 scripts/install_profile.py
python3 scripts/install_profile.py --apply
```

第一条仅检查。`--apply` 自动找到当前 Python 环境中的 `semgrep/mcp/server.py`，按 AST 只改 custom-rule 函数，保留其它函数（包括 tmpfs helper）和 `commands/mcp.py` 的修改。安装 `baseline_profile.py`，同时打印唯一的 `server.py.pre-profile-<hash>.bak` 备份路径。已有标记或关键代码不匹配会拒绝安装，不会盲目覆盖。不要加 sudo，也不要把补丁装到系统 Python。

只测跨机时 B 需要安装服务端打点；测 A 的本地 stdio 时 A 也需要安装。客户端脚本仅在 A 运行。**必须重启 MCP 服务，才会加载安装后的代码。无需重新编译或 insmod 内核模块。** 默认不开启计时，未开启时不添加结果字段。

## 2. B 开启打点并重启自己的 HTTP 服务

在 B 的实验终端设置：

```bash
export SEMGREP_MCP_PROFILE=1
export SEMGREP_MCP_PROFILE_ENGINE_TIME=1
export SEMGREP_MCP_DISABLE_TRACING=true
```

随后使用你之前已经验证成功的 **同一条 HTTP MCP 启动命令**（相同监听地址、端口和认证配置）。`PROFILE_ENGINE_TIME=1` 会给扫描命令加 `--time`，收集 Semgrep 原生内部统计；关闭此开关可测更轻量的外层打点。两组结果不要混合。

需要新建服务且只通过 SSH 隧道访问时，修改版 Semgrep 的示例是：

```bash
semgrep mcp -t streamable-http --host 127.0.0.1 --port 8000 --no-auth
```

它只监听 B 的回环地址，A 必须走你已建立的 SSH 转发。若用直接跨机端点，保留你已验证且受控的配置；不要把无认证服务暴露到公共网络。无需改已有防火墙、UB 服务和启动配置。

## 3. A 运行（同一连接预热后采样）

从本测试目录执行，只选择需要测的一种路径。`--samples 100` 的 100 次调用共用一个会话，预热不计入统计；每个文件仍使用独立的会话。

### A 本地 stdio

```bash
RUN="results/profile-local-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"
for f in fixtures/cases/*.py; do
  name=$(basename "$f" .py)
  python3 scripts/run_mcp.py --transport stdio --source "$f" \
    --profile --engine-time --warmup 10 --samples 100 \
    --output "$RUN/stdio-$name.json" || break
  python3 scripts/check_findings.py "$RUN/stdio-$name.json" \
    --expected "$f.expected.json" || break
done
python3 scripts/summarize_profile.py "$RUN" --output "$RUN/summary.json"
```

客户端保留了原先关闭 tracing 和 `--no-auth` 的设置，并为它自己的 stdio 子进程开启 profile，无需你额外 export。不会关闭其它用户的服务。每次扫描仍是 custom-rule 的 CLI 扫描：连接复用不是扫描器进程复用。

### 跨机 HTTP（也可填写同机 HTTP 端点）

```bash
MCP_URL='http://B的受控地址:端口/mcp'
RUN="results/profile-http-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"
for f in fixtures/cases/*.py; do
  name=$(basename "$f" .py)
  python3 scripts/run_mcp.py --transport http --url "$MCP_URL" --source "$f" \
    --profile --engine-time --warmup 10 --samples 100 \
    --output "$RUN/http-$name.json" || break
  python3 scripts/check_findings.py "$RUN/http-$name.json" \
    --expected "$f.expected.json" || break
done
python3 scripts/summarize_profile.py "$RUN" --output "$RUN/summary.json"
```

走隧道时 URL 填 A 的转发端点。HTTP 的开关必须设置在 B 的服务进程中；客户端的 `--profile`/`--engine-time` 会检查响应确实有打点，不会替你修改远端环境。缺失会报错，不会输出一个假占比。已有结果文件不会被覆盖。

首次安装可先只测一个文件、`--warmup 0 --samples 1` 确认打点出现，再运行整批。100 次约需该文件单次耗时的 100 倍，按之前约 0.2 秒一次估计，六个用例大约 2–3 分钟，实际依机器负载变化。

## 4. 字段怎么解释

每次响应的 `mcp_scan_results.baseline_profile` 带一个请求 ID 和服务端**时长**，不使用 A/B 的绝对时间相减，所以不要求时钟同步。客户端将同一次响应和端到端时长保存为一条 sample。

| 字段 | 范围 |
|---|---|
| `call_elapsed_ns` | A：`call_tool` 开始至完整返回；不含会话初始化、读取本地文件、客户端保存 JSON |
| `tool_total_ns` | B：实际 custom-rule 函数入口至返回，包含 finally 中的清理；不含外层 MCP wrapper 和响应编码 |
| `workspace_ns` / `validation_ns` | B：工作区上下文查询、输入校验 |
| `input_prepare_ns` / `rule_write_ns` | B：准备源码临时文件、写规则；tmpfs 也是文件操作，不是零成本 |
| `scan_args_ns` | B：构造扫描参数 |
| `scan_backend_ns` | B：运行 Semgrep 命令到输出返回；**包含进程启动、初始化、规则加载、扫描和输出等，不是纯解析匹配时间** |
| `result_decode_ns` / `normalize_ns` | B：解析扫描 JSON、整理返回路径 |
| `scan_metrics_ns` / `elicitation_ns` / `findings_metrics_ns` | B：指标或交互处理；未走到的阶段不出现 |
| `cleanup_ns` | B：删除临时目录 |
| `engine_profile_decode_ns` / `other_tool_ns` | B：额外读取原生统计，以及未被其它阶段覆盖的函数时间 |
| `outside_tool_ns` | A 端总时长减 B 工具时长：**双向传输 + 框架 + 编解码 + 排队调度 + 打点结果封装等** |

上面的服务端 phases 加 `outside_tool_ns` 构成端到端时长的分解。工具总时间在附加 profile 字段之前截断，添加/序列化打点数据的开销落在工具外项。打点会略增返回体积，因此比较 baseline 时也应统一开关，并单独保存无打点对照。

启用 `--time` 后，`engine_time` 保留原生 JSON；汇总额外显示 `native/cli_total_time`、`native/cli_config_time`、`native/cli_core_time`（若原生输出有这些字段）、规则解析和各目标 parse/match 时间和。该信息是 `scan_backend_ns` 内部的**嵌套诊断**，各项可能包含彼此或采用不同聚合口径，不能再次加入顶层百分比。

**不能把 `outside_tool_ns` 称为纯网络时间，也不能把它拆成单向上传和下载时间。** 做到后者需要同步时钟或额外传输层测量；本版不会编造这两个值。同机 HTTP 与跨机 HTTP 的差值也不是自动成立的网络时间。

汇总按 transport、端点、输入/规则哈希、服务端版本和原生计时开关分组，输出各阶段均值/p50/p99。占比使用 `sum(阶段时长) / sum(端到端时长)`；不做 p50 相减。若服务端阶段缺失、时间为负或阶段和不一致会明确报错。

旧 `cases-20261029-120220` 没有服务端时间，不能倒推出 breakdown；请保留，不覆盖。

## 5. 关闭或恢复

只关闭计时：去掉 A 的 `--profile --engine-time`；B `unset SEMGREP_MCP_PROFILE SEMGREP_MCP_PROFILE_ENGINE_TIME` 并重启自己的 MCP 服务。

恢复源码：先停止自己的 MCP 服务，把安装时打印的那份 `server.py.pre-profile-<hash>.bak` 复制回同目录的 `server.py`，再重启。该备份包含安装前已有的 tmpfs 等修改。`baseline_profile.py` 不再被导入，保留不影响行为。不要用原版 server.py 覆盖你自己的其它补丁。
