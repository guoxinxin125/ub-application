import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_nested_runtime import headers


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "check_write_probe", ROOT / "scripts" / "check-write-probe.py")
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)

A_LOG = """UB_DOMAIN_WRITE_A_PASS worker=0 cpu=1 slot=3 identity=101 index=1 value=44 ticket=44 transitions=3
UB_DOMAIN_WRITE_A_PASS worker=0 cpu=1 slot=3 identity=101 index=2 value=43 ticket=43 transitions=3
UB_DOMAIN_WRITE_A_PASS worker=1 cpu=2 slot=5 identity=102 index=1 value=1000044 ticket=1000044 transitions=3
UB_DOMAIN_WRITE_A_PASS worker=1 cpu=2 slot=5 identity=102 index=2 value=1000043 ticket=1000043 transitions=3
UB_DOMAIN_WRITE_CAMPAIGN_PASS workers=2 calls_per_worker=2 rewrites_per_worker=1 final_cells=4 b_observation=check_owner_log
"""
B_LOG = """UB_DOMAIN_WRITE_SLOT_READY slot=3 identity=101
UB_DOMAIN_WRITE_OBSERVED slot=3 identity=101 index=1 value=42
UB_DOMAIN_WRITE_OBSERVED slot=3 identity=101 index=1 value=44
UB_DOMAIN_WRITE_OBSERVED slot=3 identity=101 index=2 value=43
UB_DOMAIN_WRITE_SLOT_READY slot=5 identity=102
UB_DOMAIN_WRITE_OBSERVED slot=5 identity=102 index=1 value=1000044
UB_DOMAIN_WRITE_OBSERVED slot=5 identity=102 index=2 value=1000043
"""


def mock_headers(tmp):
    headers(tmp)
    if os.name == "nt":
        (tmp / "sched.h").write_text(
            "#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n"
            "typedef struct { uint64_t bits[16]; } cpu_set_t;\n"
            "#define CPU_SETSIZE 1024\n"
            "#define CPU_ZERO(s) memset((s),0,sizeof(*(s)))\n"
            "#define CPU_SET(n,s) ((s)->bits[(n)/64] |= 1ULL<<((n)%64))\n"
            "#define CPU_ISSET(n,s) (!!((s)->bits[(n)/64] & (1ULL<<((n)%64))))\n"
            "static inline int CPU_COUNT(const cpu_set_t *s) { int n=0; for(int i=0;i<16;i++) n+=__builtin_popcountll(s->bits[i]); return n; }\n"
            "int sched_getaffinity(int, size_t, cpu_set_t *);\n"
            "int sched_setaffinity(int, size_t, const cpu_set_t *);\n",
            encoding="utf-8")


