/* B-only producer of bounded User/Home read routing images. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lrpc/ub_remote_code.h>

int main(int argc, char **argv)
{
    struct ud_remote_bundle bundle = {0};
    unsigned int i, padding = 0, read_home = 0, nginx_count = 1;
    int padding_seen = 0, count_seen = 0;
    FILE *out;
    if (argc < 4 || strcmp(argv[2], "--service") ||
        (strcmp(argv[3], "user") && strcmp(argv[3], "home"))) goto usage;
    read_home = !strcmp(argv[3], "home");
    for (i = 4; i < (unsigned int)argc; i += 2) {
        char *end;
        unsigned long n;
        if (i + 1 >= (unsigned int)argc || !argv[i+1][0] ||
            strspn(argv[i+1], "0123456789") != strlen(argv[i+1])) goto usage;
        errno = 0; n = strtoul(argv[i+1], &end, 10);
        if (errno || *end) goto usage;
        if (!strcmp(argv[i], "--nop-padding") && !padding_seen++ && n <= 4)
            padding = (unsigned int)n;
        else if (!strcmp(argv[i], "--nginx-count") && !count_seen++ && (n == 1 || n == 3))
            nginx_count = (unsigned int)n;
        else goto usage;
    }
    bundle.manifest.magic = UD_REMOTE_MAGIC;
    bundle.manifest.abi = UD_REMOTE_ABI;
    bundle.manifest.mode = UD_CODE_REMOTE_SOCIAL;
    bundle.manifest.reserved = (read_home ? UD_SOCIAL_CODE_HOME : UD_SOCIAL_CODE_USER) |
        (nginx_count == 3 ? UD_SOCIAL_CODE_THREE_NGINX : 0);
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
        if (i == UD_DIRECT && nginx_count == 3) {
            for (j = 0; j < UD_RS_LB3_WORDS; ++j)
                bundle.image[i][j] = ud_remote_social_lb3_word(j);
            bundle.manifest.words[i] = UD_RS_LB3_WORDS + padding + 1;
            for (j = 0; j < padding; ++j) bundle.image[i][UD_RS_LB3_WORDS+j] = UD_RS_NOP;
            bundle.image[i][UD_RS_LB3_WORDS+padding] = UD_RS_RETURN;
        } else {
            bundle.manifest.words[i] = 5 + padding;
            bundle.image[i][0] = first;
            bundle.image[i][1] = UD_RS_STR_TARGET;
            bundle.image[i][2] = action;
            bundle.image[i][3] = UD_RS_STR_ACTION;
            for (j = 0; j < padding; ++j) bundle.image[i][4+j] = UD_RS_NOP;
            bundle.image[i][4+padding] = UD_RS_RETURN;
        }
    }
    if (!ud_remote_bundle_valid(&bundle)) return 1;
    out = fopen(argv[1], "wb");
    if (!out) { perror("fopen"); return 1; }
    {
        int failed = fwrite(&bundle, 1, sizeof(bundle), out) != sizeof(bundle);
        if (fclose(out)) failed = 1;
        if (failed) { perror("write remote social bundle"); return 1; }
    }
    printf("UB_REMOTE_SOCIAL_BUNDLE_READY service=%s nginx_count=%u nop_padding=%u file=%s\n",
           read_home ? "home" : "user", nginx_count, padding, argv[1]);
    return 0;
usage:
    fprintf(stderr, "usage: %s OUTPUT --service user|home [--nop-padding 0..4] [--nginx-count 1|3]\n", argv[0]);
    return 2;
}
