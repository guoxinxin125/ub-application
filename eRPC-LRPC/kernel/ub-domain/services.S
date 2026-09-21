/* Fixed integer-only EL0 payloads. x0=A-stack, x1=B heap. */
.section .rodata.ud_services,"a"
.balign 4
.global ud_direct_start, ud_direct_end
ud_direct_start:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    ldr x2, [x0]
    ldr x3, [x0, #8]
    add x2, x2, x3
    ldr x3, [x1]
    add x2, x2, x3
    str x2, [x0, #16]
    ldp x29, x30, [sp], #16
    brk #0x4c0
ud_direct_end:
.global ud_middle_start, ud_middle_end
ud_middle_start:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    ldr x2, [x0]
    brk #0x4c1
    add x2, x2, #1
    str x2, [x0, #16]
    ldp x29, x30, [sp], #16
    brk #0x4c0
ud_middle_end:
.global ud_leaf_start, ud_leaf_end
ud_leaf_start:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    ldr x2, [x0]
    add x2, x2, #1
    str x2, [x0, #16]
    ldp x29, x30, [sp], #16
    brk #0x4c0
ud_leaf_end:
.global ud_query_start, ud_query_end
ud_query_start:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    ldr x2, [x0]
    ldr x3, [x0, #8]
    mov x4, #0
    mov x5, #0
1:  add x6, x1, #64
    add x6, x6, x4, lsl #6
    ldr x7, [x6]
    ldr x4, [x6, #8]
    and x4, x4, #31
    cmp x7, x3
    b.eq 2f
    subs x2, x2, #1
    b.ne 1b
    b 3f
2:  ldr x5, [x6, #16]
3:  str x5, [x0, #16]
    ldp x29, x30, [sp], #16
    brk #0x4c0
ud_query_end:
.section .note.GNU-stack,"",%progbits
