#ifndef ERPC_LRPC_LRPC_H
#define ERPC_LRPC_LRPC_H

#include <stddef.h>
#include <stdint.h>

#include <linux/ub_lrpc.h>

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

int lrpc_publish(int fd, const void *code, size_t code_size,
		 uint32_t procedure_id, uint64_t epoch);
int lrpc_publish_services(int fd, const void *code, size_t code_size,
			  const uint32_t *procedure_ids, size_t count,
			  uint64_t epoch);
int lrpc_bind(struct lrpc_handle *handle, const char *device,
	      uint32_t procedure_id, uint64_t expected_epoch);
int lrpc_pin(struct lrpc_handle *handle);
int lrpc_invoke(struct lrpc_handle *handle, struct lrpc_astack *call);
int lrpc_invoke_bytes(struct lrpc_handle *handle, uint32_t procedure_id,
		      const void *request, size_t request_size,
		      void *response, size_t *response_size);
int lrpc_shadow_run(const char *device, uint32_t procedure_id,
		    uint64_t expected_epoch);
typedef long (*lrpc_shadow_handler_fn)(struct lrpc_astack *call);
int lrpc_shadow_serve(const char *device, uint32_t procedure_id,
		      uint64_t expected_epoch, lrpc_shadow_handler_fn handler);
int lrpc_shadow_serve_current_stack(const char *device, uint32_t procedure_id,
				    uint64_t expected_epoch,
				    lrpc_shadow_handler_fn handler);
void lrpc_close(struct lrpc_handle *handle);
long lrpc_call_on_stack(void *entry, void *arg, void *stack_top);

#ifdef __cplusplus
}
#endif
#endif
