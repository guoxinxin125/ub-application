#ifndef LRPC_UB_PROVIDER_OPTIONS_H
#define LRPC_UB_PROVIDER_OPTIONS_H
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ubs_mem_def.h>
#include <lrpc/lrpc_ubsm.h>

/* Match tests/ubs-mem-two-node: use the local hostname, not DNS discovery.
 * Explicit override handles installations whose UB provider name differs. */
struct ub_provider_options {
    struct lrpc_ubsm_provider provider;
    char hostname[MAX_HOST_NAME_DESC_LENGTH];
    const char *override;
};

static inline int ub_provider_parse(struct ub_provider_options *o, int argc,
                                    char **argv, int start,
                                    const char **pos, unsigned int maximum)
{
    unsigned int count = 0;
    int i;
    memset(o, 0, sizeof(*o));
    o->provider.socket_id = o->provider.numa_id = o->provider.port_id = UINT32_MAX;
    for (i = start; i < argc; ++i) {
        uint32_t *field = NULL;
        if (!strcmp(argv[i], "--provider-host")) {
            if (++i == argc || o->override) goto bad;
            o->override = argv[i];
            continue;
        }
        if (!strcmp(argv[i], "--provider-socket")) field = &o->provider.socket_id;
        else if (!strcmp(argv[i], "--provider-numa")) field = &o->provider.numa_id;
        else if (!strcmp(argv[i], "--provider-port")) field = &o->provider.port_id;
        if (field) {
            char *end;
            unsigned long long value;
            if (++i == argc || !argv[i][0] || argv[i][0] == '-') goto bad;
            errno = 0;
            value = strtoull(argv[i], &end, 10);
            if (errno || *end || value > UINT32_MAX) goto bad;
            *field = (uint32_t)value;
        } else {
            if (argv[i][0] == '-' || count == maximum) goto bad;
            pos[count++] = argv[i];
        }
    }
    return (int)count;
bad:
    fprintf(stderr, "Invalid UB provider arguments near '%s'\n", i < argc ? argv[i] : "end");
    return -1;
}

static inline int ub_provider_resolve(struct ub_provider_options *o, const char *legacy)
{
    const char *host = o->override;
    if (legacy && host) { fprintf(stderr, "Provider hostname specified twice\n"); return -1; }
    if (!host) host = legacy;
    if (host) {
        if (!host[0] || strlen(host) >= sizeof(o->hostname)) {
            fprintf(stderr, "UB provider hostname must contain 1 to %u bytes\n", (unsigned int)sizeof(o->hostname)-1);
            return -1;
        }
        memcpy(o->hostname, host, strlen(host)+1);
    } else {
        if (gethostname(o->hostname, sizeof(o->hostname))) { perror("gethostname"); return -1; }
        if (!memchr(o->hostname, 0, sizeof(o->hostname)) || !o->hostname[0]) {
            fprintf(stderr, "Local hostname is empty or too long for UBSM; use --provider-host\n");
            return -1;
        }
    }
    o->provider.host_name = o->hostname;
    printf("UB_PROVIDER host=%s source=%s socket=%u numa=%u port=%u "
           "auto_id=%u\n", o->hostname, host ? "explicit" : "gethostname",
           o->provider.socket_id, o->provider.numa_id, o->provider.port_id, UINT32_MAX);
    return 0;
}
#endif
