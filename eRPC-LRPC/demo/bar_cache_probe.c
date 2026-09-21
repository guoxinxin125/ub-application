#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <lrpc/lrpc.h>

#define PROBE_SAMPLES 10001U
#define PROBE_OFFSET 4096U

static volatile uint64_t probe_sink;

static uint64_t tsc_begin(void)
{
	uint32_t low;
	uint32_t high;

	__asm__ volatile("lfence\n\trdtsc"
			 : "=a"(low), "=d"(high)
			 :
			 : "memory");
	return ((uint64_t)high << 32) | low;
}

static uint64_t tsc_end(void)
{
	uint32_t low;
	uint32_t high;
	uint32_t auxiliary;

	__asm__ volatile("rdtscp\n\tlfence"
			 : "=a"(low), "=d"(high), "=c"(auxiliary)
			 :
			 : "memory");
	(void)auxiliary;
	return ((uint64_t)high << 32) | low;
}

static void flush_line(const volatile void *address)
{
	__asm__ volatile("clflush (%0)" : : "r"(address) : "memory");
	__asm__ volatile("mfence" : : : "memory");
}

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static uint64_t percentile(uint64_t *samples, size_t count, size_t numerator,
			   size_t denominator)
{
	size_t index = ((count - 1) * numerator) / denominator;

	return samples[index];
}

static int pin_to_cpu_zero(void)
{
	cpu_set_t cpus;

	CPU_ZERO(&cpus);
	CPU_SET(0, &cpus);
	return sched_setaffinity(0, sizeof(cpus), &cpus);
}

struct probe_result {
	uint64_t empty_p50;
	uint64_t warm_p50;
	uint64_t warm_p99;
	uint64_t cold_p50;
	uint64_t cold_p99;
	uint64_t warm_adjusted;
	uint64_t cold_adjusted;
	const char *classification;
};

static int measure_mapping(volatile uint64_t *address,
			   struct probe_result *result)
{
	uint64_t *empty_samples = NULL;
	uint64_t *warm_samples = NULL;
	uint64_t *cold_samples = NULL;
	uint64_t begin, end, value;
	size_t i;

	*address = UINT64_C(0x55434d454d50524f);
	probe_sink ^= *address;
	empty_samples = malloc(PROBE_SAMPLES * sizeof(*empty_samples));
	warm_samples = malloc(PROBE_SAMPLES * sizeof(*warm_samples));
	cold_samples = malloc(PROBE_SAMPLES * sizeof(*cold_samples));
	if (!empty_samples || !warm_samples || !cold_samples) {
		errno = ENOMEM;
		free(empty_samples);
		free(warm_samples);
		free(cold_samples);
		return -1;
	}

	for (i = 0; i < PROBE_SAMPLES; i++) {
		begin = tsc_begin();
		end = tsc_end();
		empty_samples[i] = end - begin;

		value = *address;
		probe_sink ^= value;
		begin = tsc_begin();
		value = *address;
		end = tsc_end();
		warm_samples[i] = end - begin;
		probe_sink ^= value;

		flush_line(address);
		begin = tsc_begin();
		value = *address;
		end = tsc_end();
		cold_samples[i] = end - begin;
		probe_sink ^= value;
	}
	qsort(empty_samples, PROBE_SAMPLES, sizeof(*empty_samples), compare_u64);
	qsort(warm_samples, PROBE_SAMPLES, sizeof(*warm_samples), compare_u64);
	qsort(cold_samples, PROBE_SAMPLES, sizeof(*cold_samples), compare_u64);
	result->empty_p50 = percentile(empty_samples, PROBE_SAMPLES, 1, 2);
	result->warm_p50 = percentile(warm_samples, PROBE_SAMPLES, 1, 2);
	result->warm_p99 = percentile(warm_samples, PROBE_SAMPLES, 99, 100);
	result->cold_p50 = percentile(cold_samples, PROBE_SAMPLES, 1, 2);
	result->cold_p99 = percentile(cold_samples, PROBE_SAMPLES, 99, 100);
	result->warm_adjusted = result->warm_p50 > result->empty_p50 ?
		result->warm_p50 - result->empty_p50 : 0;
	result->cold_adjusted = result->cold_p50 > result->empty_p50 ?
		result->cold_p50 - result->empty_p50 : 0;

	/* A WB mapping has a cheap warm hit and a slower post-CLFLUSH load. */
	if (result->cold_adjusted >= result->warm_adjusted + 80 &&
	    result->cold_adjusted >=
		2 * (result->warm_adjusted ? result->warm_adjusted : 1))
		result->classification = "effective-wb";
	else if (result->warm_adjusted >= 80 &&
		 result->cold_adjusted <= result->warm_adjusted +
			result->warm_adjusted / 2 + 20)
		result->classification = "effective-uc";
	else
		result->classification = "inconclusive";

