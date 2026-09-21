/* B owns the allocation. A resolves its import PAs. No shadow PID or TTBR
 * activation is involved; PASS only certifies address-query round trips. */
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <lrpc/lrpc_ubsm_pa.h>

static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped = 1; }

static int probe(struct lrpc_ubsm_region *region)
{
    ubsmem_shmem_info_t info;
    struct lrpc_ubsm_pa address;
    int rc = lrpc_ubsm_import_info(region, &info);
    if (rc)
        return rc;
    printf("UB_PA_IMPORT name=%s units=%u unit_size=%" PRIu64 "\n",
           region->name, info.mem_num, info.mem_unit_size);
    /* Query every 4 KiB slot, plus each slot's last byte. This also catches
     * discontinuities within a future 16/64 KiB ARM64 table allocation. */
    for (uint64_t offset = 0; offset < region->size; offset += 4096) {
        struct lrpc_ubsm_pa end;
        rc = lrpc_ubsm_query_pa(region, &info, offset, &address);
        if (rc)
            return rc;
        rc = lrpc_ubsm_query_pa(region, &info, offset + 4095, &end);
        if (rc)
            return rc;
        if ((address.pa & 4095) || end.pa < address.pa ||
            end.pa - address.pa != 4095) {
            fprintf(stderr, "UB_PA_NONLINEAR offset=0x%" PRIx64 "\n", offset);
            return -1;
        }
        printf("UB_PA_PAGE offset=0x%" PRIx64 " memid=%" PRIu64
               " unit_offset=0x%" PRIx64 " pa=0x%" PRIx64 "\n",
               offset, address.mem_id, address.unit_offset, address.pa);
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct lrpc_ubsm_region region;
    struct lrpc_ubsm_provider provider = {0};
    int owner, rc, cleanup;
    if (argc < 3 ||
        (strcmp(argv[1], "owner") && strcmp(argv[1], "import")))
        goto usage;
    owner = !strcmp(argv[1], "owner");
    if (argc != (owner ? 4 : 3))
        goto usage;
    if (signal(SIGINT, stop) == SIG_ERR || signal(SIGTERM, stop) == SIG_ERR)
        return 1;
    if (owner) {
        provider.host_name = argv[3];
        provider.socket_id = provider.numa_id = provider.port_id = UINT32_MAX;
        rc = lrpc_ubsm_owner_open(&region, argv[2], LRPC_UBSM_REGION_SIZE,
                                  &provider);
    } else {
        rc = lrpc_ubsm_remote_open(&region, argv[2], LRPC_UBSM_REGION_SIZE);
    }
    if (rc) {
        fprintf(stderr, "UB_PA_FAIL open=%d\n", rc);
        return 1;
    }
    if (owner) {
        printf("UB_PA_OWNER_READY name=%s provider=%s\n", argv[2], argv[3]);
        puts("Stop importer first, then Ctrl-C owner to deallocate.");
        fflush(stdout);
        while (!stopped)
            sleep(1);
    } else {
        rc = probe(&region);
    }
    cleanup = lrpc_ubsm_close(&region, owner);
    if (rc || cleanup) {
        fprintf(stderr, "UB_PA_FAIL query=%d cleanup=%d\n", rc, cleanup);
        return 1;
    }
    puts(owner ? "UB_PA_OWNER_CLOSED" :
         "UB_PA_QUERY_PASS page_walk_tested=0 execution_tested=0");
    return 0;
usage:
    fprintf(stderr, "usage: %s owner NAME B_PROVIDER_HOST\n"
                    "       %s import NAME\n", argv[0], argv[0]);
    return 2;
}
