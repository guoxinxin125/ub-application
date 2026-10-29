#!/usr/bin/env python3
"""Call the real Semgrep MCP custom-rule tool and retain its raw response."""

import argparse
import asyncio
import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import time
from datetime import datetime, timezone

from mcp.client.session import ClientSession
from mcp.client.stdio import StdioServerParameters, stdio_client
from mcp.client.streamable_http import streamablehttp_client


ROOT = Path(__file__).resolve().parents[1]


async def call(session, source: str, rule: str, filename: str):
    await session.initialize()
    names = {tool.name for tool in (await session.list_tools()).tools}
    name = "semgrep_scan_with_custom_rule"
    if name not in names:
        raise RuntimeError(f"{name} unavailable; available tools: {sorted(names)}")
    started = time.perf_counter_ns()
    response = await session.call_tool(
        name,
        {"code_files": [{"path": filename, "content": source}], "rule": rule},
    )
    elapsed_ns = time.perf_counter_ns() - started
    return response, elapsed_ns


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--transport", choices=("stdio", "http"), required=True)
    parser.add_argument("--url", help="MCP endpoint, required for HTTP")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=ROOT / "fixtures/sample.py")
    parser.add_argument("--rule", type=Path, default=ROOT / "rules/eval.yaml")
    args = parser.parse_args()
    if args.transport == "http" and not args.url:
        parser.error("--url is required for HTTP")

    source = args.source.read_text(encoding="utf-8")
    rule = args.rule.read_text(encoding="utf-8")
    request = {"code_files": [{"path": args.source.name, "content": source}], "rule": rule}
    request_bytes = len(json.dumps(request, ensure_ascii=False).encode("utf-8"))

    if args.transport == "stdio":
        executable = shutil.which("semgrep")
        if executable is None:
            raise RuntimeError("semgrep not found in PATH; activate the private venv")
        env = os.environ.copy()
        env["SEMGREP_IS_HOSTED"] = "true"
        env["SEMGREP_SEND_METRICS"] = "off"
        params = StdioServerParameters(command=executable, args=["mcp", "-t", "stdio"], env=env)
        async with stdio_client(params) as (reader, writer):
            async with ClientSession(reader, writer) as session:
                response, elapsed_ns = await call(session, source, rule, args.source.name)
    else:
        async with streamablehttp_client(args.url) as (reader, writer, _):
            async with ClientSession(reader, writer) as session:
                response, elapsed_ns = await call(session, source, rule, args.source.name)

    raw = response.model_dump(mode="json")
    if response.isError:
        raise RuntimeError(f"MCP tool returned an error: {raw}")
    try:
        version = subprocess.run(["semgrep", "--version"], capture_output=True,
                                 text=True, check=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        version = None
    record = {
        "schema": 1,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "transport": args.transport,
        "endpoint": args.url if args.transport == "http" else "stdio-child",
        "tool": "semgrep_scan_with_custom_rule",
        "local_semgrep_version": version,
        "source_file": args.source.name,
        "source_bytes": len(source.encode("utf-8")),
        "source_sha256": hashlib.sha256(source.encode("utf-8")).hexdigest(),
        "rule_sha256": hashlib.sha256(rule.encode("utf-8")).hexdigest(),
        "request_bytes_json": request_bytes,
        "response_bytes_json": len(json.dumps(raw, ensure_ascii=False).encode("utf-8")),
        "call_elapsed_ns": elapsed_ns,
        "response": raw,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8")
    print(f"MCP_CALL_OK transport={args.transport} elapsed_ns={elapsed_ns} output={args.output}")


if __name__ == "__main__":
    asyncio.run(main())
