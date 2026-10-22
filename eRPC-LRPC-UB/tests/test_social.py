"""Local UB SocialNetwork protocol/adapter tests. No actual UB, MongoDB or MMU."""
from pathlib import Path
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import unittest

from test_nested_runtime import headers

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT.parent/'eRPC/apps/social_network_cxl'
spec = importlib.util.spec_from_file_location('prepare_social_ub', ROOT/'scripts/prepare-social.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class Config(unittest.TestCase):
    def test_independent_ab_templates_and_default_cli(self):
        templates = [ROOT/'config'/('social-user-read-'+side+'.json') for side in ('a','b')]
        before = [path.read_bytes() for path in templates]
        source_a, source_b = [json.loads(value) for value in before]
        for value in before:
            self.assertNotIn(b'IP98', value)
            self.assertNotIn(b'IP99', value)
        self.assertEqual(source_a['common']['numa_client_node'], 0)
        self.assertEqual(source_b['common']['numa_client_node'], 1)
        expected = [prepare.prepare(template, '192.0.2.10', '192.0.2.20',
                                    samples=11, numa_a=2, numa_b=3)[index]
                    for index, template in enumerate((source_a, source_b))]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)/'campaign'
            command = [os.sys.executable, str(ROOT/'scripts/prepare-social.py'),
                       '--output', str(output), '--host-a', '192.0.2.10',
                       '--host-b', '192.0.2.20', '--numa-a', '2', '--numa-b', '3',
                       '--samples', '11']
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for side, config in zip(('a','b'), expected):
                actual = json.loads((output/('config-'+side+'.json')).read_text())
                self.assertEqual(actual, config)
                self.assertEqual(actual['user_timeline_mongodb']['port'], '22012' if side=='a' else '20012')
                self.assertEqual(actual['post_storage_mongodb']['port'], '20014')
                self.assertNotIn('A_IP', json.dumps(actual))
                self.assertNotIn('B_IP', json.dumps(actual))
        self.assertEqual(before, [path.read_bytes() for path in templates])

    def test_home_read_private_config_and_roles(self):
        module_spec = importlib.util.spec_from_file_location('home_roles', ROOT/'scripts/check-social-cpus.py')
        module = importlib.util.module_from_spec(module_spec); module_spec.loader.exec_module(module)
        template = json.loads((ROOT/'config/social-user-read-a.json').read_text(encoding='utf-8'))
        a, b = prepare.prepare(template, '192.0.2.10', '192.0.2.20', workload='home')
        for config in (a, b):
            self.assertEqual((config['client']['user_num'], config['client']['home_num']), (0, 1))
            self.assertEqual(config['social_graph_mongodb']['connections'],
                             config['user_timeline_mongodb']['connections'])
            self.assertEqual(module.services_for(config, 'A', 'domain'), ['home_timeline', 'client'])
            self.assertEqual(module.services_for(config, 'B', 'rpc'), ['post_storage', 'nginx', 'load_balance'])
            self.assertTrue(config['home_timeline']['server_addr'].startswith('192.0.2.10:'))
            self.assertTrue(config['user_timeline']['server_addr'].startswith('192.0.2.20:'))

    def test_home_log_checker_selects_home_timeline(self):
        module_spec = importlib.util.spec_from_file_location('home_log_check', ROOT/'scripts/check-social.py')
        module = importlib.util.module_from_spec(module_spec); module_spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            a, b = Path(directory)/'a', Path(directory)/'b'
            a.mkdir(); b.mkdir()
            (a/'client.log').write_text(
                'SN_DOMAIN_READY backend=ub chain=fused transitions=3 slot=0 identity=100\n'
                'SN_DOMAIN_DONE backend=ub calls=2\n'
                'SN_DOMAIN_E2E completed=2 expected=2 measured=2 clock=steady_ns scope=call-to-consume fixture=0\n'
                'SN_DOMAIN_MEASUREMENT total=2 measured=2\n'
                'SN_DOMAIN_CLASS phase=measured class=all count=2\n'
                'SN_DOMAIN_CLASS phase=measured class=empty count=1\n'
                'SN_DOMAIN_CLASS phase=measured class=nonempty count=1\n')
            (a/'home_timeline.log').write_text(
                'SN_DOMAIN_READY backend=ub chain=fused transitions=3 slot=1 identity=101\n'
                'SN_DOMAIN_DONE backend=ub calls=2\n')
            (b/'post_storage.log').write_text('storage ready\n')
            self.assertEqual(module.check(a, 'domain', 2, b, 'fused', 'home')['nonempty'], 1)
            with self.assertRaises(FileNotFoundError): module.check(a, 'domain', 2, b, 'fused', 'user')

    def test_cpu_preflight_rejects_disallowed_or_overlapping_blocks(self):
        module_spec = importlib.util.spec_from_file_location('cpu_preflight', ROOT/'scripts/check-social-cpus.py')
        module = importlib.util.module_from_spec(module_spec);module_spec.loader.exec_module(module)
        config = {'common':{'client_num':1,'server_num':1,'numa_client_node':0,'numa_server_node':0},
                  'client':{'bind_core_offset':0},'user_timeline':{'bind_core_offset':3}}
        self.assertEqual(module.cpulist('0-3,6,8-9'),[0,1,2,3,6,8,9])
        self.assertEqual(module.check(config,['client','user_timeline'],{0:list(range(10))},set(range(10))),list(range(6)))
        with self.assertRaises(ValueError): module.check(config,['client'],{0:list(range(10))},{0,1})
        config['user_timeline']['bind_core_offset']=2
        with self.assertRaises(ValueError): module.check(config,['client','user_timeline'],{0:list(range(10))},set(range(10)))

    def test_log_acceptance_requires_completed_calls_and_distinct_connections(self):
        module_spec = importlib.util.spec_from_file_location('log_check', ROOT/'scripts/check-social.py')
        module = importlib.util.module_from_spec(module_spec);module_spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            peer = path/'b'; peer.mkdir()
            client = ('SN_DOMAIN_READY backend=ub chain=legacy transitions=4 slot=0 identity=100\n'
                      'SN_DOMAIN_DONE backend=ub calls=10\n'
                      'SN_DOMAIN_E2E completed=10 expected=10 measured=10 clock=steady_ns scope=call-to-consume fixture=0\n'
                      'SN_DOMAIN_MEASUREMENT total=10 measured=10\n'
                      'SN_DOMAIN_CLASS phase=measured class=all count=10\n'
                      'SN_DOMAIN_CLASS phase=measured class=empty count=2\n'
                      'SN_DOMAIN_CLASS phase=measured class=nonempty count=8\n')
            timeline = 'SN_DOMAIN_READY backend=ub chain=legacy transitions=4 slot=1 identity=101\nSN_DOMAIN_DONE backend=ub calls=10\n'
            (path/'client.log').write_text(client);(path/'user_timeline.log').write_text(timeline)
            (peer/'post_storage.log').write_text('storage loaded\n')
            with self.assertRaises(ValueError): module.check(path,'domain',10)
            self.assertEqual(module.check(path,'domain',10,peer)['nonempty'],8)
            with self.assertRaises(ValueError): module.check(path,'domain',10,peer,'fused')
            (path/'client.log').write_text(client.replace('chain=legacy transitions=4','chain=fused transitions=3'))
            (path/'user_timeline.log').write_text(timeline.replace('chain=legacy transitions=4','chain=fused transitions=3'))
            self.assertEqual(module.check(path,'domain',10,peer,'fused')['nonempty'],8)
            command = [os.sys.executable, str(ROOT/'scripts/check-social.py'),
                       '--logs', str(path), '--peer-logs', str(peer),
                       '--mode', 'domain', '--samples', '10']
            result = subprocess.run(command,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('chain=fused',result.stdout)
            (path/'client.log').write_text(client)
            (path/'user_timeline.log').write_text(timeline)
            for bad in (timeline.replace('slot=1','slot=0'),timeline.replace('calls=10','calls=9'),
                        timeline+'SN_DOMAIN_ERROR op=fault\n'):
                (path/'user_timeline.log').write_text(bad)
                with self.assertRaises(ValueError): module.check(path,'domain',10,peer)
            (path/'user_timeline.log').write_text('SN_DOMAIN_CONFIG enabled=0\n')
            (path/'client.log').write_text(client.replace('SN_DOMAIN_READY backend=ub chain=legacy transitions=4 slot=0 identity=100',
                                                       'SN_DOMAIN_CONFIG enabled=0'))
            for name in ('nginx','load_balance'): (peer/(name+'.log')).write_text('started\n')
            self.assertEqual(module.check(path,'rpc',10,peer)['all'],10)
            for marker in ('SN_DOMAIN_ERROR','SN_MONGODB_ERROR','SN_DOMAIN_CONFIG_ERROR'):
                (peer/'post_storage.log').write_text(marker+' op=storage-fault\n')
                with self.assertRaises(ValueError): module.check(path,'rpc',10,peer)
            (path/'post_storage.log').write_text('stale local storage log\n')
            (peer/'post_storage.log').unlink()
            with self.assertRaises(FileNotFoundError): module.check(path,'rpc',10,peer)

    def test_private_topology_and_database_preservation(self):
        original = json.loads((APP/'config/config.ub98.json').read_text(encoding='utf-8'))
        before = json.dumps(original, sort_keys=True)
        a, b = prepare.prepare(original, '192.0.2.98', '192.0.2.99')
        self.assertEqual(json.dumps(original, sort_keys=True), before)
        for c, numa in ((a, 0), (b, 1)):
            self.assertEqual(c['common']['numa_client_node'], numa)
            self.assertEqual(c['client']['user_num'], 1)
            self.assertEqual(c['client']['home_num']+c['client']['write_num'], 0)
            for service in ('client','user_timeline'):
                self.assertTrue(c[service]['server_addr'].startswith('192.0.2.98:'))
            for service in ('post_storage','nginx','load_balance'):
                self.assertTrue(c[service]['server_addr'].startswith('192.0.2.99:'))
            for key in original:
                if key.endswith('_mongodb'): self.assertEqual(original[key], c[key])

    def test_explicit_database_overrides_only_and_service_roles(self):
        original = json.loads((APP/'config/config.ub98.json').read_text(encoding='utf-8'))
        a,b = prepare.prepare(original,'192.0.2.99','192.0.2.98',
                              timeline_db_host='127.0.0.1',timeline_db_port=22012)
        module_spec = importlib.util.spec_from_file_location('roles',ROOT/'scripts/check-social-cpus.py')
        module = importlib.util.module_from_spec(module_spec);module_spec.loader.exec_module(module)
        for config in (a,b):
            self.assertEqual(config['user_timeline_mongodb']['port'],'22012')
            self.assertEqual(config['user_timeline_mongodb']['addr'],'127.0.0.1')
            self.assertEqual(config['post_storage_mongodb'],original['post_storage_mongodb'])
            for mode in ('domain','rpc'):
                self.assertEqual(module.services_for(config,'A',mode),['user_timeline','client'])
            self.assertEqual(module.services_for(config,'B','domain'),['post_storage'])
            self.assertEqual(module.services_for(config,'B','rpc'),['post_storage','nginx','load_balance'])
        for side,config in (('A',a),('B',b)):
            services = module.services_for(config,side,'rpc')
            node=config['common']['numa_client_node']
            self.assertTrue(module.check(config,services,{node:list(range(20))},set(range(20))))
        b['post_storage']['server_addr']=a['client']['server_addr']
        with self.assertRaises(ValueError): module.services_for(b,'B','domain')
        with self.assertRaises(ValueError):
            prepare.prepare(original,'192.0.2.99','192.0.2.98',timeline_db_port=0)

    def test_bad_endpoint_id_and_samples_rejected(self):
        original = json.loads((APP/'config/config.ub98.json').read_text(encoding='utf-8'))
        for a,b,n in (('IP_A','192.0.2.99',10), ('192.0.2.98','192.0.2.98',10),
                      ('192.0.2.98','192.0.2.99',0), ('192.0.2.98','192.0.2.99',5001)):
            with self.assertRaises(ValueError): prepare.prepare(original,a,b,n)
        original['nginx']['rpc_id'] = original['client']['rpc_id']
        with self.assertRaises(ValueError): prepare.prepare(original,'192.0.2.98','192.0.2.99')

    def test_cli_never_overwrites_config_or_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)/'campaign'
            source = APP/'config/config.ub98.json'
            before = source.read_bytes()
            command = [os.sys.executable,str(ROOT/'scripts/prepare-social.py'),'--input',str(source),
                       '--output',str(output),'--host-a','192.0.2.98','--host-b','192.0.2.99']
            self.assertEqual(subprocess.run(command,capture_output=True).returncode,0)
            roles = subprocess.run([os.sys.executable,str(ROOT/'scripts/check-social-cpus.py'),
                '--config',str(output/'config-b.json'),'--side','B','--mode','domain','--print-services'],
                capture_output=True,text=True)
            self.assertEqual(roles.returncode,0,roles.stderr)
            self.assertEqual(roles.stdout.strip(),'post_storage')
            self.assertNotEqual(subprocess.run(command,capture_output=True).returncode,0)
            self.assertEqual(before,source.read_bytes())


