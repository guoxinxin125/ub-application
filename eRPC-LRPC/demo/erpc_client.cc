#include <errno.h>
#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "erpc_compat.h"
#include "remote_app_bench.h"

#ifdef LRPC_BACKEND_UBSM
#define LRPC_DEFAULT_DEVICE "/dev/ub_lrpc_ctl"
#define ERPC_RESULT_MARKER "ERPC_UB_LRPC_RESULT"
#define ERPC_PASS_MARKER "ERPC_UB_LRPC_PASS"
#define BREAKDOWN_MARKER "UB_LRPC_BREAKDOWN_AVG"
#else
#define LRPC_DEFAULT_DEVICE "/dev/ub_lrpc0"
#define ERPC_RESULT_MARKER "ERPC_LRPC_RESULT"
#define ERPC_PASS_MARKER "ERPC_LRPC_PASS"
#define BREAKDOWN_MARKER "LRPC_BREAKDOWN_AVG"
#endif

static bool done;
static void continuation(void *, void *) { done = true; }

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : LRPC_DEFAULT_DEVICE;
	erpc::Rpc rpc(dev);
	uint64_t request[2] = {20, 22};
	uint64_t response = 0;
	erpc::MsgBuffer req{reinterpret_cast<uint8_t *>(request), sizeof(request)};
	erpc::MsgBuffer resp{reinterpret_cast<uint8_t *>(&response), 0};

	if (!rpc.ok()) {
		fprintf(stderr, "client: bind %s: %s\n", dev, strerror(errno));
		return 1;
	}
#ifndef LRPC_BACKEND_UBSM
	void *bad = mmap(nullptr, UB_LRPC_CODE_SIZE,
			 PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED,
			 rpc.native_handle_for_test(), UB_LRPC_CODE_OFFSET);
	if (bad != MAP_FAILED) {
		fprintf(stderr, "client: kernel allowed a W+X service mapping\n");
		munmap(bad, UB_LRPC_CODE_SIZE);
		return 3;
	}
	puts("WX_POLICY_PASS");
	bad = mmap(nullptr, UB_LRPC_CODE_SIZE, PROT_READ | PROT_EXEC, MAP_SHARED,
		   rpc.native_handle_for_test(), UB_LRPC_CODE_OFFSET);
	if (bad != MAP_FAILED) {
		fprintf(stderr, "client: caller mapped shadow-only service code\n");
		munmap(bad, UB_LRPC_CODE_SIZE);
		return 4;
	}
	bad = mmap(nullptr, 4096, PROT_READ, MAP_SHARED,
		   rpc.native_handle_for_test(), UB_LRPC_DATA_OFFSET);
	if (bad != MAP_FAILED) {
		fprintf(stderr, "client: caller mapped shadow-only service data\n");
		munmap(bad, 4096);
		return 5;
	}
	puts("CALLER_ISOLATION_PASS");
#endif
	uint64_t first_begin = ra_bench_clock(CLOCK_MONOTONIC_RAW);
	rpc.enqueue_request(0, 1, &req, &resp, continuation, nullptr);
	uint64_t first_elapsed = ra_bench_clock(CLOCK_MONOTONIC_RAW) - first_begin;

	printf(ERPC_RESULT_MARKER " value=%llu rc=%d bytes=%zu "
	       "cpu=%llu/%llu/%llu done=%d caller_pid=%llu shadow_pid=%llu\n",
	       (unsigned long long)response, rpc.last_rc(), resp.data_size_,
	       (unsigned long long)rpc.last_cpu_before(),
	       (unsigned long long)rpc.last_cpu_service(),
	       (unsigned long long)rpc.last_cpu_after(), done,
	       (unsigned long long)rpc.last_caller_pid(),
	       (unsigned long long)rpc.last_shadow_pid());
	if (!done || rpc.last_rc() || response != 142 || resp.data_size_ != 8 ||
	    rpc.last_cpu_before() != rpc.last_cpu_service() ||
	    rpc.last_cpu_before() != rpc.last_cpu_after() ||
	    rpc.last_caller_pid() == rpc.last_shadow_pid())
		return 2;
	ra_bench_first_call("direct", first_elapsed);
	puts(ERPC_PASS_MARKER);

	constexpr size_t samples = 1000;
	uint64_t total_elapsed = 0, total_outside_kernel = 0;
	uint64_t total_dispatch = 0, total_service = 0, total_resume = 0;
	struct ra_bench_warm warm;
	struct ra_bench_audit audit;
	memset(&audit, 0, sizeof(audit));
	const char *backend = getenv("LRPC_EXECUTION_BACKEND");
	bool audit_enabled = backend && !strcmp(backend, "remote-domain");
	ra_bench_warm_begin(&warm, "direct");
	while (ra_bench_warm_more(&warm)) {
		done = false;
		resp.data_size_ = 0;
		rpc.enqueue_request(0, 1, &req, &resp, continuation, nullptr);
		if (!done || rpc.last_rc() || response != 142 || resp.data_size_ != 8) return 7;
	}
	for (size_t i = 0; i < 100 + samples; i++) {
		if (i == 100 && audit_enabled) ra_bench_audit_begin(&audit);
		done = false;
		resp.data_size_ = 0;
		auto begin = std::chrono::steady_clock::now();
		rpc.enqueue_request(0, 1, &req, &resp, continuation, nullptr);
		auto end = std::chrono::steady_clock::now();
		if (!done || rpc.last_rc() || response != 142)
			return 6;
		if (i < 100)
			continue;
		uint64_t user_ns = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				end - begin).count());
		uint64_t dispatch = rpc.shadow_dispatch_ns() - rpc.call_enter_ns();
		uint64_t service = rpc.shadow_return_ns() - rpc.shadow_dispatch_ns();
		uint64_t resume = rpc.caller_resume_ns() - rpc.shadow_return_ns();
		uint64_t kernel_ns = rpc.caller_resume_ns() - rpc.call_enter_ns();
		/* The four kernel timestamps do not cover request preparation or
		 * shared A-stack accesses immediately before and after ioctl(CALL). */
		total_elapsed += user_ns;
		total_outside_kernel += user_ns > kernel_ns ? user_ns - kernel_ns : 0;
		total_dispatch += dispatch;
		total_service += service;
		total_resume += resume;
	}
	if (audit_enabled) ra_bench_audit_end(&audit, "direct", "baseline");
	if (backend && !strcmp(backend, "ub-remote-domain")) {
		printf("UB_DOMAIN_ERPC_API_LATENCY samples=%zu avg_ns=%llu breakdown=unavailable\n",
		       samples, (unsigned long long)(total_elapsed / samples));
		return 0;
	}
	printf(BREAKDOWN_MARKER " samples=%zu total_ns=%llu "
	       "outside_kernel_timestamps_ns=%llu "
	       "call_to_shadow_ns=%llu shadow_user_service_ns=%llu "
	       "return_to_caller_ns=%llu\n", samples,
	       (unsigned long long)(total_elapsed / samples),
	       (unsigned long long)(total_outside_kernel / samples),
	       (unsigned long long)(total_dispatch / samples),
	       (unsigned long long)(total_service / samples),
	       (unsigned long long)(total_resume / samples));
	return 0;
}
