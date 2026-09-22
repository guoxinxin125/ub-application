#!/usr/bin/env python3
"""Parse UB-Tigon and spr4 two-node logs without double-counting host 1."""

from __future__ import annotations

import argparse
import csv
import re
import shlex
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Iterable

META_PREFIX = "TIGON_RUN_META "
END_RE = re.compile(r"TIGON_RUN_END\s+(.*)")
TOTAL_COMMIT_RE = re.compile(r"\btotal_commit:\s*([0-9.eE+-]+)")
ABORT_RATE_RE = re.compile(r"\babort_rate:\s*([0-9.eE+-]+)")

CONFIG_FIELDS = [
    "system", "workload", "run_id", "host_id", "mode", "query", "transport",
    "partitions", "workers", "keys", "rw_ratio", "zipf", "cross_ratio",
    "neworder_dist", "payment_dist", "warmup_seconds", "run_seconds", "total_seconds", "logging",
]
RAW_FIELDS = CONFIG_FIELDS + [
    "repetition", "throughput_txn_s", "abort_rate", "consistency_passed",
    "run_status", "peer_log_found", "peer_status", "source_log",
]
SUMMARY_FIELDS = [
    "system", "workload", "mode", "query", "transport", "partitions", "workers",
    "keys", "rw_ratio", "zipf", "cross_ratio", "neworder_dist", "payment_dist",
    "warmup_seconds", "run_seconds", "total_seconds", "logging", "samples", "throughput_mean_txn_s",
    "throughput_median_txn_s", "throughput_min_txn_s", "throughput_max_txn_s",
    "abort_rate_mean", "remote_efficiency_vs_local",
]


def parse_kv(text: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in shlex.split(text):
        if "=" in token:
            key, value = token.split("=", 1)
            result[key] = value
    return result


def last_float(pattern: re.Pattern[str], text: str) -> float | None:
    matches = pattern.findall(text)
    return float(matches[-1]) if matches else None


def parse_log(path: Path) -> dict[str, object] | None:
    text = path.read_text(encoding="utf-8", errors="replace")
    meta_line = next((line for line in text.splitlines() if line.startswith(META_PREFIX)), None)
    if meta_line is None:
        return None
    meta = parse_kv(meta_line[len(META_PREFIX):])
    row: dict[str, object] = {field: meta.get(field, "") for field in CONFIG_FIELDS}
    rep = meta.get("repetition", "")
    if not rep:
        match = re.search(r"_r([0-9]+)$", meta.get("run_id", ""))
        rep = match.group(1) if match else ""
    row["repetition"] = rep
    row["throughput_txn_s"] = last_float(TOTAL_COMMIT_RE, text)
    row["abort_rate"] = last_float(ABORT_RATE_RE, text)
    row["consistency_passed"] = "yes" if "TPC-C consistency check passed!" in text else ""
    ends = END_RE.findall(text)
    row["run_status"] = parse_kv(ends[-1]).get("status", "missing-end") if ends else "missing-end"
    row["peer_log_found"] = ""
    row["peer_status"] = ""
    row["source_log"] = str(path)
    return row


def log_paths(inputs: Iterable[Path]) -> Iterable[Path]:
    for item in inputs:
        if item.is_file():
            yield item
        elif item.is_dir():
            yield from sorted(item.rglob("*.log"))


def identity(row: dict[str, object]) -> tuple[object, ...]:
    return tuple(row.get(field, "") for field in (
        "system", "workload", "run_id", "mode", "query", "transport",
        "partitions", "workers", "keys", "rw_ratio", "zipf", "cross_ratio",
        "neworder_dist", "payment_dist",
    ))


def select_cluster_rows(rows: list[dict[str, object]]) -> list[dict[str, object]]:
    peers = {identity(row): row for row in rows if row.get("system") == "ub" and row.get("host_id") == "1"}
    selected: list[dict[str, object]] = []
    for row in rows:
        if row.get("system") == "ub" and row.get("host_id") != "0":
            continue
        if row.get("system") == "ub":
            peer = peers.get(identity(row))
            row["peer_log_found"] = "yes" if peer else "no"
            row["peer_status"] = peer.get("run_status", "") if peer else ""
        selected.append(row)
    return selected


def summary_key(row: dict[str, object]) -> tuple[object, ...]:
    return tuple(row.get(field, "") for field in SUMMARY_FIELDS[:17])


def baseline_key(row: dict[str, object]) -> tuple[object, ...]:
    excluded = {"cross_ratio", "neworder_dist", "payment_dist"}
    return tuple(row.get(field, "") for field in SUMMARY_FIELDS[:17] if field not in excluded)


def summarize(rows: list[dict[str, object]]) -> list[dict[str, object]]:
    groups: dict[tuple[object, ...], list[dict[str, object]]] = defaultdict(list)
    for row in rows:
        valid = row.get("throughput_txn_s") is not None and row.get("run_status") == "ok"
        if row.get("system") == "ub":
            valid = valid and row.get("peer_log_found") == "yes" and row.get("peer_status") == "ok"
        if row.get("workload") == "tpcc":
            valid = valid and row.get("consistency_passed") == "yes"
        if valid:
            groups[summary_key(row)].append(row)

    summaries: list[dict[str, object]] = []
    for key, samples in sorted(groups.items(), key=lambda item: tuple(map(str, item[0]))):
        first = samples[0]
        values = [float(row["throughput_txn_s"]) for row in samples]
        aborts = [float(row["abort_rate"]) for row in samples if row.get("abort_rate") is not None]
        summary = {field: first.get(field, "") for field in SUMMARY_FIELDS[:17]}
        summary.update({
            "samples": len(values),
            "throughput_mean_txn_s": statistics.fmean(values),
            "throughput_median_txn_s": statistics.median(values),
            "throughput_min_txn_s": min(values),
            "throughput_max_txn_s": max(values),
            "abort_rate_mean": statistics.fmean(aborts) if aborts else "",
            "remote_efficiency_vs_local": "",
        })
        summaries.append(summary)

    baselines: dict[tuple[object, ...], float] = {}
    for row in summaries:
        is_local = (row["workload"] == "ycsb" and row["cross_ratio"] == "0") or (
            row["workload"] == "tpcc" and row["neworder_dist"] == "0" and row["payment_dist"] == "0"
        )
        if is_local:
            baselines[baseline_key(row)] = float(row["throughput_median_txn_s"])
    for row in summaries:
        baseline = baselines.get(baseline_key(row))
        if baseline:
            row["remote_efficiency_vs_local"] = float(row["throughput_median_txn_s"]) / baseline
    return summaries


def write_csv(path: Path, fields: list[str], rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path, help="log files or directories")
    parser.add_argument("--raw-output", type=Path, default=Path("two_node_raw.csv"))
    parser.add_argument("--summary-output", type=Path, default=Path("two_node_summary.csv"))
    args = parser.parse_args()

    parsed = [row for path in log_paths(args.inputs) if (row := parse_log(path)) is not None]
    cluster_rows = select_cluster_rows(parsed)
    write_csv(args.raw_output, RAW_FIELDS, cluster_rows)
    write_csv(args.summary_output, SUMMARY_FIELDS, summarize(cluster_rows))
    print(f"parsed_logs={len(parsed)} cluster_rows={len(cluster_rows)} raw={args.raw_output} summary={args.summary_output}")
    return 0 if cluster_rows else 1


if __name__ == "__main__":
    raise SystemExit(main())