@unittest.skipUnless(shutil.which('g++'), 'requires native g++')
class Adapter(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        tmp = Path(cls.temp.name); headers(tmp)
        # Affinity is a fixture; no host thread is pinned by this test.
        (tmp/'sched.h').write_text('typedef struct { int value; } cpu_set_t;\n#define CPU_COUNT(p) (1)\n'
                                  'static inline int sched_getaffinity(int p,unsigned long n,cpu_set_t*s)'
                                  '{(void)p;(void)n;(void)s;return 0;}\n')
        src = tmp/'adapter.cpp'; cls.exe = tmp/'adapter.exe'
        src.write_text(r'''
#include <cassert>
#include <thread>
#include "domain_proxy_runtime.h"
struct Mock {std::thread::id owner;};
static unsigned opens, closes, calls;
extern "C" int us_client_open(struct lrpc_handle *h) {
 h->fd=7; h->astack_map=new Mock{std::this_thread::get_id()}; ++opens; return 0;
}
extern "C" int us_client_info(const struct lrpc_handle *,struct ud_connection_info *info) {
 *info={}; info->multi=1; info->slot=opens-1; info->identity=opens; return 0;
}
extern "C" int us_client_close(struct lrpc_handle *h) {
 auto *m=static_cast<Mock *>(h->astack_map); assert(m->owner==std::this_thread::get_id());
 delete m; h->fd=-1; ++closes; return 0;
}
extern "C" int us_client_call(struct lrpc_handle *h,struct us_call *c) {
 auto *m=static_cast<Mock *>(h->astack_map); assert(m->owner==std::this_thread::get_id());
 assert(c->abi==US_ABI && c->message.source==17 && c->message.value && !c->message.ticket);
 bool reverse=c->direction==US_REVERSE;
 bool home=c->message.type==(reverse ? US_HOME_RESP : US_HOME_REQ);
 assert(home || c->message.type==(reverse ? US_USER_RESP : US_USER_REQ));
 c->entries=2; const char *path=std::getenv("LRPC_UB_NESTED_PATH");
 c->transitions=path && !std::strcmp(path,"fused") ? 3 : 4;
 c->domain[0]=reverse ? 2 : 1; c->domain[1]=reverse ? 1 : 2;
 c->message.action=reverse ? US_ACTION_CLIENT : US_ACTION_RPC; c->message.target=reverse ? 0 : (home ? 103 : 102);
 if(std::getenv("TEST_BAD_RESULT")) ++c->message.machine;
 if(std::getenv("TEST_BAD_TRANSITIONS")) c->transitions=6;
 ++calls; return 0;
}
int main() {
 struct us_heap heap{}; us_heap_init(&heap); assert(us_heap_valid(&heap));
 ++heap.config[1].allowed_grants; assert(!us_heap_valid(&heap));
 sn_domain::Runtime client,timeline;
 client.open("client",0,1,17); timeline.open("user_timeline",0,1);
 if(!sn_domain::enabled()) { assert(!opens); return 0; }
 for(uint32_t i=1;i<=1000;i++) {
  assert(client.forward(i,i-1,17)==2);
  sn_domain::Origin origin{2,17,0,1,i,2};
  timeline.reverse(i,origin,98,0x1000+i,64,2048);
 }
 for(uint32_t i=1;i<=1000;i++) {
  assert(client.forward(i,i-1,17,sn_domain::ReadService::Home)==2);
  sn_domain::Origin origin{2,17,0,1,i,2};
  timeline.reverse(i,origin,98,0x1000+i,64,2048,sn_domain::ReadService::Home);
 }
 client.close(); timeline.close();
 assert(opens==2 && closes==2 && calls==4000);
 return 0;
}
''',encoding='utf-8')
        flags = ['-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wsign-conversion',
                 '-Wold-style-cast','-pedantic','-pthread','-DERPC_SN_DOMAIN_PROXY=1',
                 '-DERPC_SN_DOMAIN_UB=1','-I'+str(tmp),'-I'+str(ROOT/'include'),
                 '-I'+str(ROOT/'include/uapi'),'-I'+str(APP)]
        result = subprocess.run([shutil.which('g++'),*flags,str(src),'-o',str(cls.exe)],capture_output=True,text=True)
        if result.returncode: raise AssertionError(result.stdout+result.stderr)

    def run_adapter(self, **extra):
        env = {k:v for k,v in os.environ.items() if not k.startswith(('ERPC_SN_DOMAIN_','TEST_'))}
        env.pop('LRPC_UB_NESTED_PATH', None)
        env.update(ERPC_SN_DOMAIN_PROXY='1',LRPC_UBSM_SDK_MODE='external', **extra)
        return subprocess.run([str(self.exe)],env=env,capture_output=True,text=True,timeout=20)

    def test_actual_adapter_forward_reverse_and_handle_preservation(self):
        result = self.run_adapter()
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stderr.count('SN_DOMAIN_READY'),2)
        self.assertEqual(result.stderr.count('SN_DOMAIN_DONE'),2)
        self.assertIn('backend=ub config=B code=A astack=A',result.stderr)
        self.assertIn('chain=legacy entries=2 transitions=4',result.stderr)

    def test_adapter_explicit_fused_route(self):
        result = self.run_adapter(LRPC_UB_NESTED_PATH='fused')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stderr.count('SN_DOMAIN_READY'),2)
        self.assertIn('chain=fused entries=2 transitions=3',result.stderr)

    def test_corrupt_result_and_transition_count_fail_stop(self):
        for key in ('TEST_BAD_RESULT','TEST_BAD_TRANSITIONS'):
            result = self.run_adapter(**{key:'1'})
            self.assertNotEqual(result.returncode,0)
            self.assertIn('ub-result-contract',result.stderr)
            self.assertIn('fallback=none',result.stderr)

    def test_owned_sdk_and_x86_diagnostics_rejected(self):
        for key,value,marker in (('LRPC_UBSM_SDK_MODE','owned','ub-sdk-mode'),
                                 ('ERPC_SN_DOMAIN_KERNEL_EVERY','1','ub-x86-kernel-profile'),
                                 ('ERPC_SN_DOMAIN_DIAGNOSTIC','1','ub-x86-kernel-profile')):
            # Update after defaults to permit intentional negative values.
            env = {k:v for k,v in os.environ.items() if not k.startswith('ERPC_SN_DOMAIN_')}
            env.update(ERPC_SN_DOMAIN_PROXY='1',LRPC_UBSM_SDK_MODE='external'); env[key]=value
            result = subprocess.run([str(self.exe)],env=env,capture_output=True,text=True,timeout=20)
            self.assertNotEqual(result.returncode,0)
            self.assertIn(marker,result.stderr)


