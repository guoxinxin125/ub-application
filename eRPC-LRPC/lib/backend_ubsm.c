#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <ubs_mem.h>
#include <ubs_mem_def.h>

#include <lrpc/lrpc_ubsm.h>

_Static_assert(sizeof(struct lrpc_ubsm_metadata) <= LRPC_UBSM_DATA_OFFSET,
	       "UBSM metadata overlaps service data");
_Static_assert(LRPC_UBSM_DATA_OFFSET +
	       LRPC_UBSM_MAX_PROCS * LRPC_UBSM_SERVICE_SLOT_SIZE <=
	       LRPC_UBSM_REGION_SIZE,
	       "UBSM procedure slots exceed the shared region");

static int lrpc_ubsm_initialize(struct lrpc_ubsm_region *region)
{
	ubsmem_options_t options = {0};
	int ret;

	ret = ubsmem_set_logger_level(3);
	if (ret != UBSM_OK)
		return ret;
	ret = ubsmem_init_attributes(&options);
	if (ret != UBSM_OK)
		return ret;
	ret = ubsmem_initialize(&options);
	if (ret == UBSM_OK)
		region->initialized = 1;
	return ret;
}

static int lrpc_ubsm_prepare(struct lrpc_ubsm_region *region,
			     const char *name, size_t size)
{
	if (!region || !name || !name[0] ||
	    strlen(name) >= MAX_SHM_NAME_LENGTH ||
	    size < LRPC_UBSM_REGION_SIZE || size % LRPC_UBSM_REGION_SIZE != 0)
		return UBSM_ERR_PARAM_INVALID;
	memset(region, 0, sizeof(*region));
	memcpy(region->name, name, strlen(name) + 1);
	region->size = size;
	return lrpc_ubsm_initialize(region);
}

int lrpc_ubsm_owner_open(struct lrpc_ubsm_region *region, const char *name,
			 size_t size, const struct lrpc_ubsm_provider *provider)
{
	ubs_mem_provider_t location = {0};
	void *base = NULL;
	int ret;

	if (!provider || !provider->host_name || !provider->host_name[0] ||
	    strlen(provider->host_name) >= sizeof(location.host_name))
		return UBSM_ERR_PARAM_INVALID;
	ret = lrpc_ubsm_prepare(region, name, size);
	if (ret != UBSM_OK)
		return ret;
	memcpy(location.host_name, provider->host_name,
	       strlen(provider->host_name) + 1);
	location.socket_id = provider->socket_id;
	location.numa_id = provider->numa_id;
	location.port_id = provider->port_id;
	ret = ubsmem_shmem_allocate_with_provider(
		&location, region->name, region->size,
		S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP,
		UBSM_FLAG_ONLY_IMPORT_NONCACHE | UBSM_FLAG_WR_DELAY_COMP);
	if (ret != UBSM_OK)
		goto fail;
	region->owner = 1;
	ret = ubsmem_shmem_map(NULL, region->size, PROT_READ | PROT_WRITE,
			       MAP_SHARED, region->name, 0, &base);
	if (ret != UBSM_OK)
		goto fail;
	if (!base || base == MAP_FAILED) {
		ret = UBSM_ERR_MEMORY;
		goto fail;
	}
	region->base = base;
	region->mapped = 1;
	return UBSM_OK;

fail:
	(void)lrpc_ubsm_close(region, region->owner);
	return ret;
}

int lrpc_ubsm_remote_open(struct lrpc_ubsm_region *region, const char *name,
			  size_t size)
{
	void *base = NULL;
	int ret = lrpc_ubsm_prepare(region, name, size);

	if (ret != UBSM_OK)
		return ret;
	ret = ubsmem_shmem_map(NULL, region->size, PROT_READ | PROT_WRITE,
			       MAP_SHARED, region->name, 0, &base);
	if (ret != UBSM_OK)
		goto fail;
	if (!base || base == MAP_FAILED) {
		ret = UBSM_ERR_MEMORY;
		goto fail;
	}
	region->base = base;
	region->mapped = 1;
	return UBSM_OK;

fail:
	(void)lrpc_ubsm_close(region, 0);
	return ret;
}

