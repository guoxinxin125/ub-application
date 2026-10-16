#ifndef LRPC_UBSM_PA_H
#define LRPC_UBSM_PA_H

#include <stdbool.h>
#include <lrpc/lrpc_ubsm.h>
#include <ubs_mem.h>

/* Connection-time diagnostics, not a pinned kernel page-table handle.
 * Keep region mapped and the owner alive until all consumers have stopped.
 * SDK errors are positive; local/OBMM errors are negative errno values. */
struct lrpc_ubsm_pa {
    uint64_t mem_id;
    uint64_t unit_offset;
    uint64_t pa;
};

int lrpc_ubsm_import_info(const struct lrpc_ubsm_region *region,
                          ubsmem_shmem_info_t *info);
int lrpc_ubsm_query_pa(const struct lrpc_ubsm_region *region,
                       const ubsmem_shmem_info_t *info, uint64_t offset,
                       struct lrpc_ubsm_pa *result);
#endif
