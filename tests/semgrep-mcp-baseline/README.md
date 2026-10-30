# Semgrep MCP 固定输入基线

新增的 **1 / 4 / 16 KiB 六个输入用例**及批量执行命令见 [CASES.md](CASES.md)。已跑通 MCP 的机器可以直接从该文档开始，不必重新执行下方安装或连通性检查。

需要测量**扫描、输入准备和工具外开销的占比**，直接使用 [PROFILING.md](PROFILING.md)：包含 A/B 安装、服务重启、预热采样和汇总的完整顺序，保留已有结果及本地脚本修正。

需要进一步粗估**单向上传/响应返回**，见 [HTTP_TIMESTAMPS.md](HTTP_TIMESTAMPS.md)，其中明确区分零偏差假设、误差上界和双向合计。

本机 **stdio 管道、SDK 编解码与排队**细分见 [STDIO_TIMESTAMPS.md](STDIO_TIMESTAMPS.md)，仅需 A 运行，和跨机 HTTP 分开报告。

本目录用于保存 AArch64 UB 机器上原版 Semgrep 的可复现实验输入和结果。它不修改 Semgrep 源码，不涉及 UB 模块。先使用 `semgrep_scan_with_custom_rule`：该工具接收明确的规则和源码，走 CLI 扫描路径；**不是 daemon 性能基线**。

## 固定内容

- Semgrep 版本：建议两端均使用 `1.182.0`，运行时记录实际版本。
- `fixtures/sample.py`：两个 `eval` 命中、一个 `len` 不命中。
- `rules/eval.yaml`：单条 Python 规则 `baseline.python.eval-call`。
- `expected.json`：按规则 ID、文件名和行号定义的预期命中。
- 扫描目标只包含 `fixtures/sample.py`，不扫描本目录的脚本和规则文件。

在 A/B 同步整个 `tests/semgrep-mcp-baseline/` 目录。不要复制包含真实代码或凭据的结果到公开仓库。

## 1. 先运行普通 CLI 校验规则

在已激活 Semgrep 虚拟环境的机器上，从本目录执行：

```bash
semgrep scan --config rules/eval.yaml --metrics=off \
  --disable-version-check --json fixtures/sample.py \
  > results/cli.json
python3 scripts/check_findings.py results/cli.json
```

如尚无 `results/`，先执行 `mkdir -p results`。预期为 `FINDINGS_PASS`。此结果与 MCP 扫描可能有不同的临时文件路径，校验时只比较文件名、规则 ID 和行号。

## 2. 本地 MCP（stdio）

使用已经安装 Semgrep 的同一虚拟环境；MCP Python SDK 应随该环境安装。`SEMGREP_IS_HOSTED=true` 只用于让 `semgrep_scan_with_custom_rule` 可见；本步骤没有开放网络端口。

```bash
python3 scripts/run_mcp.py --transport stdio --output results/mcp-stdio.json
python3 scripts/check_findings.py results/mcp-stdio.json
```

脚本启动自己的 Semgrep MCP 子进程，并用相同源码、规则调用工具。若机器已有自定义 Semgrep 安装，记录 `which semgrep` 和 `semgrep --version`。

## 3. 跨机 MCP（HTTP）

在 B 启动你们已验证过的 MCP HTTP 服务，具体启动命令、监听地址和认证方式填写到 `results/run-notes.md`。不要为了测试把无认证服务暴露到不受控网络。A 上运行：

```bash
python3 scripts/run_mcp.py --transport http \
  --url 'http://B的受控地址:端口/mcp' \
  --output results/mcp-remote.json
python3 scripts/check_findings.py results/mcp-remote.json
```

若使用 SSH 隧道，`--url` 填本机转发端点。HTTP 服务若要求 OAuth，先用你们已经跑通的认证客户端/方法；本脚本不注入凭据，也不绕过认证。可先用 `--url http://127.0.0.1:8000/mcp` 测同机 HTTP，再换跨机 URL。

## 结果记录与解释

`run_mcp.py` 保存 MCP 原始响应、请求/响应字节数、单次调用耗时、Semgrep 版本（若本机可用）、时间戳以及工具名。`check_findings.py` 只验证正确性，**不会把一次调用耗时当成稳定性能数据**。请在 `results/run-notes.md` 记录两端机器、版本、启动参数、CLI/daemon 分支、文件哈希、端口访问控制及是否使用 `/dev/shm` 临时目录。`results/` 下的 JSON 是运行后生成的，仓库不预填虚构测量结果。

若将来需要固定规则的 daemon 基线，必须另外实现或确认规则配置可复现，再与这套 CLI 基线分别报告。
