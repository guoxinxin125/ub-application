/* Bounded UserTimeline routing ABI. No eRPC objects or process pointers. */
#ifndef UB_SOCIAL_H
#define UB_SOCIAL_H
#include <linux/ub_lrpc_ub_domain.h>
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif
#define US_MAGIC 0x5542534f43494131ULL
#define US_ABI 1U
#define US_SLOTS 15360U
#define US_MAX_NGINX 3U
#define US_NGINX_FIRST 2U
#define US_FORWARD 0U
#define US_REVERSE 1U
#define US_USER_REQ 9U
#define US_USER_RESP 10U
#define US_HOME_REQ 11U
#define US_HOME_RESP 12U
#define US_WRITE_REQ 13U /* bounded write probe, not a PostStorage transaction */
#define US_SHARED_WRITE_REQ 14U
#define US_SHARED_READ_REQ 15U
#define US_SHARED_FETCH_ADD_REQ 16U /* single-instruction LSE fetch-add probe */
#define US_SHARED_VALUE_INDEX 1U
#define US_SHARED_COUNTER_INDEX 2U
#define US_ACTION_RPC 1U
#define US_ACTION_DOMAIN 2U
#define US_ACTION_CLIENT 3U
#define US_MAPPED_OFFSET 128U
/* key is the caller's zero-based selection key, not the multi-client wire
 * request number. Zero means invalid count, never a valid nginx identity. */
static inline __u32 us_select_nginx(__u32 key, __u32 count)
{
    if (!count || count > US_MAX_NGINX) return 0;
    return US_NGINX_FIRST + (key % US_SLOTS) % count;
}
struct us_config {
    __u32 abi, kind, slot_modulus, target_count;
    __u32 nginx[4];
    __u32 compose_endpoint, user_endpoint, home_endpoint, return_domain;
    __u32 allowed_grants;
};
struct us_heap { __u64 magic; struct us_config config[2]; };
struct us_message {
    __u64 source, request, machine, block, payload_offset, payload_bytes, value, ticket;
    __u32 type, action, target, status;
};
struct us_call {
    struct us_message message;
    __u32 abi, direction, entries, domain[2], fault_vector, transitions, cpu;
};
#ifdef __cplusplus
#define US_ASSERT static_assert
#else
#define US_ASSERT _Static_assert
#endif
US_ASSERT(sizeof(struct us_config) == 52, "AArch64 config offsets");
US_ASSERT(sizeof(struct us_heap) == 112, "config heap layout");
US_ASSERT(sizeof(struct us_message) == 80, "routing message layout");
US_ASSERT(__builtin_offsetof(struct us_message, action) == 68, "service instruction offsets");
US_ASSERT(__builtin_offsetof(struct us_message, target) == 72, "service instruction offsets");
US_ASSERT(US_MAPPED_OFFSET + sizeof(struct us_call) <= UD_PAGE, "routing A-stack fits page");
#undef US_ASSERT
static inline void us_heap_init_count(struct us_heap *h, unsigned int count)
{
    unsigned int i, j;
    h->magic = US_MAGIC;
    for (i = 0; i < 2; ++i) {
        struct us_config *c = &h->config[i];
        c->abi = US_ABI; c->kind = i+1; c->slot_modulus = US_SLOTS; c->target_count = count;
        for (j = 0; j < 4; ++j) c->nginx[j] = j < count ? US_NGINX_FIRST+j : 0;
        c->compose_endpoint = 101; c->user_endpoint = 102; c->home_endpoint = 103;
        c->return_domain = 1; c->allowed_grants = 0;
    }
}
static inline void us_heap_init(struct us_heap *h) { us_heap_init_count(h, 1); }
static inline unsigned long us_config_offset(unsigned int d)
{
    return 8 + ((d == 1 || d == 2) ? sizeof(struct us_config) : 0);
}
/* A trusted setup audit only; per-call service instructions read B config. */
static inline int us_heap_valid(const struct us_heap *h)
{
    struct us_heap expected;
    us_heap_init(&expected);
    return !memcmp(h, &expected, sizeof(expected));
}
static inline int us_heap_valid_count(const struct us_heap *h, unsigned int count)
{
    struct us_heap expected;
    if (count != 1 && count != 3) return 0;
    us_heap_init_count(&expected, count);
    return !memcmp(h, &expected, sizeof(expected));
}
#define UD_SOCIAL_CALL _IO('U', 0x8a)
#endif
