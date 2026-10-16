#ifndef UB_DOMAIN_CLIENT_H
#define UB_DOMAIN_CLIENT_H
#include <lrpc/lrpc.h>
#define UD_HANDLE_MARKER (UINT64_MAX - 1)
int ud_lrpc_bind(struct lrpc_handle *h, uint32_t procedure, uint64_t epoch);
int ud_lrpc_invoke(struct lrpc_handle *h, struct lrpc_astack *call);
void ud_lrpc_close(struct lrpc_handle *h);
#endif
