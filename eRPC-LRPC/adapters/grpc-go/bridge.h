#ifndef ERPC_LRPC_GRPC_GO_BRIDGE_H
#define ERPC_LRPC_GRPC_GO_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

struct lrpc_handle;

struct lrpc_handle *lrpc_go_open(const char *device, uint32_t procedure_id,
				 uint64_t epoch);
void lrpc_go_close(struct lrpc_handle *handle);
int lrpc_go_invoke(struct lrpc_handle *handle, uint32_t procedure_id,
		   const void *request, size_t request_size,
		   void *response, size_t *response_size);
int lrpc_go_serve(const char *device, uint32_t procedure_id, uint64_t epoch);

#endif
