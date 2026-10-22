"""Same-host-only profile correlation; no UB hardware is exercised."""

import importlib.util
import io
import os
from contextlib import redirect_stdout
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/analyze-social-flow.py"
SPEC = importlib.util.spec_from_file_location("social_flow", SCRIPT)
flow = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(flow)


def event(role, request, stage, timestamp):
    return (f"SN_FLOW_EVENT role={role} thread=1 request={request} "
            f"stage={stage} at_ns={timestamp} clock=steady_ns\n")


class SocialFlow(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "requires a C++ compiler")
    def test_flow_events_require_both_profile_flags_and_flush_on_exit(self):
        app = Path(__file__).resolve().parents[2] / "eRPC/apps/social_network_cxl"
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "flow.cpp"
            binary = Path(tmp) / "flow.exe"
            source.write_text(
                '#include "ub_breakdown.h"\n'
                'int main() { sn_profile::flow_point("client", 1, "after_pop"); }\n',
                encoding="utf-8")
            build = subprocess.run([shutil.which("g++"), "-std=c++17", "-DERPC_UB",
                                    "-I" + str(app), str(source), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            for domain, profile, expected in (("0", "1", False), ("1", "0", False),
                                              ("1", "1", True)):
                env = dict(os.environ, ERPC_SN_DOMAIN_PROFILE=domain, ERPC_SN_PROFILE=profile)
                run = subprocess.run([str(binary)], env=env, capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual("SN_FLOW_EVENT" in run.stderr, expected)
                if expected:
                    self.assertIn("SN_FLOW_CONTRACT", run.stderr)

    def test_same_host_pairs_and_warmup(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "client.log").write_text(
                "SN_DOMAIN_MEASUREMENT source=1 lane=0 abi=2 warmup=1 measured=1 total=2\n"
                + event("client", 1, "after_pop", 10)
                + event("client", 1, "consume_done", 90)
                + event("client", 2, "after_pop", 100)
                + event("client", 2, "consume_done", 140)
                + "SN_FLOW_CONTRACT thread=1 events=4 dropped=0\n", encoding="utf-8")
            events, contracts, warmup, _ = flow.load_logs(directory, ("client",))
            self.assertEqual((contracts, warmup), (1, 1))
            output = io.StringIO()
            with redirect_stdout(output):
                flow.summarize("rpc", "A", events, flow.A_PAIRS, warmup)
            self.assertIn("stage=client_e2e count=1", output.getvalue())
            self.assertIn("p50_ns=40", output.getvalue())
            self.assertNotIn("p50_ns=80", output.getvalue())

    def test_duplicate_or_reversed_events_are_not_silently_attributed(self):
        events = {
            (2, "user_timeline", "storage_enqueue"): [100, 110],
            (2, "user_timeline", "storage_callback"): [300],
            (3, "user_timeline", "storage_enqueue"): [200],
            (3, "user_timeline", "storage_callback"): [150],
            (4, "user_timeline", "storage_enqueue"): [400],
            (4, "user_timeline", "storage_callback"): [500],
        }
        output = io.StringIO()
        with redirect_stdout(output):
            flow.summarize("rpc", "A", events, flow.A_PAIRS, 0)
        self.assertIn("stage=storage_roundtrip count=1 ambiguous=2 p50_ns=100", output.getvalue())

    def test_cross_process_same_host_gap_uses_only_one_host(self):
        a = {(1, "client", "enqueue_done"): [100],
             (1, "user_timeline", "request_arrival"): [125]}
        b = {(1, "load_balance", "forward_enqueue"): [100000],
             (1, "nginx", "forward_receive"): [100035]}
        output = io.StringIO()
        with redirect_stdout(output):
            flow.summarize("domain", "A", a, flow.A_PAIRS, 0)
            flow.summarize("rpc", "B", b, flow.B_PAIRS, 0)
        self.assertIn("stage=client_to_timeline_local count=1 ambiguous=0 p50_ns=25", output.getvalue())
        self.assertIn("stage=lb_to_nginx_local count=1 ambiguous=0 p50_ns=35", output.getvalue())

    def test_domain_wire_request_is_mapped_back_to_original(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "client.log").write_text(
                "SN_DOMAIN_MEASUREMENT warmup=1 measured=1\n"
                + event("client", 2, "enqueue_done", 100)
                + "SN_FLOW_EVENT role=client thread=1 request=5 stage=wire_identity at_ns=90 original=2 clock=steady_ns\n"
                + "SN_FLOW_CONTRACT thread=1 events=2 dropped=0\n", encoding="utf-8")
            (directory / "user_timeline.log").write_text(
                event("user_timeline", 5, "request_arrival", 125), encoding="utf-8")
            a, _, _, mapping = flow.load_logs(
                directory, ("client", "user_timeline"), require_map=True)
            self.assertEqual(mapping, {5: 2})
            self.assertEqual(a[(2, "user_timeline", "request_arrival")], [125])
            (directory / "post_storage.log").write_text(
                event("post_storage", 5, "request_arrival", 100000), encoding="utf-8")
            b, _, _, _ = flow.load_logs(
                directory, ("post_storage",), wire_map=mapping, require_map=True)
            self.assertEqual(b[(2, "post_storage", "request_arrival")], [100000])
            with self.assertRaisesRegex(ValueError, "missing original request"):
                flow.load_logs(directory, ("user_timeline",), require_map=True)

    def test_dropped_events_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "client.log").write_text(
                "SN_FLOW_CONTRACT thread=1 events=32768 dropped=1\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "dropped"):
                flow.load_logs(directory, ("client",))

    def test_multiple_client_lanes_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "client.log").write_text(
                "SN_DOMAIN_MEASUREMENT source=1 lane=0 warmup=1\n"
                "SN_DOMAIN_MEASUREMENT source=1 lane=1 warmup=1\n",
                encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "one client source/lane"):
                flow.load_logs(directory, ("client",))


if __name__ == "__main__":
    unittest.main()
