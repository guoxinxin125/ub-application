#ifndef UB_DOMAIN_POOL_H
#define UB_DOMAIN_POOL_H
#include <lrpc/ub_domain_layout.h>
static inline int ud_pool_header_valid(const struct ud_pool_header *h)
{
    return h->magic == UD_POOL_MAGIC && h->generation && h->abi == UD_POOL_ABI &&
           h->clients == UD_CLIENTS && h->domains == UD_DOMAINS && !h->reserved;
}
/* B only: initialize a fresh region, then service independent preparation
 * slots. Polling never modifies shared images/heap or an already complete slot. */
int ud_pool_initialize(void *base, __u64 generation);
int ud_pool_initialize_profile(void *base, __u64 generation, unsigned int profile);
int ud_pool_poll(void *base);
#endif
