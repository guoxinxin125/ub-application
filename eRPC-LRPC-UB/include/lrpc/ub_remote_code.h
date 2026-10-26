#ifndef LRPC_UB_REMOTE_CODE_H
#define LRPC_UB_REMOTE_CODE_H

#include <linux/ub_lrpc_ub_domain.h>
#include <lrpc/ub_remote_social.h>

/* Microbenchmark verifier; the separate social verifier is also included for
 * bundle dispatch below. Neither is an arbitrary AArch64 loader. The micro
 * format permits straight-line integer arithmetic, fixed loads/result store
 * and terminal BRK. Branch, call, SVC and extra memory access are rejected. */
#define UD_R_LDR_A 0xf9400002U       /* ldr x2, [x0] */
#define UD_R_LDR_B 0xf9400403U       /* ldr x3, [x0, #8] */
#define UD_R_LDR_HEAP 0xf9400024U    /* ldr x4, [x1] */
#define UD_R_STR_RESULT 0xf9000802U  /* str x2, [x0, #16] */
#define UD_R_NOP 0xd503201fU
#define UD_R_BRK_RETURN 0xd4209800U /* brk #0x4c0 */
#define UD_R_BRK_NESTED 0xd4209820U /* brk #0x4c1 */
#define UD_R_ADD_B 0x8b030042U      /* add x2, x2, x3 */
#define UD_R_ADD_HEAP 0x8b040042U   /* add x2, x2, x4 */
#define UD_R_SUB_B 0xcb030042U
#define UD_R_SUB_HEAP 0xcb040042U
#define UD_R_EOR_B 0xca030042U
#define UD_R_EOR_HEAP 0xca040042U

static inline __u32 ud_r_add_imm(unsigned int value)
{
    return 0x91000042U | (value << 10);
}

static inline int ud_r_arithmetic(__u32 word, unsigned int domain)
{
    if ((word & 0xffc003ffU) == 0x91000042U ||
        (word & 0xffc003ffU) == 0xd1000042U)
        return 1; /* add/sub x2, x2, #imm12, no shift */
    if (domain == UD_DIRECT)
        return word == UD_R_ADD_B || word == UD_R_ADD_HEAP ||
               word == UD_R_SUB_B || word == UD_R_SUB_HEAP ||
               word == UD_R_EOR_B || word == UD_R_EOR_HEAP;
    return 0;
}

static inline int ud_remote_image_valid(unsigned int domain,
                                        const __u32 *words, unsigned int count)
{
    unsigned int i, body, end;
    if (!words || domain >= UD_DOMAINS || count > UD_REMOTE_MAX_WORDS)
        return 0;
    if (domain == UD_QUERY)
        return count == 1 && words[0] == UD_R_BRK_RETURN;
    if (domain == UD_DIRECT) {
        if (count < 6 || words[0] != UD_R_LDR_A ||
            words[1] != UD_R_LDR_B || words[2] != UD_R_LDR_HEAP)
            return 0;
        body = 3;
    } else if (domain == UD_MIDDLE) {
        /* The existing nested gate resumes at the instruction after +12. */
        if (count < 7 || words[0] != UD_R_LDR_A ||
            words[1] != UD_R_NOP || words[2] != UD_R_NOP ||
            words[3] != UD_R_BRK_NESTED)
            return 0;
        body = 4;
    } else {
        if (count < 4 || words[0] != UD_R_LDR_A) return 0;
        body = 1;
    }
    end = count - 2;
    if (end <= body || words[end] != UD_R_STR_RESULT ||
        words[end + 1] != UD_R_BRK_RETURN)
        return 0;
    for (i = body; i < end; ++i)
        if (!ud_r_arithmetic(words[i], domain)) return 0;
    return 1;
}

static inline int ud_remote_bundle_valid(const struct ud_remote_bundle *bundle)
{
    unsigned int d, i;
    if (!bundle || bundle->manifest.magic != UD_REMOTE_MAGIC ||
        bundle->manifest.abi != UD_REMOTE_ABI ||
        (bundle->manifest.mode != UD_CODE_REMOTE_VERIFIED &&
         bundle->manifest.mode != UD_CODE_REMOTE_SOCIAL) ||
        (bundle->manifest.mode == UD_CODE_REMOTE_SOCIAL ?
         (bundle->manifest.reserved != UD_SOCIAL_CODE_USER &&
          bundle->manifest.reserved != UD_SOCIAL_CODE_HOME) :
         bundle->manifest.reserved))
        return 0;
    if (bundle->manifest.mode == UD_CODE_REMOTE_SOCIAL &&
        bundle->image[UD_MIDDLE][0] !=
        (bundle->manifest.reserved == UD_SOCIAL_CODE_HOME ?
         UD_RS_LDR_HOME : UD_RS_LDR_USER))
        return 0;
    for (d = 0; d < UD_DOMAINS; ++d) {
        if (!(bundle->manifest.mode == UD_CODE_REMOTE_SOCIAL ?
              ud_remote_social_image_valid(d, bundle->image[d], bundle->manifest.words[d]) :
              ud_remote_image_valid(d, bundle->image[d], bundle->manifest.words[d])))
            return 0;
        for (i = bundle->manifest.words[d]; i < UD_REMOTE_MAX_WORDS; ++i)
            if (bundle->image[d][i]) return 0;
    }
    return 1;
}

#endif
