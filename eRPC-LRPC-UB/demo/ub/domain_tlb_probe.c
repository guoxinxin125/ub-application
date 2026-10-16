#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <lrpc/lrpc.h>
#include <linux/ub_lrpc_ub_domain.h>

struct options {
    const char *test;
    unsigned int samples, warmup;
};

static void usage(const char *program)
{
    printf("usage: %s [--test direct|nested] [--samples N] [--warmup N]\n"
           "defaults: direct, samples=20, warmup=2\n", program);
}

static int count(const char *text, unsigned int minimum, unsigned int *out)
{
    char *end;
    unsigned long value;
    if (!text[0] || strspn(text, "0123456789") != strlen(text)) return -1;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || *end || value < minimum || value > 100000) return -1;
    *out = (unsigned int)value;
    return 0;
}

static int parse(int argc, char **argv, struct options *o)
{
    int i;
    *o = (struct options){"direct", 20, 2};
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 1; }
        if (!strcmp(argv[i], "--test")) {
            if (++i == argc) goto bad;
            o->test = argv[i];
        } else if (!strcmp(argv[i], "--samples") || !strcmp(argv[i], "--warmup")) {
            int samples = !strcmp(argv[i], "--samples");
            if (++i == argc || count(argv[i], samples ? 1 : 0,
                    samples ? &o->samples : &o->warmup)) goto bad;
        } else goto bad;
    }
    if (strcmp(o->test, "direct") && strcmp(o->test, "nested")) goto bad;
    return 0;
bad:
    fprintf(stderr, "Invalid TLB probe options (samples: 1..100000; warmup: 0..100000)\n");
    usage(argv[0]);
    return -1;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int compare_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static int pair(struct lrpc_handle *h, unsigned int procedure, int flush,
                uint64_t a, uint64_t b, uint64_t expected,
                struct ud_tlb_probe *out)
{
    struct ud_tlb_probe p = {.a = a, .b = b, .procedure = procedure,
                             .flush = (unsigned int)flush};
    if (ioctl(h->fd, UD_TLB_PROBE, &p)) {
        fprintf(stderr, "UB_TLB_PROBE_CALL_FAIL flush=%d errno=%d first_esr=0x%" PRIx64
                " first_far=0x%" PRIx64 " second_esr=0x%" PRIx64
                " second_far=0x%" PRIx64 "\n", flush, errno,
                (uint64_t)p.first_esr, (uint64_t)p.first_far,
                (uint64_t)p.second_esr, (uint64_t)p.second_far);
        return -1;
    }
    if (p.first_result != expected || p.second_result != expected ||
        !p.first_ns || !p.second_ns || p.first_ns > INT64_MAX ||
        p.second_ns > INT64_MAX || p.first_cpu != p.second_cpu ||
        p.first_cpu != (unsigned int)h->shadow_cpu ||
        p.first_esr || p.second_esr) {
        fprintf(stderr, "UB_TLB_PROBE_RESULT_FAIL flush=%d result=%" PRIu64
                "/%" PRIu64 " expected=%" PRIu64 " cpu=%u/%u expected_cpu=%d\n",
                flush, (uint64_t)p.first_result, (uint64_t)p.second_result,
                expected, p.first_cpu, p.second_cpu, h->shadow_cpu);
        return -1;
    }
    *out = p;
    return 0;
}

