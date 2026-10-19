#!/usr/bin/env python3
"""Run repeatable single-connection Direct and Nested UB benchmarks."""

import argparse
import csv
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
from datetime import datetime, timezone


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts" / "run-ub-domain-smoke.sh"
CASES = (("direct", "direct", "legacy", 142, 2),
         ("nested-legacy", "nested", "legacy", 43, 6),
         ("nested-fused", "nested", "fused", 43, 4))
FIELDS = ("round", "case", "min_ns", "p50_ns", "p99_ns", "max_ns", "avg_ns")


def parse_result(output, case, warmup, samples):
    label, test, path, expected, transitions = case
    results = [line for line in output.splitlines()
               if line.startswith("UB_DOMAIN_RESULT ")]
    passes = [line for line in output.splitlines()
              if line.startswith("UB_DOMAIN_TEST_PASS ")]
    raw_lines = [line for line in output.splitlines()
                 if line.startswith("UB_DOMAIN_SAMPLES ")]
    if len(results) != 1 or len(passes) != 1 or len(raw_lines) != 1:
        raise ValueError(f"{label}: expected exactly one result, raw sample line and PASS")

    def fields(line):
        return dict(re.findall(r"\b([a-z][a-z0-9_]*)=([^\s]+)", line))

    result, passed, raw = fields(results[0]), fields(passes[0]), fields(raw_lines[0])
    expected_fields = {"case": test, "result": str(expected),
                       "nested_path": path, "transitions": str(transitions),
                       "scope": "lrpc_invoke", "samples": str(samples),
                       "warmup": str(warmup)}
    for key, value in expected_fields.items():
        if result.get(key) != value:
            raise ValueError(f"{label}: {key}={result.get(key)!r}, expected {value!r}")
    if (passed.get("test") != test or passed.get("cases") != "1" or
            passed.get("calls") != str(warmup + samples)):
        raise ValueError(f"{label}: invalid PASS marker")
    if passed.get("restart_owner_before_next_connection") != "0":
        raise ValueError("B owner must be started with --multi for repeated runs")
    if raw.get("case") != test or raw.get("count") != str(samples):
        raise ValueError(f"{label}: invalid raw sample header")
    try:
        values = [int(item) for item in raw["values_ns"].split(",")]
    except (KeyError, ValueError) as error:
        raise ValueError(f"{label}: malformed raw samples") from error
    if len(values) != samples or any(value < 0 for value in values) or values != sorted(values):
        raise ValueError(f"{label}: raw sample count or order mismatch")
    row = {key: float(result[key]) if key == "avg_ns" else int(result[key])
           for key in FIELDS[2:]}
    if ((row["min_ns"], row["p50_ns"], row["p99_ns"], row["max_ns"]) !=
            (values[0], values[(samples - 1) * 50 // 100],
             values[(samples - 1) * 99 // 100], values[-1]) or
            abs(row["avg_ns"] - statistics.mean(values)) > 0.051):
        raise ValueError(f"{label}: result and raw sample metrics disagree")
    return row, values


def summarize(rows, raw_by_case, rounds):
    summary = {}
    for label, _, _, _, _ in CASES:
        selected = [row for row in rows if row["case"] == label]
        if len(selected) != rounds:
            raise ValueError(f"{label}: {len(selected)} completed rounds, expected {rounds}")
        values = sorted(raw_by_case[label])
        if len(values) != sum(row["samples"] for row in selected):
            raise ValueError(f"{label}: incomplete pooled samples")
        summary[label] = {
            "rounds": rounds,
            "calls_measured": len(values),
            "pooled_p50_ns": values[(len(values) - 1) * 50 // 100],
            "pooled_p99_ns": values[(len(values) - 1) * 99 // 100],
            "pooled_avg_ns": statistics.mean(values),
            "round_p50_median_ns": statistics.median(row["p50_ns"] for row in selected),
            "round_p99_median_ns": statistics.median(row["p99_ns"] for row in selected),
            "round_avg_mean_ns": statistics.mean(row["avg_ns"] for row in selected),
            "round_avg_min_ns": min(row["avg_ns"] for row in selected),
            "round_avg_max_ns": max(row["avg_ns"] for row in selected),
        }
    return summary


def write_csv(path, rows):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=("round", "case", "samples") + FIELDS[2:])
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpu", type=int, required=True, help="allowed A-side CPU")
    parser.add_argument("--output", type=Path, required=True, help="new, nonexistent result directory")
    parser.add_argument("--name", default="lrpc_ub_domain", help="B owner's shared-memory name")
    parser.add_argument("--rounds", type=int, default=20)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--samples", type=int, default=1000)
    args = parser.parse_args()
    if args.cpu < 0 or args.rounds < 1 or not 0 <= args.warmup <= 1000000 or not 1 <= args.samples <= 1000000:
        parser.error("require cpu>=0, rounds>=1, warmup=0..1000000, samples=1..1000000")
    if not args.name or not RUNNER.is_file():
        parser.error("invalid shared-memory name or missing run-ub-domain-smoke.sh")
    args.output.mkdir(parents=True, exist_ok=False)
    config = {"cpu": args.cpu, "name": args.name, "rounds": args.rounds,
              "warmup": args.warmup, "samples": args.samples,
              "started_utc": datetime.now(timezone.utc).isoformat(),
              "scope": "lrpc_invoke", "required_owner": "--multi",
              "aggregation": "pooled quantiles use all raw timed calls; round metrics also retained"}
    (args.output / "config.json").write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    rows = []
    raw_by_case = {label: [] for label, _, _, _, _ in CASES}
    try:
        for round_no in range(1, args.rounds + 1):
            for offset in range(len(CASES)):
                case = CASES[(round_no - 1 + offset) % len(CASES)]
                label, test, path, _, _ = case
                command = ("sh", str(RUNNER), "--cpu", str(args.cpu), "--test", test,
                           "--nested-path", path, "--warmup", str(args.warmup),
                           "--samples", str(args.samples), "--name", args.name,
                           "--emit-samples")
                print(f"UB_CAMPAIGN_RUN round={round_no}/{args.rounds} case={label}", flush=True)
                completed = subprocess.run(command, cwd=ROOT, env=os.environ.copy(),
                                           text=True, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, errors="replace", check=False)
                (args.output / f"round-{round_no:02d}-{label}.log").write_text(
                    completed.stdout, encoding="utf-8")
                if completed.returncode:
                    raise RuntimeError(f"{label} round {round_no}: exit {completed.returncode}")
                measurement, values = parse_result(completed.stdout, case, args.warmup, args.samples)
                raw_by_case[label].extend(values)
                rows.append({"round": round_no, "case": label, "samples": args.samples,
                             **measurement})
                write_csv(args.output / "rounds.csv", rows)
        summary = summarize(rows, raw_by_case, args.rounds)
        (args.output / "summary.json").write_text(
            json.dumps({"config": config, "cases": summary}, indent=2) + "\n", encoding="utf-8")
        for label, metrics in summary.items():
            print(f"UB_CAMPAIGN_RESULT case={label} rounds={args.rounds} "
                  f"measured_calls={metrics['calls_measured']} "
                  f"pooled_p50_ns={metrics['pooled_p50_ns']} "
                  f"pooled_p99_ns={metrics['pooled_p99_ns']} "
                  f"pooled_avg_ns={metrics['pooled_avg_ns']:.1f} "
                  f"round_p50_median_ns={metrics['round_p50_median_ns']:.1f} "
                  f"round_p99_median_ns={metrics['round_p99_median_ns']:.1f}")
        print(f"UB_CAMPAIGN_PASS output={args.output}")
    except (OSError, ValueError, RuntimeError, KeyboardInterrupt) as error:
        print(f"UB_CAMPAIGN_FAIL completed_runs={len(rows)} output={args.output} error={error}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
