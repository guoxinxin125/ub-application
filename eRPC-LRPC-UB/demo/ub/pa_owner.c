#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <lrpc/lrpc_ubsm.h>
#include "provider_options.h"

static volatile sig_atomic_t stopped;
static void stop(int signo) { (void)signo; stopped = 1; }

int main(int argc, char **argv)
{
    struct lrpc_ubsm_region region;
    struct ub_provider_options options;
    const char *pos[1];
    int rc;
    if (ub_provider_parse(&options, argc, argv, 1, pos, 1) != 1 ||
        ub_provider_resolve(&options, NULL)) {
        fprintf(stderr, "usage: %s NAME [--provider-host HOST] "
                "[--provider-numa N] [--provider-socket N] [--provider-port N]\n",
                argv[0]);
        return 2;
    }
    if (signal(SIGINT, stop) == SIG_ERR || signal(SIGTERM, stop) == SIG_ERR)
        return 1;
    rc = lrpc_ubsm_owner_open(&region, pos[0], LRPC_UBSM_REGION_SIZE,
                              &options.provider);
    if (rc) {
        fprintf(stderr, "UB_PA_FAIL open=%d\n", rc);
        return 1;
    }
    printf("UB_PA_OWNER_READY name=%s provider=%s\n", pos[0],
           options.provider.host_name);
    fflush(stdout);
    while (!stopped) sleep(1);
    rc = lrpc_ubsm_close(&region, 1);
    if (rc) {
        fprintf(stderr, "UB_PA_FAIL cleanup=%d\n", rc);
        return 1;
    }
    puts("UB_PA_OWNER_CLOSED");
    return 0;
}