class WriteProbeLogTests(unittest.TestCase):
    def check(self, a_log=A_LOG, b_log=B_LOG):
        with tempfile.TemporaryDirectory() as directory:
            a_path = Path(directory) / "a.log"
            b_path = Path(directory) / "b.log"
            a_path.write_text(a_log, encoding="utf-8")
            b_path.write_text(b_log, encoding="utf-8")
            return CHECKER.verify(a_path, b_path)

    def test_two_connections_and_rewrite_final_values(self):
        self.assertIn("UB_DOMAIN_WRITE_VISIBILITY_PASS workers=2 final_cells=4",
                      self.check())

    def test_missing_b_observation_fails(self):
        with self.assertRaisesRegex(ValueError, "B final value mismatch"):
            self.check(b_log=B_LOG.replace(
                "UB_DOMAIN_WRITE_OBSERVED slot=5 identity=102 index=2 value=1000043\n", ""))

    def test_stale_previous_connection_does_not_pass(self):
        with self.assertRaisesRegex(ValueError, "B did not announce"):
            self.check(b_log=B_LOG.replace("identity=102", "identity=999"))

    def test_old_value_after_rewrite_fails(self):
        with self.assertRaisesRegex(ValueError, "B final value mismatch"):
            self.check(b_log=B_LOG.replace(
                "UB_DOMAIN_WRITE_OBSERVED slot=3 identity=101 index=1 value=44\n", ""))

    def test_a_success_marker_required(self):
        with self.assertRaisesRegex(ValueError, "successful campaign"):
            self.check(a_log=A_LOG.replace(A_LOG.splitlines()[-1] + "\n", ""))

    def test_a_duplicate_result_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate or invalid A result"):
            self.check(a_log=A_LOG + A_LOG.splitlines()[0] + "\n")

    def test_owner_failure_rejected(self):
        with self.assertRaisesRegex(ValueError, "B owner log contains a failure"):
            self.check(b_log=B_LOG + "UB_DOMAIN_OWNER_FAIL poll=-5\n")

    @unittest.skipUnless(shutil.which("gcc"), "requires native gcc")
    def test_c_probe_syntax_with_mock_uapi(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            mock_headers(tmp)
            command = [shutil.which("gcc"), "-std=gnu11", "-Wall", "-Wextra",
                       "-Werror", "-fsyntax-only", "-I" + str(tmp),
                       "-I" + str(ROOT / "include"),
                       "-I" + str(ROOT / "include/uapi"),
                       str(ROOT / "demo/ub/domain_write_probe.c")]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(os.name == "nt" and shutil.which("gcc"),
                         "Windows mock exercises concurrency without UB hardware")
    def test_c_probe_two_workers_with_mock_domain(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            mock_headers(tmp)
            stub = tmp / "mock_domain.c"
            stub.write_text(r'''
#include <sched.h>
#include <lrpc/ub_social_client.h>
static int next_fd = 1;
static __thread unsigned int local_cpu;
int sched_getaffinity(int pid, size_t bytes, cpu_set_t *set) {
    (void)pid; (void)bytes; CPU_ZERO(set); CPU_SET(1,set); CPU_SET(2,set); return 0;
}
int sched_setaffinity(int pid, size_t bytes, const cpu_set_t *set) {
    (void)pid; (void)bytes;
    if (CPU_COUNT(set)!=1) return -1;
    local_cpu = CPU_ISSET(1,set) ? 1 : 2; return 0;
}
int us_client_open_write(struct lrpc_handle *h) {
    h->fd = __atomic_fetch_add(&next_fd,1,__ATOMIC_RELAXED); return 0;
}
int us_client_info(const struct lrpc_handle *h, struct ud_connection_info *i) {
    *i=(struct ud_connection_info){.multi=1,.slot=(unsigned)(h->fd-1),
        .identity=(unsigned)(100+h->fd),.generation=77,.capacity=UD_CLIENTS};
    return 0;
}
int us_client_call(struct lrpc_handle *h, struct us_call *c) {
    (void)h; c->message.ticket=c->message.value;
    c->message.action=US_ACTION_RPC; c->message.target=104;
    c->entries=2; c->domain[0]=1; c->domain[1]=2;
    c->transitions=3; c->cpu=local_cpu; return 0;
}
int us_client_close(struct lrpc_handle *h) { h->fd=-1; return 0; }
''', encoding="utf-8")
            exe = tmp / "write_probe.exe"
            command = [shutil.which("gcc"), "-std=gnu11", "-Wall", "-Wextra",
                       "-Werror", "-pthread", "-I" + str(tmp),
                       "-I" + str(ROOT / "include"),
                       "-I" + str(ROOT / "include/uapi"),
                       str(ROOT / "demo/ub/domain_write_probe.c"), str(stub),
                       "-o", str(exe)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe), "--cpus", "1,2", "--calls", "2",
                                     "--rewrites", "1", "--observe-ms", "0"],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count("UB_DOMAIN_WRITE_A_PASS"), 4)
            self.assertIn("UB_DOMAIN_WRITE_CAMPAIGN_PASS workers=2", result.stdout)


if __name__ == "__main__":
    unittest.main()
