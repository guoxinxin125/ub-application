#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lrpc/ub_social_client.h>
#include <lrpc/ub_nested_path.h>
int main(int argc, char **argv)
{
    struct lrpc_handle h = {.fd=-1};
    struct ud_connection_info info;
    unsigned long samples = 1, i;
    unsigned int path, nginx_count = 1, routes[US_MAX_NGINX] = {0};
    char *end;
    int failed = 0;
    for (int arg = 1; arg < argc; arg += 2) {
        unsigned long n;
        if (arg + 1 >= argc || !argv[arg+1][0] || argv[arg+1][0] == '-') return 2;
        errno = 0; n = strtoul(argv[arg+1], &end, 10);
        if (errno || *end) return 2;
        if (!strcmp(argv[arg], "--samples") && n && n <= 1000000) samples = n;
        else if (!strcmp(argv[arg], "--nginx-count") && (n == 1 || n == 3)) nginx_count = (unsigned int)n;
        else {
            fprintf(stderr, "usage: %s [--samples 1..1000000] [--nginx-count 1|3] (pin one CPU)\n", argv[0]);
            return 2;
        }
    }
    if (ud_nested_path_parse_profile(getenv("LRPC_UB_NESTED_PATH"), UD_PROFILE_SOCIAL, &path)) {
        perror("UB_SOCIAL_PATH_FAIL"); return 2;
    }
    if (us_client_open_count(&h, nginx_count)) { perror("UB_SOCIAL_OPEN_FAIL"); return 1; }
    if (us_client_info(&h, &info)) { failed = 1; goto out; }
    for (i = 0; i < samples; ++i) {
        unsigned int reverse;
        for (reverse = 0; reverse < 2; ++reverse) {
            struct us_call c = {0};
            struct us_message expected;
            unsigned int nginx = us_select_nginx((__u32)i, nginx_count);
            c.abi = US_ABI; c.direction = reverse;
            c.message.source = 1; c.message.request = reverse ? i+1 : i;
            c.message.value = i+1; c.message.type = reverse ? US_USER_RESP : US_USER_REQ;
            if (reverse && nginx_count == 3) c.domain[0] = nginx;
            /* Opaque synthetic handle, never dereferenced by routing domains. */
            if (reverse) {
                c.message.machine = 99; c.message.block = 4096;
                c.message.payload_offset = 128; c.message.payload_bytes = 2048;
            }
            expected = c.message;
            expected.action = reverse ? US_ACTION_CLIENT : US_ACTION_RPC;
            expected.target = reverse ? 0 : 102;
            if (us_client_call(&h, &c) || memcmp(&expected, &c.message, sizeof(expected)) ||
                c.domain[0] != (reverse ? nginx : 1U) || c.domain[1] != (reverse ? 1U : nginx) ||
                c.entries != 2 || c.transitions != (path == UD_NESTED_FUSED ? 3U : 4U)) {
                perror("UB_SOCIAL_ROUTE_FAIL"); failed = 1; goto out;
            }
            if (!reverse) ++routes[nginx-US_NGINX_FIRST];
        }
    }
out:
    if (us_client_close(&h)) { perror("UB_SOCIAL_CLOSE_FAIL"); failed = 1; }
    if (!failed) {
        for (i = 0; i < nginx_count; ++i)
            printf("UB_SOCIAL_ROUTE_HIT domain=%u calls=%u\n", US_NGINX_FIRST+(unsigned int)i, routes[i]);
        printf("UB_SOCIAL_ROUTE_PASS samples=%lu calls=%lu slot=%u identity=%llu nginx_count=%u "
                        "entries_per_chain=2 transitions_per_chain=%u chain=%s backend=ub read_only=1\n",
                        samples, samples*2, info.slot, (unsigned long long)info.identity,
                        nginx_count,
                        path == UD_NESTED_FUSED ? 3U : 4U, ud_nested_path_name(path));
    }
    return failed;
}
