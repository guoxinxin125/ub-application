.text
.align 2
.globl lrpc_call_on_stack
.type lrpc_call_on_stack,%function
lrpc_call_on_stack:
	/* x0=entry, x1=argument, x2=new stack top. */
	stp x29, x30, [sp, #-32]!
	str x19, [sp, #16]
	mov x29, sp
	mov x19, sp
	mov x16, x0
	mov x0, x1
	and x2, x2, #-16
	mov sp, x2
	blr x16
	mov sp, x19
	ldr x19, [sp, #16]
	ldp x29, x30, [sp], #32
	ret
.size lrpc_call_on_stack, .-lrpc_call_on_stack
.section .note.GNU-stack,"",%progbits
