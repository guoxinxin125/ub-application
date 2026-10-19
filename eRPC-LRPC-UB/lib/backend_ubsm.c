#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>

#include <ubs_mem.h>
#include <ubs_mem_def.h>

#include <lrpc/lrpc_ubsm.h>

static pthread_mutex_t sdk_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int sdk_refs;
static int sdk_external;
struct shared_import {
    struct shared_import *next;
    char name[49];
    void *base;
    size_t size;
    unsigned int refs;
};
static pthread_mutex_t imports_lock = PTHREAD_MUTEX_INITIALIZER;
static struct shared_import *imports;

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
	const char *mode = getenv("LRPC_UBSM_SDK_MODE");
	int external = mode && !strcmp(mode, "external"), ret = UBSM_OK;
	if (mode && strcmp(mode, "external") && strcmp(mode, "owned"))
		return UBSM_ERR_PARAM_INVALID;
	pthread_mutex_lock(&sdk_lock);
	if (sdk_refs && sdk_external != external) ret = UBSM_ERR_PARAM_INVALID;
	if (!sdk_refs && !external) {
		ret = ubsmem_set_logger_level(3);
		if (!ret) ret = ubsmem_init_attributes(&options);
		if (!ret) ret = ubsmem_initialize(&options);
	}
	if (!ret) {
		sdk_external = external;
		++sdk_refs; region->initialized = 1;
	}
	pthread_mutex_unlock(&sdk_lock);
	return ret;
}
static int lrpc_ubsm_finalize(void)
{
    int ret = UBSM_OK;
    pthread_mutex_lock(&sdk_lock);
    if (!sdk_refs) ret = UBSM_ERR_PARAM_INVALID;
    else if (sdk_refs == 1 && !sdk_external) {
        ret = ubsmem_finalize();
        if (!ret) --sdk_refs;
    } else --sdk_refs;
    pthread_mutex_unlock(&sdk_lock);
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
	struct shared_import *mapping;
	int ret = lrpc_ubsm_prepare(region, name, size);

	if (ret != UBSM_OK)
		return ret;
	pthread_mutex_lock(&imports_lock);
	for (mapping = imports; mapping; mapping = mapping->next)
		if (!strcmp(mapping->name, name)) break;
	if (mapping) {
		if (mapping->size != size) { ret = UBSM_ERR_PARAM_INVALID; goto unlock_fail; }
		++mapping->refs;
		region->base = mapping->base; region->mapped = 1;
		region->shared_mapping = mapping;
		pthread_mutex_unlock(&imports_lock);
		return UBSM_OK;
	}
	mapping = calloc(1, sizeof(*mapping));
	if (!mapping) { ret = UBSM_ERR_MEMORY; goto unlock_fail; }
	ret = ubsmem_shmem_map(NULL, region->size, PROT_READ | PROT_WRITE,
			       MAP_SHARED, region->name, 0, &base);
	if (ret != UBSM_OK) { free(mapping); goto unlock_fail; }
	if (!base || base == MAP_FAILED) {
		ret = UBSM_ERR_MEMORY;
		free(mapping); goto unlock_fail;
	}
	region->base = base;
	region->mapped = 1;
	memcpy(mapping->name, name, strlen(name)+1);
	mapping->base = base; mapping->size = size; mapping->refs = 1;
	mapping->next = imports; imports = mapping;
	region->shared_mapping = mapping;
	pthread_mutex_unlock(&imports_lock);
	return UBSM_OK;
unlock_fail:
	pthread_mutex_unlock(&imports_lock);
	(void)lrpc_ubsm_close(region, 0);
	return ret;
}


int lrpc_ubsm_close(struct lrpc_ubsm_region *region, int deallocate)
{
	int first_error = UBSM_OK;
	int ret;

	if (!region)
		return UBSM_ERR_PARAM_INVALID;
	if (region->shared_mapping) {
		struct shared_import *mapping = region->shared_mapping, **link;
		pthread_mutex_lock(&imports_lock);
		if (mapping->refs == 1) {
			ret = ubsmem_shmem_unmap(mapping->base, mapping->size);
			/* Keep SDK/mapping alive on failure; never invalidate peers. */
			if (ret) { pthread_mutex_unlock(&imports_lock); return ret; }
			for (link = &imports; *link != mapping; link = &(*link)->next) {}
			*link = mapping->next; free(mapping);
		} else --mapping->refs;
		region->shared_mapping = NULL; region->mapped = 0; region->base = NULL;
		pthread_mutex_unlock(&imports_lock);
	}
	if (region->mapped) {
		ret = ubsmem_shmem_unmap(region->base, region->size);
		if (ret != UBSM_OK) return ret;
		region->mapped = 0;
		region->base = NULL;
	}
	if (deallocate && region->owner) {
		ret = ubsmem_shmem_deallocate(region->name);
		if (ret != UBSM_OK) return ret;
		region->owner = 0;
	}
	if (region->initialized) {
		ret = lrpc_ubsm_finalize();
		if (ret != UBSM_OK && first_error == UBSM_OK)
			first_error = ret;
		if (!ret) region->initialized = 0;
	}
	return first_error;
}
