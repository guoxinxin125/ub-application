#ifndef UB_SOCIAL_CLIENT_H
#define UB_SOCIAL_CLIENT_H
#include <lrpc/lrpc.h>
#include <lrpc/ub_social.h>
#ifdef __cplusplus
extern "C" {
#endif
int us_client_open(struct lrpc_handle *handle);
/* Explicit topology count; the B bundle must advertise the same count. */
int us_client_open_count(struct lrpc_handle *handle, unsigned int nginx_count);
int us_client_open_write(struct lrpc_handle *handle);
int us_client_open_shared(struct lrpc_handle *handle);
/* One pinned owner thread. Zero-initialize each request's output fields
 * (action/target/status/ticket); for a three-nginx reverse call, set domain[0]
 * to the nginx identity recorded on the forward path (2..4). The connection's
 * mapped A-stack is reused.
 * Embedded eRPC callers select external SDK ownership before starting threads. */
int us_client_call(struct lrpc_handle *handle, struct us_call *call);
int us_client_close(struct lrpc_handle *handle);
int us_client_info(const struct lrpc_handle *handle, struct ud_connection_info *info);
#ifdef __cplusplus
}
#endif
#endif
