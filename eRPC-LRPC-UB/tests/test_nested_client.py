"""Actual invoke function and demo CLIs against local fixtures; no UB/kernel."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

from test_nested_runtime import headers

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('gcc'), 'requires native gcc')
class Client(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.tmp = Path(cls.temp.name)
        headers(cls.tmp)
        (cls.tmp / 'sys').mkdir()
        (cls.tmp / 'sys/ioctl.h').write_text('int fixture_ioctl(int, unsigned long, ...);\n')
        (cls.tmp / 'fixture.h').write_text('''
#include <time.h>
int fixture_setenv(const char *, const char *, int);
int fixture_clock_gettime(int, struct timespec *);
#define setenv fixture_setenv
#define clock_gettime fixture_clock_gettime
#define ioctl fixture_ioctl
#define CLOCK_MONOTONIC_RAW 0
''')
        (cls.tmp / 'fixture.c').write_text(r'''
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lrpc/lrpc.h>
#include <lrpc/ub_nested_path.h>
static unsigned path, calls;
int fixture_setenv(const char *name, const char *value, int overwrite) {
 (void)overwrite;
 if(!strcmp(name,"LRPC_UB_NESTED_PATH")) return ud_nested_path_parse(value,&path);
 return strcmp(name,"LRPC_EXECUTION_BACKEND") || strcmp(value,"ub-remote-domain");
}
int fixture_clock_gettime(int kind, struct timespec *t) {
 static unsigned tick; (void)kind; t->tv_sec=0; t->tv_nsec=++tick; return 0;
}
int lrpc_bind(struct lrpc_handle *h, const char *device, uint32_t procedure, uint64_t epoch) {
 if(strcmp(device,"/dev/ub_lrpc_domain") || procedure!=1 || epoch!=1) return -1;
 memset(h,0,sizeof(*h)); h->fd=7; h->shadow_cpu=1;
 printf("FIXTURE_BIND path=%s\n",ud_nested_path_name(path)); return 0;
}
int lrpc_invoke(struct lrpc_handle *h, struct lrpc_astack *c) {
 uint64_t a=0,b=0,result; (void)h; calls++;
 if(getenv("TEST_FAIL")) { errno=EIO; return -1; }
 memcpy(&a,c->payload,8); if(c->request_size==16) memcpy(&b,c->payload+8,8);
 result=c->procedure_id==1 ? a+b+100 : c->procedure_id==2 ? a+2 : 1000+b;
 memcpy(c->payload,&result,8); c->response_size=8; c->status=0;
 c->caller_cpu_before=c->caller_cpu_after=1; return 0;
}
int fixture_ioctl(int fd, unsigned long command, ...) {
 va_list ap; void *arg; assert(fd==7); calls++;
 if(getenv("TEST_FAIL")) { errno=EIO; return -1; }
 va_start(ap,command); arg=va_arg(ap,void *); va_end(ap);
 if(command==UD_TRACE_CALL) {
  struct ud_trace_call *t=arg;
  int nested=t->procedure==2, fused=nested && path==UD_NESTED_FUSED;
  t->result=nested ? 43 : 142; t->cpu=1;
  t->transitions=nested ? ud_nested_transitions(path) : 2;
  t->segments=nested && !fused ? 3 : 1; t->segment_ns[0]=10; t->gate_total_ns=12;
  if(nested && !fused) t->segment_ns[1]=t->segment_ns[2]=t->handoff_ns[0]=t->handoff_ns[1]=1;
  if(getenv("TEST_STALE_TRANSITIONS")) t->transitions=6;
  return 0;
 }
 if(command==UD_TLB_PROBE) {
  struct ud_tlb_probe *p=arg;
  p->first_result=p->second_result=p->procedure==2 ? 43 : 142;
  p->first_ns=p->flush ? 1000 : 620; p->second_ns=600;
  p->first_cpu=p->second_cpu=1; return 0;
 }
 errno=ENOTTY; return -1;
}
int ud_lrpc_sample_remote_heap(struct lrpc_handle *h, uint64_t *v, uint64_t *ns) {
 (void)h; *v=100; *ns=440; return 0;
}
void lrpc_close(struct lrpc_handle *h) { (void)h; printf("FIXTURE_CLOSE calls=%u\n",calls); }
int ud_lrpc_connection_info(const struct lrpc_handle *h, struct ud_connection_info *info) {
 (void)h; memset(info,0,sizeof(*info)); info->multi=!!getenv("TEST_POOL"); return 0;
}
'''.replace('#include <errno.h>', '#include <errno.h>\n#include <assert.h>\n#include <time.h>'))
        cls.executables = {}
        for name in ('bench', 'trace', 'tlb_probe'):
            exe = cls.tmp / (name + '.exe')
            command = [shutil.which('gcc'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                       '-D__USE_MINGW_ANSI_STDIO=1', '-I' + str(cls.tmp),
                       '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'include/uapi'),
                       '-include', str(cls.tmp / 'fixture.h'),
                       str(ROOT / ('demo/ub/domain_' + name + '.c')),
                       str(cls.tmp / 'fixture.c'), '-o', str(exe)]
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode: raise AssertionError(result.stdout + result.stderr)
            cls.executables[name] = exe

    def run_demo(self, name, *args, **environment):
        env = dict(os.environ)
        for key in ('LRPC_UB_NESTED_PATH', 'TEST_FAIL', 'TEST_STALE_TRANSITIONS'):
            env.pop(key, None)
        env.update(environment)
        return subprocess.run([str(self.executables[name]), *args], env=env, capture_output=True, text=True)

    def test_cli_paths_and_environment_override(self):
        for demo in self.executables:
            for path in ('legacy', 'fused'):
                with self.subTest(demo=demo, path=path):
                    result = self.run_demo(demo, '--test', 'nested', '--nested-path', path,
                                           '--samples', '2', '--warmup', '1', LRPC_UB_NESTED_PATH='invalid')
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn('FIXTURE_BIND path=' + path, result.stdout)
                    self.assertIn('nested_path=' + path, result.stdout)
                    self.assertIn('_PASS', result.stdout)
            result = self.run_demo(demo, '--test', 'nested', '--samples', '1', '--warmup', '0',
                                   LRPC_UB_NESTED_PATH='fused')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('FIXTURE_BIND path=fused', result.stdout)

    def test_invalid_path_rejected_before_bind(self):
        for demo in self.executables:
            for args, env in ((['--nested-path', 'invalid'], {}),
                              ([], {'LRPC_UB_NESTED_PATH': 'invalid'}), (['--nested-path'], {})):
                result = self.run_demo(demo, *args, **env)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertNotIn('FIXTURE_BIND', result.stdout)

    def test_fused_trace_has_no_fake_subsegments(self):
        result = self.run_demo('trace', '--test', 'nested', '--nested-path', 'fused',
                               '--samples', '1', '--warmup', '0')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('field=fused_nested_gate', result.stdout)
        self.assertIn('transitions=4', result.stdout)
        self.assertNotIn('field=leaf_gate', result.stdout)
        self.assertNotIn('field=first_domain_gate', result.stdout)
        self.assertNotIn('handoff_leaf_ns=', result.stdout)
        result = self.run_demo('trace', '--test', 'nested', '--nested-path', 'fused',
                               '--samples', '1', '--warmup', '0', TEST_STALE_TRANSITIONS='1')
        self.assertEqual(result.returncode, 1)
        self.assertNotIn('UB_DOMAIN_TRACE_PASS', result.stdout)

    def test_default_fused_and_explicit_legacy(self):
        for demo in self.executables:
            for test in ('direct', 'nested'):
                result = self.run_demo(demo, '--test', test, '--samples', '1', '--warmup', '0')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('FIXTURE_BIND path=fused', result.stdout)
                if demo == 'trace':
                    self.assertIn('transitions=' + ('4' if test == 'nested' else '2'), result.stdout)
            result = self.run_demo(demo, '--test', 'nested', '--samples', '1', '--warmup', '0',
                                   LRPC_UB_NESTED_PATH='legacy')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('FIXTURE_BIND path=legacy', result.stdout)

    def test_profile_defaults_preserve_social_and_allow_explicit_fused(self):
        src, exe = self.tmp / 'path_defaults.c', self.tmp / 'path_defaults.exe'
        src.write_text(r'''
#include <assert.h>
#include <lrpc/ub_nested_path.h>
int main(void) {
 unsigned path;
 assert(!ud_nested_path_parse_profile(NULL,0,&path) && path==UD_NESTED_FUSED);
 assert(!ud_nested_path_parse_profile("legacy",0,&path) && path==UD_NESTED_LEGACY);
 assert(!ud_nested_path_parse_profile(NULL,1,&path) && path==UD_NESTED_LEGACY);
 assert(!ud_nested_path_parse_profile("legacy",1,&path) && path==UD_NESTED_LEGACY);
 assert(!ud_nested_path_parse_profile("fused",1,&path) && path==UD_NESTED_FUSED);
 assert(ud_nested_path_parse_profile("",0,&path)==-1 && errno==EINVAL);
 return 0;
}
''')
        result = subprocess.run([shutil.which('gcc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                 '-I' + str(self.tmp), '-I' + str(ROOT / 'include'),
                                 '-I' + str(ROOT / 'include/uapi'),
                                 str(src), '-o', str(exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_bench_emits_only_measured_samples_when_requested(self):
        args = ('--test', 'direct', '--samples', '3', '--warmup', '2')
        ordinary = self.run_demo('bench', *args)
        self.assertEqual(ordinary.returncode, 0, ordinary.stderr)
        self.assertNotIn('UB_DOMAIN_SAMPLES ', ordinary.stdout)
        measured = self.run_demo('bench', *args, '--emit-samples')
        self.assertEqual(measured.returncode, 0, measured.stderr)
        lines = [line for line in measured.stdout.splitlines()
                 if line.startswith('UB_DOMAIN_SAMPLES ')]
        self.assertEqual(len(lines), 1)
        self.assertIn('case=direct count=3 ', lines[0])
        self.assertEqual(len(lines[0].split('values_ns=')[1].split(',')), 3)

    def test_owner_restart_marker_matches_pool_mode(self):
        for demo in self.executables:
            result = self.run_demo(demo, '--samples', '1', '--warmup', '0', TEST_POOL='1')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('restart_owner_before_next_connection=0', result.stdout)

    def test_failures_close_without_pass(self):
        for demo in self.executables:
            result = self.run_demo(demo, '--test', 'nested', '--nested-path', 'fused',
                                   '--samples', '1', '--warmup', '0', TEST_FAIL='1')
            self.assertEqual(result.returncode, 1)
            self.assertIn('FIXTURE_CLOSE', result.stdout)
            self.assertNotIn('_PASS', result.stdout)

    def test_actual_mapped_invoke_checks_selected_path(self):
        source = (ROOT / 'lib/ub_domain_client.c').read_text(encoding='utf-8')
        client = source[source.index('struct ud_client {'):source.index('static int ud_error')]
        invoke = source[source.index('int ud_lrpc_invoke('):source.index('int ud_lrpc_sample_remote_heap(')]
        fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <lrpc/lrpc.h>
#include <lrpc/ub_nested_path.h>
struct lrpc_ubsm_region { void *base; };
''' + client + r'''
static struct ud_client client;
static unsigned calls, stale, wrong_path, fail_call, wrong_cpu;
static int sched_getcpu(void) { return wrong_cpu ? 2 : 1; }
#define getpid fixture_getpid
static int fixture_getpid(void) { return 123; }
static int ud_error(int rc) { errno=-rc; return -1; }
static int ioctl(int fd, unsigned long command, unsigned long procedure) {
 assert(fd==7 && command==UD_MAPPED_CALL); calls++;
 struct ud_mapped *m=(void *)((char *)client.local_args+UD_MIDDLE*client.page_size);
 assert(procedure==2);
 if(fail_call) { errno=EIO; return -1; }
 m->result=m->a+2; m->procedure=2; m->cpu=1; m->status=0;
 m->transitions=wrong_path ? 6 : ud_nested_transitions(client.nested_path);
 if(!stale) __atomic_store_n(&m->completion,m->completion+1,__ATOMIC_RELEASE);
 return 0;
}
''' + invoke + r'''
static void *wrong_owner(void *arg) {
 struct lrpc_handle *h=arg;
 struct lrpc_astack *c=calloc(1,sizeof(*c)); assert(c);
 c->abi=LRPC_ASTACK_ABI; c->procedure_id=2; c->request_size=8;
 assert(ud_lrpc_invoke(h,c)==-1 && errno==EPERM); free(c); return NULL;
}
int main(void) {
 client.owner=pthread_self();
 client.page_size=4096; client.map_size=4*4096; client.local_args=calloc(1,client.map_size);
 struct lrpc_handle h={.fd=7,.shadow_cpu=1,.astack_map=&client};
 struct lrpc_astack *c=calloc(1,sizeof(*c)); assert(client.local_args && c);
 c->abi=LRPC_ASTACK_ABI; c->procedure_id=2; c->request_size=8;
 for(unsigned path=0;path<=1;path++) {
  client.nested_path=path;
  for(uint64_t i=0;i<1000;i++) {
   memcpy(c->payload,&i,8); assert(!ud_lrpc_invoke(&h,c)); uint64_t result;
   memcpy(&result,c->payload,8); assert(result==i+2 && !client.invoking);
  }
 }
 wrong_path=1; assert(ud_lrpc_invoke(&h,c)==-1 && errno==EPROTO && !client.invoking);
 wrong_path=0; stale=1; assert(ud_lrpc_invoke(&h,c)==-1 && errno==EPROTO && !client.invoking);
 stale=0; fail_call=1; assert(ud_lrpc_invoke(&h,c)==-1 && errno==EIO && !client.invoking);
 fail_call=0; unsigned old=calls; client.invoking=1;
 assert(ud_lrpc_invoke(&h,c)==-1 && errno==EBUSY && calls==old);
 client.invoking=0; wrong_cpu=1; assert(ud_lrpc_invoke(&h,c)==-1 && errno==EXDEV && calls==old);
 wrong_cpu=0; pthread_t other; assert(!pthread_create(&other,NULL,wrong_owner,&h));
 assert(!pthread_join(other,NULL) && calls==old);
 free(c); free(client.local_args); return 0;
}
'''
        src, exe = self.tmp / 'mapped.c', self.tmp / 'mapped.exe'
        src.write_text(fixture)
        result = subprocess.run([shutil.which('gcc'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                                 '-I' + str(self.tmp), '-I' + str(ROOT / 'include'),
                                 '-I' + str(ROOT / 'include/uapi'), str(src), '-pthread', '-o', str(exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__': unittest.main(verbosity=2)
