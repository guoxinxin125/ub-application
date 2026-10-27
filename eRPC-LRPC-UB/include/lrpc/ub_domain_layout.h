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
        c->wb_index == c->nc_index || !ud_pa_valid(c->heap_pa) ||
        (c->write_pa && (!ud_pa_valid(c->write_pa) ||
                         c->write_pa == c->heap_pa))) return 0;
    for (d = 0; d < UD_DOMAINS; ++d) {
        if (!ud_pa_valid(c->code_pa[d]) || !ud_pa_valid(c->stack_pa[d]) ||
            !ud_pa_valid(c->args_pa[d])) return 0;
        for (l = 0; l < 4; ++l)
            if (!ud_pa_valid(c->table_pa[d][l]) ||
                (c->write_pa && c->write_pa == c->table_pa[d][l])) return 0;
        if (c->write_pa && (c->write_pa == c->code_pa[d] ||
                            c->write_pa == c->stack_pa[d] ||
                            c->write_pa == c->args_pa[d])) return 0;
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
        if (c->write_pa && d == 1)
            table[8] = c->write_pa | nc | UD_UXN; /* B data, RW/NX; config stays RO. */
    }
}

/* Construct the existing page-table contract for one extra nginx entry.
 * The caller separately validates the source PAs against the imported VMA. */
static inline int ud_social_extra_contract(const struct ud_contract *base,
                                           const struct ud_social_extra *extra,
                                           unsigned int index,
                                           struct ud_contract *out)
{
    unsigned int l, d, e, j, k;
    if (!base || !extra || !out || base->write_pa || index >= UD_SOCIAL_EXTRA_DOMAINS ||
        extra->abi != UD_SOCIAL_EXTRA_ABI ||
        extra->domains != UD_SOCIAL_EXTRA_DOMAINS || extra->nonce != base->nonce ||
        !ud_contract_valid(base))
        return 0;
    /* Separate nginx domains must never silently alias one another or the
     * original domains' tables/code/stacks, even on a malformed import. */
    for (e = 0; e < UD_SOCIAL_EXTRA_DOMAINS; ++e) {
        const struct ud_social_extra_domain *x = &extra->domain[e];
        if (!ud_pa_valid(x->code_pa) || !ud_pa_valid(x->stack_pa) ||
            x->code_pa == x->stack_pa || x->code_pa == base->heap_pa ||
            x->stack_pa == base->heap_pa) return 0;
        for (d = 0; d < UD_DOMAINS; ++d) {
            if (x->code_pa == base->code_pa[d] || x->code_pa == base->stack_pa[d] ||
                x->code_pa == base->args_pa[d] || x->stack_pa == base->code_pa[d] ||
                x->stack_pa == base->stack_pa[d] || x->stack_pa == base->args_pa[d]) return 0;
            for (l = 0; l < 4; ++l)
                if (x->code_pa == base->table_pa[d][l] || x->stack_pa == base->table_pa[d][l]) return 0;
        }
        for (j = 0; j < UD_SOCIAL_EXTRA_DOMAINS; ++j) {
            const struct ud_social_extra_domain *y = &extra->domain[j];
            if (e != j && (x->code_pa == y->code_pa || x->code_pa == y->stack_pa ||
                           x->stack_pa == y->code_pa || x->stack_pa == y->stack_pa)) return 0;
            for (k = 0; k < 4; ++k)
                if (x->code_pa == y->table_pa[k] || x->stack_pa == y->table_pa[k]) return 0;
        }
        for (l = 0; l < 4; ++l) {
            __u64 table = x->table_pa[l];
            if (!ud_pa_valid(table) || table == base->heap_pa) return 0;
            for (d = 0; d < UD_DOMAINS; ++d) {
                if (table == base->code_pa[d] || table == base->stack_pa[d] ||
                    table == base->args_pa[d]) return 0;
                for (k = 0; k < 4; ++k) if (table == base->table_pa[d][k]) return 0;
            }
            for (j = 0; j < UD_SOCIAL_EXTRA_DOMAINS; ++j)
                for (k = 0; k < 4; ++k)
                    if ((j != e || k != l) && table == extra->domain[j].table_pa[k]) return 0;
        }
    }
    d = (index & 1U) ? UD_LEAF : UD_MIDDLE;
    *out = *base;
    out->code_pa[d] = extra->domain[index].code_pa;
    out->stack_pa[d] = extra->domain[index].stack_pa;
    for (l = 0; l < 4; ++l) out->table_pa[d][l] = extra->domain[index].table_pa[l];
    return ud_contract_valid(out);
}
#endif
