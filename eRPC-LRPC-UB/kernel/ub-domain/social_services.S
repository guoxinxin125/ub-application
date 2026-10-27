/* x0=A-local us_message, x1=B-owned immutable us_config. Integer only. */
.section .rodata.us_services,"a"
.balign 4
.global us_lb_forward_start, us_lb_forward_end
us_lb_forward_start:
    ldr w2, [x1, #8]             /* slot_modulus */
    ldr w3, [x1, #12]            /* target_count */
    ldr x4, [x0, #8]             /* request selection key */
    udiv x5, x4, x2
    msub x4, x5, x2, x4
    udiv x5, x4, x3
    msub x4, x5, x3, x4
    add x5, x1, #16
    ldr w4, [x5, x4, lsl #2]
    str w4, [x0, #72]           /* stable target domain, not session */
    mov w4, #2
    str w4, [x0, #68]           /* ACTION_DOMAIN */
    brk #0x4c0
us_lb_forward_end:
.global us_nginx_forward_write_start, us_nginx_forward_write_end
us_nginx_forward_write_start:
    ldr w2, [x0, #64]
    cmp w2, #13                 /* US_WRITE_REQ: isolated per-connection page */
    b.ne 3f
    ldr x2, [x0, #8]           /* request selects one of 512 u64 slots */
    and x2, x2, #511
    mov x3, #0x8000
    movk x3, #1, lsl #32        /* UD_WRITE = 0x100008000 */
    add x3, x3, x2, lsl #3
    ldr x4, [x0, #48]          /* value */
    str x4, [x3]               /* domain EL0 -> B-owned NC page */
    dsb sy                     /* complete the off-box store before read-back */
    ldr x4, [x3]
    str x4, [x0, #56]          /* ticket: local read-back, not B observation */
    mov w2, #104
    b 5f
3:  cmp w2, #11
    b.eq 4f
    ldr w2, [x1, #36]
    b 5f
4:  ldr w2, [x1, #40]
5:  str w2, [x0, #72]
    mov w2, #1
    str w2, [x0, #68]
    brk #0x4c0
us_nginx_forward_write_end:
.arch_extension lse
.global us_nginx_forward_shared_start, us_nginx_forward_shared_end
us_nginx_forward_shared_start:
    ldr w2, [x0, #64]
    cmp w2, #14                 /* shared cell store */
    b.eq 6f
    cmp w2, #15                 /* shared cell/counter read */
    b.eq 7f
    cmp w2, #16                 /* LSE fetch-add on shared counter */
    b.eq 8f
    cmp w2, #11
    b.eq 11f
    ldr w2, [x1, #36]
    b 12f
11: ldr w2, [x1, #40]
    b 12f
6:  mov x3, #0x8000
    movk x3, #1, lsl #32        /* UD_WRITE, same B PA for every connection */
    add x3, x3, #8             /* shared cell */
    ldr x4, [x0, #48]
    stlr x4, [x3]
    dsb sy
    ldar x4, [x3]
    str x4, [x0, #56]
    b 10f
7:  mov x3, #0x8000
    movk x3, #1, lsl #32
    ldr x4, [x0, #8]           /* validated index 1 or 2 */
    add x3, x3, x4, lsl #3
    ldar x4, [x3]
    dsb sy
    str x4, [x0, #56]
    b 10f
8:  mov x3, #0x8000
    movk x3, #1, lsl #32
    add x3, x3, #16            /* shared counter */
    mov x4, #1
    ldaddal x4, x5, [x3]       /* one RMW, x5 receives the previous value */
    dsb sy
    str x5, [x0, #56]          /* previous counter value */
10: mov w2, #105
12: str w2, [x0, #72]
    mov w2, #1
    str w2, [x0, #68]
    brk #0x4c0
us_nginx_forward_shared_end:
.global us_nginx_return_start, us_nginx_return_end
us_nginx_return_start:
    ldr w2, [x1, #44]           /* return_domain=LB */
    str w2, [x0, #72]
    mov w2, #2
    str w2, [x0, #68]
    brk #0x4c0
us_nginx_return_end:
.global us_lb_return_start, us_lb_return_end
us_lb_return_start:
    mov w2, #0
    str w2, [x0, #72]           /* caller resolves provenance locally */
    mov w2, #3
    str w2, [x0, #68]           /* ACTION_CLIENT */
    brk #0x4c0
us_lb_return_end:
.section .note.GNU-stack,"",%progbits
