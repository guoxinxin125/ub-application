#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lrpc/lrpc.h>
#include <lrpc/lrpc_ubsm.h>

static struct lrpc_handle downstream;
static volatile uint64_t *leaf_service_value;

static long leaf_handler(struct lrpc_astack *call)
{
	uint64_t value;

	if (!call || call->request_size != sizeof(value) ||
	    __atomic_load_n(leaf_service_value, __ATOMIC_ACQUIRE) != 100)
		return -EINVAL;
	memcpy(&value, call->payload, sizeof(value));
	value++;
	memcpy(call->payload, &value, sizeof(value));
	memcpy(call->payload + sizeof(value), &call->shadow_pid,
	       sizeof(call->shadow_pid));
	call->service_data = (uint64_t)(uintptr_t)leaf_service_value;
	call->response_size = 2 * sizeof(value);
	call->status = 0;
	return 0;
}

static long middle_handler(struct lrpc_astack *call)
{
	struct lrpc_astack nested;
	uint64_t value;
	int rc;

	if (!call || call->request_size != sizeof(value))
		return -EINVAL;
	memset(&nested, 0, offsetof(struct lrpc_astack, payload));
	nested.abi = LRPC_ASTACK_ABI;
	nested.procedure_id = 3;
	nested.request_size = sizeof(value);
	memcpy(nested.payload, call->payload, sizeof(value));
	rc = lrpc_invoke(&downstream, &nested);
	if (rc || nested.status || nested.response_size != 2 * sizeof(value))
		return rc ? rc : -EIO;
	memcpy(&value, nested.payload, sizeof(value));
	value++;
	memcpy(call->payload, &value, sizeof(value));
	memcpy(call->payload + sizeof(value),
	       nested.payload + sizeof(value), sizeof(value));
	call->response_size = 2 * sizeof(value);
	call->status = 0;
	return 0;
}

int main(int argc, char **argv)
{
	const char *name = LRPC_UBSM_DEFAULT_NAME;
	const char *device = "/dev/ub_lrpc_ctl";
	struct lrpc_ubsm_region region;
	struct lrpc_ubsm_proc_desc proc;
	uint32_t procedure_id;
	lrpc_shadow_handler_fn handler;
	int is_leaf;
	int ret;

	if (argc < 2 || argc > 4 ||
	    (strcmp(argv[1], "middle") && strcmp(argv[1], "leaf"))) {
		fprintf(stderr,
			"usage: %s middle|leaf [SHM_NAME] [CONTROL_DEVICE]\n",
			argv[0]);
		return 2;
	}
	is_leaf = !strcmp(argv[1], "leaf");
	if (argc > 2)
		name = argv[2];
	if (argc > 3)
		device = argv[3];
	procedure_id = is_leaf ? 3 : 2;
	handler = is_leaf ? leaf_handler : middle_handler;

	ret = lrpc_ubsm_remote_open(&region, name, LRPC_UBSM_REGION_SIZE);
	if (ret) {
		fprintf(stderr, "UB_LRPC_NESTED_SHADOW_FAIL import error=%d\n",
			ret);
		return 1;
	}
	ret = lrpc_ubsm_lookup_procedure(&region, procedure_id, 1, &proc);
	if (ret) {
		fprintf(stderr,
			"UB_LRPC_NESTED_SHADOW_FAIL proc=%u metadata error=%d\n",
			procedure_id, ret);
		(void)lrpc_ubsm_close(&region, 0);
		return 1;
	}
	if (is_leaf) {
		leaf_service_value = lrpc_ubsm_at(
			&region, proc.data_offset, sizeof(*leaf_service_value));
		if (!leaf_service_value) {
			(void)lrpc_ubsm_close(&region, 0);
			return 1;
		}
	} else if (lrpc_bind(&downstream, device, 3, proc.code_epoch)) {
		fprintf(stderr, "UB_LRPC_NESTED_SHADOW_FAIL bind leaf: %s\n",
			strerror(errno));
		(void)lrpc_ubsm_close(&region, 0);
		return 1;
	}
	printf("UB_LRPC_NESTED_SHADOW_READY role=%s proc=%u epoch=%llu\n",
	       argv[1], procedure_id, (unsigned long long)proc.code_epoch);
	fflush(stdout);
	ret = lrpc_shadow_serve(device, procedure_id, proc.code_epoch, handler);
	if (!is_leaf)
		lrpc_close(&downstream);
	leaf_service_value = NULL;
	if (lrpc_ubsm_close(&region, 0) && !ret)
		ret = -1;
	return ret ? 1 : 0;
}
