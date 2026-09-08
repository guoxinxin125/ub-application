#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lrpc/lrpc_ubsm.h>

static volatile sig_atomic_t stop_requested;

static void request_stop(int signo)
{
	(void)signo;
	stop_requested = 1;
}

int main(int argc, char **argv)
{
	const char *provider_host;
	const char *name = LRPC_UBSM_DEFAULT_NAME;
	struct lrpc_ubsm_provider provider;
	struct lrpc_ubsm_region region;
	struct lrpc_ubsm_metadata *meta;
	const uint32_t procedures[] = {1, 2, 3, 4};
	int ret;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s PROVIDER_HOST [SHM_NAME]\n", argv[0]);
		return 2;
	}
	provider_host = argv[1];
	if (argc == 3)
		name = argv[2];
	memset(&provider, 0, sizeof(provider));
	provider.host_name = provider_host;
	provider.socket_id = UINT32_MAX;
	provider.numa_id = UINT32_MAX;
	provider.port_id = UINT32_MAX;

	ret = lrpc_ubsm_owner_open(&region, name, LRPC_UBSM_REGION_SIZE,
				   &provider);
	if (ret) {
		fprintf(stderr, "UB_LRPC_OWNER_FAIL open error=%d\n", ret);
		return 1;
	}
	meta = lrpc_ubsm_at(&region, 0, sizeof(*meta));
	if (!meta) {
		fprintf(stderr, "UB_LRPC_OWNER_FAIL invalid layout\n");
		(void)lrpc_ubsm_close(&region, 1);
		return 1;
	}
	for (size_t i = 0; i < sizeof(procedures) / sizeof(procedures[0]); i++) {
		volatile uint64_t *service_value = lrpc_ubsm_at(
			&region, LRPC_UBSM_DATA_OFFSET +
				 (size_t)(procedures[i] - 1) *
				 LRPC_UBSM_SERVICE_SLOT_SIZE,
			sizeof(*service_value));
		if (!service_value) {
			fprintf(stderr, "UB_LRPC_OWNER_FAIL invalid service slot\n");
			(void)lrpc_ubsm_close(&region, 1);
			return 1;
		}
		__atomic_store_n(service_value, 100, __ATOMIC_RELAXED);
	}
	ret = lrpc_ubsm_publish(&region, 1, procedures,
				sizeof(procedures) / sizeof(procedures[0]));
	if (ret) {
		fprintf(stderr, "UB_LRPC_OWNER_FAIL publish error=%d\n", ret);
		(void)lrpc_ubsm_close(&region, 1);
		return 1;
	}

	if (signal(SIGINT, request_stop) == SIG_ERR ||
	    signal(SIGTERM, request_stop) == SIG_ERR) {
		perror("signal");
		(void)lrpc_ubsm_close(&region, 1);
		return 1;
	}
	printf("UB_LRPC_OWNER_READY name=%s provider=%s epoch=1 procs=1,2,3,4 "
	       "value=100 pid=%d\n", name, provider_host, getpid());
	printf("UB_LRPC_OWNER_IDLE stop shadow first, then send SIGINT here\n");
	fflush(stdout);
	while (!stop_requested)
		pause();

	__atomic_store_n(&meta->ready, 0, __ATOMIC_RELEASE);
	ret = lrpc_ubsm_close(&region, 1);
	if (ret) {
		fprintf(stderr, "UB_LRPC_OWNER_FAIL cleanup error=%d\n", ret);
		return 1;
	}
	puts("UB_LRPC_OWNER_CLEANUP_PASS");
	return 0;
}
