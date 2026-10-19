#ifndef UB_DOMAIN_CLIENT_H
#define UB_DOMAIN_CLIENT_H
#include <lrpc/lrpc.h>
#include <linux/ub_lrpc_ub_domain.h>
#define UD_HANDLE_MARKER (UINT64_MAX - 1)
int ud_lrpc_bind(struct lrpc_handle *h, uint32_t procedure, uint64_t epoch);
int ud_lrpc_invoke(struct lrpc_handle *h, struct lrpc_astack *call);
/* Diagnostic: time one A-user-view read of B's imported NC heap. */
int ud_lrpc_sample_remote_heap(struct lrpc_handle *h, uint64_t *value,
                               uint64_t *elapsed_ns);
void ud_lrpc_close(struct lrpc_handle *h);
int ud_lrpc_close_checked(struct lrpc_handle *h);
int ud_lrpc_connection_info(const struct lrpc_handle *h, struct ud_connection_info *info);
#endif
