#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <lrpc/lrpc_ubsm_pa.h>
#include <lrpc/ub_domain_layout.h>
#include "ub_domain_client.h"

struct ud_client { struct lrpc_ubsm_region region; };
static int client_active;
static int ud_error(int rc) { errno = rc < 0 ? -rc : EIO; return -1; }

int ud_lrpc_bind(struct lrpc_handle *h, uint32_t procedure, uint64_t epoch)
{
    const char *name = getenv("LRPC_UB_DOMAIN_NAME");
    struct ud_client *client;
    struct ud_prepare p = {0};
    struct ud_publication *pub;
    ubsmem_shmem_info_t info;
    struct lrpc_ubsm_pa address;
    struct timespec begin, now;
    cpu_set_t set;
    int rc, saved;
    unsigned int d, l;
    if (!h || procedure < 1 || procedure > 3 || epoch != 1) return ud_error(-EINVAL);
    if (__sync_lock_test_and_set(&client_active, 1)) return ud_error(-EBUSY);
    memset(h, 0, sizeof(*h)); h->fd = -1; h->astack_offset = UD_HANDLE_MARKER;
    client = calloc(1, sizeof(*client));
    if (!client) { __sync_lock_release(&client_active); return -1; }
    h->astack_map = client;
    if (sched_getaffinity(0, sizeof(set), &set)) goto fail;
    if (CPU_COUNT(&set) != 1) { errno = EXDEV; goto fail; }
    h->shadow_cpu = sched_getcpu();
    if (h->shadow_cpu < 0) goto fail;
    if (!name || !name[0]) name = "lrpc_ub_domain";
    rc = lrpc_ubsm_remote_open(&client->region, name, UD_BYTES);
    if (rc) { fprintf(stderr, "UB_DOMAIN_FAIL import=%d\n", rc); ud_error(rc); goto fail; }
    rc = lrpc_ubsm_import_info(&client->region, &info);
    if (rc) { ud_error(rc); goto fail; }
    /* Populate lazy OBMM PTEs before the kernel performs a non-faulting walk.
     * Connection-time only; B has announced OWNER_READY and owns the bytes. */
    for (size_t off = 0; off < UD_BYTES; off += UD_PAGE)
        (void)*((volatile unsigned char *)client->region.base + off);
    h->fd = open("/dev/ub_lrpc_domain", O_RDWR | O_CLOEXEC);
    if (h->fd < 0) goto fail;
    p.import_va = (uintptr_t)client->region.base;
    if (ioctl(h->fd, UD_PREPARE, &p)) goto fail;
    if (!ud_contract_valid(&p.contract)) { errno = EPROTO; goto fail; }
    /* Check independent SDK/OBMM and kernel VMA translations agree. */
    for (d = 0; d < UD_DOMAINS; ++d) for (l = 0; l < 4; ++l) {
        rc = lrpc_ubsm_query_pa(&client->region, &info, UD_TABLE(d,l), &address);
        if (rc) { ud_error(rc); goto fail; }
        if (address.pa != p.contract.table_pa[d][l]) { errno = ESTALE; goto fail; }
    }
    rc = lrpc_ubsm_query_pa(&client->region, &info, UD_HEAP, &address);
    if (rc) { ud_error(rc); goto fail; }
    if (address.pa != p.contract.heap_pa) { errno = ESTALE; goto fail; }
    pub = client->region.base;
    if (pub->request || pub->complete) { errno = EBUSY; goto fail; }
    memcpy(&pub->contract, &p.contract, sizeof(p.contract));
    __atomic_store_n(&pub->request, p.contract.nonce, __ATOMIC_RELEASE);
    if (clock_gettime(CLOCK_MONOTONIC, &begin)) goto fail;
    while (__atomic_load_n(&pub->complete, __ATOMIC_ACQUIRE) != p.contract.nonce) {
        if (clock_gettime(CLOCK_MONOTONIC, &now)) goto fail;
        if (now.tv_sec - begin.tv_sec >= 30) { errno = ETIMEDOUT; goto fail; }
        usleep(1000);
    }
    if (ioctl(h->fd, UD_COMMIT)) goto fail;
    h->epoch = 1;
    printf("UB_DOMAIN_BOUND cpu=%d page_tables=B heap=B code=A astack=A estack=A "
           "shadow_pid=0 tlb=flush\n", h->shadow_cpu);
    return 0;
fail:
    saved = errno;
    ud_lrpc_close(h);
    errno = saved;
    return -1;
}
int ud_lrpc_invoke(struct lrpc_handle *h, struct lrpc_astack *call)
{
    struct ud_call c = {0};
    if (!call || call->abi != LRPC_ASTACK_ABI || call->procedure_id < 1 ||
        call->procedure_id > 3 ||
        call->request_size != (call->procedure_id == 2 ? 8U : 16U)) return ud_error(-EINVAL);
    c.procedure = call->procedure_id;
    memcpy(&c.a, call->payload, 8);
    if (c.procedure != 2) memcpy(&c.b, call->payload+8, 8);
    if (ioctl(h->fd, UD_CALL, &c)) {
        int saved_errno = errno;
        fprintf(stderr, "UB_DOMAIN_CALL_FAIL errno=%d esr=0x%llx far=0x%llx\n",
                saved_errno, (unsigned long long)c.esr, (unsigned long long)c.far);
        errno = saved_errno;
        return -1;
    }
    if (c.esr || c.transitions != (c.procedure == 2 ? 6U : 2U)) return ud_error(-EPROTO);
    memcpy(call->payload, &c.result, 8);
    call->response_size = 8; call->status = 0;
    call->shadow_pid = 0; call->caller_pid = getpid();
    call->caller_cpu_before = call->caller_cpu_in_service = c.cpu;
    call->caller_cpu_after = sched_getcpu();
    /* Existing scheduler breakdown stamps do not describe this gate. */
    call->call_enter_ns = call->shadow_dispatch_ns = 0;
    call->shadow_return_ns = call->caller_resume_ns = 0;
    return 0;
}
void ud_lrpc_close(struct lrpc_handle *h)
{
    struct ud_client *client = h->astack_map;
    if (h->fd >= 0) close(h->fd);
    if (client) {
        int rc = lrpc_ubsm_close(&client->region, 0);
        if (rc) fprintf(stderr, "UB_DOMAIN_CLOSE_FAIL error=%d\n", rc);
        free(client);
    }
    memset(h, 0, sizeof(*h)); h->fd = -1;
    __sync_lock_release(&client_active);
}
