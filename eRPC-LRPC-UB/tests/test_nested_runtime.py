"""Compile actual ud_run/ud_run_fused with gate/OS fixtures, not a UB test."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def headers(tmp):
    linux = tmp / 'linux'
    linux.mkdir(exist_ok=True)
    (linux / 'types.h').write_text('#include <stdint.h>\ntypedef uint64_t __u64; typedef uint32_t __u32;\n')
    (linux / 'ioctl.h').write_text('#define _IO(t,n) (((t)<<8)|(n))\n#define _IOWR(t,n,s) _IO(t,n)\n#define _IOW(t,n,s) _IO(t,n)\n#define _IOR(t,n,s) _IO(t,n)\n')
    (linux / 'ub_lrpc.h').write_text('/* legacy API not used */\n')


@unittest.skipUnless(shutil.which('gcc'), 'requires native gcc')
class Runtime(unittest.TestCase):
    def test_actual_C_dispatch_with_and_without_trace(self):
        source = (ROOT / 'kernel/ub-domain/domain.c').read_text(encoding='utf-8')
        helpers = source[source.index('static void ud_initial_regs('):source.index('/* Comparison helper:')]
        # ud_returned/ud_run_fused/ud_run are compiled verbatim.
        fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifndef ESTALE
#define ESTALE 116
#endif
#include <linux/ub_lrpc_ub_domain.h>
typedef uint64_t u64;
#define noinline
struct ud_regs { u64 x[31], sp, pc, pstate, esr, far, vector; };
#include "nested_gate.h"
struct ud_session {
 int committed; unsigned nested_path, profile;
 struct ud_prepare p; unsigned long asid[UD_DOMAINS];
 u64 storage[UD_DOMAINS][512]; void *args[UD_DOMAINS];
 struct ud_initial initial[UD_DOMAINS]; void *empty_root;
};
static struct ud_session *active;
static unsigned calls, fused_calls, legacy_phase, bad_gate, fail_mapping;
static unsigned locks, irq_depth, preempt_depth;
static u64 ticks;
#define current (&fake_current)
static struct { void *mm; } fake_current;
#define read_sysreg(name) (0x14001b5b5503510ULL)
#define virt_to_phys(p) ((u64)(uintptr_t)(p))
#define local_irq_save(f) do { (f)=0; irq_depth++; } while (0)
#define local_irq_restore(f) do { (void)(f); irq_depth--; } while (0)
static void mmap_read_lock(void *mm) { (void)mm; locks++; }
static void mmap_read_unlock(void *mm) { (void)mm; locks--; }
static void preempt_disable(void) { preempt_depth++; }
static void preempt_enable(void) { preempt_depth--; }
static int ud_mapping(struct ud_session *s, int init) { (void)s; (void)init; return fail_mapping ? -ESTALE : 0; }
static int ud_supported(void) { return 0; }
static unsigned raw_smp_processor_id(void) { return 1; }
static u64 ktime_get_ns(void) { return ++ticks; }
static const unsigned char *ud_service(unsigned d, unsigned long *size) {
 static const unsigned char bytes[64]={0}; *size=d==UD_MIDDLE ? 32 : 28; return bytes;
}
static void returned(struct ud_regs *r, unsigned d, int nested) {
 unsigned long size; ud_service(d,&size);
 r->pc=UD_VA+(nested ? UD_NESTED_TRAP_OFFSET : size-4);
 r->esr=(0x3cULL<<26)|(nested ? 0x4c1 : 0x4c0); r->far=0; r->vector=8;
}
static void ud_enter_selected(struct ud_regs *r, u64 root, u64 tcr, u64 empty, unsigned save) {
 unsigned d=(root>>12)&3; (void)tcr; (void)empty;
 assert(locks==1 && irq_depth==1 && preempt_depth==1); calls++;
 if (d==UD_MIDDLE && save) {
  r->x[2]=active->storage[d][0]; returned(r,d,1); legacy_phase=1;
 } else {
  if (d==UD_MIDDLE) { assert(legacy_phase==1); active->storage[d][2]=r->x[2]+1; }
  else if (d==UD_LEAF) active->storage[d][2]=active->storage[d][0]+1;
  else active->storage[d][2]=active->storage[d][0]+active->storage[d][1]+100;
  returned(r,d,0);
 }
}
static void ud_enter_nested_selected(struct ud_nested_gate *g, u64 tcr, u64 empty) {
 (void)tcr; (void)empty; fused_calls++;
 assert(locks==1 && irq_depth==1 && preempt_depth==1);
 assert(g->middle_root==((0x42ULL<<48)|0x101000));
 assert(g->leaf_root==((0x43ULL<<48)|0x102000));
 assert(g->middle_args==(uintptr_t)active->args[UD_MIDDLE]);
 assert(g->leaf_args==(uintptr_t)active->args[UD_LEAF]);
 assert(g->call_pc==UD_VA+12 && g->leaf_pc==UD_VA+24 && g->return_pc==UD_VA+28);
 assert(g->mode==UDN_MODE_MICRO);
 active->storage[UD_MIDDLE][2]=active->storage[UD_MIDDLE][0]+2;
 g->domain=bad_gate==1 ? UD_LEAF : UD_MIDDLE;
 g->transitions=bad_gate==3 ? 6 : 4; g->complete=bad_gate ? 0 : 1;
 returned(&g->middle,UD_MIDDLE,0);
 if (bad_gate==1) { g->leaf.esr=0x24ULL<<26; g->leaf.far=0xdead; }
}
''' + helpers + r'''
int main(void) {
 struct ud_session s={0}; active=&s;
 assert(sizeof(struct ud_nested_gate)==UDN_SIZE);
 assert(offsetof(struct ud_nested_gate,leaf)==UDN_LEAF);
 assert(offsetof(struct ud_nested_gate,middle_root)==UDN_MIDDLE_ROOT);
 assert(offsetof(struct ud_nested_gate,leaf_root)==UDN_LEAF_ROOT);
 assert(offsetof(struct ud_nested_gate,middle_args)==UDN_MIDDLE_ARGS);
 assert(offsetof(struct ud_nested_gate,leaf_args)==UDN_LEAF_ARGS);
 assert(offsetof(struct ud_nested_gate,call_pc)==UDN_CALL_PC);
 assert(offsetof(struct ud_nested_gate,leaf_pc)==UDN_LEAF_PC);
 assert(offsetof(struct ud_nested_gate,return_pc)==UDN_RETURN_PC);
 assert(offsetof(struct ud_nested_gate,transitions)==UDN_TRANSITIONS);
 assert(offsetof(struct ud_nested_gate,complete)==UDN_COMPLETE);
 assert(offsetof(struct ud_nested_gate,domain)==UDN_DOMAIN);
 assert(offsetof(struct ud_nested_gate,mode)==UDN_MODE);
 assert(offsetof(struct ud_nested_gate,social_args)==UDN_SOCIAL_ARGS);
 assert(offsetof(struct ud_nested_gate,social_target)==UDN_SOCIAL_TARGET);
 for(unsigned d=0;d<UD_DOMAINS;d++) {
  s.args[d]=s.storage[d]; s.p.contract.table_pa[d][0]=0x100000+d*0x1000;
  s.asid[d]=0x41+d; s.initial[d]=(struct ud_initial){UD_ARGS,UD_DATA,UD_STACK+4096,UD_VA};
 }
 for(unsigned path=0;path<=1;path++) for(unsigned mapped=0;mapped<=1;mapped++) {
  s.nested_path=path;
  for(unsigned iteration=0;iteration<1000;iteration++) {
   struct ud_call c={.a=iteration,.procedure=2}; struct ud_trace_call t={0};
   s.committed=1; s.storage[UD_MIDDLE][0]=iteration; calls=fused_calls=0;
   assert(!ud_run(&s,&c,mapped,&t)); assert(c.result==iteration+2 && !c.esr && c.cpu==1);
   assert(c.transitions==(path ? 4U : 6U)); assert(calls==(path ? 0U : 3U));
   assert(fused_calls==path); assert(!locks && !irq_depth && !preempt_depth);
#ifdef UD_TRACE_ENABLE
   assert(t.segments==(path ? 1U : 3U) && t.segment_ns[0]);
   if(path) assert(!t.segment_ns[1] && !t.segment_ns[2] && !t.handoff_ns[0] && !t.handoff_ns[1]);
#endif
  }
  struct ud_call direct={.a=21,.b=21,.procedure=1};
  s.storage[UD_DIRECT][0]=s.storage[UD_DIRECT][1]=21;
  calls=fused_calls=0; assert(!ud_run(&s,&direct,mapped,NULL));
  assert(direct.result==142 && direct.transitions==2 && calls==1 && !fused_calls);
 }
 for(bad_gate=1;bad_gate<=3;bad_gate++) {
  s.committed=1; s.nested_path=1;
  struct ud_call c={.a=41,.procedure=2};
  assert(ud_run(&s,&c,0,NULL)==-EIO && !s.committed);
  if(bad_gate==1) assert(c.esr==(0x24ULL<<26) && c.far==0xdead);
  assert(!locks && !irq_depth && !preempt_depth);
 }
 s.committed=1; fail_mapping=1; calls=fused_calls=0;
 struct ud_call c={.a=41,.procedure=2};
 assert(ud_run(&s,&c,0,NULL)==-ESTALE && !calls && !fused_calls);
 assert(!locks && !irq_depth && !preempt_depth);
 puts("NESTED_RUNTIME_FIXTURE_PASS hardware_tested=0"); return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            headers(tmp)
            (tmp / 'fixture.c').write_text(fixture)
            for trace in (False, True):
                exe = tmp / ('runtime-trace.exe' if trace else 'runtime.exe')
                command = [shutil.which('gcc'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                           '-I' + str(tmp), '-I' + str(ROOT / 'include/uapi'),
                           '-I' + str(ROOT / 'kernel/ub-domain')]
                if trace: command += ['-DUD_TRACE_ENABLE']
                result = subprocess.run(command + [str(tmp / 'fixture.c'), '-o', str(exe)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__': unittest.main(verbosity=2)
