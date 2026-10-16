#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <lrpc/lrpc.h>
struct bench_options {
    const char *test;
    unsigned int samples, warmup;
    int prepare_only;
};
static void usage(const char *program)
{
    printf("usage: %s [--test all|direct|nested|query-1|query-8|query-32] "
           "[--samples N] [--warmup N]\n"
           "       %s --prepare-only\n"
           "defaults: --test all --samples 1000 --warmup 100\n"
           "first execution: --test direct --samples 1 --warmup 0\n",
           program, program);
}
static int parse_count(const char *text, unsigned int minimum, unsigned int *out)
{
    char *end;
    unsigned long value;
    if (!text[0] || strspn(text, "0123456789") != strlen(text)) return -1;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || *end || value < minimum || value > 1000000) return -1;
    *out = (unsigned int)value;
    return 0;
}
/* Validate every argument before bind/PREPARE touches the kernel or B. */
static int parse_options(int argc, char **argv, struct bench_options *options)
{
    int i, execution_options = 0;
    *options = (struct bench_options){"all", 1000, 100, 0};
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 1; }
        if (!strcmp(argv[i], "--prepare-only")) { options->prepare_only = 1; continue; }
        if (!strcmp(argv[i], "--test")) {
            if (++i == argc) goto bad;
            options->test = argv[i];
        } else if (!strcmp(argv[i], "--samples") || !strcmp(argv[i], "--warmup")) {
            int samples = !strcmp(argv[i], "--samples");
            if (++i == argc || parse_count(argv[i], samples ? 1 : 0,
                    samples ? &options->samples : &options->warmup)) goto bad;
        } else goto bad;
        execution_options = 1;
    }
    if (options->prepare_only && execution_options) goto bad;
    if (strcmp(options->test, "all") && strcmp(options->test, "direct") &&
        strcmp(options->test, "nested") && strcmp(options->test, "query-1") &&
        strcmp(options->test, "query-8") && strcmp(options->test, "query-32")) goto bad;
    return 0;
bad:
    fprintf(stderr, "Invalid benchmark options (samples: 1..1000000; warmup: 0..1000000)\n");
    usage(argv[0]);
    return -1;
}
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
                uint64_t a, uint64_t b, uint64_t expected,
                const struct bench_options *options)
{
    uint64_t *samples = calloc(options->samples, sizeof(*samples)), total = 0;
    struct lrpc_astack *call = calloc(1, sizeof(*call));
    unsigned int i;
    if (!call || !samples) { free(call); free(samples); return -1; }
    printf("UB_DOMAIN_TEST_BEGIN case=%s warmup=%u samples=%u total_calls=%u\n",
           name, options->warmup, options->samples, options->warmup+options->samples);
    for (i = 0; i < options->warmup + options->samples; ++i) {
        uint64_t req[2] = {a, b}, result = 0, start, elapsed;
        call->abi = LRPC_ASTACK_ABI;
        call->procedure_id = proc;
        call->request_size = proc == 2 ? 8 : 16;
        memcpy(call->payload, req, call->request_size);
        start = now_ns();
        if (lrpc_invoke(h, call)) {
            perror("lrpc_invoke"); free(call); free(samples); return -1;
        }
        elapsed = now_ns() - start;
        memcpy(&result, call->payload, sizeof(result));
        if (call->response_size != 8 || call->status || result != expected ||
            call->caller_cpu_before != call->caller_cpu_after) {
            fprintf(stderr, "UB_DOMAIN_FAIL case=%s result=%" PRIu64 " expected=%" PRIu64 "\n",
                    name, result, expected);
            free(call); free(samples); return -1;
        }
        if (i >= options->warmup) { samples[i-options->warmup] = elapsed; total += elapsed; }
    }
    qsort(samples, options->samples, sizeof(samples[0]), compare);
    printf("UB_DOMAIN_RESULT case=%s result=%" PRIu64 " samples=%u warmup=%u "
           "min_ns=%" PRIu64 " p50_ns=%" PRIu64 " p99_ns=%" PRIu64
           " max_ns=%" PRIu64 " avg_ns=%.1f tlb=retain astack=mmap scope=lrpc_invoke\n",
           name, expected, options->samples, options->warmup,
           samples[0], samples[(options->samples-1)*50/100], samples[(options->samples-1)*99/100],
           samples[options->samples-1], (double)total/options->samples);
    free(call);
    free(samples);
    return 0;
}
int main(int argc, char **argv)
{
    struct lrpc_handle h;
    struct bench_options options;
    static const struct {
        const char *name;
        unsigned int procedure;
        uint64_t a, b, expected;
    } cases[] = {
        {"direct", 1, 21, 21, 142}, {"nested", 2, 41, 0, 43},
        {"query-1", 3, 1, 0, 1000}, {"query-8", 3, 8, 7, 1007},
        {"query-32", 3, 32, 31, 1031},
    };
    unsigned int i, completed = 0;
    int failed = 0, parsed = parse_options(argc, argv, &options);
    if (parsed) return parsed < 0 ? 2 : 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("UB_DOMAIN_RUN test=%s warmup=%u samples=%u prepare_only=%d\n",
           options.test, options.warmup, options.samples, options.prepare_only);
    if (setenv("LRPC_EXECUTION_BACKEND", "ub-remote-domain", 1)) return 1;
    if (lrpc_bind(&h, "/dev/ub_lrpc_domain", 1, 1)) {
        perror("UB_DOMAIN_BIND_FAIL"); return 1;
    }
    if (options.prepare_only) {
        lrpc_close(&h);
        puts("UB_DOMAIN_PREPARE_PASS execution_tested=0 restart_owner_before_next_connection=1");
        return 0;
    }
    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        if (strcmp(options.test, "all") && strcmp(options.test, cases[i].name)) continue;
        failed = run(&h, cases[i].name, cases[i].procedure, cases[i].a, cases[i].b,
                     cases[i].expected, &options);
        if (failed) break;
        completed++;
    }
    lrpc_close(&h);
    if (!failed) {
        printf("UB_DOMAIN_TEST_PASS test=%s cases=%u calls=%u restart_owner_before_next_connection=1\n",
               options.test, completed, completed*(options.warmup+options.samples));
        if (!strcmp(options.test, "all")) puts("UB_DOMAIN_E2E_PASS");
    }
    return failed ? 1 : 0;
}
