#ifndef LRPC_UB_REMOTE_SOCIAL_H
#define LRPC_UB_REMOTE_SOCIAL_H

#include <linux/ub_lrpc_ub_domain.h>

/* Four straight-line, read-only routing images. Only x0's action/target
 * fields may be written; x1 reads validated B config and, for 3 nginx, one
 * bounded entry from its target array.
 * Padding is an optional B-authored NOP sequence, never a branch or syscall.
 * This is deliberately narrower than the legacy social_services.S profile. */
#define UD_RS_LDR_NGINX 0xb9401022U /* ldr w2, [x1, #16] */
#define UD_RS_LDR_USER  0xb9402422U /* ldr w2, [x1, #36] */
#define UD_RS_LDR_HOME  0xb9402822U /* ldr w2, [x1, #40] */
#define UD_RS_LDR_RETURN 0xb9402c22U /* ldr w2, [x1, #44] */
#define UD_RS_STR_TARGET 0xb9004802U /* str w2, [x0, #72] */
#define UD_RS_STR_ACTION 0xb9004402U /* str w2, [x0, #68] */
#define UD_RS_MOV_0 0x52800002U
#define UD_RS_MOV_RPC 0x52800022U
#define UD_RS_MOV_DOMAIN 0x52800042U
#define UD_RS_MOV_CLIENT 0x52800062U
#define UD_RS_NOP 0xd503201fU
#define UD_RS_RETURN 0xd4209800U /* brk #0x4c0 */

/* Branch-free LB image: key -> (key % slot_modulus) % target_count, then
 * load the selected domain identity from the bounded four-entry config. */
#define UD_RS_LB3_WORDS 12U
static inline __u32 ud_remote_social_lb3_word(unsigned int index)
{
    switch (index) {
    case 0: return 0xb9400822U; /* ldr w2,[x1,#8] */
    case 1: return 0xb9400c23U; /* ldr w3,[x1,#12] */
    case 2: return 0xf9400404U; /* ldr x4,[x0,#8] */
    case 3: return 0x9ac20885U; /* udiv x5,x4,x2 */
    case 4: return 0x9b0290a4U; /* msub x4,x5,x2,x4 */
    case 5: return 0x9ac30885U; /* udiv x5,x4,x3 */
    case 6: return 0x9b0390a4U; /* msub x4,x5,x3,x4 */
    case 7: return 0x91004025U; /* add x5,x1,#16 */
    case 8: return 0xb86478a4U; /* ldr w4,[x5,x4,lsl #2] */
    case 9: return 0xb9004804U; /* str w4,[x0,#72] */
    case 10: return 0x52800044U; /* mov w4,#2 */
    case 11: return 0xb9004404U; /* str w4,[x0,#68] */
    default: return 0;
    }
}

static inline unsigned int ud_remote_social_count(__u32 tag)
{
    __u32 kind = tag & UD_SOCIAL_CODE_KIND_MASK;
    __u32 topology = tag & ~UD_SOCIAL_CODE_KIND_MASK;
    if (kind != UD_SOCIAL_CODE_USER && kind != UD_SOCIAL_CODE_HOME) return 0;
    if (!topology) return 1;
    return topology == UD_SOCIAL_CODE_THREE_NGINX ? 3 : 0;
}

static inline int ud_remote_social_image_valid(unsigned int domain,
                                               const __u32 *words,
                                               unsigned int count)
{
    __u32 first, action;
    unsigned int i;
    if (!words || domain >= UD_DOMAINS || count < 5 ||
        count > UD_REMOTE_MAX_WORDS || words[count - 1] != UD_RS_RETURN)
        return 0;
    if (domain == UD_DIRECT && words[0] == ud_remote_social_lb3_word(0)) {
        if (count < UD_RS_LB3_WORDS + 1) return 0;
        for (i = 0; i < UD_RS_LB3_WORDS; ++i)
            if (words[i] != ud_remote_social_lb3_word(i)) return 0;
        for (; i + 1 < count; ++i)
            if (words[i] != UD_RS_NOP) return 0;
        return 1;
    }
    switch (domain) {
    case UD_DIRECT:
        first = UD_RS_LDR_NGINX; action = UD_RS_MOV_DOMAIN; break;
    case UD_MIDDLE:
        if (words[0] != UD_RS_LDR_USER && words[0] != UD_RS_LDR_HOME)
            return 0;
        first = words[0]; action = UD_RS_MOV_RPC; break;
    case UD_LEAF:
        first = UD_RS_LDR_RETURN; action = UD_RS_MOV_DOMAIN; break;
    default:
        first = UD_RS_MOV_0; action = UD_RS_MOV_CLIENT; break;
    }
    if (words[0] != first || words[1] != UD_RS_STR_TARGET ||
        words[2] != action || words[3] != UD_RS_STR_ACTION)
        return 0;
    for (i = 4; i + 1 < count; ++i)
        if (words[i] != UD_RS_NOP) return 0;
    return 1;
}

#endif