int lrpc_ubsm_publish(struct lrpc_ubsm_region *region, uint64_t epoch,
		      const uint32_t *procedure_ids, size_t count)
{
	struct lrpc_ubsm_metadata *meta;
	size_t i;

	if (!region || !region->owner || !region->mapped || !epoch ||
	    !procedure_ids || !count || count > LRPC_UBSM_MAX_PROCS)
		return -EINVAL;
	meta = lrpc_ubsm_at(region, 0, sizeof(*meta));
	if (!meta)
		return -EINVAL;
	__atomic_store_n(&meta->ready, 0, __ATOMIC_RELEASE);
	memset(meta, 0, sizeof(*meta));
	for (i = 0; i < count; i++) {
		size_t j;
		uint64_t offset;

		if (!procedure_ids[i] ||
		    procedure_ids[i] > LRPC_UBSM_MAX_PROCS)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (procedure_ids[j] == procedure_ids[i])
				return -EINVAL;
		offset = LRPC_UBSM_DATA_OFFSET +
			 (uint64_t)(procedure_ids[i] - 1) *
			 LRPC_UBSM_SERVICE_SLOT_SIZE;
		if (offset > region->size ||
		    LRPC_UBSM_SERVICE_SLOT_SIZE > region->size - offset)
			return -EINVAL;
		meta->proc[i].procedure_id = procedure_ids[i];
		meta->proc[i].flags = LRPC_UBSM_PROC_LOCAL_CODE;
		meta->proc[i].data_offset = offset;
		meta->proc[i].data_size = LRPC_UBSM_SERVICE_SLOT_SIZE;
		meta->proc[i].code_epoch = epoch;
	}
	meta->magic = LRPC_UBSM_MAGIC;
	meta->epoch = epoch;
	meta->region_size = region->size;
	meta->abi_version = LRPC_UBSM_ABI_VERSION;
	meta->num_procs = (uint32_t)count;
	__atomic_store_n(&meta->ready, 1, __ATOMIC_RELEASE);
	return 0;
}

int lrpc_ubsm_lookup_procedure(struct lrpc_ubsm_region *region,
			       uint32_t procedure_id, uint64_t expected_epoch,
			       struct lrpc_ubsm_proc_desc *result)
{
	struct lrpc_ubsm_metadata *meta;
	uint32_t i;

	if (!region || !region->mapped || !procedure_id || !expected_epoch ||
	    !result)
		return -EINVAL;
	meta = lrpc_ubsm_at(region, 0, sizeof(*meta));
	if (!meta || !__atomic_load_n(&meta->ready, __ATOMIC_ACQUIRE))
		return -EAGAIN;
	if (meta->magic != LRPC_UBSM_MAGIC ||
	    meta->abi_version != LRPC_UBSM_ABI_VERSION ||
	    meta->region_size != region->size ||
	    !meta->num_procs || meta->num_procs > LRPC_UBSM_MAX_PROCS)
		return -EPROTO;
	if (meta->epoch != expected_epoch)
		return -ESTALE;
	for (i = 0; i < meta->num_procs; i++) {
		if (meta->proc[i].procedure_id != procedure_id)
			continue;
		if (meta->proc[i].code_epoch != expected_epoch ||
		    meta->proc[i].data_offset > region->size ||
		    meta->proc[i].data_size >
			region->size - meta->proc[i].data_offset)
			return -EPROTO;
		*result = meta->proc[i];
		return 0;
	}
	return -ENOENT;
}

void *lrpc_ubsm_at(struct lrpc_ubsm_region *region, size_t offset,
		   size_t length)
{
	if (!region || !region->base || !length || offset > region->size ||
	    length > region->size - offset) {
		errno = EINVAL;
		return NULL;
	}
	return (uint8_t *)region->base + offset;
}

int lrpc_ubsm_close(struct lrpc_ubsm_region *region, int deallocate)
{
	int first_error = UBSM_OK;
	int ret;

	if (!region)
		return UBSM_ERR_PARAM_INVALID;
	if (region->mapped) {
		ret = ubsmem_shmem_unmap(region->base, region->size);
		if (ret != UBSM_OK && first_error == UBSM_OK)
			first_error = ret;
		region->mapped = 0;
		region->base = NULL;
	}
	if (deallocate && region->owner) {
		ret = ubsmem_shmem_deallocate(region->name);
		if (ret != UBSM_OK && first_error == UBSM_OK)
			first_error = ret;
		region->owner = 0;
	}
	if (region->initialized) {
		ret = ubsmem_finalize();
		if (ret != UBSM_OK && first_error == UBSM_OK)
			first_error = ret;
		region->initialized = 0;
	}
	return first_error;
}
