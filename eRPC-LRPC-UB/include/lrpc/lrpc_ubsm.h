#ifndef ERPC_LRPC_UBSM_H
#define ERPC_LRPC_UBSM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Region lifetime only; domain publication uses ub_lrpc_ub_domain.h. */
#define LRPC_UBSM_REGION_SIZE (4UL * 1024UL * 1024UL)

struct lrpc_ubsm_provider {
	const char *host_name;
	uint32_t socket_id;
	uint32_t numa_id;
	uint32_t port_id;
};

struct lrpc_ubsm_region {
	void *base;
	size_t size;
	char name[49];
	int initialized;
	int mapped;
	int owner;
	/* Process-local shared import reference, never a cross-machine pointer. */
	void *shared_mapping;
};
int lrpc_ubsm_owner_open(struct lrpc_ubsm_region *region, const char *name,
			 size_t size, const struct lrpc_ubsm_provider *provider);
int lrpc_ubsm_remote_open(struct lrpc_ubsm_region *region, const char *name,
			  size_t size);
int lrpc_ubsm_close(struct lrpc_ubsm_region *region, int deallocate);

#ifdef __cplusplus
}
#endif

#endif
