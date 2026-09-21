#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lrpc/lrpc.h>
#include <lrpc/lrpc_ubsm.h>

static void request_stop(int signo)
{
	(void)signo;
}

int main(int argc, char **argv)
{
	const char *device = "/dev/ub_lrpc_ctl";
	const char *name = LRPC_UBSM_DEFAULT_NAME;
	struct lrpc_ubsm_region region;
	struct lrpc_ubsm_proc_desc proc;
	struct lrpc_ubsm_local_code local_code;
	volatile uint64_t *remote_service_value;
	struct sigaction action;
	int serve_errno = 0;
	int ret;

	memset(&local_code, 0, sizeof(local_code));

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
	if (!ret)
		ret = lrpc_ubsm_localize_code(&region, &proc, &local_code);
	if (ret || !remote_service_value || !local_code.entry) {
		fprintf(stderr, "UB_LRPC_SHADOW_FAIL procedure metadata error=%d\n",
			ret);
		lrpc_ubsm_release_local_code(&local_code);
		(void)lrpc_ubsm_close(&region, 0);
		return 1;
	}
	printf("UB_LRPC_CODE_LOCALIZED proc=1 remote_offset=%llu bytes=%llu "
	       "hash=0x%llx local_entry=%p permissions=rx remote_value=%llu\n",
	       (unsigned long long)proc.code_offset,
	       (unsigned long long)proc.code_size,
	       (unsigned long long)proc.code_hash, local_code.entry,
	       (unsigned long long)*remote_service_value);
	fflush(stdout);
	memset(&action, 0, sizeof(action));
	action.sa_handler = request_stop;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) ||
	    sigaction(SIGTERM, &action, NULL)) {
		lrpc_ubsm_release_local_code(&local_code);
		perror("sigaction");
		(void)lrpc_ubsm_close(&region, 0);
		return 1;
	}
	ret = lrpc_shadow_serve_with_data(
		device, 1, proc.code_epoch,
		(lrpc_shadow_handler_fn)local_code.entry,
		(void *)(uintptr_t)remote_service_value);
	serve_errno = errno;
	if (ret && serve_errno != EINTR)
		fprintf(stderr, "UB_LRPC_SHADOW_STOP device=%s error=%s\n",
			device, strerror(serve_errno));
	lrpc_ubsm_release_local_code(&local_code);
	if (lrpc_ubsm_close(&region, 0))
		return 1;
	return ret && serve_errno != EINTR ? 1 : 0;
}
