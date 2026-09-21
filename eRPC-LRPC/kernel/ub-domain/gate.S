/* SPDX-License-Identifier: GPL-2.0 */
/* Bounded experimental EL0 gate. Linux TTBR1 and SP_EL1 remain active.
 * All exceptions return through this local vector while DAIF is masked.
 * Never call Linux C code until the original TTBR0/TCR/VBAR/SP_EL0 return.
 * x0=context, x1=root PA, x2=temporary TCR. No SIMD instructions allowed. */
.text
.balign 16
.global ud_enter
.type ud_enter, %function
ud_enter:
    hint #34 /* bti c */
    sub sp, sp, #256
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    str x18, [sp, #192]
    str x0, [sp, #96]
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
    adr x3, ud_vectors
    msr vbar_el1, x3
    isb
    dsb sy
    msr ttbr0_el1, x1
    msr tcr_el1, x2
    isb
    tlbi vmalle1
    dsb sy
    isb
    mov x30, x0
    ldr x3, [x30, #248]
    msr sp_el0, x3
    ldr x3, [x30, #256]
    msr elr_el1, x3
    mov x3, #0x3c0 /* EL0t, DAIF masked; no caller-controlled PSTATE */
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
.size ud_enter, .-ud_enter

.macro UD_VECTOR slot
    .balign 128
    stp x16, x17, [sp, #160]
    mov x17, #\slot
    b ud_trap
.endm
.balign 2048
ud_vectors:
    UD_VECTOR 0
    UD_VECTOR 1
    UD_VECTOR 2
    UD_VECTOR 3
    UD_VECTOR 4
    UD_VECTOR 5
    UD_VECTOR 6
    UD_VECTOR 7
    UD_VECTOR 8
    UD_VECTOR 9
    UD_VECTOR 10
    UD_VECTOR 11
    UD_VECTOR 12
    UD_VECTOR 13
    UD_VECTOR 14
    UD_VECTOR 15
.balign 128
ud_trap:
    ldr x16, [sp, #96]
    str x17, [x16, #288]
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
    mrs x1, elr_el1
    stp x0, x1, [x16, #248]
    mrs x0, spsr_el1
    mrs x1, esr_el1
    stp x0, x1, [x16, #264]
    mrs x0, far_el1
    str x0, [x16, #280]
    dsb sy
    ldr x0, [sp, #104]
    msr ttbr0_el1, x0
    ldr x0, [sp, #112]
    msr tcr_el1, x0
    isb
    tlbi vmalle1
    dsb sy
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
    ldr x18, [sp, #192]
    add sp, sp, #256
    ret
.section .note.GNU-stack,"",%progbits
