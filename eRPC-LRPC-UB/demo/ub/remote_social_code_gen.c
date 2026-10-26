/* B-only producer of the bounded, single-nginx User/Home read router. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lrpc/ub_remote_code.h>

int main(int argc, char **argv)
{
    struct ud_remote_bundle bundle = {0};
    unsigned int i, padding = 0, read_home = 0;
    FILE *out;
    if (argc < 4 || strcmp(argv[2], "--service") ||
        (strcmp(argv[3], "user") && strcmp(argv[3], "home"))) goto usage;
    read_home = !strcmp(argv[3], "home");
    if (argc == 6) {
        char *end;
        unsigned long n;
        if (strcmp(argv[4], "--nop-padding") || !argv[5][0] ||
            strspn(argv[5], "0123456789") != strlen(argv[5])) goto usage;
        errno = 0; n = strtoul(argv[5], &end, 10);
        if (errno || *end || n > 4) goto usage;
        padding = (unsigned int)n;
    } else if (argc != 4) goto usage;
    bundle.manifest.magic = UD_REMOTE_MAGIC;
    bundle.manifest.abi = UD_REMOTE_ABI;
    bundle.manifest.mode = UD_CODE_REMOTE_SOCIAL;
    bundle.manifest.reserved = read_home ? UD_SOCIAL_CODE_HOME : UD_SOCIAL_CODE_USER;
    for (i = 0; i < UD_DOMAINS; ++i) {
        __u32 first, action;
        unsigned int j;
        switch (i) {
        case UD_DIRECT:
            first = UD_RS_LDR_NGINX; action = UD_RS_MOV_DOMAIN; break;
        case UD_MIDDLE:
            first = read_home ? UD_RS_LDR_HOME : UD_RS_LDR_USER;
            action = UD_RS_MOV_RPC; break;
        case UD_LEAF:
            first = UD_RS_LDR_RETURN; action = UD_RS_MOV_DOMAIN; break;
        default:
            first = UD_RS_MOV_0; action = UD_RS_MOV_CLIENT; break;
        }
        bundle.manifest.words[i] = 5 + padding;
        bundle.image[i][0] = first;
        bundle.image[i][1] = UD_RS_STR_TARGET;
        bundle.image[i][2] = action;
        bundle.image[i][3] = UD_RS_STR_ACTION;
        for (j = 0; j < padding; ++j) bundle.image[i][4+j] = UD_RS_NOP;
        bundle.image[i][4+padding] = UD_RS_RETURN;
    }
    if (!ud_remote_bundle_valid(&bundle)) return 1;
    out = fopen(argv[1], "wb");
    if (!out) { perror("fopen"); return 1; }
    {
        int failed = fwrite(&bundle, 1, sizeof(bundle), out) != sizeof(bundle);
        if (fclose(out)) failed = 1;
        if (failed) { perror("write remote social bundle"); return 1; }
    }
    printf("UB_REMOTE_SOCIAL_BUNDLE_READY service=%s nop_padding=%u file=%s\n",
           read_home ? "home" : "user", padding, argv[1]);
    return 0;
usage:
    fprintf(stderr, "usage: %s OUTPUT --service user|home [--nop-padding 0..4]\n", argv[0]);
    return 2;
}
