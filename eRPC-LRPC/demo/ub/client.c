#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lrpc/lrpc.h>

int main(int argc, char **argv)
{
	const char *device = argc > 1 ? argv[1] : "/dev/ub_lrpc_ctl";
	struct lrpc_handle handle;
	struct lrpc_astack call;
	uint64_t inputs[2] = {20, 22};
	uint64_t result = 0;
	int ret;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [CONTROL_DEVICE]\n", argv[0]);
		return 2;
	}
	if (lrpc_bind(&handle, device, 1, 1)) {
		fprintf(stderr, "UB_LRPC_CLIENT_FAIL bind %s: %s\n",
			device, strerror(errno));
		return 1;
	}
	memset(&call, 0, sizeof(call));
	call.abi = LRPC_ASTACK_ABI;
	call.procedure_id = 1;
	call.request_size = sizeof(inputs);
	memcpy(call.payload, inputs, sizeof(inputs));
	ret = lrpc_invoke(&handle, &call);
	if (!ret && !call.status && call.response_size == sizeof(result))
		memcpy(&result, call.payload, sizeof(result));
	printf("UB_LRPC_RESULT value=%llu rc=%d status=%llu cpu=%llu/%llu/%llu "
	       "caller_pid=%llu shadow_pid=%llu\n",
	       (unsigned long long)result, ret,
	       (unsigned long long)call.status,
	       (unsigned long long)call.caller_cpu_before,
	       (unsigned long long)call.caller_cpu_in_service,
	       (unsigned long long)call.caller_cpu_after,
	       (unsigned long long)call.caller_pid,
	       (unsigned long long)call.shadow_pid);
	lrpc_close(&handle);
	if (ret || call.status || result != 142 ||
	    call.caller_pid == call.shadow_pid ||
	    call.caller_cpu_before != call.caller_cpu_in_service ||
	    call.caller_cpu_before != call.caller_cpu_after) {
		puts("UB_LRPC_FAIL");
		return 1;
	}
	puts("UB_LRPC_PASS");
	return 0;
}
