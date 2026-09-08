#ifndef ERPC_LRPC_SERVICE_IMAGE_H
#define ERPC_LRPC_SERVICE_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Position-independent x86-64 service image. ABI: long fn(lrpc_astack *).
 * It records RDTSCP's CPU id, dereferences shadow-only service_data, and adds
 * that B-side value to the first two uint64_t request words.
 */
static const uint8_t lrpc_add_service_image[] = {
	0x0f, 0x01, 0xf9,                         /* rdtscp */
	0x48, 0x89, 0x4f, 0x30,                   /* mov rcx,48(rdi) */
	0x48, 0x8b, 0x57, 0x40,                   /* mov 64(rdi),rdx */
	0x48, 0x8b, 0x02,                         /* mov (rdx),rax */
	0x48, 0x03, 0x47, 0x60,                   /* add 96(rdi),rax */
	0x48, 0x03, 0x47, 0x68,                   /* add 104(rdi),rax */
	0x48, 0x89, 0x47, 0x60,                   /* mov rax,96(rdi) */
	0x48, 0xc7, 0x47, 0x18, 0x08, 0, 0, 0,   /* response_size=8 */
	0x48, 0xc7, 0x47, 0x20, 0x00, 0, 0, 0,   /* status=0 */
	0x31, 0xc0,                               /* xor eax,eax */
	0xc3                                      /* ret */
};

static const size_t lrpc_add_service_image_size =
	sizeof(lrpc_add_service_image);

#endif
