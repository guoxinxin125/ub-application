import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_write_probe import mock_headers


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "check_shared_probe", ROOT / "scripts" / "check-shared-probe.py")
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)

A_LOG = """UB_DOMAIN_SHARED_READ_PASS workers=2 rounds=4 generation=77 final_cell=1045
UB_DOMAIN_SHARED_ATOMIC_PASS workers=2 increments_per_worker=3 unique_old_values=6 final_counter=6
UB_DOMAIN_SHARED_CAMPAIGN_PASS workers=2 rounds=4 increments_per_worker=3 generation=77 final_cell=1045 final_counter=6 b_observation=check_owner_log
"""
B_LOG = """UB_DOMAIN_SHARED_READY generation=77 cell=0 counter=0
UB_DOMAIN_SHARED_OBSERVED generation=77 index=1 value=42
UB_DOMAIN_SHARED_OBSERVED generation=77 index=1 value=1045
UB_DOMAIN_SHARED_OBSERVED generation=77 index=2 value=6
"""


class SharedProbeTests(unittest.TestCase):
    def test_shared_service_uses_one_lse_rmw_without_retry(self):
        source = (ROOT / "kernel/ub-domain/social_services.S").read_text(encoding="utf-8")
        service = source.split("us_nginx_forward_shared_start:", 1)[1].split(
            "us_nginx_forward_shared_end:", 1)[0]
        self.assertIn(".arch_extension lse", source)
        self.assertEqual(service.count("ldaddal"), 1)
        self.assertRegex(service, r"(?m)^\s*ldaddal x4, x5, \[x3\]")
        self.assertNotRegex(service, r"\b(?:ldaxr|stlxr|ldxr|stxr)\b")

    def verify(self, a_log=A_LOG, b_log=B_LOG):
        with tempfile.TemporaryDirectory() as directory:
            a = Path(directory) / "a.log"
            b = Path(directory) / "b.log"
            a.write_text(a_log, encoding="utf-8")
            b.write_text(b_log, encoding="utf-8")
            return CHECKER.verify(a, b)

    def test_matching_remote_final_values(self):
        self.assertIn("UB_DOMAIN_SHARED_VISIBILITY_PASS workers=2", self.verify())

    def test_stale_owner_rejected(self):
        with self.assertRaisesRegex(ValueError, "generation"):
            self.verify(b_log=B_LOG.replace("generation=77", "generation=99"))

    def test_missing_final_remote_counter_rejected(self):
        with self.assertRaisesRegex(ValueError, "B final shared state mismatch"):
            self.verify(b_log=B_LOG.replace("index=2 value=6", "index=2 value=5"))

    def test_a_inconsistent_atomic_summary_rejected(self):
        with self.assertRaisesRegex(ValueError, "summaries disagree"):
            self.verify(a_log=A_LOG.replace("unique_old_values=6", "unique_old_values=5"))

    def test_a_failure_rejected(self):
        with self.assertRaisesRegex(ValueError, "contains a failure"):
            self.verify(a_log=A_LOG + "UB_DOMAIN_SHARED_WORKER_FAIL worker=1 errno=5\n")

    def test_b_owner_failure_rejected(self):
        with self.assertRaisesRegex(ValueError, "contains a failure"):
            self.verify(b_log=B_LOG + "UB_DOMAIN_OWNER_FAIL poll=-5\n")

    @unittest.skipUnless(shutil.which("gcc"), "requires native gcc")
    def test_probe_syntax_with_mock_uapi(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            mock_headers(tmp)
            command = [shutil.which("gcc"), "-std=gnu11", "-Wall", "-Wextra",
                       "-Werror", "-fsyntax-only", "-I" + str(tmp),
                       "-I" + str(ROOT / "include"),
                       "-I" + str(ROOT / "include/uapi"),
                       str(ROOT / "demo/ub/domain_shared_probe.c")]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(os.name == "nt" and shutil.which("gcc"),
                         "Windows mock checks orchestration, not UB atomics")
    def test_two_connection_mock_and_duplicate_ticket_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            mock_headers(tmp)
            stub = tmp / "mock_domain.c"
            stub.write_text(r'''
#include <sched.h>
#include <stdlib.h>
#include <lrpc/ub_social_client.h>
static int next_fd=1;
static __thread unsigned int local_cpu;
static uint64_t shared_cell, shared_counter;
int sched_getaffinity(int pid,size_t bytes,cpu_set_t *set) {
 (void)pid;(void)bytes;CPU_ZERO(set);CPU_SET(1,set);CPU_SET(2,set);return 0;
}
int sched_setaffinity(int pid,size_t bytes,const cpu_set_t *set) {
 (void)pid;(void)bytes;
 if(CPU_COUNT(set)!=1)return -1;
 local_cpu=CPU_ISSET(1,set)?1:2;return 0;
}
int us_client_open_shared(struct lrpc_handle *h) {
 h->fd=__atomic_fetch_add(&next_fd,1,__ATOMIC_RELAXED);return 0;
}
int us_client_info(const struct lrpc_handle *h,struct ud_connection_info *i) {
 *i=(struct ud_connection_info){.multi=1,.slot=(unsigned)(h->fd-1),
   .identity=(unsigned)(100+h->fd),.generation=77,.capacity=UD_CLIENTS};return 0;
}
int us_client_call(struct lrpc_handle *h,struct us_call *c) {
 (void)h;
 switch(c->message.type) {
 case US_SHARED_WRITE_REQ:
  __atomic_store_n(&shared_cell,c->message.value,__ATOMIC_RELEASE);
  c->message.ticket=__atomic_load_n(&shared_cell,__ATOMIC_ACQUIRE);break;
 case US_SHARED_READ_REQ:
  c->message.ticket=__atomic_load_n(
    c->message.request==US_SHARED_VALUE_INDEX?&shared_cell:&shared_counter,
    __ATOMIC_ACQUIRE);break;
 case US_SHARED_FETCH_ADD_REQ:
  c->message.ticket=getenv("TEST_DUPLICATE_TICKET")?0:
      __atomic_fetch_add(&shared_counter,1,__ATOMIC_ACQ_REL);break;
 default:return -1;
 }
 c->message.action=US_ACTION_RPC;c->message.target=105;
 c->entries=2;c->domain[0]=1;c->domain[1]=2;
 c->transitions=3;c->cpu=local_cpu;return 0;
}
int us_client_close(struct lrpc_handle *h) {h->fd=-1;return 0;}
''', encoding="utf-8")
            exe = tmp / "shared_probe.exe"
            command = [shutil.which("gcc"), "-std=gnu11", "-Wall", "-Wextra",
                       "-Werror", "-pthread", "-I" + str(tmp),
                       "-I" + str(ROOT / "include"),
                       "-I" + str(ROOT / "include/uapi"),
                       str(ROOT / "demo/ub/domain_shared_probe.c"), str(stub),
                       "-o", str(exe)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            args = [str(exe), "--cpus", "1,2", "--rounds", "4",
                    "--increments", "100", "--observe-ms", "0"]
            result = subprocess.run(args, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("UB_DOMAIN_SHARED_CAMPAIGN_PASS workers=2", result.stdout)
            result = subprocess.run(args, capture_output=True, text=True, timeout=15,
                                    env={**os.environ, "TEST_DUPLICATE_TICKET": "1"})
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("UB_DOMAIN_SHARED_CAMPAIGN_FAIL", result.stderr)


if __name__ == "__main__":
    unittest.main()
