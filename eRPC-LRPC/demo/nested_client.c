#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <lrpc/lrpc.h>

int main(void)
{
	struct lrpc_handle handle;
	struct lrpc_astack call;
	uint64_t value = 41, leaf_pid = 0, total_ns = 0;
	struct timespec begin, end;
	const size_t warmup = 100, samples = 1000;
	int rc;

	if (lrpc_bind(&handle, "/dev/ub_lrpc0", 2, 1)) {
		fprintf(stderr, "nested client: bind: %s\n", strerror(errno));
		return 1;
	}
	for (size_t i = 0; i < warmup + samples; i++) {
		memset(&call, 0, offsetof(struct lrpc_astack, payload));
		call.abi = LRPC_ASTACK_ABI;
		call.procedure_id = 2;
		call.request_size = sizeof(value);
		value = 41;
		memcpy(call.payload, &value, sizeof(value));
		clock_gettime(CLOCK_MONOTONIC, &begin);
		rc = lrpc_invoke(&handle, &call);
		clock_gettime(CLOCK_MONOTONIC, &end);
		memcpy(&value, call.payload, sizeof(value));
		memcpy(&leaf_pid, call.payload + sizeof(value), sizeof(leaf_pid));
		if (rc || call.status || value != 43 || !leaf_pid)
			return 2;
		if (i >= warmup)
			total_ns += (uint64_t)(end.tv_sec - begin.tv_sec) *
				1000000000ULL + (uint64_t)(end.tv_nsec - begin.tv_nsec);
	}
	printf("LRPC_NESTED_RESULT value=%llu rc=%d root_pid=%d middle_pid=%llu "
	       "leaf_pid=%llu cpu=%llu/%llu/%llu\n",
	       (unsigned long long)value, rc, getpid(),
	       (unsigned long long)call.shadow_pid,
	       (unsigned long long)leaf_pid,
	       (unsigned long long)call.caller_cpu_before,
	       (unsigned long long)call.caller_cpu_in_service,
	       (unsigned long long)call.caller_cpu_after);
	if (leaf_pid == call.shadow_pid || call.shadow_pid == (uint64_t)getpid() ||
	    call.caller_cpu_before != call.caller_cpu_in_service ||
	    call.caller_cpu_before != call.caller_cpu_after)
		return 2;
	printf("LRPC_NESTED_PASS calls=%zu switches=%zu depth=2 avg_ns=%llu\n",
	       samples, samples * 4,
	       (unsigned long long)(total_ns / samples));
	lrpc_close(&handle);
	return 0;
}
