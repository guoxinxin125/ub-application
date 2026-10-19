/* SPDX-License-Identifier: GPL-2.0 */
/* Pure bounded allocator; caller serializes all operations. One A kernel. */
#ifndef UD_POOL_SLOTS_H
#define UD_POOL_SLOTS_H
#define UD_POOL_BANKS 16U
struct ud_pool_bank {
    __u64 heap, generation;
    unsigned long used, quarantined;
};
static inline int ud_slots_acquire(struct ud_pool_bank *banks, __u64 heap,
                                   __u64 generation, unsigned int *bank,
                                   unsigned int *slot)
{
    unsigned int i, empty = UD_POOL_BANKS;
    if (!heap || !generation) return -EINVAL;
    for (i = 0; i < UD_POOL_BANKS; ++i) {
        if (!banks[i].heap) { if (empty == UD_POOL_BANKS) empty = i; continue; }
        if (banks[i].heap == heap) {
            if (banks[i].generation != generation) return -ESTALE;
            break;
        }
    }
    if (i == UD_POOL_BANKS) {
        if (empty == UD_POOL_BANKS) return -ENOSPC;
        i = empty; banks[i].heap = heap; banks[i].generation = generation;
    }
    for (*slot = 0; *slot < UD_CLIENTS; ++*slot)
        if (!(banks[i].used & (1UL << *slot))) {
            banks[i].used |= 1UL << *slot; *bank = i; return 0;
        }
    return -ENOSPC;
}
static inline void ud_slots_release(struct ud_pool_bank *banks, unsigned int bank,
                                    unsigned int slot, int exposed, int reusable)
{
    unsigned long bit = 1UL << slot;
    if (exposed && !reusable) banks[bank].quarantined |= bit;
    else banks[bank].used &= ~bit;
    if (!banks[bank].used) {
        banks[bank].heap = banks[bank].generation = 0;
        banks[bank].quarantined = 0;
    }
}
#endif
