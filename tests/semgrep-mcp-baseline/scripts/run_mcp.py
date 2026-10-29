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


async def calls(session, source: str, rule: str, filename: str, options):
    await session.initialize()
    names = {tool.name for tool in (await session.list_tools()).tools}
    name = "semgrep_scan_with_custom_rule"
    if name not in names:
        raise RuntimeError(f"{name} unavailable; available tools: {sorted(names)}")
    records = []
    for index in range(options.warmup + options.samples):
        started = time.perf_counter_ns()
        response = await session.call_tool(
            name, {"code_files": [{"path": filename, "content": source}], "rule": rule}
        )
        elapsed_ns = time.perf_counter_ns() - started
        raw = response.model_dump(mode="json")
        if response.isError:
            raise RuntimeError(f"MCP tool returned an error: {raw}")
        structured = raw.get("structuredContent") or {}
        if not structured:
            for block in raw.get("content", []):
                if block.get("type") == "text":
                    candidate = json.loads(block["text"])
                    if isinstance(candidate, dict) and "results" in candidate:
                        structured = candidate
                        break
        if structured.get("errors"):
            raise RuntimeError(f"Semgrep scan errors: {structured['errors']}")
        profile = structured.get("mcp_scan_results", {}).get("baseline_profile")
        if options.profile and profile is None:
            raise RuntimeError("Server profile missing: install profiling and restart with SEMGREP_MCP_PROFILE=1")
        if options.engine_time and profile is not None and "engine_time" not in profile:
            raise RuntimeError("Native timings missing: enable SEMGREP_MCP_PROFILE_ENGINE_TIME=1 on server")
        if index >= options.warmup:
            records.append({"sample": index - options.warmup, "call_elapsed_ns": elapsed_ns,
                            "server_profile": profile, "response": raw})
    return records


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--transport", choices=("stdio", "http"), required=True)
    parser.add_argument("--url", help="MCP endpoint, required for HTTP")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=ROOT / "fixtures/sample.py")
    parser.add_argument("--rule", type=Path, default=ROOT / "rules/eval.yaml")
    parser.add_argument("--samples", type=int, default=1)
    parser.add_argument("--warmup", type=int, default=0)
    parser.add_argument("--profile", action="store_true", help="Require server timings; enable them on stdio child")
    parser.add_argument("--engine-time", action="store_true", help="Also enable --time on stdio child; requires --profile")
    args = parser.parse_args()
    if args.transport == "http" and not args.url:
        parser.error("--url is required for HTTP")
    if args.samples < 1 or args.warmup < 0:
        parser.error("samples must be positive and warmup nonnegative")
    if args.engine_time and not args.profile:
        parser.error("--engine-time requires --profile")
    if args.output.exists():
        parser.error(f"output exists; use a new path: {args.output}")

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
        # Disable OpenTelemetry tracing/log export (telemetry.semgrep.dev)
        # so startup stays fully offline.
        env["SEMGREP_MCP_DISABLE_TRACING"] = "true"
        if args.profile:
            env["SEMGREP_MCP_PROFILE"] = "1"
        if args.engine_time:
            env["SEMGREP_MCP_PROFILE_ENGINE_TIME"] = "1"
        # --no-auth skips the OAuth well-known metadata fetch to semgrep.dev,
        # which would otherwise fail on machines without DNS/network access.
        params = StdioServerParameters(
            command=executable, args=["mcp", "-t", "stdio", "--no-auth"], env=env
        )
        async with stdio_client(params) as (reader, writer):
            async with ClientSession(reader, writer) as session:
                records = await calls(session, source, rule, args.source.name, args)
    else:
        async with streamablehttp_client(args.url) as (reader, writer, _):
            async with ClientSession(reader, writer) as session:
                records = await calls(session, source, rule, args.source.name, args)

    raw = records[-1]["response"]
    elapsed_ns = records[-1]["call_elapsed_ns"]
    try:
        version = subprocess.run(["semgrep", "--version"], capture_output=True,
                                 text=True, check=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        version = None
    record = {
        "schema": 2,
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
        "warmup": args.warmup,
        "samples": records,
        "timing_scope": "call_tool; excludes session setup, file reads and client record serialization",
        "response": raw,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n",
                           encoding="utf-8")
    print(f"MCP_CALL_OK transport={args.transport} samples={len(records)} last_elapsed_ns={elapsed_ns} output={args.output}")


if __name__ == "__main__":
    asyncio.run(main())