@unittest.skipUnless(shutil.which('gcc'), 'requires native gcc')
class Kernel(unittest.TestCase):
    def test_actual_mapped_client_completion_cpu_and_owner_checks(self):
        source = (ROOT/'lib/ub_domain_client.c').read_text(encoding='utf-8')
        client = source[source.index('struct ud_client {'):source.index('static int ud_bind_profile(')]
        invoke = source[source.index('int us_client_call('):source.index('int ud_lrpc_invoke(')]
        fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <lrpc/ub_social_client.h>
#define UD_HANDLE_MARKER (UINT64_MAX-1)
struct lrpc_ubsm_region {void *base;};
''' + client + r'''
static struct ud_client client;
static unsigned fail,stale,bad_transitions,wrong_cpu,calls;
static int sched_getcpu(void) {return wrong_cpu ? 2 : 1;}
static int ioctl(int fd,unsigned long command) {
 assert(fd==7 && command==UD_SOCIAL_CALL);++calls;
 if(fail) {errno=EIO;return -1;}
 struct ud_mapped *m=client.local_args;
 struct us_call *c=(void *)((char *)m+US_MAPPED_OFFSET);
 c->entries=2;c->transitions=bad_transitions ? 6 : (client.nested_path==UD_NESTED_FUSED ? 3 : 4);c->cpu=1;
 c->domain[0]=c->direction ? 2 : 1;c->domain[1]=c->direction ? 1 : 2;
 c->message.action=c->direction ? US_ACTION_CLIENT : US_ACTION_RPC;
 c->message.target=c->direction ? 0 : 102;
 if(!stale) __atomic_add_fetch(&m->completion,1,__ATOMIC_RELEASE);
 return 0;
}
''' + invoke + r'''
static void *other_owner(void *arg) {
 struct us_call c={.abi=US_ABI};assert(us_client_call(arg,&c)==-1 && errno==EPERM);return NULL;
}
int main(void) {
 struct lrpc_handle h={.fd=7,.shadow_cpu=1,.astack_offset=UD_HANDLE_MARKER,.astack_map=&client};
 struct us_call c={.abi=US_ABI};
 unsigned old;
 client.owner=pthread_self();client.profile=UD_PROFILE_SOCIAL;client.local_args=calloc(1,4096);
 assert(client.local_args);
 for(unsigned path=0;path<=1;path++) {
 client.nested_path=path;
 for(unsigned i=0;i<1000;i++) {
  memset(&c,0,sizeof(c));c.abi=US_ABI;c.direction=i%2;c.message.source=17;
  c.message.value=i+1;c.message.request=i+1;c.message.machine=99;
  assert(!us_client_call(&h,&c) && c.entries==2 && c.transitions==(path ? 3U : 4U) && !client.invoking);
  assert(c.message.value==i+1 && c.message.machine==99);
 }
 bad_transitions=1;assert(us_client_call(&h,&c)==-1 && errno==EPROTO && !client.invoking);
 bad_transitions=0;stale=1;assert(us_client_call(&h,&c)==-1 && errno==EPROTO && !client.invoking);
 stale=0;fail=1;assert(us_client_call(&h,&c)==-1 && errno==EIO && !client.invoking);fail=0;
 old=calls;wrong_cpu=1;assert(us_client_call(&h,&c)==-1 && errno==EXDEV && calls==old);wrong_cpu=0;
 client.invoking=1;assert(us_client_call(&h,&c)==-1 && errno==EBUSY && calls==old);client.invoking=0;
 }
 pthread_t t;assert(!pthread_create(&t,NULL,other_owner,&h));assert(!pthread_join(t,NULL) && calls==old);
 free(client.local_args);return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory); headers(tmp)
            src,exe = tmp/'client.c',tmp/'client.exe';src.write_text(fixture,encoding='utf-8')
            result = subprocess.run([shutil.which('gcc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror','-pthread',
                                     '-I'+str(tmp),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'include/uapi'),
                                     str(src),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result = subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_actual_kernel_route_dispatch_and_fault_restoration(self):
        source = (ROOT/'kernel/ub-domain/domain.c').read_text(encoding='utf-8')
        dispatch = source[source.index('static int ud_run_social('):source.index('static long ud_ioctl(')]
        fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <lrpc/ub_social.h>
#include <lrpc/ub_domain_layout.h>
typedef uint64_t u64;
#define U32_MAX 0xffffffffU
struct ud_regs {u64 x[31],sp,pc,pstate,esr,far,vector;};
#include "nested_gate.h"
struct ud_session {
 void *args[UD_DOMAINS], *empty_root;
 struct ud_prepare p; unsigned long asid[UD_DOMAINS]; bool committed;
 unsigned nested_path, profile;
};
static struct ud_session *active;
static struct us_heap heap;
static unsigned locks,irqs,preempt,entries,fail_mapping,fail_supported,fault,corrupt;
static int fake_current;
#define current (&fake_current)
#define mm fake_mm
/* current->mm is syntactic OS fixture, no process page tables here. */
#undef current
static struct {void *mm;} task;
#define current (&task)
static void mmap_read_lock(void *m) {(void)m;++locks;}
static void mmap_read_unlock(void *m) {(void)m;assert(locks==1);--locks;}
static void preempt_disable(void) {assert(!preempt);++preempt;}
static void preempt_enable(void) {assert(preempt==1);--preempt;}
#define local_irq_save(f) do {(f)=0;assert(!irqs);++irqs;} while(0)
#define local_irq_restore(f) do {(void)(f);assert(irqs==1);--irqs;} while(0)
#define read_sysreg(r) 0ULL
#define virt_to_phys(p) ((void)(p),0ULL)
static unsigned raw_smp_processor_id(void) {return 1;}
static u64 ktime_get_ns(void) {static u64 n;return ++n;}
static int ud_mapping(struct ud_session *s,bool init) {(void)s;(void)init;return fail_mapping ? -ESTALE : 0;}
static int ud_supported(void) {return fail_supported ? -EOPNOTSUPP : 0;}
static const unsigned char *ud_profile_service(unsigned p,unsigned d,unsigned long *size) {
 static unsigned char code[8]; assert((p==UD_PROFILE_SOCIAL || p==UD_PROFILE_SOCIAL_WRITE) && d<4);*size=8;return code;
}
static void ud_initial_regs(struct ud_session *s,unsigned d,struct ud_regs *r) {
 (void)s;memset(r,0,sizeof(*r));r->x[0]=UD_ARGS;r->x[1]=UD_DATA+us_config_offset(d);
}
static void ud_enter_selected(struct ud_regs *r,u64 root,u64 tcr,u64 empty,unsigned save) {
 unsigned d=(unsigned)((root&UD_PA_MASK)/UD_PAGE)-1;
 (void)tcr;(void)empty;assert(!save && irqs==1 && preempt==1 && locks==1 && d<4);
 assert(r->x[0]==UD_ARGS+US_MAPPED_OFFSET && r->x[1]==UD_DATA+us_config_offset(d));
 struct us_call *c=(void *)((char *)active->args[0]+US_MAPPED_OFFSET);
 const struct us_config *config=&heap.config[(d==1||d==2) ? 1 : 0];
 unsigned expected=c->direction*2+entries;
 assert(d==expected); ++entries;
 if(d==0) {c->message.action=US_ACTION_DOMAIN;c->message.target=config->nginx[0];}
 if(d==1) {c->message.action=US_ACTION_RPC;c->message.target=c->message.type==US_WRITE_REQ ? 104 : c->message.type==US_HOME_REQ ? config->home_endpoint : config->user_endpoint;
            if(c->message.type==US_WRITE_REQ) c->message.ticket=c->message.value;}
 if(d==2) {c->message.action=US_ACTION_DOMAIN;c->message.target=config->return_domain;}
 if(d==3) {c->message.action=US_ACTION_CLIENT;c->message.target=0;}
 r->vector=8;r->esr=(0x3cULL<<26)|0x4c0;r->pc=UD_VA+4;
 if(fault==entries) {r->esr=0xdead;r->vector=5;}
 if(corrupt==entries) ++c->message.payload_offset;
}
static void ud_enter_nested_selected(struct ud_nested_gate *g,u64 tcr,u64 empty) {
 struct us_call *c=(void *)((char *)active->args[0]+US_MAPPED_OFFSET);
 assert(g->mode==UDN_MODE_SOCIAL && g->social_args==(uintptr_t)active->args[0]);
 assert(g->middle.x[0]==UD_ARGS+US_MAPPED_OFFSET && g->leaf.x[0]==g->middle.x[0]);
 assert(g->social_target==(c->direction ? 1U : 2U));
 g->domain=1;g->transitions=1;
 ud_enter_selected(&g->middle,g->middle_root,tcr,empty,0);
 g->transitions=2;
 if(g->middle.vector!=8 || (g->middle.esr&0xffff)!=0x4c0 ||
    c->message.action!=US_ACTION_DOMAIN || c->message.target!=g->social_target) return;
 g->domain=2;
 ud_enter_selected(&g->leaf,g->leaf_root,tcr,empty,0);
 g->transitions=3;
 if(g->leaf.vector==8 && (g->leaf.esr&0xffff)==0x4c0) g->complete=1;
}
''' + dispatch + r'''
static void initialize(struct ud_session *s,unsigned direction,unsigned request) {
 struct us_call *c=(void *)((char *)s->args[0]+US_MAPPED_OFFSET);
 memset(c,0,sizeof(*c));c->abi=US_ABI;c->direction=direction;
 c->message.source=17;c->message.request=request;c->message.value=request+1;
 c->message.type=direction ? US_USER_RESP : US_USER_REQ;
 c->message.machine=99;c->message.block=0x123400;c->message.payload_offset=64;c->message.payload_bytes=2048;
 s->committed=true;entries=0;active=s;
}
int main(void) {
 struct ud_session s={.profile=UD_PROFILE_SOCIAL};us_heap_init(&heap);
 for(unsigned d=0;d<4;d++) {
  s.args[d]=calloc(1,4096);assert(s.args[d]);
  s.p.contract.table_pa[d][0]=(d+1)*4096;s.asid[d]=100+d;
 }
 for(unsigned path=0;path<=1;path++) for(unsigned n=0;n<1000;n++) for(unsigned direction=0;direction<2;direction++) for(unsigned home=0;home<2;home++) {
  s.nested_path=path;
  initialize(&s,direction,n);
  if(home) ((struct us_call *)((char *)s.args[0]+US_MAPPED_OFFSET))->message.type=direction ? US_HOME_RESP : US_HOME_REQ;
  assert(!ud_run_social(&s));
  struct us_call *c=(void *)((char *)s.args[0]+US_MAPPED_OFFSET);
  assert(c->entries==2 && c->transitions==(path ? 3U : 4U) && c->cpu==1 && !locks && !irqs && !preempt);
  assert(c->domain[0]==(direction ? 2U : 1U) && c->domain[1]==(direction ? 1U : 2U));
 }
 for(unsigned path=0;path<=1;path++) for(unsigned direction=0;direction<2;direction++) for(unsigned f=1;f<=2;f++) {
  s.nested_path=path;
  initialize(&s,direction,1);fault=f;assert(ud_run_social(&s)==-EIO && !s.committed);
  assert(!locks && !irqs && !preempt);fault=0;
  initialize(&s,direction,1);corrupt=f;assert(ud_run_social(&s)==-EPROTO && !s.committed);
  assert(!locks && !irqs && !preempt);corrupt=0;
 }
 s.profile=UD_PROFILE_SOCIAL_WRITE;
 for(unsigned path=0;path<=1;path++) {
  s.nested_path=path;
  initialize(&s,0,1);
  struct us_call *write=(void *)((char *)s.args[0]+US_MAPPED_OFFSET);
  write->message.type=US_WRITE_REQ;
  assert(!ud_run_social(&s) && write->message.ticket==write->message.value);
 }
 s.profile=UD_PROFILE_SOCIAL;
 initialize(&s,0,1);
 ((struct us_call *)((char *)s.args[0]+US_MAPPED_OFFSET))->message.type=US_WRITE_REQ;
 assert(ud_run_social(&s)==-EINVAL && !locks && !irqs && !preempt);
 initialize(&s,0,1);fail_mapping=1;assert(ud_run_social(&s)==-ESTALE && !locks && !irqs && !preempt);
 fail_mapping=0;fail_supported=1;assert(ud_run_social(&s)==-EOPNOTSUPP && !locks && !irqs && !preempt);
 for(unsigned d=0;d<4;d++) free(s.args[d]);
 return 0;
}
'''
        # Windows errno lacks Linux ESTALE; the OS boundaries above are mocks.
        fixture = '#ifndef ESTALE\n#define ESTALE 116\n#endif\n'+fixture
        fixture = fixture.replace('static int fake_current;','').replace('#define mm fake_mm','')
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory); headers(tmp)
            src, exe = tmp/'kernel.c',tmp/'kernel.exe';src.write_text(fixture,encoding='utf-8')
            result = subprocess.run([shutil.which('gcc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                                     '-I'+str(tmp),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'include/uapi'),
                                     '-I'+str(ROOT/'kernel/ub-domain'),
                                     str(src),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result = subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__ == '__main__': unittest.main(verbosity=2)
