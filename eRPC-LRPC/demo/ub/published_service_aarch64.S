#include <lrpc/lrpc_abi_offsets.h>

.section .rodata.lrpc_service_image,"a",%progbits
.balign 4
.global lrpc_ub_add_image_start
.global lrpc_ub_add_image_entry
.global lrpc_ub_add_image_end
.type lrpc_ub_add_image_start,%object
lrpc_ub_add_image_start:
lrpc_ub_add_image_entry:
	/* long handler(struct lrpc_astack *call), with no external references. */
	cbz x0, .Linvalid
	ldr x1, [x0, #LRPC_ASTACK_REQUEST_SIZE_OFFSET]
	cmp x1, #16
	b.ne .Linvalid
	ldr x1, [x0, #LRPC_ASTACK_SERVICE_DATA_OFFSET]
	cbz x1, .Linvalid
	ldar x1, [x1]
	ldr x2, [x0, #LRPC_ASTACK_PAYLOAD_OFFSET]
	ldr x3, [x0, #(LRPC_ASTACK_PAYLOAD_OFFSET + 8)]
	add x1, x1, x2
	add x1, x1, x3
	str x1, [x0, #LRPC_ASTACK_PAYLOAD_OFFSET]
	mov x1, #8
	str x1, [x0, #LRPC_ASTACK_RESPONSE_SIZE_OFFSET]
	str xzr, [x0, #LRPC_ASTACK_STATUS_OFFSET]
	mov x0, #0
	ret

.Linvalid:
	cbz x0, .Lreturn_error
	mov x1, #22
	str x1, [x0, #LRPC_ASTACK_STATUS_OFFSET]
	str xzr, [x0, #LRPC_ASTACK_RESPONSE_SIZE_OFFSET]
.Lreturn_error:
	mov x0, #22
	neg x0, x0
	ret
lrpc_ub_add_image_end:
.size lrpc_ub_add_image_start, .-lrpc_ub_add_image_start
.section .note.GNU-stack,"",%progbits
