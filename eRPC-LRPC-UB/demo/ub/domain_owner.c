#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <lrpc/lrpc_ubsm.h>
#include <lrpc/ub_domain_layout.h>
#include <lrpc/ub_domain_services.h>
#include "provider_options.h"
static volatile sig_atomic_t stopped;
static void stop(int signo) { (void)signo; stopped = 1; }
int main(int argc, char **argv)
{
    struct lrpc_ubsm_region region;
    struct ub_provider_options options;
    const char *pos[2], *name;
    struct ud_publication *pub;
    struct ud_contract c;
    unsigned int d, l;
    unsigned long size;
    int rc, failed = 0, count;
    count = ub_provider_parse(&options, argc, argv, 1, pos, 2);
    if (count < 0) {
        fprintf(stderr, "usage: %s [SHM_NAME] [--provider-host HOST] "
                "[--provider-numa N] [--provider-socket N] [--provider-port N]\n"
                "legacy: %s B_PROVIDER_HOST SHM_NAME\n", argv[0], argv[0]);
        return 2;
    }
    name = count == 2 ? pos[1] : count == 1 ? pos[0] : "lrpc_ub_domain";
    if (ub_provider_resolve(&options, count == 2 ? pos[0] : NULL)) return 2;
    if (signal(SIGINT, stop) == SIG_ERR || signal(SIGTERM, stop) == SIG_ERR) return 1;
    rc = lrpc_ubsm_owner_open(&region, name, UD_BYTES, &options.provider);
    if (rc) { fprintf(stderr, "UB_DOMAIN_OWNER_FAIL open=%d\n", rc); return 1; }
    memset(region.base, 0, UD_BYTES);
    pub = region.base;
    puts("UB_DOMAIN_OWNER_READY single_connection=1 page_tables=B heap=B");
    fflush(stdout);
    while (!stopped && !__atomic_load_n(&pub->request, __ATOMIC_ACQUIRE)) usleep(1000);
    if (stopped) goto out;
    memcpy(&c, &pub->contract, sizeof(c));
    if (!ud_contract_valid(&c) || c.nonce != pub->request) {
        fprintf(stderr, "UB_DOMAIN_OWNER_FAIL contract\n"); failed = 1; goto out;
    }
    for (d = 0; d < UD_DOMAINS; ++d) {
        struct ud_initial initial = {UD_ARGS, UD_DATA, UD_STACK + UD_PAGE, UD_VA};
        const unsigned char *code = ud_service(d, &size);
        for (l = 0; l < 4; ++l)
            ud_build_table(&c, d, l, (__u64 *)((char *)region.base + UD_TABLE(d,l)));
        memcpy((char *)region.base + UD_IMAGE(d), code, size);
        memcpy((char *)region.base + UD_CONTEXT(d), &initial, sizeof(initial));
    }
    {
        __u64 *heap = (__u64 *)((char *)region.base + UD_HEAP);
        heap[0] = 100;
        for (d = 0; d < 32; ++d) {
            heap[8+d*8] = d;
            heap[9+d*8] = (d+1) & 31;
            heap[10+d*8] = 1000+d;
        }
    }
    pub->b_initializations = 1;
    __atomic_store_n(&pub->complete, c.nonce, __ATOMIC_RELEASE);
    puts("UB_DOMAIN_PUBLISHED domains=4 shadow_pid=0 b_service_executions=0");
    puts("Keep B alive and immutable. Stop A before Ctrl-C here. Restart owner for a new connection.");
    fflush(stdout);
    while (!stopped) sleep(1);
out:
    rc = lrpc_ubsm_close(&region, 1);
    return failed || rc ? 1 : 0;
}
