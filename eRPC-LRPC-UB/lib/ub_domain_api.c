#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <lrpc/lrpc.h>
#include "ub_domain_client.h"

/* Minimal public API bridge for the transfer package. The UB connection,
 * mmap arguments and gate protocol remain in the original ub_domain_client.c. */
int lrpc_bind(struct lrpc_handle *h, const char *device,
              uint32_t procedure_id, uint64_t expected_epoch)
{
    const char *backend = getenv("LRPC_EXECUTION_BACKEND");
    if (!h || !device || strcmp(device, "/dev/ub_lrpc_domain") ||
        !backend || strcmp(backend, "ub-remote-domain")) {
        errno = EINVAL;
        return -1;
    }
    return ud_lrpc_bind(h, procedure_id, expected_epoch);
}

int lrpc_invoke(struct lrpc_handle *h, struct lrpc_astack *call)
{
    if (!h || h->fd < 0 || h->astack_offset != UD_HANDLE_MARKER) {
        errno = EINVAL;
        return -1;
    }
    return ud_lrpc_invoke(h, call);
}

void lrpc_close(struct lrpc_handle *h)
{
    if (h && h->fd >= 0 && h->astack_offset == UD_HANDLE_MARKER)
        ud_lrpc_close(h);
}
