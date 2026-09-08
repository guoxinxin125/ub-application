.text
.globl lrpc_call_on_stack
.type lrpc_call_on_stack,@function
lrpc_call_on_stack:
	movq %rsp, %r8
	movq %rdi, %rax
	movq %rsi, %rdi
	movq %rdx, %rsp
	andq $-16, %rsp
	subq $16, %rsp
	movq %r8, 8(%rsp)
	call *%rax
	movq 8(%rsp), %rsp
	ret
	.size lrpc_call_on_stack, .-lrpc_call_on_stack
.section .note.GNU-stack,"",@progbits
