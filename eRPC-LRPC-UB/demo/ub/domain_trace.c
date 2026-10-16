#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <lrpc/lrpc.h>
#include <linux/ub_lrpc_ub_domain.h>
#include "../../lib/ub_domain_client.h"

enum { FULL, GATE, MIDDLE_FIRST, LEAF, MIDDLE_RETURN,
       HANDOFF_TO_LEAF, HANDOFF_TO_MIDDLE, REMOTE_A_VIEW, FIELDS };

static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static void summary(const char *test, const char *field, uint64_t *values,
                    unsigned int samples)
{
    uint64_t total = 0;
    unsigned int i;
    for (i = 0; i < samples; ++i) total += values[i];
    qsort(values, samples, sizeof(*values), compare_u64);
    printf("UB_DOMAIN_TRACE_RESULT test=%s field=%s samples=%u min_ns=%" PRIu64
           " p50_ns=%" PRIu64 " p99_ns=%" PRIu64 " max_ns=%" PRIu64
           " avg_ns=%.1f\n", test, field, samples, values[0],
           values[(samples - 1) * 50 / 100], values[(samples - 1) * 99 / 100],
           values[samples - 1], (double)total / samples);
}

static int count(const char *text, unsigned int minimum, unsigned int *out)
{
    char *end;
    unsigned long value;
    if (!text[0] || strspn(text, "0123456789") != strlen(text)) return -1;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || *end || value < minimum || value > 10000) return -1;
    *out = (unsigned int)value;
    return 0;
}

