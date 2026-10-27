#include <errno.h>
#include <string.h>
#include <lrpc/ub_domain_pool.h>
#include <lrpc/ub_domain_services.h>
_Static_assert(UD_POOL_TABLE(UD_CLIENTS-1, UD_DOMAINS-1, 3) + UD_PAGE <= UD_BYTES,
               "pool tables exceed region");
_Static_assert(UD_WRITE_HEAP(UD_CLIENTS-1) + UD_PAGE <= UD_BYTES,
               "write pages exceed region");
_Static_assert(UD_SHARED_HEAP >= UD_WRITE_HEAP(UD_CLIENTS-1) + UD_PAGE &&
               UD_SHARED_HEAP + UD_PAGE <= UD_BYTES, "shared page overlaps region");
_Static_assert(sizeof(struct ud_pool_publication) <= UD_PAGE, "publication exceeds slot");
int ud_pool_initialize(void *base, __u64 generation)
{
    return ud_pool_initialize_profile(base, generation, UD_PROFILE_MICRO);
}
int ud_pool_initialize_profile(void *base, __u64 generation, unsigned int profile)
{
    struct ud_pool_header *h = base;
    struct ud_profile_header *tag;
    __u64 *heap;
    unsigned int d;
    if (!base || !generation || profile > UD_PROFILE_SOCIAL_SHARED) return -EINVAL;
    memset(base, 0, UD_BYTES);
    for (d = 0; d < UD_DOMAINS; ++d) {
        struct ud_initial initial = {UD_ARGS, UD_DATA, UD_STACK+UD_PAGE, UD_VA};
        if (profile != UD_PROFILE_MICRO) initial.heap += us_config_offset(d);
        if (profile != UD_PROFILE_SOCIAL) {
            unsigned long size;
            const unsigned char *code = ud_profile_service(profile, d, &size);
            if (!code || !size || size > UD_PAGE) return -EINVAL;
            memcpy((char *)base + UD_IMAGE(d), code, size);
        } /* Read-only social images must be published by B before OWNER_READY. */
        memcpy((char *)base + UD_CONTEXT(d), &initial, sizeof(initial));
    }
    heap = (__u64 *)((char *)base + UD_HEAP);
    heap[0] = 100;
    for (d = 0; d < 32; ++d) {
        heap[8+d*8] = d; heap[9+d*8] = (d+1)&31; heap[10+d*8] = 1000+d;
    }
    if (profile != UD_PROFILE_MICRO) us_heap_init((void *)heap);
    tag = (void *)((char *)base + UD_PROFILE_OFFSET);
    *tag = (struct ud_profile_header){UD_PROFILE_MAGIC, UD_POOL_ABI, profile};
    h->generation = generation; h->abi = UD_POOL_ABI;
    h->clients = UD_CLIENTS; h->domains = UD_DOMAINS;
    __atomic_store_n(&h->magic, UD_POOL_MAGIC, __ATOMIC_RELEASE);
    return 0;
}
int ud_pool_poll(void *base)
{
    unsigned int slot, d, l;
    struct ud_profile_header *tag;
    int published = 0;
    if (!base || !ud_pool_header_valid(base)) return -EINVAL;
    tag = (void *)((char *)base + UD_PROFILE_OFFSET);
    if (tag->magic != UD_PROFILE_MAGIC || tag->abi != UD_POOL_ABI ||
        tag->profile > UD_PROFILE_SOCIAL_SHARED) return -EINVAL;
    for (slot = 0; slot < UD_CLIENTS; ++slot) {
        struct ud_pool_publication *p = (void *)((char *)base + UD_POOL_SLOT(slot));
        struct ud_contract c;
        __u64 request = __atomic_load_n(&p->pub.request, __ATOMIC_ACQUIRE);
        if (!request || request == __atomic_load_n(&p->pub.complete, __ATOMIC_RELAXED)) continue;
        memcpy(&c, &p->pub.contract, sizeof(c));
        p->error = !ud_contract_valid(&c) || c.nonce != request || p->reserved ||
                   (!!c.write_pa != (tag->profile == UD_PROFILE_SOCIAL_WRITE ||
                                     tag->profile == UD_PROFILE_SOCIAL_SHARED)) ? EINVAL : 0;
        if (!p->error) {
            /* Never clear the shared page when another connection joins. */
            if (tag->profile == UD_PROFILE_SOCIAL_WRITE)
                memset((char *)base + UD_WRITE_HEAP(slot), 0, UD_PAGE);
            for (d = 0; d < UD_DOMAINS; ++d) for (l = 0; l < 4; ++l)
                ud_build_table(&c, d, l, (void *)((char *)base + UD_POOL_TABLE(slot,d,l)));
            ++p->pub.b_initializations;
        }
        __atomic_store_n(&p->pub.complete, request, __ATOMIC_RELEASE);
        ++published;
    }
    return published;
}