int main(int argc, char **argv)
{
    struct options o;
    struct lrpc_handle h;
    struct ud_tlb_probe flushed, control;
    uint64_t *cold_first = NULL, *cold_second = NULL;
    uint64_t *control_first = NULL, *control_second = NULL;
    int64_t *effect = NULL;
    uint64_t a, b, expected;
    unsigned int procedure, i;
    int parsed = parse(argc, argv, &o), failed = 1;
    if (parsed) return parsed < 0 ? 2 : 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    procedure = !strcmp(o.test, "nested") ? 2 : 1;
    a = procedure == 2 ? 41 : 21;
    b = procedure == 2 ? 0 : 21;
    expected = procedure == 2 ? 43 : 142;
    cold_first = calloc(o.samples, sizeof(*cold_first));
    cold_second = calloc(o.samples, sizeof(*cold_second));
    control_first = calloc(o.samples, sizeof(*control_first));
    control_second = calloc(o.samples, sizeof(*control_second));
    effect = calloc(o.samples, sizeof(*effect));
    if (!cold_first || !cold_second || !control_first ||
        !control_second || !effect) { perror("calloc"); goto free_arrays; }
    printf("UB_TLB_PROBE_BEGIN test=%s samples=%u warmup=%u calls=%u "
           "tlbi=%u timing=kernel_gate_only\n", o.test, o.samples, o.warmup,
           2U + 4U * (o.samples + o.warmup), o.samples + o.warmup);
    if (setenv("LRPC_EXECUTION_BACKEND", "ub-remote-domain", 1)) goto free_arrays;
    if (lrpc_bind(&h, "/dev/ub_lrpc_domain", 1, 1)) {
        perror("UB_TLB_PROBE_BIND_FAIL"); goto free_arrays;
    }
    /* Prime translations once. Each cycle then measures: TLBI+first+second,
     * followed by a no-TLBI first+second control on the same pinned CPU. */
    if (pair(&h, procedure, 0, a, b, expected, &control)) goto close_handle;
    for (i = 0; i < o.warmup + o.samples; ++i) {
        unsigned int slot;
        if (pair(&h, procedure, 1, a, b, expected, &flushed) ||
            pair(&h, procedure, 0, a, b, expected, &control)) goto close_handle;
        if (i < o.warmup) continue;
        slot = i - o.warmup;
        cold_first[slot] = flushed.first_ns;
        cold_second[slot] = flushed.second_ns;
        control_first[slot] = control.first_ns;
        control_second[slot] = control.second_ns;
        effect[slot] = ((int64_t)flushed.first_ns - (int64_t)flushed.second_ns) -
                       ((int64_t)control.first_ns - (int64_t)control.second_ns);
        printf("UB_TLB_PROBE_SAMPLE index=%u flush_first_ns=%" PRIu64
               " flush_second_ns=%" PRIu64 " control_first_ns=%" PRIu64
               " control_second_ns=%" PRIu64 " effect_ns=%" PRId64
               " cpu=%u\n", slot, cold_first[slot], cold_second[slot],
               control_first[slot], control_second[slot], effect[slot],
               flushed.first_cpu);
    }
    qsort(cold_first, o.samples, sizeof(*cold_first), compare_u64);
    qsort(cold_second, o.samples, sizeof(*cold_second), compare_u64);
    qsort(control_first, o.samples, sizeof(*control_first), compare_u64);
    qsort(control_second, o.samples, sizeof(*control_second), compare_u64);
    qsort(effect, o.samples, sizeof(*effect), compare_i64);
    printf("UB_TLB_PROBE_RESULT test=%s samples=%u flush_first_p50_ns=%" PRIu64
           " flush_second_p50_ns=%" PRIu64 " control_first_p50_ns=%" PRIu64
           " control_second_p50_ns=%" PRIu64 " paired_effect_p50_ns=%" PRId64
           " paired_effect_p99_ns=%" PRId64 " interpretation=timing_only\n",
           o.test, o.samples, cold_first[(o.samples-1)*50/100],
           cold_second[(o.samples-1)*50/100], control_first[(o.samples-1)*50/100],
           control_second[(o.samples-1)*50/100], effect[(o.samples-1)*50/100],
           effect[(o.samples-1)*99/100]);
    puts("UB_TLB_PROBE_PASS correctness=1 page_walk_proven=0 restart_owner_before_next_connection=1");
    failed = 0;
close_handle:
    lrpc_close(&h);
free_arrays:
    free(cold_first); free(cold_second);
    free(control_first); free(control_second); free(effect);
    return failed;
}
