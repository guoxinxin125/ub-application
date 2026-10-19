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
.global us_nginx_forward_start, us_nginx_forward_end
us_nginx_forward_start:
    ldr w2, [x1, #36]           /* UserTimeline endpoint */
    str w2, [x0, #72]
    mov w2, #1
    str w2, [x0, #68]           /* ACTION_RPC */
    brk #0x4c0
us_nginx_forward_end:
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