int main(int argc, char **argv)
{
    static const char *const names[FIELDS] = {
        "full_ioctl", "kernel_gate_total", "first_domain_gate",
        "leaf_gate", "middle_resume_gate", "handoff_to_leaf",
        "handoff_to_middle", "a_view_remote_heap_load"
    };
    const char *test = "direct";
    unsigned int samples = 20, warmup = 2, procedure, i, f;
    uint64_t *values[FIELDS] = {0}, expected, a, b, remote_value;
    struct lrpc_handle h;
    int parsed = 0, failed = 1;

    for (i = 1; i < (unsigned int)argc; ++i) {
        if (!strcmp(argv[i], "--test")) {
            if (++i >= (unsigned int)argc) goto usage;
            test = argv[i];
        } else if (!strcmp(argv[i], "--samples") || !strcmp(argv[i], "--warmup")) {
            int is_samples = !strcmp(argv[i], "--samples");
            if (++i >= (unsigned int)argc ||
                count(argv[i], is_samples ? 1 : 0,
                      is_samples ? &samples : &warmup)) goto usage;
        } else goto usage;
    }
    if (strcmp(test, "direct") && strcmp(test, "nested")) goto usage;
    procedure = !strcmp(test, "nested") ? 2 : 1;
    a = procedure == 2 ? 41 : 21;
    b = procedure == 2 ? 0 : 21;
    expected = procedure == 2 ? 43 : 142;
    for (f = 0; f < FIELDS; ++f) {
        values[f] = calloc(samples, sizeof(*values[f]));
        if (!values[f]) { perror("calloc"); goto free_values; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("UB_DOMAIN_TRACE_BEGIN test=%s warmup=%u samples=%u "
           "service_load_isolated=0 page_walk_proven=0\n", test, warmup, samples);
    if (setenv("LRPC_EXECUTION_BACKEND", "ub-remote-domain", 1)) goto free_values;
    if (lrpc_bind(&h, "/dev/ub_lrpc_domain", 1, 1)) {
        perror("UB_DOMAIN_TRACE_BIND_FAIL"); goto free_values;
    }
    parsed = 1;
    for (i = 0; i < warmup + samples; ++i) {
        struct ud_trace_call trace = {.a = a, .b = b, .procedure = procedure};
        uint64_t start = now_ns(), elapsed;
        if (ioctl(h.fd, UD_TRACE_CALL, &trace)) {
            perror("UB_DOMAIN_TRACE_CALL_FAIL"); goto close_handle;
        }
        elapsed = now_ns() - start;
        if (trace.result != expected || trace.esr || trace.far ||
            trace.cpu != (unsigned int)h.shadow_cpu ||
            trace.transitions != (procedure == 2 ? 6U : 2U) ||
            trace.segments != (procedure == 2 ? 3U : 1U) ||
            !trace.gate_total_ns || !trace.segment_ns[0]) {
            fprintf(stderr, "UB_DOMAIN_TRACE_VERIFY_FAIL result=%" PRIu64
                    " transitions=%u segments=%u cpu=%u esr=0x%" PRIx64
                    " far=0x%" PRIx64 "\n", (uint64_t)trace.result,
                    trace.transitions, trace.segments, trace.cpu,
                    (uint64_t)trace.esr, (uint64_t)trace.far);
            goto close_handle;
        }
        if (i < warmup) continue;
        values[FULL][i - warmup] = elapsed;
        values[GATE][i - warmup] = trace.gate_total_ns;
        values[MIDDLE_FIRST][i - warmup] = trace.segment_ns[0];
        values[LEAF][i - warmup] = trace.segment_ns[1];
        values[MIDDLE_RETURN][i - warmup] = trace.segment_ns[2];
        values[HANDOFF_TO_LEAF][i - warmup] = trace.handoff_ns[0];
        values[HANDOFF_TO_MIDDLE][i - warmup] = trace.handoff_ns[1];
        printf("UB_DOMAIN_TRACE_SAMPLE test=%s index=%u full_ns=%" PRIu64
               " gate_total_ns=%" PRIu64 " first_gate_ns=%" PRIu64
               " leaf_gate_ns=%" PRIu64 " resume_gate_ns=%" PRIu64
               " handoff_leaf_ns=%" PRIu64 " handoff_middle_ns=%" PRIu64
               " transitions=%u\n", test, i - warmup, elapsed,
               (uint64_t)trace.gate_total_ns, (uint64_t)trace.segment_ns[0],
               (uint64_t)trace.segment_ns[1], (uint64_t)trace.segment_ns[2],
               (uint64_t)trace.handoff_ns[0], (uint64_t)trace.handoff_ns[1],
               trace.transitions);
    }
    /* A separate diagnostic read through A's NC import, not the EL0 payload's
     * [x1] load. Run after gate samples to avoid perturbing their workload. */
    for (i = 0; i < warmup + samples; ++i) {
        uint64_t elapsed;
        if (ud_lrpc_sample_remote_heap(&h, &remote_value, &elapsed)) {
            perror("UB_DOMAIN_TRACE_REMOTE_READ_FAIL"); goto close_handle;
        }
        if (remote_value != 100) {
            fprintf(stderr, "UB_DOMAIN_TRACE_REMOTE_VALUE_FAIL value=%" PRIu64 "\n",
                    remote_value);
            goto close_handle;
        }
        if (i >= warmup) values[REMOTE_A_VIEW][i - warmup] = elapsed;
    }
    for (f = 0; f < FIELDS; ++f) {
        if (procedure == 1 && f >= LEAF && f <= HANDOFF_TO_MIDDLE) continue;
        summary(test, names[f], values[f], samples);
    }
    puts("UB_DOMAIN_TRACE_PASS timing=instrumented gate_segments_include_service=1 "
         "remote_load=a_view_separate_sample page_walk_proven=0 "
         "restart_owner_before_next_connection=1");
    failed = 0;
close_handle:
    if (parsed) lrpc_close(&h);
free_values:
    for (f = 0; f < FIELDS; ++f) free(values[f]);
    return failed;
usage:
    fprintf(stderr, "usage: %s [--test direct|nested] [--samples 1..10000] "
            "[--warmup 0..10000]\n", argv[0]);
    return 2;
}
