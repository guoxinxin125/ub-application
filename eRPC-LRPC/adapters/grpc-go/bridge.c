#define _GNU_SOURCE
#include <stdlib.h>

#include "bridge.h"
#include <lrpc/lrpc.h>
#include "../../lib/lrpc.c"

extern int go_lrpc_dispatch(void *payload, size_t request_size,
			    size_t *response_size);

static long lrpc_go_handler(struct lrpc_astack *call)
{
	size_t response_size = LRPC_PAYLOAD_MAX;
	int ret = go_lrpc_dispatch(call->payload, call->request_size,
				   &response_size);

	if (ret) {
		call->status = (uint64_t)(-ret);
		call->response_size = 0;
		return ret;
	}
	call->status = 0;
	call->response_size = response_size;
	return 0;
}

struct lrpc_handle *lrpc_go_open(const char *device, uint32_t procedure_id,
				 uint64_t epoch)
{
	struct lrpc_handle *handle = calloc(1, sizeof(*handle));

	if (!handle)
		return NULL;
	if (lrpc_bind(handle, device, procedure_id, epoch)) {
		free(handle);
		return NULL;
	}
	return handle;
}

void lrpc_go_close(struct lrpc_handle *handle)
{
	if (!handle)
		return;
	lrpc_close(handle);
	free(handle);
}

int lrpc_go_invoke(struct lrpc_handle *handle, uint32_t procedure_id,
		   const void *request, size_t request_size,
		   void *response, size_t *response_size)
{
	if (lrpc_pin(handle))
		return -1;
	return lrpc_invoke_bytes(handle, procedure_id, request, request_size,
				 response, response_size);
}

int lrpc_go_serve(const char *device, uint32_t procedure_id, uint64_t epoch)
{
	return lrpc_shadow_serve_current_stack(device, procedure_id, epoch,
				       lrpc_go_handler);
}
