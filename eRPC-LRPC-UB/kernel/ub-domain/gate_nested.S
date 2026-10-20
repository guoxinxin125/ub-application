/* SPDX-License-Identifier: GPL-2.0 */
/* Optional bounded middle -> leaf -> middle gate. A's TTBR1 and kernel SP
 * remain active throughout. No Linux C code runs under a shadow TTBR0.
 * x0=kernel-owned ud_nested_gate, x1=shadow TCR, x2=local empty root.
 * DAIF remains masked until A's complete environment has been restored. */
#include "nested_gate.h"
#ifdef UD_GATE_VHE
#define ud_enter_nested ud_enter_nested_vhe
#define ud_nested_vectors ud_nested_vectors_vhe
#define ud_nested_trap ud_nested_trap_vhe
#define ttbr0_el1 ttbr0_el2
#define tcr_el1 tcr_el2
#define vbar_el1 vbar_el2
#define elr_el1 elr_el2
#define spsr_el1 spsr_el2
#define esr_el1 esr_el2
#define far_el1 far_el2
#endif
.text
.balign 16
.global ud_enter_nested
.type ud_enter_nested, %function
ud_enter_nested:
    hint #34 /* bti c */
    sub sp, sp, #256
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    str x0, [sp, #96]
    str x18, [sp, #176]
    str x2, [sp, #184]
    str x0, [sp, #192] /* active ud_regs: middle is at offset zero */
    str xzr, [sp, #200] /* phase: 0=middle call, 1=leaf, 2=middle return */
    mrs x3, ttbr0_el1
    str x3, [sp, #104]
    mrs x3, tcr_el1
    str x3, [sp, #112]
    mrs x3, vbar_el1
    str x3, [sp, #120]
    mrs x3, sp_el0
    str x3, [sp, #128]
    mrs x3, daif
    str x3, [sp, #136]
    mrs x3, elr_el1
    str x3, [sp, #144]
    mrs x3, spsr_el1
    str x3, [sp, #152]
    msr daifset, #15
    mov x3, #1
    str x3, [x0, #UDN_TRANSITIONS]
    str x3, [x0, #UDN_DOMAIN] /* UD_MIDDLE */
    str xzr, [x0, #UDN_COMPLETE]
    adr x3, ud_nested_vectors
    msr vbar_el1, x3
    isb
    dsb sy
    msr ttbr0_el1, x2
    isb
    msr tcr_el1, x1
    isb
    ldr x1, [x0, #UDN_MIDDLE_ROOT]
    msr ttbr0_el1, x1
    isb
    b .Ludn_enter
.size ud_enter_nested, .-ud_enter_nested

/* Restore the selected service, including a suspended middle. Always use a
 * fixed EL0t/DAIF PSTATE; saved service PSTATE is diagnostic only. */
.Ludn_enter:
    ldr x30, [sp, #192]
    ldr x3, [x30, #248]
    msr sp_el0, x3
    ldr x3, [x30, #256]
    msr elr_el1, x3
    mov x3, #0x3c0
    msr spsr_el1, x3
    ldp x0, x1, [x30, #0]
    ldp x2, x3, [x30, #16]
    ldp x4, x5, [x30, #32]
    ldp x6, x7, [x30, #48]
    ldp x8, x9, [x30, #64]
    ldp x10, x11, [x30, #80]
    ldp x12, x13, [x30, #96]
    ldp x14, x15, [x30, #112]
    ldp x16, x17, [x30, #128]
    ldp x18, x19, [x30, #144]
    ldp x20, x21, [x30, #160]
    ldp x22, x23, [x30, #176]
    ldp x24, x25, [x30, #192]
    ldp x26, x27, [x30, #208]
    ldp x28, x29, [x30, #224]
    ldr x30, [x30, #240]
    eret

.macro UDN_VECTOR slot
    .balign 128
    stp x16, x17, [sp, #160]
    mov x17, #\slot
    b ud_nested_trap
.endm
.balign 2048
ud_nested_vectors:
    UDN_VECTOR 0
    UDN_VECTOR 1
    UDN_VECTOR 2
    UDN_VECTOR 3
    UDN_VECTOR 4
    UDN_VECTOR 5
    UDN_VECTOR 6
    UDN_VECTOR 7
    UDN_VECTOR 8
    UDN_VECTOR 9
    UDN_VECTOR 10
    UDN_VECTOR 11
    UDN_VECTOR 12
    UDN_VECTOR 13
    UDN_VECTOR 14
    UDN_VECTOR 15
.balign 128
ud_nested_trap:
    ldr x16, [sp, #192]
    str x17, [x16, #288]
    mrs x17, elr_el1
    str x17, [x16, #256]
    mrs x17, esr_el1
    str x17, [x16, #272]
    mrs x17, far_el1
    str x17, [x16, #280]
    /* Recognize only the synchronous EL0 BRK at this phase's approved PC. */
    ldr x17, [x16, #288]
    cmp x17, #8
    b.ne .Ludn_restore
    ldr x17, [x16, #272]
    lsr x17, x17, #26
    cmp x17, #0x3c
    b.ne .Ludn_restore
    ldr x17, [sp, #200]
    cbz x17, .Ludn_middle_call
    cmp x17, #1
    b.eq .Ludn_leaf_return
    cmp x17, #2
    b.ne .Ludn_restore
    /* Final middle return: do not save its dead service context. */
    ldr x17, [x16, #272]
    and x17, x17, #0xffff
    cmp x17, #0x4c0
    b.ne .Ludn_restore
    ldr x17, [sp, #96]
    ldr x17, [x17, #UDN_RETURN_PC]
    ldr x16, [x16, #256]
    cmp x16, x17
    b.ne .Ludn_restore
    ldr x16, [sp, #96]
    mov x17, #1
    str x17, [x16, #UDN_COMPLETE]
    b .Ludn_restore

.Ludn_middle_call:
    ldr x17, [sp, #96]
    ldr x17, [x17, #UDN_MODE]
    cbnz x17, .Ludn_social_call
    ldr x17, [x16, #272]
    and x17, x17, #0xffff
    cmp x17, #0x4c1
    b.ne .Ludn_restore
    b .Ludn_check_middle_pc
.Ludn_social_call:
    ldr x17, [x16, #272]
    and x17, x17, #0xffff
    cmp x17, #0x4c0
    b.ne .Ludn_restore
.Ludn_check_middle_pc:
    ldr x17, [sp, #96]
    ldr x17, [x17, #UDN_CALL_PC]
    ldr x16, [x16, #256]
    cmp x16, x17
    b.ne .Ludn_restore
    ldr x16, [sp, #96]
    ldr x17, [x16, #UDN_MODE]
    cbnz x17, .Ludn_social_switch
    ldr x16, [sp, #192]
    /* Preserve all middle GPRs, SP and PC exactly once, at suspension. */
    stp x0, x1, [x16, #0]
    stp x2, x3, [x16, #16]
    stp x4, x5, [x16, #32]
    stp x6, x7, [x16, #48]
    stp x8, x9, [x16, #64]
    stp x10, x11, [x16, #80]
    stp x12, x13, [x16, #96]
    stp x14, x15, [x16, #112]
    ldp x0, x1, [sp, #160]
    stp x0, x1, [x16, #128]
    stp x18, x19, [x16, #144]
    stp x20, x21, [x16, #160]
    stp x22, x23, [x16, #176]
    stp x24, x25, [x16, #192]
    stp x26, x27, [x16, #208]
    stp x28, x29, [x16, #224]
    str x30, [x16, #240]
    mrs x0, sp_el0
    str x0, [x16, #248]
    mrs x0, spsr_el1
    str x0, [x16, #264]
    ldr x0, [sp, #96]
    ldr x1, [x0, #UDN_LEAF_ARGS]
    ldr x2, [x0, #16] /* suspended middle x2 is the nested argument */
    str x2, [x1]
    stp xzr, xzr, [x1, #8]
    add x1, x0, #UDN_LEAF
    str x1, [sp, #192]
    mov x1, #1
    str x1, [sp, #200]
    mov x1, #2 /* UD_LEAF */
    str x1, [x0, #UDN_DOMAIN]
    ldr x1, [x0, #UDN_LEAF_ROOT]
    b .Ludn_switch

.Ludn_social_switch:
    /* Validate the first service's routing decision before entering the
     * second domain. Read via A's kernel alias, not a PAN-protected EL0 VA. */
    dsb sy
    mov x0, x16
    ldr x1, [x0, #UDN_SOCIAL_ARGS]
    ldr w2, [x1, #UDN_SOCIAL_ACTION_OFFSET]
    cmp w2, #2 /* US_ACTION_DOMAIN */
    b.ne .Ludn_restore
    ldr w2, [x1, #UDN_SOCIAL_TARGET_OFFSET]
    ldr w1, [x0, #UDN_SOCIAL_TARGET]
    cmp w2, w1
    b.ne .Ludn_restore
    add x1, x0, #UDN_LEAF
    str x1, [sp, #192]
    mov x1, #1
    str x1, [sp, #200]
    mov x1, #2
    str x1, [x0, #UDN_DOMAIN]
    ldr x1, [x0, #UDN_LEAF_ROOT]
    b .Ludn_switch

.Ludn_leaf_return:
    ldr x17, [x16, #272]
    and x17, x17, #0xffff
    cmp x17, #0x4c0
    b.ne .Ludn_restore
    ldr x17, [sp, #96]
    ldr x17, [x17, #UDN_LEAF_PC]
    ldr x16, [x16, #256]
    cmp x16, x17
    b.ne .Ludn_restore
    ldr x16, [sp, #96]
    ldr x17, [x16, #UDN_MODE]
    cbnz x17, .Ludn_social_complete
    /* Complete prior EL0 stores before reading the kernel alias of A-stack. */
    dsb sy
    ldr x0, [sp, #96]
    ldr x1, [x0, #UDN_LEAF_ARGS]
    ldr x2, [x1, #16]
    str x2, [x0, #16] /* resume middle with the leaf result in x2 */
    ldr x1, [x0, #256]
    add x1, x1, #4
    str x1, [x0, #256]
    str x0, [sp, #192]
    mov x1, #2
    str x1, [sp, #200]
    mov x1, #1 /* UD_MIDDLE */
    str x1, [x0, #UDN_DOMAIN]
    ldr x1, [x0, #UDN_MIDDLE_ROOT]
    b .Ludn_switch
.Ludn_social_complete:
    /* The second routing service is final; there is no middle continuation. */
    mov x17, #1
    str x17, [x16, #UDN_COMPLETE]
    b .Ludn_restore
.Ludn_switch:
    ldr x2, [x0, #UDN_TRANSITIONS]
    add x2, x2, #1
    str x2, [x0, #UDN_TRANSITIONS]
    /* Same TCR/VBAR across shadow domains. Retain pinned ASID translations;
     * use the local empty root between roots, with no per-call TLBI. */
    dsb sy
    ldr x2, [sp, #184]
    msr ttbr0_el1, x2
    isb
    msr ttbr0_el1, x1
    isb
    b .Ludn_enter

.Ludn_restore:
    ldr x0, [sp, #96]
    ldr x1, [x0, #UDN_TRANSITIONS]
    add x1, x1, #1
    str x1, [x0, #UDN_TRANSITIONS]
    dsb sy
    ldr x0, [sp, #184]
    msr ttbr0_el1, x0
    isb
    ldr x0, [sp, #112]
    msr tcr_el1, x0
    isb
    ldr x0, [sp, #104]
    msr ttbr0_el1, x0
    isb
    ldr x0, [sp, #120]
    msr vbar_el1, x0
    ldr x0, [sp, #128]
    msr sp_el0, x0
    ldr x0, [sp, #144]
    msr elr_el1, x0
    ldr x0, [sp, #152]
    msr spsr_el1, x0
    isb
    ldr x0, [sp, #136]
    msr daif, x0
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldr x18, [sp, #176]
    add sp, sp, #256
    ret
.section .note.GNU-stack,"",%progbits
