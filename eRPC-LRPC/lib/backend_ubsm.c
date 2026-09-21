#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ubs_mem.h>
#include <ubs_mem_def.h>

#include <lrpc/lrpc_ubsm.h>

static uint64_t lrpc_ubsm_hash(const void *data, size_t size)
{
	const uint8_t *bytes = data;
	uint64_t hash = UINT64_C(14695981039346656037);
	size_t i;

	for (i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

_Static_assert(sizeof(struct lrpc_ubsm_metadata) <= LRPC_UBSM_DATA_OFFSET,
	       "UBSM metadata overlaps service data");
_Static_assert(LRPC_UBSM_CODE_OFFSET +
	       LRPC_UBSM_MAX_PROCS * LRPC_UBSM_CODE_SLOT_SIZE <=
	       LRPC_UBSM_REGION_SIZE,
	       "UBSM procedure slots exceed the shared region");

static int lrpc_ubsm_initialize(struct lrpc_ubsm_region *region)
{
	/*
	 * The current UBS Memory SDK defines ubsmem_options_t as an empty
	 * GNU C struct.  A C initializer such as {0} therefore tries to
	 * initialize a non-existent first member and fails with
	 * "excess elements in struct initializer".  The SDK initializer is
	 * the supported way to prepare this opaque options object.
	 */
	ubsmem_options_t options;
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

int lrpc_ubsm_publish_services(struct lrpc_ubsm_region *region,
			       uint64_t epoch,
			       const struct lrpc_ubsm_service *services,
			       size_t count)
{
	struct lrpc_ubsm_metadata *meta;
	size_t i;

	if (!region || !region->owner || !region->mapped || !epoch ||
	    !services || !count || count > LRPC_UBSM_MAX_PROCS)
		return -EINVAL;
	meta = lrpc_ubsm_at(region, 0, sizeof(*meta));
	if (!meta)
		return -EINVAL;
	__atomic_store_n(&meta->ready, 0, __ATOMIC_RELEASE);
	memset(meta, 0, sizeof(*meta));
	for (i = 0; i < count; i++) {
		size_t j;
		uint64_t offset;

		if (!services[i].procedure_id ||
		    services[i].procedure_id > LRPC_UBSM_MAX_PROCS)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (services[j].procedure_id == services[i].procedure_id)
				return -EINVAL;
		if (services[i].flags == LRPC_UBSM_PROC_LOCAL_CODE) {
			if (services[i].code || services[i].code_size ||
			    services[i].code_entry_offset)
				return -EINVAL;
		} else if (services[i].flags ==
			   LRPC_UBSM_PROC_PUBLISHED_CODE) {
			if (!services[i].code || !services[i].code_size ||
			    services[i].code_size % sizeof(uint32_t) ||
			    services[i].code_size > LRPC_UBSM_CODE_SLOT_SIZE ||
			    services[i].code_entry_offset % sizeof(uint32_t) ||
			    services[i].code_entry_offset >= services[i].code_size)
				return -EINVAL;
		} else {
			return -EINVAL;
		}
		offset = LRPC_UBSM_DATA_OFFSET +
			 (uint64_t)(services[i].procedure_id - 1) *
			 LRPC_UBSM_SERVICE_SLOT_SIZE;
		if (offset > region->size ||
		    LRPC_UBSM_SERVICE_SLOT_SIZE > region->size - offset)
			return -EINVAL;
		meta->proc[i].procedure_id = services[i].procedure_id;
		meta->proc[i].flags = services[i].flags;
		meta->proc[i].data_offset = offset;
		meta->proc[i].data_size = LRPC_UBSM_SERVICE_SLOT_SIZE;
		if (services[i].flags == LRPC_UBSM_PROC_PUBLISHED_CODE) {
			void *destination;

			offset = LRPC_UBSM_CODE_OFFSET +
				 (uint64_t)(services[i].procedure_id - 1) *
				 LRPC_UBSM_CODE_SLOT_SIZE;
			destination = lrpc_ubsm_at(region, offset,
						   services[i].code_size);
			if (!destination)
				return -EINVAL;
			memcpy(destination, services[i].code,
			       services[i].code_size);
			meta->proc[i].code_offset = offset;
			meta->proc[i].code_size = services[i].code_size;
			meta->proc[i].code_entry_offset =
				services[i].code_entry_offset;
			meta->proc[i].code_hash = lrpc_ubsm_hash(
				services[i].code, services[i].code_size);
		}
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

int lrpc_ubsm_publish(struct lrpc_ubsm_region *region, uint64_t epoch,
		      const uint32_t *procedure_ids, size_t count)
{
	struct lrpc_ubsm_service services[LRPC_UBSM_MAX_PROCS];
	size_t i;

	if (!procedure_ids || !count || count > LRPC_UBSM_MAX_PROCS)
		return -EINVAL;
	memset(services, 0, sizeof(services));
	for (i = 0; i < count; i++) {
		services[i].procedure_id = procedure_ids[i];
		services[i].flags = LRPC_UBSM_PROC_LOCAL_CODE;
	}
	return lrpc_ubsm_publish_services(region, epoch, services, count);
}

static int lrpc_ubsm_validate_procedure(
	struct lrpc_ubsm_region *region,
	const struct lrpc_ubsm_proc_desc *procedure,
	uint64_t expected_epoch)
{
	uint64_t expected_data;
	uint64_t expected_code;

	if (!procedure->procedure_id ||
	    procedure->procedure_id > LRPC_UBSM_MAX_PROCS ||
	    procedure->code_epoch != expected_epoch)
		return -EPROTO;
	expected_data = LRPC_UBSM_DATA_OFFSET +
		(uint64_t)(procedure->procedure_id - 1) *
		LRPC_UBSM_SERVICE_SLOT_SIZE;
	if (procedure->data_offset != expected_data ||
	    !procedure->data_size ||
	    procedure->data_size > LRPC_UBSM_SERVICE_SLOT_SIZE ||
	    procedure->data_offset > region->size ||
	    procedure->data_size > region->size - procedure->data_offset)
		return -EPROTO;
	if (procedure->flags == LRPC_UBSM_PROC_LOCAL_CODE)
		return procedure->code_offset || procedure->code_size ||
		       procedure->code_entry_offset || procedure->code_hash ?
		       -EPROTO : 0;
	if (procedure->flags != LRPC_UBSM_PROC_PUBLISHED_CODE)
		return -EPROTO;
	expected_code = LRPC_UBSM_CODE_OFFSET +
		(uint64_t)(procedure->procedure_id - 1) *
		LRPC_UBSM_CODE_SLOT_SIZE;
	if (procedure->code_offset != expected_code ||
	    !procedure->code_size ||
	    procedure->code_size % sizeof(uint32_t) ||
	    procedure->code_size > LRPC_UBSM_CODE_SLOT_SIZE ||
	    procedure->code_entry_offset % sizeof(uint32_t) ||
	    procedure->code_entry_offset >= procedure->code_size ||
	    procedure->code_offset > region->size ||
	    procedure->code_size > region->size - procedure->code_offset)
		return -EPROTO;
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
		if (lrpc_ubsm_validate_procedure(region, &meta->proc[i],
						 expected_epoch))
			return -EPROTO;
		*result = meta->proc[i];
		return 0;
	}
	return -ENOENT;
}

int lrpc_ubsm_localize_code(struct lrpc_ubsm_region *region,
			     const struct lrpc_ubsm_proc_desc *procedure,
			     struct lrpc_ubsm_local_code *result)
{
	const void *remote_code;
	void *mapping;
	long page_size;
	size_t mapping_size;

	if (!region || !procedure || !result ||
	    procedure->flags != LRPC_UBSM_PROC_PUBLISHED_CODE)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	if (lrpc_ubsm_validate_procedure(region, procedure,
					 procedure->code_epoch))
		return -EPROTO;
	remote_code = lrpc_ubsm_at(region, procedure->code_offset,
				   procedure->code_size);
	if (!remote_code)
		return -EPROTO;
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		return -EINVAL;
	mapping_size = (procedure->code_size + (size_t)page_size - 1) &
		       ~((size_t)page_size - 1);
	mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return -errno;
	memcpy(mapping, remote_code, procedure->code_size);
	if (lrpc_ubsm_hash(mapping, procedure->code_size) !=
	    procedure->code_hash) {
		munmap(mapping, mapping_size);
		return -EBADMSG;
	}
	__builtin___clear_cache(mapping,
				(char *)mapping + procedure->code_size);
	if (mprotect(mapping, mapping_size, PROT_READ | PROT_EXEC)) {
		int saved_errno = errno;

		munmap(mapping, mapping_size);
		return -saved_errno;
	}
	result->mapping = mapping;
	result->mapping_size = mapping_size;
	result->entry = (uint8_t *)mapping + procedure->code_entry_offset;
	return 0;
}

void lrpc_ubsm_release_local_code(struct lrpc_ubsm_local_code *code)
{
	if (!code)
		return;
	if (code->mapping)
		munmap(code->mapping, code->mapping_size);
	memset(code, 0, sizeof(*code));
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
