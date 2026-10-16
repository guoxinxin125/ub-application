/* Shared exact page-table construction for B and A's validator. */
#ifndef UB_DOMAIN_LAYOUT_H
#define UB_DOMAIN_LAYOUT_H
#include <linux/ub_lrpc_ub_domain.h>
#define UD_PA_MASK 0x0000fffffffff000ULL
#define UD_PXN (1ULL << 53)
#define UD_UXN (1ULL << 54)
static inline int ud_pa_valid(__u64 pa)
{
    return pa && !(pa & ~UD_PA_MASK);
}
static inline int ud_contract_valid(const struct ud_contract *c)
{
    unsigned int d, l;
    if (c->magic != UD_MAGIC || c->abi != UD_ABI || !c->nonce ||
        c->reserved || c->wb_index > 7 || c->nc_index > 7 ||
        c->wb_index == c->nc_index || !ud_pa_valid(c->heap_pa)) return 0;
    for (d = 0; d < UD_DOMAINS; ++d) {
        if (!ud_pa_valid(c->code_pa[d]) || !ud_pa_valid(c->stack_pa[d]) ||
            !ud_pa_valid(c->args_pa[d])) return 0;
        for (l = 0; l < 4; ++l)
            if (!ud_pa_valid(c->table_pa[d][l])) return 0;
    }
    return 1;
}
static inline void ud_build_table(const struct ud_contract *c,
                                  unsigned int d, unsigned int level,
                                  __u64 *table)
{
    unsigned int i;
    /* Valid page, EL0 access, AF, inner shareable, non-global, privileged NX. */
    __u64 flags = 3 | (1ULL << 6) | (3ULL << 8) | (1ULL << 10) |
                  (1ULL << 11) | UD_PXN;
    __u64 wb = flags | ((__u64)c->wb_index << 2);
    __u64 nc = flags | ((__u64)c->nc_index << 2);
    for (i = 0; i < 512; ++i) table[i] = 0;
    if (level < 3) {
        i = (UD_VA >> (39 - 9 * level)) & 511;
        table[i] = c->table_pa[d][level + 1] | 3;
    } else {
        table[0] = c->code_pa[d] | wb | (1ULL << 7); /* RX */
        table[2] = c->stack_pa[d] | wb | UD_UXN;
        table[4] = c->heap_pa | nc | (1ULL << 7) | UD_UXN;
        table[6] = c->args_pa[d] | wb | UD_UXN;
    }
}
#endif
