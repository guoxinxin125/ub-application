#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <lrpc/lrpc.h>

static struct lrpc_handle downstream;

static long leaf_handler(struct lrpc_astack *call)
{
	uint64_t value;

	if (call->request_size != sizeof(value))
		return -EINVAL;
	memcpy(&value, call->payload, sizeof(value));
	value++;
	memcpy(call->payload, &value, sizeof(value));
	memcpy(call->payload + sizeof(value), &call->shadow_pid,
	       sizeof(call->shadow_pid));
	call->response_size = 2 * sizeof(value);
	call->status = 0;
	return 0;
}

static long middle_handler(struct lrpc_astack *call)
{
	struct lrpc_astack nested;
	uint64_t value;
	int rc;

	if (call->request_size != sizeof(value))
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
	const char *dev = "/dev/ub_lrpc0";

	if (argc != 2 || (strcmp(argv[1], "middle") && strcmp(argv[1], "leaf"))) {
		fprintf(stderr, "usage: %s middle|leaf\n", argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "leaf"))
		return lrpc_shadow_serve(dev, 3, 1, leaf_handler) ? 1 : 0;
	if (lrpc_bind(&downstream, dev, 3, 1)) {
		fprintf(stderr, "nested middle: bind leaf: %s\n", strerror(errno));
		return 1;
	}
	return lrpc_shadow_serve(dev, 2, 1, middle_handler) ? 1 : 0;
}
