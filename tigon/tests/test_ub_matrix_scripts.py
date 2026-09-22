#!/usr/bin/env python3
from __future__ import annotations

import csv
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TIGON = REPO / "tigon"
SPR4 = REPO / "tigon-spr4"


def find_usable_bash() -> str | None:
    candidates = [shutil.which("bash")]
    if os.name == "nt":
        git = shutil.which("git")
        if git:
            git_root = Path(git).resolve().parent.parent
            candidates.extend([str(git_root / "bin/bash.exe"), str(git_root / "usr/bin/bash.exe")])
        candidates.extend([
            r"C:\Program Files\Git\bin\bash.exe",
            r"C:\Program Files\Git\usr\bin\bash.exe",
        ])
    for candidate in candidates:
        if not candidate or not Path(candidate).is_file():
            continue
        # System32/bash.exe is a WSL launcher.  Without an installed distro it
        # emits UTF-16 diagnostics and cannot parse or run Linux scripts.
        if os.name == "nt" and Path(candidate).parent.name.lower() == "system32":
            continue
        return candidate
    return None


BASH = find_usable_bash()


class MatrixScriptTests(unittest.TestCase):
    @unittest.skipUnless(BASH, "bash is required")
    def test_shell_syntax(self) -> None:
        scripts = [
            TIGON / "scripts/run_ub_ycsb_matrix.sh",
            TIGON / "scripts/run_ub_tpcc_matrix.sh",
            TIGON / "scripts/run_ub_two_node_compare.sh",
            SPR4 / "scripts/run_two_node_compare.sh",
        ]
        for script in scripts:
            subprocess.run([BASH, "-n", str(script)], check=True)

    @unittest.skipUnless(BASH, "bash is required")
    def test_ub_ycsb_dry_run_contains_fairness_flags(self) -> None:
        env = os.environ.copy()
        env.update({
            "UB_TIGON_DRY_RUN": "1", "UB_PROVIDER_HOST": "provider0",
            "UB_TIGON_MODES": "one-sided", "UB_TIGON_QUERIES": "rmw",
            "UB_TIGON_TRANSPORTS": "true", "UB_TIGON_THREADS": "3",
            "UB_TIGON_PARTITIONS": "2", "UB_TIGON_KEYS": "300000",
            "UB_TIGON_RW_RATIO": "95", "UB_TIGON_ZIPF": "0.7",
            "UB_TIGON_CROSS_RATIO": "25", "UB_TIGON_CPU_LIST": "4-6",
        })
        result = subprocess.run(
            [BASH, str(TIGON / "scripts/run_ub_ycsb_matrix.sh"), "0", "h0:30000;h1:30000", "u12345678"],
            env=env, text=True, capture_output=True, check=True,
        )
        for flag in ("--threads=3", "--partition_num=2", "--keys=300000", "--read_write_ratio=95", "--zipf=0.7", "--cross_ratio=25"):
            self.assertIn(flag, result.stdout)
        self.assertIn("taskset -c 4-6", result.stdout)

    @unittest.skipUnless(BASH, "bash is required")
    def test_ub_tpcc_default_partition_count_matches_spr4(self) -> None:
        env = os.environ.copy()
        env.update({
            "UB_TIGON_DRY_RUN": "1", "UB_PROVIDER_HOST": "provider0",
            "UB_TIGON_MODES": "one-sided", "UB_TPCC_QUERIES": "mixed",
            "UB_TIGON_TRANSPORTS": "true", "UB_TIGON_THREADS": "3",
        })
        result = subprocess.run(
            [BASH, str(TIGON / "scripts/run_ub_tpcc_matrix.sh"), "1", "h0:30000;h1:30000", "u12345678"],
            env=env, text=True, capture_output=True, check=True,
        )
        self.assertIn("--partition_num=6", result.stdout)

    @unittest.skipUnless(BASH, "bash is required")
    def test_compare_wrappers_have_matching_ycsb_command(self) -> None:
        ub_env = os.environ.copy()
        ub_env.update({
            "UB_TIGON_DRY_RUN": "1", "UB_PROVIDER_HOST": "provider0",
            "UB_COMPARE_REPETITIONS": "1", "UB_COMPARE_RW_RATIOS": "95",
            "UB_COMPARE_CROSS_RATIOS": "100",
        })
        ub = subprocess.run(
            [BASH, str(TIGON / "scripts/run_ub_two_node_compare.sh"), "ycsb", "0", "h0:30000;h1:30000", "cmp"],
            env=ub_env, text=True, capture_output=True, check=True,
        )
        spr4_env = os.environ.copy()
        spr4_env.update({
            "SPR4_COMPARE_DRY_RUN": "1", "SPR4_COMPARE_REPETITIONS": "1",
            "SPR4_COMPARE_RW_RATIOS": "95", "SPR4_COMPARE_CROSS_RATIOS": "100",
        })
        spr4 = subprocess.run(
            [BASH, str(SPR4 / "scripts/run_two_node_compare.sh"), "ycsb", "cmp"],
            env=spr4_env, text=True, capture_output=True, check=True,
        )
        for value in ("workers=3", "keys=300000", "rw_ratio=95", "zipf=0.7", "cross_ratio=100", "warmup_seconds=30", "run_seconds=30", "total_seconds=60"):
            self.assertIn(value, ub.stdout)
            self.assertIn(value, spr4.stdout)
        self.assertIn("--time_to_run=60", ub.stdout)
        self.assertRegex(spr4.stdout, r"\s60\s+30\s+BLACKHOLE")

    @unittest.skipUnless(BASH, "bash is required")
    def test_compare_wrappers_have_matching_tpcc_command(self) -> None:
        ub_env = os.environ.copy()
        ub_env.update({
            "UB_TIGON_DRY_RUN": "1", "UB_PROVIDER_HOST": "provider0",
            "UB_COMPARE_REPETITIONS": "1", "UB_COMPARE_TPCC_REMOTE_PAIRS": "60:90",
        })
        ub = subprocess.run(
            [BASH, str(TIGON / "scripts/run_ub_two_node_compare.sh"), "tpcc", "0", "h0:30000;h1:30000", "cmp"],
            env=ub_env, text=True, capture_output=True, check=True,
        )
        spr4_env = os.environ.copy()
        spr4_env.update({
            "SPR4_COMPARE_DRY_RUN": "1", "SPR4_COMPARE_REPETITIONS": "1",
            "SPR4_COMPARE_TPCC_REMOTE_PAIRS": "60:90",
        })
        spr4 = subprocess.run(
            [BASH, str(SPR4 / "scripts/run_two_node_compare.sh"), "tpcc", "cmp"],
            env=spr4_env, text=True, capture_output=True, check=True,
        )
        for value in ("partitions=6", "workers=3", "neworder_dist=60", "payment_dist=90", "warmup_seconds=30", "run_seconds=30", "total_seconds=60"):
            self.assertIn(value, ub.stdout)
            self.assertIn(value, spr4.stdout)

    @unittest.skipUnless(BASH, "bash is required")
    def test_spr4_refuses_multiple_points_without_reuse_override(self) -> None:
        env = os.environ.copy()
        env.update({
            "SPR4_COMPARE_DRY_RUN": "1",
            "SPR4_COMPARE_RW_RATIOS": "95",
            "SPR4_COMPARE_CROSS_RATIOS": "0 100",
        })
        result = subprocess.run(
            [BASH, str(SPR4 / "scripts/run_two_node_compare.sh"), "ycsb", "cmp"],
            env=env, text=True, capture_output=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("current spr4 CXL metadata is not reusable", result.stderr)


class ParserTests(unittest.TestCase):
    def test_parser_uses_host0_cluster_metric_and_remote_efficiency(self) -> None:
        parser = TIGON / "scripts/parse/parse_two_node_compare.py"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            def write(name: str, system: str, host: int, cross: int, throughput: int) -> None:
                run_id = f"cmp_y_rw95_c{cross}_r1"
                text = (
                    f"TIGON_RUN_META system={system} workload=ycsb run_id={run_id} host_id={host} repetition=1 mode={'one-sided' if system == 'ub' else 'native'} query=rmw transport={'ubq' if system == 'ub' else 'cxlq'} partitions=2 workers=3 keys=300000 rw_ratio=95 zipf=0.7 cross_ratio={cross} neworder_dist= payment_dist= warmup_seconds=30 run_seconds=30 total_seconds=60 logging=off\n"
                    f"Global Stats: total_commit: {throughput}\n"
                    f"TIGON_RUN_END run_id={run_id} host_id={host} status=ok exit_code=0\n"
                )
                (root / name).write_text(text, encoding="utf-8")

            write("ub0-local.log", "ub", 0, 0, 1000)
            write("ub1-local.log", "ub", 1, 0, 9999)
            write("ub0-remote.log", "ub", 0, 100, 800)
            write("ub1-remote.log", "ub", 1, 100, 9999)
            write("ub0-orphan.log", "ub", 0, 50, 900)
            write("spr4-local.log", "spr4", 0, 0, 2000)
            write("spr4-remote.log", "spr4", 0, 100, 1000)
            raw = root / "raw.csv"
            summary = root / "summary.csv"
            subprocess.run([sys.executable, str(parser), str(root), "--raw-output", str(raw), "--summary-output", str(summary)], check=True)
            with raw.open(encoding="utf-8", newline="") as handle:
                raw_rows = list(csv.DictReader(handle))
            self.assertEqual(len(raw_rows), 5)
            self.assertNotIn("9999.0", {row["throughput_txn_s"] for row in raw_rows})
            orphan = next(row for row in raw_rows if row["system"] == "ub" and row["cross_ratio"] == "50")
            self.assertEqual(orphan["peer_log_found"], "no")
            with summary.open(encoding="utf-8", newline="") as handle:
                rows = list(csv.DictReader(handle))
            efficiencies = {(row["system"], row["cross_ratio"]): row["remote_efficiency_vs_local"] for row in rows}
            self.assertNotIn(("ub", "50"), efficiencies)
            self.assertAlmostEqual(float(efficiencies[("ub", "100")]), 0.8)
            self.assertAlmostEqual(float(efficiencies[("spr4", "100")]), 0.5)


if __name__ == "__main__":
    unittest.main()
