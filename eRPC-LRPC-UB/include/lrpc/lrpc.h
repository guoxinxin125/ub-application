#ifndef ERPC_LRPC_LRPC_H
#define ERPC_LRPC_LRPC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LRPC_ASTACK_ABI 1
#define LRPC_PAYLOAD_MAX (60 * 1024)

struct lrpc_astack {
	uint64_t abi;
	uint64_t procedure_id;
	uint64_t request_size;
	uint64_t response_size;
	uint64_t status;
	uint64_t caller_cpu_before;
	uint64_t caller_cpu_in_service;
	uint64_t caller_cpu_after;
	uint64_t service_data;
	uint64_t shadow_pid;
	uint64_t caller_pid;
	uint64_t reserved;
	uint8_t payload[LRPC_PAYLOAD_MAX];
	uint64_t call_enter_ns;
	uint64_t shadow_dispatch_ns;
	uint64_t shadow_return_ns;
	uint64_t caller_resume_ns;
};

struct lrpc_handle {
	int fd;
	int shadow_cpu;
	void *astack_map;
	uint64_t epoch;
	uint64_t astack_offset;
};

/* UB package API only. QEMU's publisher/shadow/invoke_bytes interfaces live
 * in eRPC-LRPC/include/lrpc/lrpc.h and are not implemented by this library. */
int lrpc_bind(struct lrpc_handle *handle, const char *device,
	      uint32_t procedure_id, uint64_t expected_epoch);
int lrpc_invoke(struct lrpc_handle *handle, struct lrpc_astack *call);
void lrpc_close(struct lrpc_handle *handle);

#ifdef __cplusplus
}
#endif
#endif