	free(empty_samples);
	free(warm_samples);
	free(cold_samples);
	return 0;
}

static void print_probe_cycles(const char *marker,
			       const struct probe_result *result)
{
	printf("%s empty_p50=%llu warm_p50=%llu warm_p99=%llu "
	       "cold_p50=%llu cold_p99=%llu warm_adjusted=%llu "
	       "cold_adjusted=%llu checksum=%llu\n", marker,
	       (unsigned long long)result->empty_p50,
	       (unsigned long long)result->warm_p50,
	       (unsigned long long)result->warm_p99,
	       (unsigned long long)result->cold_p50,
	       (unsigned long long)result->cold_p99,
	       (unsigned long long)result->warm_adjusted,
	       (unsigned long long)result->cold_adjusted,
	       (unsigned long long)probe_sink);
}

int main(void)
{
	struct lrpc_handle handle;
	struct ub_lrpc_info info;
	struct probe_result remote_result;
	struct probe_result astack_result;
	void *remote_bench = MAP_FAILED;
	volatile uint64_t *remote_address;
	volatile uint64_t *astack_address;
	int exit_code = 0;

	if (pin_to_cpu_zero()) {
		fprintf(stderr, "bar_cache_probe: sched_setaffinity: %s\n",
			strerror(errno));
		return 1;
	}
	if (lrpc_bind(&handle, "/dev/ub_lrpc0", 1, 1)) {
		fprintf(stderr, "bar_cache_probe: bind: %s\n", strerror(errno));
		return 1;
	}
	memset(&info, 0, sizeof(info));
	if (ioctl(handle.fd, UB_LRPC_IOC_INFO, &info)) {
		fprintf(stderr, "bar_cache_probe: info: %s\n", strerror(errno));
		exit_code = 1;
		goto close_handle;
	}
	remote_bench = mmap(NULL, UB_LRPC_REMOTE_BENCH_SLOT_SIZE,
			    PROT_READ | PROT_WRITE, MAP_SHARED, handle.fd,
			    UB_LRPC_REMOTE_BENCH_OFFSET);
	if (remote_bench == MAP_FAILED) {
		fprintf(stderr, "bar_cache_probe: remote benchmark mmap: %s\n",
			strerror(errno));
		exit_code = 1;
		goto close_handle;
	}
	remote_address = (volatile uint64_t *)((volatile uint8_t *)remote_bench +
						PROBE_OFFSET);
	astack_address = (volatile uint64_t *)((volatile uint8_t *)handle.astack_map +
						PROBE_OFFSET);
	if (measure_mapping(remote_address, &remote_result) ||
	    measure_mapping(astack_address, &astack_result)) {
		perror("bar_cache_probe: measure_mapping");
		exit_code = 1;
		goto unmap_remote;
	}

	printf("BAR_CACHE_PROBE requested=%s classification=%s cpu=%d samples=%u\n",
	       info.cache_mode == UB_LRPC_CACHE_NONCACHED ? "noncached" : "cached",
	       remote_result.classification, sched_getcpu(), PROBE_SAMPLES);
	print_probe_cycles("BAR_CACHE_PROBE_CYCLES", &remote_result);
	if ((info.cache_mode == UB_LRPC_CACHE_NONCACHED &&
	     strcmp(remote_result.classification, "effective-uc") == 0) ||
	    (info.cache_mode == UB_LRPC_CACHE_CACHED &&
	     strcmp(remote_result.classification, "effective-wb") == 0)) {
		puts("BAR_CACHE_PROBE_PASS");
	} else {
		fprintf(stderr,
			"BAR_CACHE_PROBE_MISMATCH requested=%s classification=%s\n",
			info.cache_mode == UB_LRPC_CACHE_NONCACHED ?
				"noncached" : "cached",
			remote_result.classification);
		exit_code = 2;
	}

	printf("ASTACK_CACHE_PROBE requested=writeback classification=%s "
	       "cpu=%d samples=%u\n", astack_result.classification,
	       sched_getcpu(), PROBE_SAMPLES);
	print_probe_cycles("ASTACK_CACHE_PROBE_CYCLES", &astack_result);
	if (!strcmp(astack_result.classification, "effective-wb")) {
		puts("ASTACK_CACHE_PROBE_PASS");
	} else {
		fprintf(stderr,
			"ASTACK_CACHE_PROBE_MISMATCH requested=writeback "
			"classification=%s\n", astack_result.classification);
		exit_code = 2;
	}

unmap_remote:
	munmap(remote_bench, UB_LRPC_REMOTE_BENCH_SLOT_SIZE);

close_handle:
	lrpc_close(&handle);
	return exit_code;
}
