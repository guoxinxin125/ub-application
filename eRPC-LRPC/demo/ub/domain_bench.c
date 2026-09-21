#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <lrpc/lrpc.h>
static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &t)) { perror("clock"); exit(1); }
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static int run(struct lrpc_handle *h, const char *name, unsigned int proc,
                uint64_t a, uint64_t b, uint64_t expected)
{
    uint64_t samples[1000], total = 0;
    struct lrpc_astack *call = calloc(1, sizeof(*call));
    int i;
    if (!call) return -1;
    for (i = -100; i < 1000; ++i) {
        uint64_t req[2] = {a, b}, result = 0, start, elapsed;
        call->abi = LRPC_ASTACK_ABI;
        call->procedure_id = proc;
        call->request_size = proc == 2 ? 8 : 16;
        memcpy(call->payload, req, call->request_size);
        start = now_ns();
        if (lrpc_invoke(h, call)) {
            perror("lrpc_invoke"); free(call); return -1;
        }
        elapsed = now_ns() - start;
        memcpy(&result, call->payload, sizeof(result));
        if (call->response_size != 8 || call->status || result != expected ||
            call->caller_cpu_before != call->caller_cpu_after) {
            fprintf(stderr, "UB_DOMAIN_FAIL case=%s result=%" PRIu64 " expected=%" PRIu64 "\n",
                    name, result, expected);
            free(call); return -1;
        }
        if (i >= 0) { samples[i] = elapsed; total += elapsed; }
    }
    qsort(samples, 1000, sizeof(samples[0]), compare);
    printf("UB_DOMAIN_RESULT case=%s result=%" PRIu64 " samples=1000 warmup=100 "
           "min_ns=%" PRIu64 " p50_ns=%" PRIu64 " p99_ns=%" PRIu64
           " max_ns=%" PRIu64 " avg_ns=%.1f tlb=flush scope=lrpc_invoke\n", name, expected,
           samples[0], samples[499], samples[989], samples[999], total/1000.0);
    free(call);
    return 0;
}
int main(int argc, char **argv)
{
    struct lrpc_handle h;
    int failed;
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--prepare-only"))) return 2;
    if (setenv("LRPC_EXECUTION_BACKEND", "ub-remote-domain", 1)) return 1;
    if (lrpc_bind(&h, "/dev/ub_lrpc_domain", 1, 1)) {
        perror("UB_DOMAIN_BIND_FAIL"); return 1;
    }
    if (argc == 2) {
        lrpc_close(&h);
        puts("UB_DOMAIN_PREPARE_PASS execution_tested=0 restart_owner_before_next_connection=1");
        return 0;
    }
    failed = run(&h, "direct", 1, 21, 21, 142) ||
             run(&h, "nested", 2, 41, 0, 43) ||
             run(&h, "query-1", 3, 1, 0, 1000) ||
             run(&h, "query-8", 3, 8, 7, 1007) ||
             run(&h, "query-32", 3, 32, 31, 1031);
    lrpc_close(&h);
    if (!failed) puts("UB_DOMAIN_E2E_PASS");
    return failed ? 1 : 0;
}
