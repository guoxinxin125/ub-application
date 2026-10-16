#ifndef ERPC_LRPC_UBSM_H
#define ERPC_LRPC_UBSM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LRPC_UBSM_DEFAULT_NAME "lrpc_ub_demo"
#define LRPC_UBSM_REGION_SIZE (4UL * 1024UL * 1024UL)
#define LRPC_UBSM_DATA_OFFSET (64UL * 1024UL)
#define LRPC_UBSM_MAGIC UINT64_C(0x55424c5250434d45)
#define LRPC_UBSM_ABI_VERSION 3U
#define LRPC_UBSM_MAX_PROCS 16U
#define LRPC_UBSM_SERVICE_SLOT_SIZE (64UL * 1024UL)
#define LRPC_UBSM_CODE_OFFSET \
	(LRPC_UBSM_DATA_OFFSET + \
	 LRPC_UBSM_MAX_PROCS * LRPC_UBSM_SERVICE_SLOT_SIZE)
#define LRPC_UBSM_CODE_SLOT_SIZE (64UL * 1024UL)
#define LRPC_UBSM_PROC_LOCAL_CODE (1U << 0)
#define LRPC_UBSM_PROC_PUBLISHED_CODE (1U << 1)

struct lrpc_ubsm_proc_desc {
	uint32_t procedure_id;
	uint32_t flags;
	uint64_t data_offset;
	uint64_t data_size;
	uint64_t code_offset;
	uint64_t code_size;
	uint64_t code_entry_offset;
	uint64_t code_hash;
	uint64_t code_epoch;
};

struct lrpc_ubsm_service {
	uint32_t procedure_id;
	uint32_t flags;
	const void *code;
	size_t code_size;
	size_t code_entry_offset;
};

struct lrpc_ubsm_metadata {
	uint64_t magic;
	uint64_t epoch;
	uint64_t region_size;
	uint32_t abi_version;
	uint32_t num_procs;
	uint32_t ready;
	uint32_t reserved;
	struct lrpc_ubsm_proc_desc proc[LRPC_UBSM_MAX_PROCS];
};

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
};

struct lrpc_ubsm_local_code {
	void *mapping;
	size_t mapping_size;
	void *entry;
};

int lrpc_ubsm_owner_open(struct lrpc_ubsm_region *region, const char *name,
			 size_t size, const struct lrpc_ubsm_provider *provider);
int lrpc_ubsm_remote_open(struct lrpc_ubsm_region *region, const char *name,
			  size_t size);
int lrpc_ubsm_publish(struct lrpc_ubsm_region *region, uint64_t epoch,
		      const uint32_t *procedure_ids, size_t count);
int lrpc_ubsm_publish_services(struct lrpc_ubsm_region *region,
			       uint64_t epoch,
			       const struct lrpc_ubsm_service *services,
			       size_t count);
int lrpc_ubsm_lookup_procedure(struct lrpc_ubsm_region *region,
			       uint32_t procedure_id, uint64_t expected_epoch,
			       struct lrpc_ubsm_proc_desc *result);
int lrpc_ubsm_localize_code(struct lrpc_ubsm_region *region,
			     const struct lrpc_ubsm_proc_desc *procedure,
			     struct lrpc_ubsm_local_code *result);
void lrpc_ubsm_release_local_code(struct lrpc_ubsm_local_code *code);
void *lrpc_ubsm_at(struct lrpc_ubsm_region *region, size_t offset,
		   size_t length);
int lrpc_ubsm_close(struct lrpc_ubsm_region *region, int deallocate);

#ifdef __cplusplus
}
#endif

#endif
