/* Included by lrpc.c so existing upstream eRPC builds also link this backend.
 * Explicit opt-in only. No silent fallback to the scheduler-shadow backend. */
#include <linux/ub_lrpc_remote_app.h>

static int ra_backend_requested(void)
{
	const char *name = getenv("LRPC_EXECUTION_BACKEND");
	return name && !strcmp(name, "remote-domain");
}

static int ra_backend_production_first(void)
{
	const char *value = getenv("LRPC_REMOTE_PRODUCTION_FIRST");
	return value && !strcmp(value, "1");
}

static int ra_u64_compare(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}

/* Outside application timing, using the SAME RA_DIRECT root and RA_DATA VA.
 * Classifier is a conservative behavioral guard, not a PMU/memory-type proof.
 * Fail closed on ambiguous behavior; never turn ambiguity into a PASS. */
static int ra_backend_probe(int fd)
{
	enum { N = 101, WARMUP = 10 };
	uint64_t empty[N], warm[N], cold[N];
	struct ra_probe p = {0};
	unsigned int mode = 2;
	for (int i = -WARMUP; i < N; i++) {
		if (ioctl(fd, RA_PROBE) || ioctl(fd, RA_PROBE_REPORT, &p)) return -1;
		if (p.abi != RA_ABI || p.cache_mode > RA_CACHE_UC ||
		    p.warm_value != 100 || p.cold_value != 100 ||
		    !p.empty || !p.warm || !p.cold ||
		    p.empty > 1000000000ULL || p.warm > 1000000000ULL || p.cold > 1000000000ULL ||
		    (mode != 2 && p.cache_mode != mode)) { errno = EPROTO; return -1; }
		mode = p.cache_mode;
		if (i >= 0) { empty[i] = p.empty; warm[i] = p.warm; cold[i] = p.cold; }
	}
	qsort(empty, N, sizeof(uint64_t), ra_u64_compare);
	qsort(warm, N, sizeof(uint64_t), ra_u64_compare);
	qsort(cold, N, sizeof(uint64_t), ra_u64_compare);
	uint64_t e = empty[N/2], w = warm[N/2], c = cold[N/2];
	int wb = c > w + 40 && c > w * 3 / 2;
	/* UC has no meaningful cache-hit/cache-miss distinction.  Requiring the
	 * two serialized loads to have nearly equal medians made the guard reject
	 * valid UC runs when uncore/queueing delay affected only one position in
	 * the probe sequence.  Keep this fail-closed by requiring BOTH loads to be
	 * clearly slower than the timestamp-only control.  A WB mapping still
	 * fails because its warm load remains close to the empty measurement. */
	int uc = w > e + 80 && c > e + 80;
	int pass = mode == RA_CACHE_WB ? wb : uc;
	printf("REMOTE_APP_HEAP_PROBE mode=%s scope=shadow-ring3 abi=3 samples=%d "
	       "empty_p50_cycles=%llu warm_p50_cycles=%llu cold_p50_cycles=%llu "
	       "classification=%s\n", mode == RA_CACHE_WB ? "remote-wb" : "remote-nc", N,
	       (unsigned long long)e, (unsigned long long)w, (unsigned long long)c,
	       pass ? (mode == RA_CACHE_WB ? "effective-wb" : "effective-uc") : "ambiguous");
	if (!pass) { errno = EPROTO; return -1; }
	printf("REMOTE_APP_HEAP_PROBE_PASS mode=%s scope=shadow-ring3 abi=3\n",
	       mode == RA_CACHE_WB ? "remote-wb" : "remote-nc");
	return 0;
}

static int ra_backend_bind(struct lrpc_handle *h, uint32_t procedure)
{
	cpu_set_t set;
	int cpu = -1;
	int production_first = ra_backend_production_first();
	if (procedure < 1 || procedure > 3) { errno = ENOTSUP; return -1; }
	memset(h, 0, sizeof(*h)); h->fd = -1;
	if (sched_getaffinity(0, sizeof(set), &set)) return -1;
	for (int i = 0; i < CPU_SETSIZE; ++i)
		if (CPU_ISSET(i, &set)) { cpu = i; break; }
	if (CPU_ISSET(1, &set)) cpu = 1;
	if (cpu < 0) { errno = EXDEV; return -1; }
	CPU_ZERO(&set); CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set)) return -1;
	h->fd = open("/dev/ub_lrpc_remote_app", O_RDWR | O_CLOEXEC);
	if (h->fd < 0) return -1;
	if (ioctl(h->fd, production_first ? RA_PREPARE_FIRST_USE : RA_PREPARE) ||
	    (!production_first && ra_backend_probe(h->fd))) {
		int saved = errno; close(h->fd); h->fd = -1; errno = saved; return -1;
	}
	if (production_first)
		printf("REMOTE_APP_DIAGNOSTIC_POLICY production_first=1 "
		       "root_probe=skipped heap_probe=skipped heap_record_check=skipped\n");
	h->shadow_cpu = cpu;
	h->epoch = 1;
	/* Per-handle discriminator, never infer backend from changing env later. */
	h->astack_offset = UINT64_MAX;
	printf("REMOTE_APP_BOUND backend=remote-domain procedure=%u cpu=%d "
	       "page_tables=BAR2 heap=BAR2 context=local stack=local pcid=on tlb=retain\n",
	       procedure, cpu);
	return 0;
}

static int ra_backend_invoke(struct lrpc_handle *h, struct lrpc_astack *call)
{
	struct ra_call c = {0};
	if (!call || call->abi != LRPC_ASTACK_ABI ||
	    (call->procedure_id < 1 || call->procedure_id > 3) ||
	    call->request_size != (call->procedure_id == 2 ? 8U : 16U)) {
		errno = EINVAL; return -1;
	}
	c.procedure = (uint32_t)call->procedure_id;
	memcpy(&c.a, call->payload, 8);
	if (c.procedure != 2) memcpy(&c.b, call->payload + 8, 8);
	/* Args/results remain A-local WB. RA_CALL enters the remote root;
	 * RA_REPORT copies the completed kernel-local result back to userspace.
	 * Both ioctls are INCLUDED in lrpc_invoke / eRPC end-to-end timing. */
	if (ioctl(h->fd, RA_CALL, &c) || ioctl(h->fd, RA_REPORT, &c)) return -1;
	if (c.fault_vector || c.transitions != (c.procedure == 2 ? 4U : 2U)) {
		errno = EPROTO; return -1;
	}
	memcpy(call->payload, &c.result, 8);
	call->response_size = 8; call->status = 0;
	call->caller_cpu_before = c.cpu; call->caller_cpu_in_service = c.cpu;
	call->caller_cpu_after = (uint64_t)sched_getcpu();
	call->caller_pid = (uint64_t)getpid();
	/* B domain is not a Linux task. Zero explicitly means no shadow PID. */
	call->shadow_pid = 0;
	call->call_enter_ns = c.enter_ns; call->shadow_dispatch_ns = c.dispatch_ns;
	call->shadow_return_ns = c.return_ns; call->caller_resume_ns = c.resume_ns;
	return 0;
}
