#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/random.h>
#include <lrpc/lrpc_ubsm.h>
#include <lrpc/ub_domain_layout.h>
#include <lrpc/ub_domain_services.h>
#include <lrpc/ub_domain_pool.h>
#include <lrpc/ub_remote_code.h>
#include "provider_options.h"
static volatile sig_atomic_t stopped;
static void stop(int signo) { (void)signo; stopped = 1; }
int main(int argc, char **argv)
{
    struct lrpc_ubsm_region region;
    struct ub_provider_options options;
    const char *pos[2] = {NULL, NULL}, *name, *remote_file = NULL;
    struct ud_remote_bundle remote_bundle = {0};
    FILE *remote_input;
    struct ud_publication *pub;
    struct ud_contract c;
    unsigned int d, l, slot, index;
    unsigned long size;
    static __u64 observed[UD_CLIENTS][UD_PAGE / sizeof(__u64)];
    static __u64 observed_identity[UD_CLIENTS];
    int rc, failed = 0, count, multi = 0, social = 0, write = 0, shared = 0, experimental_nginx3 = 0, i, j;
    for (i = j = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--social")) {
            if (social) return 2;
            social = 1;
        } else if (!strcmp(argv[i], "--social-write")) {
            if (social || write) return 2;
            social = write = 1;
        } else if (!strcmp(argv[i], "--social-shared")) {
            if (social || shared) return 2;
            social = shared = 1;
        } else if (!strcmp(argv[i], "--multi")) {
            if (multi) return 2;
            multi = 1;
        } else if (!strcmp(argv[i], "--remote-code")) {
            if (remote_file || i + 1 == argc) return 2;
            remote_file = argv[++i];
        } else if (!strcmp(argv[i], "--experimental-nginx3")) {
            if (experimental_nginx3) return 2;
            experimental_nginx3 = 1;
        } else argv[j++] = argv[i];
    }
    argc = j; argv[j] = NULL;
    if (social) multi = 1;
    if (remote_file && (write || shared || (multi && !social))) {
        fputs("--remote-code supports micro direct/nested or --social read routing only\n", stderr);
        return 2;
    }
    if (social && !write && !shared && !remote_file) {
        fputs("--social read routing requires --remote-code BUNDLE\n", stderr);
        return 2;
    }
    if (experimental_nginx3 && (!social || write || shared || !remote_file)) {
        fputs("--experimental-nginx3 requires --social --remote-code BUNDLE\n", stderr);
        return 2;
    }
    count = ub_provider_parse(&options, argc, argv, 1, pos, 2);
    if (count < 0) {
        fprintf(stderr, "usage: %s [SHM_NAME] [--provider-host HOST] "
                "[--provider-numa N] [--provider-socket N] [--provider-port N] "
                "[--multi|--social|--social-write|--social-shared] [--remote-code BUNDLE] [--experimental-nginx3]\n"
                "legacy: %s B_PROVIDER_HOST SHM_NAME\n", argv[0], argv[0]);
        return 2;
    }
    name = count == 2 ? pos[1] : count == 1 ? pos[0] : "lrpc_ub_domain";
    if (ub_provider_resolve(&options, count == 2 ? pos[0] : NULL)) return 2;
    if (remote_file) {
        remote_input = fopen(remote_file, "rb");
        if (!remote_input) { perror("remote-code open"); return 1; }
        if (fread(&remote_bundle, 1, sizeof(remote_bundle), remote_input) !=
                sizeof(remote_bundle) || fgetc(remote_input) != EOF ||
            ferror(remote_input) || !ud_remote_bundle_valid(&remote_bundle)) {
            fputs("UB_DOMAIN_OWNER_FAIL remote-code format or verifier\n", stderr);
            fclose(remote_input);
            return 1;
        }
        if (fclose(remote_input)) { perror("remote-code close"); return 1; }
        if (remote_bundle.manifest.mode !=
            (social ? UD_CODE_REMOTE_SOCIAL : UD_CODE_REMOTE_VERIFIED)) {
            fputs("UB_DOMAIN_OWNER_FAIL remote-code/profile mismatch\n", stderr);
            return 2;
        }
        if (social && ud_remote_social_count(remote_bundle.manifest.reserved) == 3 &&
            !experimental_nginx3) {
            fputs("UB_DOMAIN_OWNER_FAIL three-nginx requires --experimental-nginx3\n", stderr);
            return 2;
        }
        if (experimental_nginx3 && (!social || ud_remote_social_count(remote_bundle.manifest.reserved) != 3)) {
            fputs("UB_DOMAIN_OWNER_FAIL --experimental-nginx3 requires a three-nginx social bundle\n", stderr);
            return 2;
        }
    }
    if (signal(SIGINT, stop) == SIG_ERR || signal(SIGTERM, stop) == SIG_ERR) return 1;
    rc = lrpc_ubsm_owner_open(&region, name, UD_BYTES, &options.provider);
    if (rc) { fprintf(stderr, "UB_DOMAIN_OWNER_FAIL open=%d\n", rc); return 1; }
    if (multi) {
        __u64 generation;
        if (getrandom(&generation, sizeof(generation), 0) != sizeof(generation) || !generation ||
            ud_pool_initialize_profile(region.base, generation,
                                       shared ? UD_PROFILE_SOCIAL_SHARED :
                                       write ? UD_PROFILE_SOCIAL_WRITE :
                                       social ? UD_PROFILE_SOCIAL : UD_PROFILE_MICRO)) {
            fprintf(stderr, "UB_DOMAIN_OWNER_FAIL pool-init\n"); failed = 1; goto out;
        }
        if (remote_file) {
            if (social)
                us_heap_init_count((void *)((char *)region.base + UD_HEAP),
                                   ud_remote_social_count(remote_bundle.manifest.reserved));
            memcpy((char *)region.base + UD_REMOTE_META_OFFSET,
                   &remote_bundle.manifest, sizeof(remote_bundle.manifest));
            for (d = 0; d < UD_DOMAINS; ++d)
                memcpy((char *)region.base + UD_IMAGE(d), remote_bundle.image[d],
                       remote_bundle.manifest.words[d] * sizeof(__u32));
        }
        printf("UB_DOMAIN_OWNER_READY multi=1 capacity=%u generation=%llu page_tables=B heap=B profile=%s code_mode=%s nginx_count=%u\n",
               UD_CLIENTS, (unsigned long long)generation,
               shared ? "social-shared-probe" :
               write ? "social-write-probe" : social ? "social-user-read" : "micro",
               remote_file ? "remote-social" : "legacy",
               social && remote_file ? ud_remote_social_count(remote_bundle.manifest.reserved) : 0);
        fflush(stdout);
        if (shared) {
            printf("UB_DOMAIN_SHARED_READY generation=%llu cell=0 counter=0\n",
                   (unsigned long long)generation);
            fflush(stdout);
        }
        while (!stopped) {
            rc = ud_pool_poll(region.base);
            if (rc < 0) { failed = 1; break; }
            if (rc) {
                printf("UB_DOMAIN_POOL_PUBLISHED count=%d b_service_executions=0\n", rc);
                fflush(stdout);
            }
            if (write) {
                /* Observer only. No B service dispatch; changed values prove B visibility. */
                for (slot = 0; slot < UD_CLIENTS; ++slot) {
                    struct ud_pool_publication *publication =
                        (void *)((char *)region.base + UD_POOL_SLOT(slot));
                    __u64 identity = __atomic_load_n(&publication->pub.complete,
                                                      __ATOMIC_ACQUIRE);
                    volatile __u64 *slots = (volatile __u64 *)((char *)region.base + UD_WRITE_HEAP(slot));
                    if (!identity) continue;
                    if (identity != observed_identity[slot]) {
                        /* Reused slots may receive exactly the same test values. */
                        memset(observed[slot], 0, sizeof(observed[slot]));
                        observed_identity[slot] = identity;
                        printf("UB_DOMAIN_WRITE_SLOT_READY slot=%u identity=%llu\n",
                               slot, (unsigned long long)identity);
                        fflush(stdout);
                    }
                    for (index = 0; index < UD_PAGE / sizeof(__u64); ++index) {
                        __u64 value = __atomic_load_n(&slots[index], __ATOMIC_ACQUIRE);
                        if (value != observed[slot][index]) {
                            observed[slot][index] = value;
                            if (value) {
                                printf("UB_DOMAIN_WRITE_OBSERVED slot=%u identity=%llu "
                                       "index=%u value=%llu\n",
                                       slot, (unsigned long long)identity, index,
                                       (unsigned long long)value);
                                fflush(stdout);
                            }
                        }
                    }
                }
            }
            if (shared) {
                volatile __u64 *cells = (volatile __u64 *)((char *)region.base + UD_SHARED_HEAP);
                static __u64 previous[2];
                __u64 values[2];
                values[0] = __atomic_load_n(&cells[US_SHARED_VALUE_INDEX], __ATOMIC_ACQUIRE);
                values[1] = __atomic_load_n(&cells[US_SHARED_COUNTER_INDEX], __ATOMIC_ACQUIRE);
                for (index = 0; index < 2; ++index) if (values[index] != previous[index]) {
                    previous[index] = values[index];
                    printf("UB_DOMAIN_SHARED_OBSERVED generation=%llu index=%u value=%llu\n",
                           (unsigned long long)generation, index + 1,
                           (unsigned long long)values[index]);
                    fflush(stdout);
                }
            }
            usleep(1000);
        }
        goto out;
    }
    memset(region.base, 0, UD_BYTES);
    pub = region.base;
    if (remote_file)
        memcpy((char *)region.base + UD_REMOTE_META_OFFSET,
               &remote_bundle.manifest, sizeof(remote_bundle.manifest));
    printf("UB_DOMAIN_OWNER_READY single_connection=1 page_tables=B heap=B "
           "code_mode=%s\n", remote_file ? "remote-verified" : "legacy");
    fflush(stdout);
    while (!stopped && !__atomic_load_n(&pub->request, __ATOMIC_ACQUIRE)) usleep(1000);
    if (stopped) goto out;
    memcpy(&c, &pub->contract, sizeof(c));
    if (!ud_contract_valid(&c) || c.write_pa || c.nonce != pub->request) {
        fprintf(stderr, "UB_DOMAIN_OWNER_FAIL contract\n"); failed = 1; goto out;
    }
    for (d = 0; d < UD_DOMAINS; ++d) {
        struct ud_initial initial = {UD_ARGS, UD_DATA, UD_STACK + UD_PAGE, UD_VA};
        const unsigned char *code = remote_file ?
            (const unsigned char *)remote_bundle.image[d] : ud_service(d, &size);
        if (remote_file) size = remote_bundle.manifest.words[d] * sizeof(__u32);
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
    printf("UB_DOMAIN_PUBLISHED domains=4 shadow_pid=0 b_service_executions=0 "
           "code_mode=%s\n", remote_file ? "remote-verified" : "legacy");
    puts("Keep B alive and immutable. Stop A before Ctrl-C here. Restart owner for a new connection.");
    fflush(stdout);
    while (!stopped) sleep(1);
out:
    rc = lrpc_ubsm_close(&region, 1);
    return failed || rc ? 1 : 0;
}
