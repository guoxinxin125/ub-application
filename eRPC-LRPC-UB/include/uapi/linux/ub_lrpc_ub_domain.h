/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef UB_LRPC_UB_DOMAIN_H
#define UB_LRPC_UB_DOMAIN_H
#include <linux/types.h>
#include <linux/ioctl.h>
#define UD_ABI 1
#define UD_MAGIC 0x5542444f4d303031ULL
#define UD_BYTES 0x400000
#define UD_PAGE 4096
#define UD_DOMAINS 4
#define UD_TABLE(d,l) (0x10000 + (d)*0x4000 + (l)*0x1000)
#define UD_HEAP 0x30000
#define UD_IMAGE(d) (0x40000 + (d)*0x1000)
#define UD_CONTEXT(d) (0x50000 + (d)*0x1000)
#define UD_VA 0x100000000ULL
#define UD_STACK (UD_VA + 0x2000)
#define UD_DATA (UD_VA + 0x4000)
#define UD_ARGS (UD_VA + 0x6000)
#define UD_DIRECT 0
#define UD_MIDDLE 1
#define UD_LEAF 2
#define UD_QUERY 3
#define UD_NESTED_TRAP_OFFSET 12
/* The contract uses A-view PAs only. No B-process pointers. */
struct ud_contract {
    __u64 magic, nonce;
    __u32 abi, wb_index, nc_index, reserved;
    __u64 table_pa[UD_DOMAINS][4];
    __u64 heap_pa;
    __u64 code_pa[UD_DOMAINS], stack_pa[UD_DOMAINS], args_pa[UD_DOMAINS];
};
struct ud_publication {
    struct ud_contract contract;
    __u64 request, complete;
    __u64 b_initializations;
};
struct ud_prepare {
    __u64 import_va;
    struct ud_contract contract;
};
struct ud_initial {
    __u64 args, heap, sp, pc;
};
struct ud_call {
    __u64 a, b, result, elapsed_ns;
    __u64 esr, far;
    __u32 procedure, transitions, cpu, reserved;
};
#define UD_PREPARE _IOWR('U', 0x80, struct ud_prepare)
#define UD_COMMIT _IO('U', 0x81)
#define UD_CALL _IOWR('U', 0x82, struct ud_call)
/* One Linux page per domain; service sees a/b/result at offsets 0/8/16.
 * User mappings are A-local WB, RW/NX. One outstanding call per owner. */
struct ud_mapped {
    __u64 a, b, result, completion, elapsed_ns, esr, far;
    __u32 procedure, transitions, cpu, status;
};
#define UD_MAPPED_CALL _IO('U', 0x83)
/* Diagnostic only: an optional ASID-targeted TLBI precedes two identical
 * calls. Timings cover the gate, excluding TLBI, ioctl, and VMA validation.
 * A timing difference is not by itself proof of a remote page-table fetch. */
struct ud_tlb_probe {
    __u64 a, b;
    __u64 first_ns, second_ns, first_result, second_result;
    __u64 first_esr, first_far, second_esr, second_far;
    __u32 procedure, flush, first_cpu, second_cpu;
};
#define UD_TLB_PROBE _IOWR('U', 0x84, struct ud_tlb_probe)
#endif
