#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

#include <lrpc/lrpc.h>

#define CACHE_LINE_SIZE 64U
#define SAMPLE_COUNT 21U
#define LOADS_PER_SAMPLE 100000U

static uint64_t now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts)) {
		perror("clock_gettime");
		exit(1);
	}
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static uint32_t random_next(uint32_t *state)
{
	uint32_t value = *state;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*state = value;
	return value;
}

static void print_pat_entries(const struct ub_lrpc_info *info)
{
	FILE *file = fopen("/sys/kernel/debug/x86/pat_memtype_list", "r");
	char line[256];

	if (!file) {
		printf("BAR_PAT unavailable errno=%d\n", errno);
		return;
	}
	while (fgets(line, sizeof(line), file)) {
		unsigned long long start, end;
		char type[64];

		if (sscanf(line, "PAT: [mem 0x%llx-0x%llx] %63s",
			   &start, &end, type) == 3 &&
		    start < info->bar_start + info->bar_size &&
		    end > info->bar_start)
			printf("BAR_PAT %s", line);
	}
	fclose(file);
}

int main(void)
{
	struct lrpc_handle handle;
	struct ub_lrpc_info info;
	volatile uint8_t *bar;
	uint32_t *order;
	uint64_t samples[SAMPLE_COUNT];
	uint64_t total_ns = 0;
	uint32_t state = 0x4c525043U;
	uint32_t index;
	size_t lines = UB_LRPC_ASTACK_SLOT_SIZE / CACHE_LINE_SIZE;

	if (lrpc_bind(&handle, "/dev/ub_lrpc0", 1, 1)) {
		fprintf(stderr, "bar_latency: bind: %s\n", strerror(errno));
		return 1;
	}
	memset(&info, 0, sizeof(info));
	if (ioctl(handle.fd, UB_LRPC_IOC_INFO, &info)) {
		fprintf(stderr, "bar_latency: info: %s\n", strerror(errno));
		lrpc_close(&handle);
		return 1;
	}
	order = malloc(lines * sizeof(*order));
	if (!order) {
		perror("bar_latency: malloc");
		lrpc_close(&handle);
		return 1;
	}
	for (size_t i = 0; i < lines; i++)
		order[i] = (uint32_t)i;
	for (size_t i = lines - 1; i > 0; i--) {
		size_t other = random_next(&state) % (i + 1);
		uint32_t tmp = order[i];
		order[i] = order[other];
		order[other] = tmp;
	}

	bar = handle.astack_map;
	for (size_t i = 0; i < lines; i++) {
		volatile uint32_t *entry = (volatile uint32_t *)(
			bar + (size_t)order[i] * CACHE_LINE_SIZE);
		*entry = order[(i + 1) % lines];
	}
	__sync_synchronize();
	index = order[0];
	for (size_t i = 0; i < lines; i++)
		index = *(volatile uint32_t *)(bar + (size_t)index * CACHE_LINE_SIZE);

	print_pat_entries(&info);
	for (size_t sample = 0; sample < SAMPLE_COUNT; sample++) {
		uint64_t begin = now_ns();

		for (size_t load = 0; load < LOADS_PER_SAMPLE; load++)
			index = *(volatile uint32_t *)(
				bar + (size_t)index * CACHE_LINE_SIZE);
		samples[sample] = now_ns() - begin;
		total_ns += samples[sample];
	}
	qsort(samples, SAMPLE_COUNT, sizeof(samples[0]), compare_u64);
	printf("BAR_LOAD_LATENCY mode=%s bar_start=0x%llx working_set=%zu "
	       "samples=%u loads_per_sample=%u min_ns=%.2f p50_ns=%.2f "
	       "p99_ns=%.2f avg_ns=%.2f checksum=%u\n",
	       info.cache_mode == UB_LRPC_CACHE_NONCACHED ? "noncached" : "cached",
	       (unsigned long long)info.bar_start, lines * CACHE_LINE_SIZE,
	       SAMPLE_COUNT, LOADS_PER_SAMPLE,
	       (double)samples[0] / LOADS_PER_SAMPLE,
	       (double)samples[SAMPLE_COUNT / 2] / LOADS_PER_SAMPLE,
	       (double)samples[SAMPLE_COUNT - 1] / LOADS_PER_SAMPLE,
	       (double)total_ns / (SAMPLE_COUNT * LOADS_PER_SAMPLE), index);
	puts("BAR_LOAD_LATENCY_PASS");
	free(order);
	lrpc_close(&handle);
	return 0;
}
