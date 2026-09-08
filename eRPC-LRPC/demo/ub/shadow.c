#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lrpc/lrpc.h>
#include <lrpc/lrpc_ubsm.h>

static volatile uint64_t *remote_service_value;

static void request_stop(int signo)
{
	(void)signo;
}

static long ub_add_service(struct lrpc_astack *call)
{
	uint64_t lhs;
	uint64_t rhs;
	uint64_t result;

	if (!call || call->request_size != 2 * sizeof(uint64_t)) {
		if (call)
			call->status = EINVAL;
		return -EINVAL;
	}
	memcpy(&lhs, call->payload, sizeof(lhs));
	memcpy(&rhs, call->payload + sizeof(lhs), sizeof(rhs));
	result = __atomic_load_n(remote_service_value, __ATOMIC_ACQUIRE) +
		 lhs + rhs;
	memcpy(call->payload, &result, sizeof(result));
	call->service_data = (uint64_t)(uintptr_t)remote_service_value;
	call->response_size = sizeof(result);
	call->status = 0;
	return 0;
}

int main(int argc, char **argv)
{
	const char *device = "/dev/ub_lrpc_ctl";
	const char *name = LRPC_UBSM_DEFAULT_NAME;
	struct lrpc_ubsm_region region;
	struct lrpc_ubsm_proc_desc proc;
	struct sigaction action;
	int serve_errno = 0;
	int ret;

	if (argc > 1)
		name = argv[1];
	if (argc > 2)
		device = argv[2];
	if (argc > 3) {
		fprintf(stderr, "usage: %s [SHM_NAME] [CONTROL_DEVICE]\n", argv[0]);
		return 2;
	}
	ret = lrpc_ubsm_remote_open(&region, name, LRPC_UBSM_REGION_SIZE);
	if (ret) {
		fprintf(stderr, "UB_LRPC_SHADOW_FAIL import error=%d\n", ret);
		return 1;
	}
	ret = lrpc_ubsm_lookup_procedure(&region, 1, 1, &proc);
	remote_service_value = ret ? NULL : lrpc_ubsm_at(
		&region, proc.data_offset, sizeof(*remote_service_value));
	if (ret || !remote_service_value) {
		fprintf(stderr, "UB_LRPC_SHADOW_FAIL procedure metadata error=%d\n",
			ret);
		(void)lrpc_ubsm_close(&region, 0);
		return 1;
	}
	printf("UB_LRPC_SHADOW_IMPORTED name=%s remote_value=%llu\n", name,
	       (unsigned long long)*remote_service_value);
	fflush(stdout);
	memset(&action, 0, sizeof(action));
	action.sa_handler = request_stop;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) ||
	    sigaction(SIGTERM, &action, NULL)) {
		perror("sigaction");
		(void)lrpc_ubsm_close(&region, 0);
		return 1;
	}
	ret = lrpc_shadow_serve(device, 1, proc.code_epoch, ub_add_service);
	serve_errno = errno;
	if (ret && serve_errno != EINTR)
		fprintf(stderr, "UB_LRPC_SHADOW_STOP device=%s error=%s\n",
			device, strerror(serve_errno));
	remote_service_value = NULL;
	if (lrpc_ubsm_close(&region, 0))
		return 1;
	return ret && serve_errno != EINTR ? 1 : 0;
}
