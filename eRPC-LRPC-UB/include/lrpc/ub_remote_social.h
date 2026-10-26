#ifndef LRPC_UB_REMOTE_SOCIAL_H
#define LRPC_UB_REMOTE_SOCIAL_H

#include <linux/ub_lrpc_ub_domain.h>

/* Four straight-line, read-only routing entries. Only x0's action/target
 * fields may be written; x1 reads fixed offsets in B's validated us_config.
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

static inline int ud_remote_social_image_valid(unsigned int domain,
                                               const __u32 *words,
                                               unsigned int count)
{
    __u32 first, action;
    unsigned int i;
    if (!words || domain >= UD_DOMAINS || count < 5 ||
        count > UD_REMOTE_MAX_WORDS || words[count - 1] != UD_RS_RETURN)
        return 0;
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
