/* Userspace-only experiment controls. Never included in the RPC hot path.
 * Clock audit covers the whole measured loop (including validation), not the
 * sum of per-call latency samples. TSC ticks are NOT core-frequency cycles.
 */
#ifndef LRPC_REMOTE_APP_BENCH_H
#define LRPC_REMOTE_APP_BENCH_H
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

#ifdef __cplusplus
#define RA_BENCH_U64(value) static_cast<uint64_t>(value)
#define RA_BENCH_DOUBLE(value) static_cast<double>(value)
#else
#define RA_BENCH_U64(value) ((uint64_t)(value))
#define RA_BENCH_DOUBLE(value) ((double)(value))
#endif

static inline uint64_t ra_bench_clock(clockid_t clock)
{
	struct timespec ts;
	if (clock_gettime(clock, &ts)) { perror("benchmark clock"); exit(1); }
	return RA_BENCH_U64(ts.tv_sec) * UINT64_C(1000000000) +
	       RA_BENCH_U64(ts.tv_nsec);
}

static inline uint64_t ra_bench_tsc(void)
{
#if defined(__x86_64__)
	unsigned int lo, hi;
	__asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) : : "memory");
	return (RA_BENCH_U64(hi) << 32) | RA_BENCH_U64(lo);
#else
	return 0;
#endif
}

/* One sample per fresh guest run.  Setup and mandatory cache-policy probes
 * have completed, but no workload warmup call has run yet. */
static inline void ra_bench_first_call(const char *test, uint64_t elapsed_ns)
{
	printf("REMOTE_APP_FIRST_CALL test=%s workload_warmup_calls=0 "
	       "total_ns=%" PRIu64 " after_setup=1 before_time_warmup=1 "
	       "before_fixed_warmup=1\n", test, elapsed_ns);
}

struct ra_bench_warm {
	uint64_t start, duration, calls;
	unsigned long ms;
	const char *test;
	int enabled;
};

static inline void ra_bench_warm_begin(struct ra_bench_warm *w, const char *test)
{
	memset(w, 0, sizeof(*w));
	const char *backend = getenv("LRPC_EXECUTION_BACKEND");
	w->enabled = backend && !strcmp(backend, "remote-domain");
	if (!w->enabled) return;  /* Do not alter old scheduler/UB benchmarks. */
	const char *value = getenv("LRPC_REMOTE_WARMUP_MS");
	char *end = NULL;
	w->ms = value ? strtoul(value, &end, 10) : 1000;
	if ((value && (!*value || *end || *value == '-')) || w->ms > 60000) {
		fprintf(stderr, "Invalid LRPC_REMOTE_WARMUP_MS (0..60000)\n"); exit(1);
	}
	w->test = test;
	w->duration = w->ms * 1000000ULL;
	w->start = ra_bench_clock(CLOCK_MONOTONIC_RAW);
}

static inline int ra_bench_warm_more(struct ra_bench_warm *w)
{
	if (!w->enabled) return 0;
	uint64_t elapsed = ra_bench_clock(CLOCK_MONOTONIC_RAW) - w->start;
	if (elapsed < w->duration) { w->calls++; return 1; }
	printf("REMOTE_APP_PRECONDITION test=%s calls=%" PRIu64
	       " requested_ms=%lu elapsed_ns=%" PRIu64 " outside_timing=1\n",
	       w->test, w->calls, w->ms, elapsed);
	w->enabled = 0;
	return 0;
}

struct ra_bench_audit {
	uint64_t raw, mono, tsc;
	struct rusage usage;
};

static inline void ra_bench_audit_begin(struct ra_bench_audit *a)
{
	if (getrusage(RUSAGE_SELF, &a->usage)) { perror("benchmark rusage"); exit(1); }
	a->raw = ra_bench_clock(CLOCK_MONOTONIC_RAW);
	a->mono = ra_bench_clock(CLOCK_MONOTONIC);
	a->tsc = ra_bench_tsc();
}

static inline void ra_bench_audit_end(const struct ra_bench_audit *a,
				      const char *test, const char *pass)
{
	uint64_t tsc = ra_bench_tsc() - a->tsc;
	uint64_t mono = ra_bench_clock(CLOCK_MONOTONIC) - a->mono;
	uint64_t raw = ra_bench_clock(CLOCK_MONOTONIC_RAW) - a->raw;
	struct rusage usage;
	if (getrusage(RUSAGE_SELF, &usage)) { perror("benchmark rusage"); exit(1); }
	printf("REMOTE_APP_CLOCK_AUDIT test=%s pass=%s scope=whole_measured_loop "
	       "raw_ns=%" PRIu64 " monotonic_ns=%" PRIu64
	       " tsc_ticks=%" PRIu64 " tsc_ticks_per_ns=%.6f "
	       "minor_faults=%ld major_faults=%ld voluntary_switches=%ld involuntary_switches=%ld\n",
	       test, pass, raw, mono, tsc,
	       raw ? RA_BENCH_DOUBLE(tsc) / RA_BENCH_DOUBLE(raw) : 0.0,
	       usage.ru_minflt-a->usage.ru_minflt, usage.ru_majflt-a->usage.ru_majflt,
	       usage.ru_nvcsw-a->usage.ru_nvcsw, usage.ru_nivcsw-a->usage.ru_nivcsw);
}
#undef RA_BENCH_U64
#undef RA_BENCH_DOUBLE
#endif
