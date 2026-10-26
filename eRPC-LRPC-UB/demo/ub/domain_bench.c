#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <lrpc/lrpc.h>
#include <lrpc/ub_nested_path.h>
#include "../../lib/ub_domain_client.h"
struct bench_options {
    const char *test;
    unsigned int samples, warmup;
    unsigned int expected_direct, expected_nested;
    int prepare_only;
    int emit_samples;
    const char *nested_path;
    const char *code_mode;
};
static void usage(const char *program)
{
    printf("usage: %s [--test all|direct|nested|query-1|query-8|query-32] "
           "[--samples N] [--warmup N] [--nested-path legacy|fused] [--emit-samples]\n"
           "       [--code-mode legacy|remote] [--expected-direct N] [--expected-nested N]\n"
           "       %s --prepare-only\n"
           "defaults: --test all --samples 1000 --warmup 100 --nested-path fused\n"
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
    unsigned int path;
    *options = (struct bench_options){
        .test = "all", .samples = 1000, .warmup = 100,
        .expected_direct = 142, .expected_nested = 43,
        .nested_path = getenv("LRPC_UB_NESTED_PATH"),
        .code_mode = getenv("LRPC_UB_CODE_MODE")};
    if (!options->nested_path) options->nested_path = UD_NESTED_DEFAULT_NAME;
    if (!options->code_mode) options->code_mode = "legacy";
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 1; }
        if (!strcmp(argv[i], "--prepare-only")) { options->prepare_only = 1; continue; }
        if (!strcmp(argv[i], "--emit-samples")) { options->emit_samples = 1; execution_options = 1; continue; }
        if (!strcmp(argv[i], "--nested-path")) {
            if (++i == argc) goto bad;
            options->nested_path = argv[i];
            continue;
        }
        if (!strcmp(argv[i], "--code-mode")) {
            if (++i == argc) goto bad;
            options->code_mode = argv[i];
            continue;
        }
        if (!strcmp(argv[i], "--test")) {
            if (++i == argc) goto bad;
            options->test = argv[i];
        } else if (!strcmp(argv[i], "--expected-direct") ||
                   !strcmp(argv[i], "--expected-nested")) {
            int direct = !strcmp(argv[i], "--expected-direct");
            if (++i == argc || parse_count(argv[i], 0, direct ?
                    &options->expected_direct : &options->expected_nested)) goto bad;
        } else if (!strcmp(argv[i], "--samples") || !strcmp(argv[i], "--warmup")) {
            int samples = !strcmp(argv[i], "--samples");
            if (++i == argc || parse_count(argv[i], samples ? 1 : 0,
                    samples ? &options->samples : &options->warmup)) goto bad;
        } else goto bad;
        execution_options = 1;
    }
    if (options->prepare_only && execution_options) goto bad;
    if (ud_nested_path_parse(options->nested_path, &path)) goto bad;
    if (strcmp(options->code_mode, "legacy") && strcmp(options->code_mode, "remote")) goto bad;
    if (options->prepare_only && !strcmp(options->code_mode, "remote") &&
        !strcmp(options->test, "all")) options->test = "direct";
    if (strcmp(options->test, "all") && strcmp(options->test, "direct") &&
        strcmp(options->test, "nested") && strcmp(options->test, "query-1") &&
        strcmp(options->test, "query-8") && strcmp(options->test, "query-32")) goto bad;
    if (!strcmp(options->code_mode, "remote") && strcmp(options->test, "direct") &&
        strcmp(options->test, "nested")) goto bad;
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
           " max_ns=%" PRIu64 " avg_ns=%.1f tlb=retain astack=mmap scope=lrpc_invoke"
           " nested_path=%s transitions=%u\n",
           name, expected, options->samples, options->warmup,
           samples[0], samples[(options->samples-1)*50/100], samples[(options->samples-1)*99/100],
           samples[options->samples-1], (double)total/options->samples, options->nested_path,
           proc == 2 ? (!strcmp(options->nested_path, "fused") ? 4U : 6U) : 2U);
    if (options->emit_samples) {
        /* Print after all timed calls; keep the default benchmark output unchanged. */
        printf("UB_DOMAIN_SAMPLES case=%s count=%u values_ns=", name, options->samples);
        for (i = 0; i < options->samples; ++i)
            printf("%s%" PRIu64, i ? "," : "", samples[i]);
        putchar('\n');
    }
    free(call);
    free(samples);
    return 0;
}
int main(int argc, char **argv)
{
    struct lrpc_handle h;
    struct ud_connection_info connection;
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
    printf("UB_DOMAIN_RUN test=%s warmup=%u samples=%u prepare_only=%d code_mode=%s\n",
           options.test, options.warmup, options.samples, options.prepare_only,
           options.code_mode);
    if (setenv("LRPC_EXECUTION_BACKEND", "ub-remote-domain", 1) ||
        setenv("LRPC_UB_NESTED_PATH", options.nested_path, 1) ||
        ((strcmp(options.code_mode, "legacy") || getenv("LRPC_UB_CODE_MODE")) &&
         setenv("LRPC_UB_CODE_MODE", options.code_mode, 1))) return 1;
    if (lrpc_bind(&h, "/dev/ub_lrpc_domain", 1, 1)) {
        perror("UB_DOMAIN_BIND_FAIL"); return 1;
    }
    if (options.prepare_only) {
        if (ud_lrpc_connection_info(&h, &connection)) { lrpc_close(&h); return 1; }
        lrpc_close(&h);
        printf("UB_DOMAIN_PREPARE_PASS execution_tested=0 restart_owner_before_next_connection=%u\n",
               !connection.multi);
        return 0;
    }
    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        if (strcmp(options.test, "all") && strcmp(options.test, cases[i].name)) continue;
        failed = run(&h, cases[i].name, cases[i].procedure, cases[i].a, cases[i].b,
                     !strcmp(options.code_mode, "remote") ?
                     (cases[i].procedure == 1 ? options.expected_direct : options.expected_nested) :
                     cases[i].expected, &options);
        if (failed) break;
        completed++;
    }
    if (ud_lrpc_connection_info(&h, &connection)) failed = 1;
    lrpc_close(&h);
    if (!failed) {
        printf("UB_DOMAIN_TEST_PASS test=%s cases=%u calls=%u restart_owner_before_next_connection=%u\n",
               options.test, completed, completed*(options.warmup+options.samples), !connection.multi);
        if (!strcmp(options.test, "all")) puts("UB_DOMAIN_E2E_PASS");
    }
    return failed ? 1 : 0;
}
