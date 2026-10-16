#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <lrpc/lrpc_ubsm_pa.h>
#include <lrpc/ub_domain_layout.h>
#include "ub_domain_client.h"

struct ud_client {
    struct lrpc_ubsm_region region;
    void *local_args;
    size_t page_size, map_size;
    unsigned int invoking;
};
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
    {
        long page_size = sysconf(_SC_PAGESIZE);
        if (page_size < UD_PAGE) { errno = EINVAL; goto fail; }
        client->page_size = (size_t)page_size;
        client->map_size = UD_DOMAINS * client->page_size;
        client->local_args = mmap(NULL, client->map_size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, h->fd, 0);
        if (client->local_args == MAP_FAILED) { client->local_args = NULL; goto fail; }
    }
    h->invoke_scratch = calloc(1, sizeof(*h->invoke_scratch));
    if (!h->invoke_scratch) goto fail;
    h->epoch = 1;
    printf("UB_DOMAIN_BOUND cpu=%d page_tables=B heap=B code=A astack=A estack=A "
           "shadow_pid=0 tlb=retain astack=mmap steady_ioctls=1 "
           "payload_user_kernel_copies=0 context_save=middle-suspend-only "
           "call_object=connection-reuse\n", h->shadow_cpu);
    return 0;
fail:
    saved = errno;
    ud_lrpc_close(h);
    errno = saved;
    return -1;
}
int ud_lrpc_invoke(struct lrpc_handle *h, struct lrpc_astack *call)
{
    struct ud_client *client;
    struct ud_mapped *m;
    uint64_t previous, completed;
    unsigned int d, idle = 0;
    int rc, saved_errno;
    if (!h || h->fd < 0 || !h->astack_map || !call || call->abi != LRPC_ASTACK_ABI || call->procedure_id < 1 ||
        call->procedure_id > 3 ||
        call->request_size != (call->procedure_id == 2 ? 8U : 16U)) return ud_error(-EINVAL);
    client = h->astack_map;
    if (!client->local_args) return ud_error(-ENOTCONN);
    if (sched_getcpu() != h->shadow_cpu) return ud_error(-EXDEV);
    if (!__atomic_compare_exchange_n(&client->invoking, &idle, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return ud_error(-EBUSY);
    d = call->procedure_id == 1 ? UD_DIRECT : call->procedure_id == 2 ? UD_MIDDLE : UD_QUERY;
    m = (void *)((char *)client->local_args + d * client->page_size);
    previous = __atomic_load_n(&m->completion, __ATOMIC_ACQUIRE);
    memcpy(&m->a, call->payload, 8);
    m->b = 0;
    if (call->procedure_id != 2) memcpy(&m->b, call->payload+8, 8);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    rc = ioctl(h->fd, UD_MAPPED_CALL, (unsigned long)call->procedure_id);
    saved_errno = errno;
    completed = __atomic_load_n(&m->completion, __ATOMIC_ACQUIRE);
    if (rc) {
        fprintf(stderr, "UB_DOMAIN_CALL_FAIL errno=%d esr=0x%llx far=0x%llx\n",
                saved_errno, (unsigned long long)m->esr, (unsigned long long)m->far);
        goto out;
    }
    if (!completed || completed == previous || m->status || m->esr ||
        m->procedure != call->procedure_id || m->cpu != (unsigned int)h->shadow_cpu ||
        m->transitions != (call->procedure_id == 2 ? 6U : 2U)) {
        rc = -1; saved_errno = EPROTO; goto out;
    }
    memcpy(call->payload, &m->result, 8);
    call->response_size = 8; call->status = 0;
    call->shadow_pid = 0; call->caller_pid = getpid();
    call->caller_cpu_before = call->caller_cpu_in_service = m->cpu;
    call->caller_cpu_after = sched_getcpu();
    /* Existing scheduler breakdown stamps do not describe this gate. */
    call->call_enter_ns = call->shadow_dispatch_ns = 0;
    call->shadow_return_ns = call->caller_resume_ns = 0;
out:
    __atomic_store_n(&client->invoking, 0, __ATOMIC_RELEASE);
    if (rc) errno = saved_errno;
    return rc;
}
int ud_lrpc_sample_remote_heap(struct lrpc_handle *h, uint64_t *value,
                               uint64_t *elapsed_ns)
{
    struct ud_client *client;
    struct timespec before, after;
    volatile const uint64_t *remote;
    if (!h || h->fd < 0 || h->astack_offset != UD_HANDLE_MARKER ||
        !h->astack_map || !value || !elapsed_ns)
        return ud_error(-EINVAL);
    client = h->astack_map;
    if (!client->region.mapped || !client->region.base)
        return ud_error(-ENOTCONN);
    remote = (volatile const uint64_t *)((const char *)client->region.base + UD_HEAP);
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &before)) return -1;
    *value = *remote;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &after)) return -1;
    *elapsed_ns = (uint64_t)(after.tv_sec - before.tv_sec) * 1000000000ULL +
                  (uint64_t)(after.tv_nsec - before.tv_nsec);
    return 0;
}
void ud_lrpc_close(struct lrpc_handle *h)
{
    struct ud_client *client = h->astack_map;
    free(h->invoke_scratch);
    if (client && client->local_args) munmap(client->local_args, client->map_size);
    if (h->fd >= 0) close(h->fd);
    if (client) {
        int rc = lrpc_ubsm_close(&client->region, 0);
        if (rc) fprintf(stderr, "UB_DOMAIN_CLOSE_FAIL error=%d\n", rc);
        free(client);
    }
    memset(h, 0, sizeof(*h)); h->fd = -1;
    __sync_lock_release(&client_active);
}
