"""Compile production pool/allocator/backend against local OS/SDK fixtures.

These tests need native gcc/pthreads, not UB hardware. The mmap/SDK and service
instructions are fixtures; publication, allocation and SDK lifetime code is real.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

from test_nested_runtime import headers

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('gcc'), 'requires native gcc and pthreads')
class Multi(unittest.TestCase):
    def compile_run(self, text, *sources, args=(), env=None):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            headers(tmp)
            (tmp / 'lrpc').mkdir()
            (tmp / 'lrpc/ub_domain_services.h').write_text('''
#include <lrpc/ub_social.h>
static inline const unsigned char *ud_service(unsigned d, unsigned long *size) {
 static const unsigned char code[4]={1,2,3,4}; *size=4; return code+d%1;
}
static inline const unsigned char *ud_profile_service(unsigned p,unsigned d,unsigned long *size) {
 (void)p; return ud_service(d,size);
}
''')
            source, exe = tmp / 'test.c', tmp / 'test.exe'
            source.write_text(text, encoding='utf-8')
            command = [shutil.which('gcc'), '-std=gnu11', '-O2', '-Wall', '-Wextra',
                       '-Werror', '-pthread', '-D__USE_MINGW_ANSI_STDIO=1',
                       '-I'+str(tmp), '-I'+str(ROOT/'include'),
                       '-I'+str(ROOT/'include/uapi'), '-I'+str(ROOT/'kernel/ub-domain'),
                       str(source), *(str(ROOT/p) for p in sources), '-o', str(exe)]
            if os.name != 'nt': command.insert(1, '-D_GNU_SOURCE')
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            result = subprocess.run([str(exe), *args], env=env, capture_output=True,
                                    text=True, timeout=40)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            return result.stdout

    def test_publication_slot_isolation_and_reuse(self):
        self.compile_run(r'''
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <lrpc/ub_domain_pool.h>
static struct ud_pool_publication *slot(void *base, unsigned s) {
 return (void *)((char *)base+UD_POOL_SLOT(s));
}
static void request(void *base, unsigned s, unsigned nonce) {
 struct ud_pool_publication *p=slot(base,s);
 struct ud_contract *c=&p->pub.contract;
 memset(c,0,sizeof(*c)); c->magic=UD_MAGIC; c->abi=UD_ABI; c->nonce=nonce;
 c->wb_index=4; c->nc_index=3; c->heap_pa=0x980000030000ULL;
 for(unsigned d=0;d<UD_DOMAINS;d++) {
  c->code_pa[d]=0x200000+(s*16+d*3)*UD_PAGE;
  c->stack_pa[d]=c->code_pa[d]+UD_PAGE; c->args_pa[d]=c->code_pa[d]+2*UD_PAGE;
  for(unsigned l=0;l<4;l++) c->table_pa[d][l]=0x980000000000ULL+UD_POOL_TABLE(s,d,l);
 }
 assert(ud_contract_valid(c));
 __atomic_store_n(&p->pub.request,nonce,__ATOMIC_RELEASE);
}
int main(void) {
 void *base=malloc(UD_BYTES), *snapshot=malloc(UD_BYTES);
 assert(base && snapshot && ud_pool_initialize(base,0)==-EINVAL);
 assert(!ud_pool_initialize(base,123) && ud_pool_header_valid(base));
 memcpy(snapshot,base,UD_BYTES);
 for(unsigned s=0;s<UD_CLIENTS;s++) request(base,s,s+1);
 assert(ud_pool_poll(base)==UD_CLIENTS);
 assert(!memcmp(snapshot,base,UD_POOL_SLOT(0))); /* includes heap/images/context */
 for(unsigned s=0;s<UD_CLIENTS;s++) {
  struct ud_pool_publication *p=slot(base,s);
  assert(!p->error && p->pub.complete==s+1 && p->pub.b_initializations==1);
  for(unsigned d=0;d<UD_DOMAINS;d++) for(unsigned l=0;l<4;l++) {
   __u64 expected[512]; ud_build_table(&p->pub.contract,d,l,expected);
   assert(!memcmp(expected,(char *)base+UD_POOL_TABLE(s,d,l),UD_PAGE));
  }
 }
 memcpy(snapshot,base,UD_BYTES); assert(ud_pool_poll(base)==0);
 assert(!memcmp(snapshot,base,UD_BYTES));
 request(base,0,99); assert(ud_pool_poll(base)==1);
 assert(slot(base,0)->pub.b_initializations==2);
 assert(!memcmp((char *)snapshot+UD_POOL_TABLE(1,0,0),
                (char *)base+UD_POOL_TABLE(1,0,0),7*0x10000));
 request(base,1,100); slot(base,1)->pub.contract.reserved=1;
 request(base,2,101); assert(ud_pool_poll(base)==2);
 assert(slot(base,1)->error==EINVAL && slot(base,1)->pub.complete==100);
 assert(slot(base,1)->pub.b_initializations==1 && !slot(base,2)->error);
 assert(!memcmp((char *)snapshot+UD_POOL_TABLE(1,0,0),
                (char *)base+UD_POOL_TABLE(1,0,0),0x10000));
 free(snapshot); free(base); return 0;
}
''', 'lib/ub_domain_pool.c')

    def test_kernel_allocator_concurrency_capacity_generation_and_quarantine(self):
        self.compile_run(r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <linux/ub_lrpc_ub_domain.h>
#ifndef ESTALE
#define ESTALE 116
#endif
#include "pool_slots.h"
static struct ud_pool_bank banks[UD_POOL_BANKS];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned bank[UD_CLIENTS], slot[UD_CLIENTS];
static void *acquire(void *arg) {
 unsigned id=*(unsigned *)arg;
 pthread_mutex_lock(&lock);
 assert(!ud_slots_acquire(banks,0x980000030000ULL,123,&bank[id],&slot[id]));
 pthread_mutex_unlock(&lock); return NULL;
}
int main(void) {
 pthread_t threads[UD_CLIENTS]; unsigned ids[UD_CLIENTS],b,s;
 for(unsigned i=0;i<UD_CLIENTS;i++) { ids[i]=i; assert(!pthread_create(&threads[i],0,acquire,&ids[i])); }
 for(unsigned i=0;i<UD_CLIENTS;i++) assert(!pthread_join(threads[i],0));
 for(unsigned i=0;i<UD_CLIENTS;i++) for(unsigned j=0;j<i;j++) assert(slot[i]!=slot[j]);
 assert(ud_slots_acquire(banks,0x980000030000ULL,123,&b,&s)==-ENOSPC);
 assert(ud_slots_acquire(banks,0x980000030000ULL,124,&b,&s)==-ESTALE);
 ud_slots_release(banks,bank[0],slot[0],1,1);
 assert(!ud_slots_acquire(banks,0x980000030000ULL,123,&b,&s) && s==slot[0]);
 ud_slots_release(banks,b,s,1,0); /* uncommitted/possibly pending B publication */
 assert(banks[b].quarantined==(1UL<<s));
 for(unsigned i=1;i<UD_CLIENTS;i++) ud_slots_release(banks,bank[i],slot[i],1,1);
 assert(banks[b].used==(1UL<<s));
 unsigned q=s;
 assert(!ud_slots_acquire(banks,0x980000030000ULL,123,&b,&s) && s!=q);
 ud_slots_release(banks,b,s,0,0); /* setup never exposed: immediately reusable */
 assert(!ud_slots_acquire(banks,0x980000030000ULL,123,&b,&s) && s!=q);
 ud_slots_release(banks,b,s,1,1);
 for(unsigned i=0;i<UD_POOL_BANKS-1;i++)
  assert(!ud_slots_acquire(banks,0x1000+i*UD_PAGE,1,&b,&s));
 assert(ud_slots_acquire(banks,0xdead000,1,&b,&s)==-ENOSPC);
 return 0;
}
''')

    def test_actual_sdk_lifetime_mapping_reuse_and_failures(self):
        source = (ROOT/'lib/backend_ubsm.c').read_text(encoding='utf-8')
        source = '\n'.join(line for line in source.splitlines()
                           if not any(part in line for part in
                                      ('#include <sys/mman.h>', '#include <ubs_mem.h>', '#include <ubs_mem_def.h>')))
        fixture = r'''
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <lrpc/lrpc_ubsm.h>
#ifndef ESTALE
#define ESTALE 116
#endif
#define UBSM_OK 0
#define UBSM_ERR_PARAM_INVALID 2
#define UBSM_ERR_MEMORY 3
#define UBSM_FLAG_ONLY_IMPORT_NONCACHE 8
#define UBSM_FLAG_WR_DELAY_COMP 16
#define MAX_SHM_NAME_LENGTH 49
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 4
#define MAP_FAILED ((void *)-1)
typedef struct {} ubsmem_options_t;
typedef struct { char host_name[256]; unsigned socket_id,numa_id,port_id; } ubs_mem_provider_t;
static unsigned inits, finals, maps, unmaps, live, fail_map, fail_unmap, fail_final;
static unsigned initialized;
static int ubsmem_set_logger_level(int n) { (void)n; return 0; }
static int ubsmem_init_attributes(ubsmem_options_t *o) { (void)o; return 0; }
static int ubsmem_initialize(ubsmem_options_t *o) {
 (void)o; assert(!initialized); initialized=1; inits++; return 0;
}
static int ubsmem_finalize(void) {
 assert(initialized && !live); if(fail_final) return 77;
 initialized=0; finals++; return 0;
}
static int ubsmem_shmem_allocate_with_provider(ubs_mem_provider_t *p,const char *n,size_t s,int mode,int flags) {
 (void)p;(void)n;(void)s;(void)mode;(void)flags; return 0;
}
static int ubsmem_shmem_deallocate(const char *n) { (void)n; return 0; }
static int ubsmem_shmem_map(void *a,size_t n,int p,int f,const char *name,off_t off,void **out) {
 (void)a;(void)p;(void)f;(void)off; assert(initialized && name[0]);
 if(fail_map) return 88;
 *out=calloc(1,n); assert(*out); maps++; live++; return 0;
}
static int ubsmem_shmem_unmap(void *p,size_t n) {
 (void)n; assert(initialized && live); if(fail_unmap) return 99;
 free(p); live--; unmaps++; return 0;
}
'''
        tests = r'''
static pthread_mutex_t test_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_cond=PTHREAD_COND_INITIALIZER;
static unsigned opened, release_all;
static void *thread(void *arg) {
 struct lrpc_ubsm_region r; (void)arg;
 assert(!lrpc_ubsm_remote_open(&r,"same",LRPC_UBSM_REGION_SIZE));
 pthread_mutex_lock(&test_lock); opened++; pthread_cond_broadcast(&test_cond);
 while(!release_all) pthread_cond_wait(&test_cond,&test_lock);
 pthread_mutex_unlock(&test_lock);
 assert(!lrpc_ubsm_close(&r,0)); return NULL;
}
int main(int argc,char **argv) {
 struct lrpc_ubsm_region a,b,c; (void)argv;
 if(argc>1) { /* externally owned SDK must never be finalized by this library */
  initialized=1;
  assert(!lrpc_ubsm_remote_open(&a,"external",LRPC_UBSM_REGION_SIZE));
  assert(!lrpc_ubsm_close(&a,0) && inits==0 && finals==0 && initialized);
  return 0;
 }
 assert(!lrpc_ubsm_remote_open(&a,"same",LRPC_UBSM_REGION_SIZE));
 assert(!lrpc_ubsm_remote_open(&b,"same",LRPC_UBSM_REGION_SIZE));
 assert(a.base==b.base && inits==1 && maps==1 && sdk_refs==2);
 assert(lrpc_ubsm_remote_open(&c,"same",2*LRPC_UBSM_REGION_SIZE)==UBSM_ERR_PARAM_INVALID);
 assert(sdk_refs==2 && maps==1);
 assert(!lrpc_ubsm_close(&a,0) && !unmaps && !finals && b.base);
 fail_map=1; assert(lrpc_ubsm_remote_open(&c,"other",LRPC_UBSM_REGION_SIZE)==88);
 assert(sdk_refs==1 && initialized && !finals); fail_map=0;
 fail_unmap=1; assert(lrpc_ubsm_close(&b,0)==99 && b.initialized && b.mapped && sdk_refs==1);
 fail_unmap=0; fail_final=1;
 assert(lrpc_ubsm_close(&b,0)==77 && b.initialized && !b.mapped && sdk_refs==1);
 fail_final=0; assert(!lrpc_ubsm_close(&b,0) && !sdk_refs && finals==1 && unmaps==1);
 pthread_t t[8];
 for(unsigned i=0;i<8;i++) assert(!pthread_create(&t[i],NULL,thread,NULL));
 pthread_mutex_lock(&test_lock);
 while(opened<8) pthread_cond_wait(&test_cond,&test_lock);
 assert(inits==2 && maps==2 && live==1 && sdk_refs==8);
 release_all=1; pthread_cond_broadcast(&test_cond); pthread_mutex_unlock(&test_lock);
 for(unsigned i=0;i<8;i++) assert(!pthread_join(t[i],NULL));
 assert(finals==2 && unmaps==2 && !sdk_refs && !imports);
 assert(!lrpc_ubsm_remote_open(&a,"one",LRPC_UBSM_REGION_SIZE));
 assert(!lrpc_ubsm_remote_open(&b,"two",LRPC_UBSM_REGION_SIZE));
 assert(a.base!=b.base && inits==3 && maps==4);
 assert(!lrpc_ubsm_close(&a,0) && initialized && live==1);
 assert(!lrpc_ubsm_close(&b,0) && !initialized && !live);
 return 0;
}
'''
        env = dict(os.environ)
        env.pop('LRPC_UBSM_SDK_MODE', None)
        self.compile_run(fixture+'\n'+source+'\n'+tests, env=env)
        env['LRPC_UBSM_SDK_MODE'] = 'external'
        self.compile_run(fixture+'\n'+source+'\n'+tests, args=('external',), env=env)

    def test_actual_kernel_pool_prepare_mapping_and_commit(self):
        source = (ROOT/'kernel/ub-domain/domain.c').read_text(encoding='utf-8')
        mapping = source[source.index('static int ud_mapping('):source.index('static void ud_free_pages(')]
        wrappers = source[source.index('static int ud_prepare_session('):source.index('static void ud_initial_regs(')]
        fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <lrpc/ub_domain_pool.h>
#include <lrpc/ub_domain_services.h>
typedef uint64_t u64;
#define __user
#define TASK_SIZE UINT64_MAX
#define GFP_KERNEL 0
#ifndef ESTALE
#define ESTALE 116
#endif
#include "pool_slots.h"
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT 93
#endif
static unsigned copyout_fail, nonce=1000;
static int copy_from_user(void *to,const void *from,size_t n) { memcpy(to,from,n); return 0; }
static int copy_to_user(void *to,const void *from,size_t n) {
 if(copyout_fail) return 1;
 memcpy(to,from,n); return 0;
}
static void *kmalloc(size_t n,int flags) { (void)flags; return malloc(n); }
#define kfree free
#define flush_icache_range(a,b) ((void)0)
struct ud_session {
 bool prepared,committed,multi,pool_reserved,pool_exposed,pool_reusable;
 unsigned profile;
 u64 pool_generation; unsigned pool_slot,pool_bank;
 struct ud_prepare p; struct ud_initial initial[UD_DOMAINS]; void *code[UD_DOMAINS];
};
static struct ud_pool_bank banks[UD_POOL_BANKS];
static int ud_import_pa(struct ud_session *s,unsigned long off,u64 *pa,unsigned index,bool init) {
 (void)s;(void)index;(void)init; *pa=0x980000000000ULL+off; return 0;
}
''' + mapping + r'''
/* Page allocation/ASID and VMA OS mechanisms are fixtures. The prepare ioctl
 * wrappers, per-slot ud_mapping and complete ud_commit below are production C. */
static void ud_free_pages(struct ud_session *s) {
 if(s->pool_reserved) ud_slots_release(banks,s->pool_bank,s->pool_slot,s->pool_exposed,s->pool_reusable);
 s->pool_reserved=false;
 for(unsigned d=0;d<UD_DOMAINS;d++) { free(s->code[d]); s->code[d]=NULL; }
}
static int ud_prepare_pages(struct ud_session *s,const struct ud_prepare *p) {
 s->p=*p;
 if(s->multi) {
  int rc=ud_slots_acquire(banks,0x980000030000ULL,s->pool_generation,&s->pool_bank,&s->pool_slot);
  if(rc) return rc;
  s->pool_reserved=true;
 }
 struct ud_contract *c=&s->p.contract; memset(c,0,sizeof(*c));
 c->magic=UD_MAGIC; c->abi=UD_ABI; c->nonce=++nonce; c->wb_index=4; c->nc_index=3;
 assert(!ud_mapping(s,true));
 for(unsigned d=0;d<UD_DOMAINS;d++) {
  c->code_pa[d]=0x200000+(s->pool_slot*16+d*3)*UD_PAGE;
  c->stack_pa[d]=c->code_pa[d]+UD_PAGE; c->args_pa[d]=c->code_pa[d]+2*UD_PAGE;
  s->code[d]=calloc(1,UD_PAGE); assert(s->code[d]);
 }
 assert(ud_contract_valid(c)); return 0;
}
''' + wrappers + r'''
int main(void) {
 void *base=malloc(UD_BYTES); assert(base && !ud_pool_initialize(base,456));
 struct ud_session s[UD_CLIENTS]={0}, ninth={0};
 struct ud_prepare old={.import_va=(uintptr_t)base};
 assert(ud_prepare_session(&ninth,&old)==-EPROTONOSUPPORT);
 for(unsigned i=0;i<UD_CLIENTS;i++) {
  struct ud_pool_prepare p={.abi=UD_POOL_ABI,.generation=456,.domain.import_va=(uintptr_t)base};
  assert(!ud_prepare_pool(&s[i],&p) && p.slot==i && s[i].prepared && s[i].pool_exposed);
  assert(ud_prepare_pool(&s[i],&p)==-EALREADY);
  struct ud_pool_publication *pub=(void *)((char *)base+UD_POOL_SLOT(i));
  pub->pub.contract=p.domain.contract; pub->pub.request=p.domain.contract.nonce;
 }
 struct ud_pool_prepare p={.abi=UD_POOL_ABI,.generation=456,.domain.import_va=(uintptr_t)base};
 assert(ud_prepare_pool(&ninth,&p)==-ENOSPC);
 assert(ud_pool_poll(base)==UD_CLIENTS);
 for(unsigned i=0;i<UD_CLIENTS;i++) assert(!ud_commit(&s[i]) && s[i].pool_reusable);
 ((struct ud_pool_header *)base)->generation=999;
 assert(ud_prepare_pool(&ninth,&p)==-ESTALE);
 ((struct ud_pool_header *)base)->generation=456;
 ud_free_pages(&s[0]); memset(&s[0],0,sizeof(s[0]));
 copyout_fail=1; assert(ud_prepare_pool(&s[0],&p)==-EFAULT && !s[0].pool_reserved);
 copyout_fail=0; assert(!ud_prepare_pool(&s[0],&p) && !p.slot);
 struct ud_pool_publication *pub=(void *)((char *)base+UD_POOL_SLOT(0));
 assert(ud_commit(&s[0])==-ESTALE); /* old publication cannot be committed */
 pub->pub.contract=p.domain.contract; pub->pub.request=p.domain.contract.nonce;
 assert(ud_pool_poll(base)==1);
 __u64 *table=(void *)((char *)base+UD_POOL_TABLE(0,0,3));
 table[6]^=UD_PAGE; assert(ud_commit(&s[0])==-EBADMSG);
 table[6]^=UD_PAGE; assert(!ud_commit(&s[0]));
 for(unsigned i=0;i<UD_CLIENTS;i++) ud_free_pages(&s[i]);
 assert(!banks[0].used && !banks[0].heap);
 assert(!ud_pool_initialize_profile(base,789,UD_PROFILE_SOCIAL));
 p=(struct ud_pool_prepare){.abi=UD_POOL_ABI,.generation=789,.domain.import_va=(uintptr_t)base};
 struct ud_session social={.profile=UD_PROFILE_SOCIAL}, wrong={0};
 assert(ud_prepare_pool(&wrong,&p)==-EPROTONOSUPPORT && !banks[0].used);
 assert(!ud_prepare_pool(&social,&p));
 pub=(void *)((char *)base+UD_POOL_SLOT(p.slot));
 pub->pub.contract=p.domain.contract;pub->pub.request=p.domain.contract.nonce;
 assert(ud_pool_poll(base)==1);
 struct us_heap *heap=(void *)((char *)base+UD_HEAP);
 heap->config[1].allowed_grants=1;
 assert(ud_commit(&social)==-EBADMSG);
 heap->config[1].allowed_grants=0;
 assert(!ud_commit(&social));ud_free_pages(&social);
 assert(!banks[0].used);free(base);return 0;
}
'''
        self.compile_run(fixture, 'lib/ub_domain_pool.c')

    def test_actual_multi_thread_campaign_and_failure_markers(self):
        source = (ROOT/'demo/ub/domain_multi.c').read_text(encoding='utf-8')
        # Emulate affinity, process creation and anonymous shared mmap only.
        # This executes actual pthread orchestration and result checks, NOT a
        # real multi-process or UB run. The process mode is cross-compiled.
        source = '\n'.join(line for line in source.splitlines() if not any(
            part in line for part in ('#define _GNU_SOURCE', '#include <sched.h>',
                                     '#include <sys/mman.h>', '#include <sys/wait.h>',
                                     '#include "../../lib/ub_domain_client.h"')))
        fixture = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <lrpc/lrpc.h>
#include <linux/ub_lrpc_ub_domain.h>
#ifdef _WIN32
#define CPU_SETSIZE 64
typedef struct { uint64_t bits; } cpu_set_t;
#define CPU_ZERO(s) ((s)->bits=0)
#define CPU_SET(c,s) ((s)->bits|=1ULL<<(c))
#define CPU_ISSET(c,s) (!!((s)->bits&(1ULL<<(c))))
#else
#include <sched.h>
#endif
static _Thread_local unsigned current_cpu;
static int fixture_getaffinity(int pid,size_t size,cpu_set_t *s) {
 (void)pid;(void)size; CPU_ZERO(s);
 for(unsigned i=0;i<4;i++) CPU_SET(i,s);
 return 0;
}
static int fixture_setaffinity(int pid,size_t size,const cpu_set_t *s) {
 (void)pid;(void)size;
 for(unsigned i=0;i<CPU_SETSIZE;i++) if(CPU_ISSET(i,s)) { current_cpu=i; return 0; }
 return -1;
}
#define sched_getaffinity fixture_getaffinity
#define sched_setaffinity fixture_setaffinity
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_ANONYMOUS 2
#define MAP_FAILED ((void *)-1)
static void *fixture_mmap(void *a,size_t n,int p,int f,int fd,long o) {
 (void)a;(void)p;(void)f;(void)fd;(void)o; return calloc(1,n);
}
static int fixture_munmap(void *a,size_t n) { (void)n; free(a); return 0; }
#define mmap fixture_mmap
#define munmap fixture_munmap
static int fixture_fork(void) { errno=ENOSYS; return -1; }
static int fixture_waitpid(int p,int *s,int f) { (void)p;(void)s;(void)f; return -1; }
#define fork fixture_fork
#define waitpid fixture_waitpid
#define WIFEXITED(s) ((s)==0)
#define WEXITSTATUS(s) (s)
static int fixture_setenv(const char *k,const char *v,int overwrite) {
 (void)k;(void)v;(void)overwrite; return 0;
}
#define setenv fixture_setenv
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW 123
#endif
static int fixture_clock(int id,struct timespec *t) {
 static unsigned long long tick; (void)id;
 t->tv_sec=0; t->tv_nsec=__atomic_add_fetch(&tick,1,__ATOMIC_RELAXED); return 0;
}
#define clock_gettime fixture_clock
struct fixture_connection { struct ud_connection_info info; unsigned cpu; pthread_t owner; };
static pthread_mutex_t allocator=PTHREAD_MUTEX_INITIALIZER;
static unsigned used, identity;
int lrpc_bind(struct lrpc_handle *h,const char *device,uint32_t procedure,uint64_t epoch) {
 assert(!strcmp(device,"/dev/ub_lrpc_domain") && procedure==1 && epoch==1);
 struct fixture_connection *c=calloc(1,sizeof(*c)); assert(c);
 pthread_mutex_lock(&allocator);
 unsigned slot; for(slot=0;slot<UD_CLIENTS && (used&(1U<<slot));slot++) {}
 assert(slot<UD_CLIENTS); used|=1U<<slot;
 c->info=(struct ud_connection_info){.abi=UD_POOL_ABI,.multi=1,.slot=slot,
            .identity=++identity,.generation=456,.capacity=UD_CLIENTS};
 pthread_mutex_unlock(&allocator);
 c->cpu=current_cpu; c->owner=pthread_self(); h->fd=7; h->astack_map=c; return 0;
}
int ud_lrpc_connection_info(const struct lrpc_handle *h,struct ud_connection_info *info) {
 struct fixture_connection *c=h->astack_map; *info=c->info;
 if(getenv("TEST_DUP_SLOT")) info->slot=0;
 return 0;
}
int lrpc_invoke(struct lrpc_handle *h,struct lrpc_astack *call) {
 struct fixture_connection *c=h->astack_map;
 assert(c && c->cpu==current_cpu && pthread_equal(c->owner,pthread_self()));
 uint64_t a=0,b=0,result; memcpy(&a,call->payload,8);
 if(call->request_size==16) memcpy(&b,call->payload+8,8);
 result=call->procedure_id==1 ? a+b+100 : call->procedure_id==2 ? a+2 : 1000+b;
 if(getenv("TEST_BAD_RESULT")) ++result;
 memcpy(call->payload,&result,8); call->status=0; call->response_size=8;
 call->caller_cpu_before=call->caller_cpu_after=current_cpu; return 0;
}
int ud_lrpc_close_checked(struct lrpc_handle *h) {
 struct fixture_connection *c=h->astack_map;
 if(!c) { errno=EIO; return -1; }
 assert(pthread_equal(c->owner,pthread_self()));
 pthread_mutex_lock(&allocator); used&=~(1U<<c->info.slot); pthread_mutex_unlock(&allocator);
 free(c); h->astack_map=NULL; h->fd=-1;
 if(getenv("TEST_CLOSE_FAIL")) { errno=EIO; return -1; }
 return 0;
}
'''
        args = ('--cpus','0,1,2,3','--test','mixed','--samples','20','--warmup','3')
        # compile_run checks exit status; wrap main for negative CLI/run checks.
        wrapped = fixture+'\n#define main campaign_main\n'+source+r'''
#undef main
int main(int argc,char **argv) {
 int result=campaign_main(argc,argv);
 if(getenv("TEST_EXPECT_FAILURE")) return result ? 0 : 1;
 return result;
}
'''
        env = dict(os.environ)
        for key in ('LRPC_UB_NESTED_PATH','TEST_DUP_SLOT','TEST_BAD_RESULT','TEST_CLOSE_FAIL', 'TEST_EXPECT_FAILURE'):
            env.pop(key, None)
        output = self.compile_run(wrapped, args=args, env=env)
        self.assertIn('UB_DOMAIN_MULTI_PASS mode=threads workers=4 unique_cpus=4 calls=220', output)
        for key in ('TEST_DUP_SLOT','TEST_BAD_RESULT','TEST_CLOSE_FAIL'):
            output = self.compile_run(wrapped, args=args, env={**env,key:'1','TEST_EXPECT_FAILURE':'1'})
            self.assertNotIn('UB_DOMAIN_MULTI_PASS', output)
        for cli in (('--cpus','0'),('--cpus','0,1,'),('--cpus','0,8'),
                    ('--cpus','0,1','--samples','0'),('--cpus','0,1','--nested-path','bad')):
            output = self.compile_run(wrapped, args=cli, env={**env,'TEST_EXPECT_FAILURE':'1'})
            self.assertNotIn('UB_DOMAIN_MULTI_PASS', output)


if __name__ == '__main__':
    unittest.main(verbosity=2)
