"""Offline checks for the UB Direct/Nested campaign collector."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "run-bench-campaign.py"
SPEC = importlib.util.spec_from_file_location("run_bench_campaign", SCRIPT)
campaign = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(campaign)


def output(case, owner_multi=True):
    _, test, path, result, transitions = case
    values = [2100] * 100 + [2200] * 800 + [2300] * 99 + [3000]
    return (f"UB_DOMAIN_RESULT case={test} result={result} samples=1000 warmup=100 "
            f"min_ns=2100 p50_ns=2200 p99_ns=2300 max_ns=3000 avg_ns=2200.7 "
            f"tlb=retain astack=mmap scope=lrpc_invoke nested_path={path} "
            f"transitions={transitions}\n"
            f"UB_DOMAIN_SAMPLES case={test} count=1000 values_ns="
            + ",".join(str(value) for value in values) + "\n"
            f"UB_DOMAIN_TEST_PASS test={test} cases=1 calls=1100 "
            f"restart_owner_before_next_connection={0 if owner_multi else 1}\n")


class CampaignTests(unittest.TestCase):
    def test_all_three_modes_parse(self):
        for case in campaign.CASES:
            with self.subTest(case=case[0]):
                row, values = campaign.parse_result(output(case), case, 100, 1000)
                self.assertEqual(row["p50_ns"], 2200)
                self.assertEqual(row["avg_ns"], 2200.7)
                self.assertEqual(len(values), 1000)

    def test_reject_legacy_owner_and_incorrect_result(self):
        case = campaign.CASES[0]
        with self.assertRaisesRegex(ValueError, "--multi"):
            campaign.parse_result(output(case, owner_multi=False), case, 100, 1000)
        with self.assertRaisesRegex(ValueError, "result"):
            campaign.parse_result(output(case).replace("result=142", "result=43"),
                                  case, 100, 1000)

    def test_round_summary_includes_exact_pooled_statistics(self):
        rows = []
        raw = {}
        for label, _, _, _, _ in campaign.CASES:
            raw[label] = [2000] * 1000 + [2400] * 1000
            for round_no, p50, p99, avg in ((1, 2000, 2400, 2100.0),
                                            (2, 2200, 2600, 2300.0)):
                rows.append({"round": round_no, "case": label, "samples": 1000,
                             "p50_ns": p50, "p99_ns": p99, "avg_ns": avg})
        summary = campaign.summarize(rows, raw, 2)
        self.assertEqual(summary["nested-fused"]["calls_measured"], 2000)
        self.assertEqual(summary["nested-fused"]["pooled_p50_ns"], 2000)
        self.assertEqual(summary["nested-fused"]["pooled_p99_ns"], 2400)
        self.assertEqual(summary["nested-fused"]["pooled_avg_ns"], 2200)
        self.assertEqual(summary["nested-fused"]["round_p50_median_ns"], 2100)
        self.assertEqual(summary["nested-fused"]["round_p99_median_ns"], 2500)
        self.assertEqual(summary["nested-fused"]["round_avg_mean_ns"], 2200)

    def test_campaign_writes_all_logs_and_summary(self):
        def fake_run(command, **_kwargs):
            test = command[command.index("--test") + 1]
            path = command[command.index("--nested-path") + 1]
            case = next(item for item in campaign.CASES
                        if item[1] == test and item[2] == path)
            self.assertIn("--emit-samples", command)
            return subprocess.CompletedProcess(command, 0, output(case))

        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "campaign"
            arguments = ["run-bench-campaign.py", "--cpu", "1", "--rounds", "2",
                         "--output", str(destination)]
            with patch.object(sys, "argv", arguments), patch.object(campaign.subprocess,
                                                                    "run", side_effect=fake_run):
                self.assertEqual(campaign.main(), 0)
            self.assertEqual(len(list(destination.glob("round-*.log"))), 6)
            self.assertEqual(len((destination / "rounds.csv").read_text().splitlines()), 7)
            result = json.loads((destination / "summary.json").read_text())
            self.assertEqual(result["cases"]["nested-legacy"]["calls_measured"], 2000)


if __name__ == "__main__":
    unittest.main()
