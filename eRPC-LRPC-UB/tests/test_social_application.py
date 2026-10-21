"""Compile actual application policy blocks; SDK/DB/network are not exercised.

Unlike adapter-only tests, these cover Client configuration/response policy,
DB failure-before-ready and the auxiliary NUMA allocator. No full app boot.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT.parent/'eRPC/apps/social_network_cxl'


@unittest.skipUnless(shutil.which('g++'), 'requires native g++')
class Application(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)

    def compile(self, name, source, *flags):
        cpp, exe = self.directory/(name+'.cpp'), self.directory/(name+'.exe')
        cpp.write_text(source, encoding='utf-8')
        result = subprocess.run([shutil.which('g++'), '-std=c++17', '-O2', '-Wall', '-Wextra',
            '-Werror', '-pedantic', '-pthread', '-I'+str(APP), *flags, str(cpp), '-o', str(exe)],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        return exe

    def run_case(self, exe, *args, **overrides):
        env = {k:v for k,v in os.environ.items() if not k.startswith('ERPC_SN_')}
        env.update(ERPC_SN_DOMAIN_MEASURE='1', ERPC_SN_DOMAIN_FINISH='1',
                   ERPC_SN_DOMAIN_FIXTURE='0')
        env.update(overrides)
        return subprocess.run([str(exe), *args], env=env, capture_output=True, text=True, timeout=10)

    def test_actual_measurement_validation_accepts_real_data_and_rejects_bad_workloads(self):
        source = (APP/'client/client.h').read_text(encoding='utf-8')
        start = source.index('  try {\n    if ((sn_domain::enabled()')
        block = source[start:source.index('\n}\n\nstd::string generate_random_string', start)]
        exe = self.compile('measurement', r'''
#include <cstring>
#include <cerrno>
#include "domain_measure.h"
static unsigned FLAGS_client_num=1, FLAGS_server_num=1, FLAGS_concurrency=1;
static unsigned user_num_=1, home_num_=0, write_num_=0;
static unsigned generate_data_num=10;
namespace sn_domain {
static bool boolean_env(const char *key) {const char *s=std::getenv(key); return s && !std::strcmp(s,"1");}
static bool enabled() {return boolean_env("ERPC_SN_DOMAIN_PROXY");}
static unsigned source_count() {return 1;}
[[noreturn]] static void fail(const char *,unsigned,const char *,unsigned,int) {std::exit(20);}
}
static void validate() {
''' + block + r'''
}
int main(int argc,char **argv) {
 if(argc>1) FLAGS_concurrency=static_cast<unsigned>(std::atoi(argv[1]));
 validate(); std::puts("CONFIG_PASS");
}
''')
        for domain in ('0','1'):
            for fixture in ('0','1'):
                env = dict(ERPC_SN_DOMAIN_PROXY=domain, ERPC_SN_RPC_BASELINE='0' if domain=='1' else '1',
                           ERPC_SN_DOMAIN_FIXTURE=fixture)
                result = self.run_case(exe, **env)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('CONFIG_PASS', result.stdout)
                self.assertNotEqual(self.run_case(exe, '2', **env).returncode, 0)
                env['ERPC_SN_DOMAIN_FINISH']='0'
                self.assertNotEqual(self.run_case(exe, **env).returncode, 0)
        self.assertNotEqual(self.run_case(exe, ERPC_SN_RPC_BASELINE='1',
                            ERPC_SN_DOMAIN_WARMUP='10').returncode, 0)

    def test_actual_baseline_response_fixture_is_opt_in(self):
        source = (APP/'client/client.cpp').read_text(encoding='utf-8')
        start = source.index('        const PostData *data = post.buffer.buf_')
        block = source[start:source.index('        const uint64_t begin', start)]
        exe = self.compile('response', r'''
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include "domain_fixture.h"
namespace sn_domain {
static bool boolean_env(const char *key) {const char *s=std::getenv(key); return s && !std::strcmp(s,"1");}
[[noreturn]] static void fail(const char *,unsigned,const char *,unsigned,int) {std::exit(20);}
}
struct Imported {struct {unsigned char *buf_;} buffer;};
static void release_imported_post(void *,Imported &) {}
struct Origin {std::atomic<unsigned> domain_fixture_nonempty{0},domain_fixture_empty{0};};
struct Context {void *rpc_=nullptr;};
static void response(bool empty,bool vm) {
 PostData actual{}; actual.init(); actual.post_id=42; actual.creator_user_id=0;
 if(vm) {actual.post_id=100000; actual.text_length=16; std::memcpy(actual.text,"social-vm-post-0",16);}
 Imported post{{empty ? nullptr : reinterpret_cast<unsigned char *>(&actual)}};
 Origin owner; Origin *origin=&owner; Context context; Context *ctx=&context;
 struct {unsigned count;} reference_resp{empty ? 0U : 1U};
 const unsigned req_number=1;
''' + block + r'''
 const bool fixture=sn_domain::boolean_env("ERPC_SN_DOMAIN_FIXTURE");
 if(origin->domain_fixture_nonempty.load()!=(fixture && !empty ? 1U : 0U) ||
    origin->domain_fixture_empty.load()!=(fixture && empty ? 1U : 0U)) std::exit(21);
 std::puts("RESPONSE_PASS");
}
int main(int argc,char **argv) {response(argc>1 && !std::strcmp(argv[1],"empty"),
                                        argc>1 && !std::strcmp(argv[1],"vm"));}
''')
        for case in ('real','empty','vm'):
            result = self.run_case(exe, case)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_case(exe, 'vm', ERPC_SN_DOMAIN_FIXTURE='1').returncode, 0)
        for case in ('real','empty'):
            self.assertEqual(self.run_case(exe, case, ERPC_SN_DOMAIN_FIXTURE='1').returncode, 20)

    def test_actual_db_query_failure_never_reaches_ready(self):
        source = (APP/'utils_mongodb.h').read_text(encoding='utf-8')
        helpers = source[source.index('[[noreturn]]'):source.index('static inline mongoc_client_pool_t*')]
        exe = self.compile('db', r'''
#include <cstdio>
#include <cstdlib>
#include <cstdint>
struct bson_error_t {uint32_t domain,code;};
struct mongoc_cursor_t {bool failed;};
static bool mongoc_cursor_error(mongoc_cursor_t *c,bson_error_t *e) {
 e->domain=7; e->code=89; return c->failed;
}
''' + helpers + r'''
int main(int argc,char **) {
 mongoc_cursor_t cursor{argc>1};
 require_mongodb_query_success(argc>2 ? nullptr : &cursor,"user_timeline");
 std::puts("READY");
}
''')
        # End of an empty, successful cursor is valid; no nonempty requirement.
        self.assertEqual(self.run_case(exe).returncode, 0)
        for args in (('query-failed',), ('null','cursor')):
            result = self.run_case(exe, *args)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('SN_MONGODB_ERROR', result.stderr)
            self.assertNotIn('READY', result.stdout)
        for service, ready in (('user_timeline','mongodb_init_finished.store'),
                               ('post_storage','domain_storage_ready.store')):
            code = (APP/service/(service+'.cpp')).read_text(encoding='utf-8')
            check = code.index('require_mongodb_query_success(cursor, "'+service+'")')
            self.assertLess(check, code.index(ready))
            self.assertLess(check, code.index('mongoc_cursor_destroy(cursor)',check))

    def test_actual_measurement_report_labels_fixture_truthfully(self):
        exe = self.compile('report', r'''
#include "domain_measure.h"
int main() {
 sn_measure::Campaign c; c.configure(2);
 if(!c.record(1,1,0,100,130,170,200,10,0) ||
    !c.record(2,2,1,300,340,420,500,0,2048) || !c.report(1,0)) return 1;
 sn_measure::Campaign bad; bad.configure(1);
 if(bad.record(1,1,0,100,90,170,200,0,0) ||
    bad.record(1,1,0,100,130,170,200,40,0)) return 2;
}
''', '-std=c++11')
        for fixture in ('0','1'):
            result = self.run_case(exe, ERPC_SN_DOMAIN_FIXTURE=fixture)
            self.assertEqual(result.returncode,0,result.stderr)
            rows = [line for line in result.stderr.splitlines() if line.startswith('SN_DOMAIN_CLASS ')]
            self.assertEqual(len(rows),6)
            self.assertTrue(all('fixture_validation_included='+fixture in row for row in rows))
            parts = [line for line in result.stderr.splitlines()
                     if line.startswith('SN_DOMAIN_BREAKDOWN ') and
                     'phase=measured class=all' in line]
            self.assertEqual(len(parts),4)
            self.assertTrue(any('stage=prepare_to_send count=2' in row and 'avg_ns=35.0' in row
                                for row in parts))
            self.assertTrue(any('stage=send_to_handler count=2' in row and 'avg_ns=60.0' in row
                                for row in parts))
            self.assertTrue(any('stage=handler_to_consume count=2' in row and 'avg_ns=55.0' in row
                                for row in parts))
            self.assertTrue(any('stage=forward_domain count=1' in row and 'composition=prepare-subset' in row
                                for row in parts))
            self.assertIn('phase=measured class=nonempty stage=send_to_handler count=1', result.stderr)
            self.assertNotIn('phase=measured class=nonempty stage=forward_domain', result.stderr)

    def test_actual_auxiliary_cpu_allocator_supports_sparse_asymmetric_nodes(self):
        source = (APP/'social_network_commons.h').read_text(encoding='utf-8')
        allocator = source[source.index('inline size_t get_bind_core(size_t numa)'):]
        program = r'''
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdio>
namespace erpc {
static std::vector<size_t> get_lcores_for_numa_node(size_t node) {
 if(node==0) return {0,1};
 if(node==1) return {20,21,22};
 if(node==2) return {40};
 if(node==3) return {60,61};
 return {};
}
}
''' + allocator + r'''
#define CHECK(x) do {if(!(x)) throw std::runtime_error(#x);} while(0)
int main() {
 CHECK(get_bind_core(0)==0); CHECK(get_bind_core(1)==0);
 CHECK(get_bind_core(2)==0); CHECK(get_bind_core(3)==0);
 CHECK(get_bind_core(3)==1); CHECK(get_bind_core(0)==1); CHECK(get_bind_core(1)==1);
 for(size_t node : {size_t(2),size_t(3),size_t(42),size_t(2)}) {
  bool failed=false; try {get_bind_core(node);} catch(const std::out_of_range &) {failed=true;}
  CHECK(failed);
 }
 CHECK(get_bind_core(1)==2); std::puts("NUMA_PASS");
}
'''
        for name,flags in (('numa-debug',()),('numa-release',('-DNDEBUG',))):
            exe = self.compile(name,program,*flags)
            result = self.run_case(exe)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_ub_package_does_not_carry_unused_scratch_or_legacy_publishers(self):
        client = (ROOT/'lib/ub_domain_client.c').read_text(encoding='utf-8')
        api = (ROOT/'include/lrpc/lrpc.h').read_text(encoding='utf-8')
        backend = (ROOT/'lib/backend_ubsm.c').read_text(encoding='utf-8')
        self.assertNotIn('invoke_scratch',client+api)
        for symbol in ('lrpc_ubsm_publish_services','lrpc_ubsm_localize_code','lrpc_ubsm_lookup_procedure'):
            self.assertNotIn(symbol,backend)
        for symbol in ('lrpc_publish(', 'lrpc_shadow_serve(', 'int lrpc_invoke_bytes('):
            self.assertNotIn(symbol,api)


if __name__ == '__main__': unittest.main(verbosity=2)
