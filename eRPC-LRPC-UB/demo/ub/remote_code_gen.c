/* B-only example producer of a bounded native AArch64 code bundle. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lrpc/ub_remote_code.h>

static int parse_imm(const char *text, unsigned int *value)
{
    char *end;
    unsigned long parsed;
    if (!text[0] || strspn(text, "0123456789") != strlen(text)) return -1;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || *end || parsed > 4095) return -1;
    *value = (unsigned int)parsed;
    return 0;
}

int main(int argc, char **argv)
{
    struct ud_remote_bundle bundle = {0};
    unsigned int direct_bias = 0, middle_add = 1, leaf_add = 1;
    FILE *output;
    int i;
    if (argc < 2) goto usage;
    for (i = 2; i < argc; i += 2) {
        unsigned int *destination;
        if (i + 1 >= argc) goto usage;
        if (!strcmp(argv[i], "--direct-bias")) destination = &direct_bias;
        else if (!strcmp(argv[i], "--middle-add")) destination = &middle_add;
        else if (!strcmp(argv[i], "--leaf-add")) destination = &leaf_add;
        else goto usage;
        if (parse_imm(argv[i + 1], destination)) goto usage;
    }
    bundle.manifest.magic = UD_REMOTE_MAGIC;
    bundle.manifest.abi = UD_REMOTE_ABI;
    bundle.manifest.mode = UD_CODE_REMOTE_VERIFIED;
    bundle.manifest.words[UD_DIRECT] = 8;
    bundle.image[UD_DIRECT][0] = UD_R_LDR_A;
    bundle.image[UD_DIRECT][1] = UD_R_LDR_B;
    bundle.image[UD_DIRECT][2] = UD_R_LDR_HEAP;
    bundle.image[UD_DIRECT][3] = UD_R_ADD_B;
    bundle.image[UD_DIRECT][4] = UD_R_ADD_HEAP;
    bundle.image[UD_DIRECT][5] = ud_r_add_imm(direct_bias);
    bundle.image[UD_DIRECT][6] = UD_R_STR_RESULT;
    bundle.image[UD_DIRECT][7] = UD_R_BRK_RETURN;
    bundle.manifest.words[UD_MIDDLE] = 7;
    bundle.image[UD_MIDDLE][0] = UD_R_LDR_A;
    bundle.image[UD_MIDDLE][1] = UD_R_NOP;
    bundle.image[UD_MIDDLE][2] = UD_R_NOP;
    bundle.image[UD_MIDDLE][3] = UD_R_BRK_NESTED;
    bundle.image[UD_MIDDLE][4] = ud_r_add_imm(middle_add);
    bundle.image[UD_MIDDLE][5] = UD_R_STR_RESULT;
    bundle.image[UD_MIDDLE][6] = UD_R_BRK_RETURN;
    bundle.manifest.words[UD_LEAF] = 4;
    bundle.image[UD_LEAF][0] = UD_R_LDR_A;
    bundle.image[UD_LEAF][1] = ud_r_add_imm(leaf_add);
    bundle.image[UD_LEAF][2] = UD_R_STR_RESULT;
    bundle.image[UD_LEAF][3] = UD_R_BRK_RETURN;
    bundle.manifest.words[UD_QUERY] = 1;
    bundle.image[UD_QUERY][0] = UD_R_BRK_RETURN;
    if (!ud_remote_bundle_valid(&bundle)) {
        fputs("UB_REMOTE_CODE_FAIL invalid generated image\n", stderr);
        return 1;
    }
    output = fopen(argv[1], "wb");
    if (!output) { perror("fopen"); return 1; }
    {
        int failed = fwrite(&bundle, 1, sizeof(bundle), output) != sizeof(bundle);
        if (fclose(output)) failed = 1;
        if (failed) {
            perror("write remote bundle");
            return 1;
        }
    }
    printf("UB_REMOTE_CODE_BUNDLE_READY file=%s direct_bias=%u middle_add=%u leaf_add=%u "
           "direct_expected=%u nested_expected=%u\n", argv[1], direct_bias,
           middle_add, leaf_add, 142 + direct_bias, 41 + middle_add + leaf_add);
    return 0;
usage:
    fprintf(stderr, "usage: %s OUTPUT [--direct-bias 0..4095] "
            "[--middle-add 0..4095] [--leaf-add 0..4095]\n", argv[0]);
    return 2;
}
