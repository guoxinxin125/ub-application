#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lrpc/ub_social_client.h>
int main(int argc, char **argv)
{
    struct lrpc_handle h = {.fd=-1};
    struct ud_connection_info info;
    unsigned long samples = 1, i;
    char *end;
    int failed = 0;
    if (argc == 3 && !strcmp(argv[1], "--samples")) {
        errno = 0; samples = strtoul(argv[2], &end, 10);
        if (errno || !argv[2][0] || *end || argv[2][0] == '-' || !samples || samples > 1000000) return 2;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--samples 1..1000000] (pin one CPU with taskset)\n", argv[0]); return 2;
    }
    if (us_client_open(&h)) { perror("UB_SOCIAL_OPEN_FAIL"); return 1; }
    if (us_client_info(&h, &info)) { failed = 1; goto out; }
    for (i = 0; i < samples; ++i) {
        unsigned int reverse;
        for (reverse = 0; reverse < 2; ++reverse) {
            struct us_call c = {0};
            struct us_message expected;
            c.abi = US_ABI; c.direction = reverse;
            c.message.source = 1; c.message.request = reverse ? i+1 : i;
            c.message.value = i+1; c.message.type = reverse ? US_USER_RESP : US_USER_REQ;
            /* Opaque synthetic handle, never dereferenced by routing domains. */
            if (reverse) {
                c.message.machine = 99; c.message.block = 4096;
                c.message.payload_offset = 128; c.message.payload_bytes = 2048;
            }
            expected = c.message;
            expected.action = reverse ? US_ACTION_CLIENT : US_ACTION_RPC;
            expected.target = reverse ? 0 : 102;
            if (us_client_call(&h, &c) || memcmp(&expected, &c.message, sizeof(expected)) ||
                c.domain[0] != (reverse ? 2U : 1U) || c.domain[1] != (reverse ? 1U : 2U) ||
                c.entries != 2 || c.transitions != 4) {
                perror("UB_SOCIAL_ROUTE_FAIL"); failed = 1; goto out;
            }
        }
    }
out:
    if (us_client_close(&h)) { perror("UB_SOCIAL_CLOSE_FAIL"); failed = 1; }
    if (!failed) printf("UB_SOCIAL_ROUTE_PASS samples=%lu calls=%lu slot=%u identity=%llu "
                        "entries_per_chain=2 transitions_per_chain=4 backend=ub read_only=1\n",
                        samples, samples*2, info.slot, (unsigned long long)info.identity);
    return failed;
}
