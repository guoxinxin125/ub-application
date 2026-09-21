#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <libobmm.h>
#include <lrpc/lrpc_ubsm_pa.h>

static ubsmem_shmem_info_t fixture;
static int lookup_error, query_error, stale;
int ubsmem_shmem_lookup(const char *name, ubsmem_shmem_info_t *info)
{
    (void)name;
    *info = fixture;
    return lookup_error;
}
int obmm_query_pa_by_memid(mem_id id, unsigned long off, unsigned long *pa)
{
    if (query_error) { errno = query_error; return -1; }
    assert(id == 11 || id == 22);
    *pa = (id == 11 ? 0x100000UL : 0x900000UL) + off;
    return 0;
}
int obmm_query_memid_by_pa(unsigned long pa, mem_id *id, unsigned long *off)
{
    *id = pa >= 0x900000 ? 22 : 11;
    *off = pa - (*id == 22 ? 0x900000 : 0x100000);
    if (stale) ++*id;
    return 0;
}
int main(void)
{
    struct lrpc_ubsm_region region = {0};
    struct lrpc_ubsm_pa out;
    ubsmem_shmem_info_t info;
    region.mapped = 1;
    region.base = &region;
    region.size = 8192;
    strcpy(region.name, "test");
    strcpy(fixture.name, "test");
    fixture.size = 8192;
    fixture.mem_unit_size = 4096;
    fixture.mem_num = 2;
    fixture.mem_id_list[0] = 11;
    fixture.mem_id_list[1] = 22;
    assert(!lrpc_ubsm_import_info(&region, &info));
    assert(!lrpc_ubsm_query_pa(&region, &info, 4095, &out));
    assert(out.pa == 0x100fff && out.mem_id == 11);
    assert(!lrpc_ubsm_query_pa(&region, &info, 4096, &out));
    assert(out.pa == 0x900000 && out.mem_id == 22 && !out.unit_offset);
    assert(lrpc_ubsm_query_pa(&region, &info, 8192, &out) == -EINVAL);
    stale = 1;
    assert(lrpc_ubsm_query_pa(&region, &info, 0, &out) == -ESTALE);
    stale = 0;
    query_error = EACCES;
    assert(lrpc_ubsm_query_pa(&region, &info, 0, &out) == -EACCES);
    query_error = 0;
    info.mem_unit_size = 0;
    assert(lrpc_ubsm_query_pa(&region, &info, 0, &out) == -EINVAL);
    lookup_error = 6050;
    assert(lrpc_ubsm_import_info(&region, &info) == 6050);
    lookup_error = 0;
    region.owner = 1;
    assert(lrpc_ubsm_import_info(&region, &info) == -EINVAL);
    region.owner = 0;
    region.mapped = 0;
    assert(lrpc_ubsm_import_info(&region, &info) == -EINVAL);
    puts("UB_PA_UNIT_PASS (mock address queries, no hardware validation)");
    return 0;
}
