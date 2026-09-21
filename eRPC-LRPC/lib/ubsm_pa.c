#include <errno.h>
#include <limits.h>
#include <string.h>
#include <libobmm.h>
#include <lrpc/lrpc_ubsm_pa.h>

static int valid_info(const struct lrpc_ubsm_region *region,
                      const ubsmem_shmem_info_t *info)
{
    if (!region || !info || !region->mapped || region->owner ||
        !region->base || !region->size || !info->mem_num ||
        info->mem_num > MAX_MEMID_NUM || !info->mem_unit_size ||
        info->size != region->size ||
        strncmp(info->name, region->name, sizeof(info->name)) != 0)
        return -EINVAL;
    /* Avoid multiplication overflow and account for a partial last unit. */
    if ((region->size - 1) / info->mem_unit_size + 1 != info->mem_num)
        return -EINVAL;
    for (uint32_t i = 0; i < info->mem_num; ++i)
        if (!info->mem_id_list[i])
            return -EINVAL;
    return 0;
}

int lrpc_ubsm_import_info(const struct lrpc_ubsm_region *region,
                          ubsmem_shmem_info_t *info)
{
    int rc;
    if (!region || !info || !region->mapped || region->owner)
        return -EINVAL;
    memset(info, 0, sizeof(*info));
    rc = ubsmem_shmem_lookup(region->name, info);
    return rc ? rc : valid_info(region, info);
}

int lrpc_ubsm_query_pa(const struct lrpc_ubsm_region *region,
                       const ubsmem_shmem_info_t *info, uint64_t offset,
                       struct lrpc_ubsm_pa *result)
{
    unsigned long pa, reverse_offset;
    mem_id id, reverse_id;
    uint64_t within;
    int rc = valid_info(region, info);
    if (rc)
        return rc;
    if (!result || offset >= region->size)
        return -EINVAL;
    within = offset % info->mem_unit_size;
    if (within > ULONG_MAX)
        return -EOVERFLOW;
    id = info->mem_id_list[offset / info->mem_unit_size];
    if (obmm_query_pa_by_memid(id, (unsigned long)within, &pa))
        return errno ? -errno : -EIO;
    if (obmm_query_memid_by_pa(pa, &reverse_id, &reverse_offset))
        return errno ? -errno : -EIO;
    if (reverse_id != id || reverse_offset != within)
        return -ESTALE;
    result->mem_id = id;
    result->unit_offset = within;
    result->pa = pa;
    return 0;
}
