#ifndef UB_NESTED_PATH_H
#define UB_NESTED_PATH_H
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <linux/ub_lrpc_ub_domain.h>
#define UD_NESTED_DEFAULT_NAME "fused"
/* Validate before bind changes B's one-shot publication. CLI may override env. */
static inline int ud_nested_path_parse(const char *name, unsigned int *path)
{
    if (!name) name = UD_NESTED_DEFAULT_NAME;
    if (!strcmp(name, "legacy")) *path = UD_NESTED_LEGACY;
    else if (!strcmp(name, "fused")) *path = UD_NESTED_FUSED;
    else { errno = EINVAL; return -1; }
    return 0;
}
/* Social defaults to its validated legacy chain; fused is an explicit choice. */
static inline int ud_nested_path_parse_profile(const char *name, unsigned int profile,
                                                unsigned int *path)
{
    if (ud_nested_path_parse(name ? name : (profile ? "legacy" : NULL), path)) return -1;
    return 0;
}
static inline const char *ud_nested_path_name(unsigned int path)
{
    return path == UD_NESTED_FUSED ? "fused" : "legacy";
}
static inline unsigned int ud_nested_transitions(unsigned int path)
{
    return path == UD_NESTED_FUSED ? 4U : 6U;
}
#endif
