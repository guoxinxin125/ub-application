/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef UB_LRPC_UB_DOMAIN_H
#define UB_LRPC_UB_DOMAIN_H
#include <linux/types.h>
#include <linux/ioctl.h>
#define UD_ABI 2
#define UD_MAGIC 0x5542444f4d303031ULL
#define UD_BYTES 0x400000
#define UD_PAGE 4096
#define UD_DOMAINS 4
#define UD_TABLE(d,l) (0x10000 + (d)*0x4000 + (l)*0x1000)
#define UD_HEAP 0x30000
#define UD_WRITE_HEAP(c) (0x300000UL + (c) * UD_PAGE)
#define UD_SHARED_HEAP 0x308000UL
#define UD_IMAGE(d) (0x40000 + (d)*0x1000)
#define UD_CONTEXT(d) (0x50000 + (d)*0x1000)
#define UD_VA 0x100000000ULL
#define UD_STACK (UD_VA + 0x2000)
#define UD_DATA (UD_VA + 0x4000)
#define UD_ARGS (UD_VA + 0x6000)
#define UD_WRITE (UD_VA + 0x8000)
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
    /* Zero for read-only profiles; B-owned private or shared page for opt-in profiles. */
    __u64 write_pa;
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
/* Opt-in diagnostic path. Each segment includes gate entry, EL0 service,
 * exception return and gate restoration; it is not a pure switch timer.
 * Legacy's two handoffs are kernel work between nested gate invocations.
 * Fused nested reports one whole-chain segment and zero handoff fields.
 * No field measures a hardware page walk or a single service load. */
struct ud_trace_call {
    __u64 a, b, result, gate_total_ns;
    __u64 segment_ns[3], handoff_ns[2];
    __u64 esr, far;
    __u32 procedure, transitions, cpu, segments;
};
#define UD_TRACE_CALL _IOWR('U', 0x85, struct ud_trace_call)
/* Connection-local choice, set before PREPARE. New userspace selects fused;
 * raw sessions without this ioctl retain legacy for ABI compatibility.
 * Does not change the B publication, argument or completion layouts. */
#define UD_NESTED_LEGACY 0U
#define UD_NESTED_FUSED 1U
#define UD_SET_NESTED_PATH _IOW('U', 0x86, __u32)
/* Optional multi-connection publisher. Contract/pool ABI 2 adds an opt-in
 * writable B page; both endpoints and A's module must be rebuilt together.
 * Slots are allocated by ONE importing A kernel, not remote atomic RMWs. */
#define UD_POOL_MAGIC 0x5542504f4f4c3031ULL
#define UD_POOL_ABI 2U
#define UD_CLIENTS 8U
#define UD_POOL_SLOT(c) (0x100000UL + (c) * 0x1000UL)
#define UD_POOL_TABLE(c,d,l) (0x110000UL + (c) * 0x10000UL + (d) * 0x4000UL + (l) * 0x1000UL)
/* Optional SocialNetwork 3-nginx extension. The existing four-domain contract
 * and pool table layout remain unchanged for micro/write/shared profiles. */
#define UD_SOCIAL_EXTRA_DOMAINS 4U /* nginx 2/3, forward and return */
#define UD_SOCIAL_EXTRA_ABI 1U
#define UD_SOCIAL_EXTRA_SLOT(c) (UD_POOL_SLOT(c) + 0x400UL)
#define UD_SOCIAL_EXTRA_TABLE(c,d,l) (0x200000UL + (c) * 0x10000UL + (d) * 0x4000UL + (l) * 0x1000UL)
struct ud_social_extra_domain {
    __u64 table_pa[4], code_pa, stack_pa;
};
struct ud_social_extra {
    __u64 nonce;
    __u32 abi, domains;
    struct ud_social_extra_domain domain[UD_SOCIAL_EXTRA_DOMAINS];
};
/* Used only for an opt-in three-nginx social pool, after PREPARE. */
#define UD_SOCIAL_EXTRA_INFO _IOR('U', 0x8c, struct ud_social_extra)
struct ud_pool_header {
    __u64 magic, generation;
    __u32 abi, clients, domains, reserved;
};
struct ud_pool_publication {
    struct ud_publication pub;
    __u32 error, reserved;
};
struct ud_pool_prepare {
    struct ud_prepare domain;
    __u64 generation;
    __u32 abi, slot;
};
struct ud_connection_info {
    __u64 identity, generation;
    __u32 abi, multi, slot, capacity;
};
#define UD_POOL_PREPARE _IOWR('U', 0x87, struct ud_pool_prepare)
#define UD_CONNECTION_INFO _IOR('U', 0x88, struct ud_connection_info)
/* Profile is immutable after PREPARE. Default remains the read-only micro path. */
#define UD_PROFILE_MICRO 0U
#define UD_PROFILE_SOCIAL 1U
#define UD_PROFILE_SOCIAL_WRITE 2U
#define UD_PROFILE_SOCIAL_SHARED 3U /* opt-in correctness probe, not app state */
#define UD_PROFILE_OFFSET 64U
#define UD_PROFILE_MAGIC 0x554250524f463031ULL
struct ud_profile_header { __u64 magic; __u32 abi, profile; };
#define UD_SET_PROFILE _IOW('U', 0x89, __u32)
/* Separate, opt-in bounded-code modes. Legacy remains default. B publishes
 * verified AArch64 code; A does not require a byte-identical local image. */
#define UD_CODE_LEGACY 0U /* micro and write/shared probes; not social read */
#define UD_CODE_REMOTE_VERIFIED 1U
#define UD_CODE_REMOTE_SOCIAL 2U /* B-published read routing; 1 or experimental 3 nginx */
#define UD_SOCIAL_CODE_USER 1U
#define UD_SOCIAL_CODE_HOME 2U
/* Single-nginx bundles retain their original USER/HOME tag. Three-nginx
 * bundles use a new tag so existing modules reject them rather than silently
 * executing a one-nginx topology. */
#define UD_SOCIAL_CODE_KIND_MASK 0xffU
#define UD_SOCIAL_CODE_THREE_NGINX (3U << 8)
#define UD_SET_CODE_MODE _IOW('U', 0x8b, __u32)
#define UD_REMOTE_META_OFFSET 0x1000UL
#define UD_REMOTE_MAGIC 0x554452454d4f5445ULL
#define UD_REMOTE_ABI 1U
#define UD_REMOTE_MAX_WORDS 32U
struct ud_remote_manifest {
    __u64 magic;
    __u32 abi, mode;
    __u32 words[UD_DOMAINS];
    __u32 reserved; /* micro: zero; social: read kind plus optional topology tag */
};
/* File consumed only by B owner; images are copied to UD_IMAGE(d). */
struct ud_remote_bundle {
    struct ud_remote_manifest manifest;
    __u32 image[UD_DOMAINS][UD_REMOTE_MAX_WORDS];
};
#endif
