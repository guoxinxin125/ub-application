# 1 / 4 / 16 KiB 扫描输入测试

## 本轮测什么

使用已经跑通的 Semgrep MCP 服务，改变源码大小和有效语句量；不要求重新安装 Semgrep或修改 UB 模块。这些是可控的合成输入，不代表真实项目的代码复杂度。

| 文件 | 字节数（UTF-8 / LF） | 内容 |
|---|---:|---|
| `comments_1k.py` | 1024 | 注释填充 |
| `statements_1k.py` | 1024 | 简单赋值语句填充 |
| `comments_4k.py` | 4096 | 注释填充 |
| `statements_4k.py` | 4096 | 简单赋值语句填充 |
| `comments_16k.py` | 16384 | 注释填充 |
| `statements_16k.py` | 16384 | 简单赋值语句填充 |

文件均位于 `fixtures/cases/`，第 3 行固定一处 `eval(user_input)`，没有其他 eval 调用。固定使用 `rules/eval.yaml`，期望一处命中。源码只供扫描，不要执行。行数和大小见 `fixtures/cases/manifest.json`；同行数不代表同字节数，不能用行数代替请求大小。所有文件均为 ASCII，减少编码差异。

## 1. 准备结果目录（A）

同步整个 `semgrep-mcp-baseline/` 目录，在其根目录执行；使用你已安装 Semgrep 的虚拟环境：

```bash
source ~/venvs/semgrep-1.182/bin/activate
RUN="results/cases-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"
wc -lc fixtures/cases/*.py
sha256sum fixtures/cases/*.py rules/eval.yaml > "$RUN/input-sha256.txt"
semgrep --version > "$RUN/semgrep-version.txt"
```

下面 CLI、本地 MCP、跨机 MCP 是可选的运行路径，不必每轮全部重测。首次建议直接选择你已跑通的 MCP 路径。

## 2. MCP 批量运行（A）

### 本地 stdio

```bash
for f in fixtures/cases/*.py; do
  name=$(basename "$f" .py)
  python3 scripts/run_mcp.py --transport stdio --source "$f" \
    --output "$RUN/stdio-$name.json" || break
  python3 scripts/check_findings.py "$RUN/stdio-$name.json" \
    --expected "$f.expected.json" || break
done
```

### 同机或跨机 HTTP

先保持你已经验证过的 MCP HTTP 服务运行。在 A 设置实际端点；同机 HTTP 使用 `127.0.0.1`，跨机使用受控的 B 端点或 SSH 转发端点：

```bash
MCP_URL='http://127.0.0.1:8000/mcp'
for f in fixtures/cases/*.py; do
  name=$(basename "$f" .py)
  python3 scripts/run_mcp.py --transport http --url "$MCP_URL" --source "$f" \
    --output "$RUN/http-$name.json" || break
  python3 scripts/check_findings.py "$RUN/http-$name.json" \
    --expected "$f.expected.json" || break
done
```

客户端调用 `semgrep_scan_with_custom_rule`，提交固定源码和 YAML 规则；该工具在当前 Semgrep 实现中走 CLI，不是 daemon。HTTP 脚本不提供 OAuth 登录流程；使用你已经配置好的受控测试服务。不要为了此测试把无认证服务开放到公共网络。

每个用例预期打印 `MCP_CALL_OK` 和 `FINDINGS_PASS count=1`。脚本遇到工具不可见、认证失败或扫描错误会报错，不会自动切换后端。

## 3. 可选：普通 CLI 扫描

仅在需要排查某个 MCP 用例时使用，不必重复证明原版扫描器可用：

```bash
for f in fixtures/cases/*.py; do
  name=$(basename "$f" .py)
  semgrep scan --config rules/eval.yaml --metrics=off --disable-version-check \
    --no-rewrite-rule-ids --json "$f" > "$RUN/cli-$name.json" || break
  python3 scripts/check_findings.py "$RUN/cli-$name.json" \
    --expected "$f.expected.json" || break
done
```

## 保存什么，不能据此宣称什么

MCP JSON 保存原始响应、源码大小和 SHA-256、规则 SHA-256、端到端调用耗时及请求/响应的应用层 JSON 大小。JSON 大小不是包含 HTTP/TCP/TLS 的线上字节数。计时从 `session.call_tool` 前到其返回，不包含建立会话和列出工具的耗时，也不包含预先读取源码；它包含请求发送、服务端处理和结果接收等。

**当前每个命令只测一个样本，每个用例重新建立会话，不应把这组数据当作预热后的稳定 p50/p99，也不能据此计算纯网络传输占比。** 这一批文件可直接用于后续连接复用、预热、多样本和服务端打点。届时还需分开报告输入准备、扫描后端、后处理与工具外开销，不能将“总耗时减扫描耗时”命名为纯网络耗时。

两台机器和不同运行轮次使用各自的新结果目录。在 `results/run-notes.md` 记录服务端启动命令、版本、临时文件位置（磁盘/tmpfs）、访问控制和计时方式。不要提交含凭据的 URL 或日志。
