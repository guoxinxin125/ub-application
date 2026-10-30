#!/usr/bin/env python3
"""Aggregate paired MCP client/server durations; never label residual as wire time."""

import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import statistics


def percentile(values, percent):
    return sorted(values)[max(0, math.ceil(len(values) * percent / 100) - 1)]


def summarize(records):
    if not records:
        raise ValueError("no samples")
    totals, phases, outside = [], defaultdict(list), []
    native = defaultdict(list)
    expected_keys = None
    for sample in records:
        profile = sample.get("server_profile")
        if not profile:
            raise ValueError("server_profile missing; old results cannot supply a breakdown")
        elapsed = sample["call_elapsed_ns"]
        total = profile["tool_total_ns"]
        parts = profile["phases_ns"]
        if elapsed <= 0 or total < 0 or elapsed < total or any(v < 0 for v in parts.values()):
            raise ValueError("invalid/negative paired durations; do not clamp or interpret as network time")
        if sum(parts.values()) != total:
            raise ValueError("server phase sum does not equal tool_total_ns")
        if expected_keys is not None and set(parts) != expected_keys:
            raise ValueError("phase sets differ across samples; split the runs")
        expected_keys = set(parts)
        totals.append(elapsed)
        outside.append(elapsed - total)
        for key, value in parts.items():
            phases[key].append(value)
        engine = profile.get("engine_time")
        if isinstance(engine, dict):
            for name, value in engine.get("profiling_times", {}).items():
                if isinstance(value, (int, float)) and value >= 0:
                    native[f"cli_{name}"].append(value)
            if isinstance(engine.get("rules_parse_time"), (int, float)):
                native["rules_parse_time"].append(engine["rules_parse_time"])
            targets = engine.get("targets", [])
            if targets:
                for field in ("parse_times", "match_times"):
                    native[f"target_{field}_sum"].append(sum(
                        x for target in targets for x in target.get(field, []) if x >= 0))
    phases["outside_tool_ns"] = outside
    total_sum = sum(totals)
    result = {
        "samples": len(records),
        "end_to_end_ms": {"avg": statistics.mean(totals)/1e6,
                          "p50": percentile(totals, 50)/1e6,
                          "p99": percentile(totals, 99)/1e6},
        "phases": {key: {"avg_ms": statistics.mean(values)/1e6,
                          "p50_ms": percentile(values, 50)/1e6,
                          "p99_ms": percentile(values, 99)/1e6,
                          "share_percent": sum(values)*100/total_sum}
                   for key, values in phases.items()},
        "share_definition": "sum of paired phase durations / sum of end-to-end durations",
        "outside_tool_definition": "transport + framework + encoding/decoding + scheduling + profiling response overhead; NOT pure network",
        "native_engine_diagnostics": {key: {"samples": len(values), "avg_ms": statistics.mean(values)*1000}
                                      for key, values in native.items()},
        "native_definition": "Semgrep --time seconds converted to ms; nested/possibly overlapping, not added to top-level shares",
    }
    boundaries = [s.get("http_boundary") for s in records]
    if any(boundaries):
        if not all(boundaries):
            raise ValueError("mixed HTTP timestamp modes; split the runs")
        fields = ("upstream_estimate_ns", "downstream_estimate_ns", "combined_http_boundary_ns",
                  "server_body_to_headers_ns", "server_request_body_read_ns",
                  "client_response_body_read_ns", "client_sdk_outside_http_ns")
        result["http_application_boundaries"] = {
            key: {"avg_ms": statistics.mean([x[key] for x in boundaries])/1e6,
                  "p50_ms": percentile([x[key] for x in boundaries], 50)/1e6,
                  "p99_ms": percentile([x[key] for x in boundaries], 99)/1e6,
                  "share_percent": sum(x[key] for x in boundaries)*100/total_sum}
            for key in fields}
        result["http_warnings"] = dict((warning, sum(warning in x["warnings"] for x in boundaries))
                                       for warning in {w for x in boundaries for w in x["warnings"]})
        result["http_boundary_note"] = "one-way wall-clock estimates; overlapping diagnostic views, not pure network; never add to existing phase shares"
    stdio = [s.get("stdio_boundary") for s in records]
    if any(stdio):
        if not all(stdio):
            raise ValueError("mixed stdio timestamp modes")
        expected = set(stdio[0]["phases_ns"])
        for sample, boundary in zip(records, stdio):
            values = boundary["phases_ns"]
            if set(values) != expected or any(v < 0 for v in values.values()) or sum(values.values()) != sample["call_elapsed_ns"]:
                raise ValueError("stdio phases must form a nonnegative end-to-end partition")
        result["stdio_partition"] = {
            key: {"avg_ms": statistics.mean([x["phases_ns"][key] for x in stdio])/1e6,
                  "p50_ms": percentile([x["phases_ns"][key] for x in stdio], 50)/1e6,
                  "p99_ms": percentile([x["phases_ns"][key] for x in stdio], 99)/1e6,
                  "share_percent": sum(x["phases_ns"][key] for x in stdio)*100/total_sum}
            for key in stdio[0]["phases_ns"]}
        framework = [b["phases_ns"]["server_processing_ns"] - s["server_profile"]["tool_total_ns"]
                     for s, b in zip(records, stdio)]
        if any(v < 0 for v in framework):
            raise ValueError("tool timing does not fit inside stdio server processing")
        result["stdio_server_framework_nested"] = {
            "avg_ms": statistics.mean(framework)/1e6,
            "share_percent": sum(framework)*100/total_sum,
            "note": "already included in server_processing_ns; includes outer MCP wrapper/metrics/dispatch"}
        result["stdio_note"] = "Alternative full partition of same-host call; do not add to original phases. Encode/decode includes SDK queues and observation overhead; IPC includes scheduling."
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    groups = defaultdict(list)
    for file in sorted(args.results.glob("*.json")):
        data = json.loads(file.read_text(encoding="utf-8"))
        if "transport" not in data or "source_file" not in data:
            continue
        for sample in data.get("samples", [data]):
            profile = sample.get("server_profile") or {}
            # Keep instrumentation settings, endpoints, hashes and engine versions separate.
            response = sample.get("response", {}).get("structuredContent") or {}
            key = (data["transport"], data.get("endpoint"), data["source_file"],
                   data.get("source_sha256"), data.get("rule_sha256"),
                   response.get("version"), profile.get("backend"), "engine_time" in profile,
                   bool(sample.get("http_boundary")),
                   (sample.get("http_boundary") or {}).get("assumed_b_minus_a_ns"),
                   (sample.get("http_boundary") or {}).get("clock_uncertainty_ns"),
                   bool(sample.get("stdio_boundary")))
            groups[key].append(sample)
    if not groups:
        parser.error("no MCP samples found")
    report = []
    for key, records in groups.items():
        try:
            result = summarize(records)
        except ValueError as error:
            parser.error(f"{key[:3]}: {error}")
        result["group"] = dict(zip(("transport", "endpoint", "source_file", "source_sha256",
                                    "rule_sha256", "server_version", "backend", "engine_time_enabled",
                                    "http_timestamps", "assumed_b_minus_a_ns", "clock_uncertainty_ns", "stdio_timestamps"), key))
        report.append(result)
        print(f"{key[0]} {key[2]} n={len(records)} e2e_avg_ms={result['end_to_end_ms']['avg']:.3f}")
        for name, value in result["phases"].items():
            print(f"  {name}: avg_ms={value['avg_ms']:.3f} share={value['share_percent']:.2f}%")
        for name, value in result["native_engine_diagnostics"].items():
            print(f"  native/{name}: avg_ms={value['avg_ms']:.3f} n={value['samples']} (nested)")
        for name, value in result.get("http_application_boundaries", {}).items():
            print(f"  HTTP/{name}: avg_ms={value['avg_ms']:.3f} share={value['share_percent']:.2f}% (diagnostic)")
        if result.get("http_warnings"):
            print(f"  HTTP_WARNINGS: {result['http_warnings']}")
        for name, value in result.get("stdio_partition", {}).items():
            print(f"  STDIO/{name}: avg_ms={value['avg_ms']:.3f} share={value['share_percent']:.2f}%")
        if result.get("stdio_server_framework_nested"):
            value = result["stdio_server_framework_nested"]
            print(f"  STDIO/server_framework_nested: avg_ms={value['avg_ms']:.3f} share={value['share_percent']:.2f}% (nested)")
    if args.output:
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2)
            stream.write("\n")


if __name__ == "__main__":
    main()
