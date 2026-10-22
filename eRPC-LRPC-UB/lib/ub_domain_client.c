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
#include <pthread.h>
#include <lrpc/lrpc_ubsm_pa.h>
#include <lrpc/ub_domain_layout.h>
#include <lrpc/ub_nested_path.h>
#include <lrpc/ub_domain_pool.h>
#include <lrpc/ub_social_client.h>
#include "ub_domain_client.h"

struct ud_client {
    struct lrpc_ubsm_region region;
    void *local_args;
    size_t page_size, map_size;
    unsigned int invoking;
    unsigned int nested_path;
    unsigned int profile;
    pthread_t owner;
    struct ud_connection_info info;
};
static int ud_error(int rc) { errno = rc < 0 ? -rc : EIO; return -1; }

static int ud_bind_profile(struct lrpc_handle *h, uint32_t procedure, uint64_t epoch, unsigned int profile)
{
    const char *name = getenv("LRPC_UB_DOMAIN_NAME");
    struct ud_client *client;
    struct ud_prepare p = {0};
    struct ud_publication *pub;
    struct ud_pool_publication *pool_pub = NULL;
    struct ud_pool_header header = {0};
    struct ud_pool_prepare pool = {0};
    ubsmem_shmem_info_t info;
    struct lrpc_ubsm_pa address;
    struct timespec begin, now;
    cpu_set_t set;
    int rc, saved;
    unsigned int d, l;
    unsigned int nested_path;
    if (!h || procedure < 1 || procedure > 3 || epoch != 1) return ud_error(-EINVAL);
    if (ud_nested_path_parse_profile(getenv("LRPC_UB_NESTED_PATH"), profile, &nested_path)) return -1;
    memset(h, 0, sizeof(*h)); h->fd = -1; h->astack_offset = UD_HANDLE_MARKER;
    client = calloc(1, sizeof(*client));
    if (!client) return -1;
    h->astack_map = client;
    client->nested_path = nested_path;
    client->profile = profile;
    client->owner = pthread_self();
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
    if (__atomic_load_n((uint64_t *)client->region.base, __ATOMIC_ACQUIRE) == UD_POOL_MAGIC) {
        memcpy(&header, client->region.base, sizeof(header));
        if (!ud_pool_header_valid(&header)) { errno = EPROTO; goto fail; }
        client->info.multi = 1;
    }
    if (profile && !client->info.multi) { errno = EPROTONOSUPPORT; goto fail; }
    if (client->info.multi) {
        struct ud_profile_header tag;
        memcpy(&tag, (char *)client->region.base+UD_PROFILE_OFFSET, sizeof(tag));
        if ((tag.magic == UD_PROFILE_MAGIC && (tag.abi != UD_POOL_ABI || tag.profile != profile)) ||
            (tag.magic != UD_PROFILE_MAGIC && profile)) { errno = EPROTONOSUPPORT; goto fail; }
    }
    h->fd = open("/dev/ub_lrpc_domain", O_RDWR | O_CLOEXEC);
    if (h->fd < 0) goto fail;
    if (profile && ioctl(h->fd, UD_SET_PROFILE, &profile)) goto fail;
    if (nested_path == UD_NESTED_FUSED &&
        ioctl(h->fd, UD_SET_NESTED_PATH, &nested_path)) {
        fprintf(stderr, "UB_DOMAIN_FAIL nested_path=fused requires updated module errno=%d\n", errno);
        goto fail;
    }
    p.import_va = (uintptr_t)client->region.base;
    if (client->info.multi) {
        pool.domain = p; pool.abi = UD_POOL_ABI; pool.generation = header.generation;
        if (ioctl(h->fd, UD_POOL_PREPARE, &pool)) goto fail;
        if (pool.slot >= UD_CLIENTS || pool.generation != header.generation || pool.abi != UD_POOL_ABI) {
            errno = EPROTO; goto fail;
        }
        p = pool.domain;
        client->info.slot = pool.slot; client->info.generation = pool.generation;
    } else if (ioctl(h->fd, UD_PREPARE, &p)) goto fail;
    client->info.abi = UD_POOL_ABI;
    client->info.identity = p.contract.nonce;
    client->info.capacity = client->info.multi ? UD_CLIENTS : 1;
    if (!ud_contract_valid(&p.contract)) { errno = EPROTO; goto fail; }
    /* Check independent SDK/OBMM and kernel VMA translations agree. */
    for (d = 0; d < UD_DOMAINS; ++d) for (l = 0; l < 4; ++l) {
        rc = lrpc_ubsm_query_pa(&client->region, &info,
                client->info.multi ? UD_POOL_TABLE(pool.slot,d,l) : UD_TABLE(d,l), &address);
        if (rc) { ud_error(rc); goto fail; }
        if (address.pa != p.contract.table_pa[d][l]) { errno = ESTALE; goto fail; }
    }
    rc = lrpc_ubsm_query_pa(&client->region, &info, UD_HEAP, &address);
    if (rc) { ud_error(rc); goto fail; }
    if (address.pa != p.contract.heap_pa) { errno = ESTALE; goto fail; }
    if (profile == UD_PROFILE_SOCIAL_WRITE) {
        rc = lrpc_ubsm_query_pa(&client->region, &info, UD_WRITE_HEAP(pool.slot), &address);
        if (rc) { ud_error(rc); goto fail; }
        if (address.pa != p.contract.write_pa) { errno = ESTALE; goto fail; }
    } else if (p.contract.write_pa) { errno = EPROTO; goto fail; }
    if (client->info.multi) {
        pool_pub = (void *)((char *)client->region.base + UD_POOL_SLOT(pool.slot));
        pub = &pool_pub->pub;
        if (__atomic_load_n(&pub->request, __ATOMIC_ACQUIRE) !=
            __atomic_load_n(&pub->complete, __ATOMIC_ACQUIRE)) { errno = EBUSY; goto fail; }
        if (pub->request == p.contract.nonce) { errno = ESTALE; goto fail; }
    } else {
        pub = client->region.base;
        if (pub->request || pub->complete) { errno = EBUSY; goto fail; }
    }
    memcpy(&pub->contract, &p.contract, sizeof(p.contract));
    __atomic_store_n(&pub->request, p.contract.nonce, __ATOMIC_RELEASE);
    if (clock_gettime(CLOCK_MONOTONIC, &begin)) goto fail;
    while (__atomic_load_n(&pub->complete, __ATOMIC_ACQUIRE) != p.contract.nonce) {
        if (clock_gettime(CLOCK_MONOTONIC, &now)) goto fail;
        if (now.tv_sec - begin.tv_sec >= 30) { errno = ETIMEDOUT; goto fail; }
        usleep(1000);
    }
    if (pool_pub && (pool_pub->error || pool_pub->reserved)) { errno = EPROTO; goto fail; }
    if (ioctl(h->fd, UD_COMMIT)) goto fail;
    if (client->info.multi) {
        struct ud_connection_info confirmed = {0};
        if (ioctl(h->fd, UD_CONNECTION_INFO, &confirmed)) goto fail;
        if (memcmp(&confirmed, &client->info, sizeof(confirmed))) { errno = ESTALE; goto fail; }
    }
    {
        long page_size = sysconf(_SC_PAGESIZE);
        if (page_size < UD_PAGE) { errno = EINVAL; goto fail; }
        client->page_size = (size_t)page_size;
        client->map_size = UD_DOMAINS * client->page_size;
        client->local_args = mmap(NULL, client->map_size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, h->fd, 0);
        if (client->local_args == MAP_FAILED) { client->local_args = NULL; goto fail; }
    }
    h->epoch = 1;
    printf("UB_DOMAIN_BOUND cpu=%d page_tables=B heap=B code=A astack=A estack=A "
           "shadow_pid=0 tlb=retain astack=mmap steady_ioctls=1 "
           "payload_user_kernel_copies=0 context_save=%s "
           "call_object=connection-reuse nested_path=%s multi=%u slot=%u identity=%llu generation=%llu service_profile=%s\n",
           h->shadow_cpu, profile ? "none" : "middle-suspend-only", ud_nested_path_name(nested_path),
           client->info.multi, client->info.slot, (unsigned long long)client->info.identity,
           (unsigned long long)client->info.generation,
           profile == UD_PROFILE_SOCIAL_WRITE ? "social-write-probe" :
           profile ? "social-user-read" : "micro");
    return 0;
fail:
    saved = errno;
    ud_lrpc_close(h);
    errno = saved;
    return -1;
}
int ud_lrpc_bind(struct lrpc_handle *h, uint32_t procedure, uint64_t epoch)
{
    return ud_bind_profile(h, procedure, epoch, UD_PROFILE_MICRO);
}
int us_client_open(struct lrpc_handle *h)
{
    return ud_bind_profile(h, 1, 1, UD_PROFILE_SOCIAL);
}
int us_client_open_write(struct lrpc_handle *h)
{
    return ud_bind_profile(h, 1, 1, UD_PROFILE_SOCIAL_WRITE);
}
int us_client_info(const struct lrpc_handle *h, struct ud_connection_info *info)
{
    return ud_lrpc_connection_info(h, info);
}
int us_client_close(struct lrpc_handle *h) { return ud_lrpc_close_checked(h); }
int us_client_call(struct lrpc_handle *h, struct us_call *call)
{
    struct ud_client *client;
    struct ud_mapped *m;
    struct us_call *mapped;
    unsigned int idle = 0;
    uint64_t previous;
    int rc, saved;
    if (!h || h->fd < 0 || h->astack_offset != UD_HANDLE_MARKER || !h->astack_map || !call ||
        call->abi != US_ABI || call->direction > US_REVERSE) return ud_error(-EINVAL);
    client = h->astack_map;
    if ((client->profile != UD_PROFILE_SOCIAL &&
         client->profile != UD_PROFILE_SOCIAL_WRITE) || !client->local_args)
        return ud_error(-ENOTCONN);
    if (!pthread_equal(client->owner, pthread_self())) return ud_error(-EPERM);
    if (sched_getcpu() != h->shadow_cpu) return ud_error(-EXDEV);
    if (!__atomic_compare_exchange_n(&client->invoking, &idle, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return ud_error(-EBUSY);
    m = client->local_args;
    mapped = (void *)((char *)m+US_MAPPED_OFFSET);
    previous = __atomic_load_n(&m->completion, __ATOMIC_ACQUIRE);
    *mapped = *call;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    rc = ioctl(h->fd, UD_SOCIAL_CALL); saved = errno;
    if (rc) fprintf(stderr, "UB_SOCIAL_CALL_FAIL errno=%d esr=0x%llx far=0x%llx\n",
                    saved, (unsigned long long)m->esr, (unsigned long long)m->far);
    if (!rc && (__atomic_load_n(&m->completion, __ATOMIC_ACQUIRE) == previous ||
                m->status || m->esr || mapped->fault_vector || mapped->entries != 2 ||
                mapped->transitions != (client->nested_path == UD_NESTED_FUSED ? 3U : 4U) ||
                mapped->cpu != (unsigned int)h->shadow_cpu ||
                sched_getcpu() != h->shadow_cpu)) { rc = -1; saved = EPROTO; }
    *call = *mapped;
    __atomic_store_n(&client->invoking, 0, __ATOMIC_RELEASE);
    if (rc) errno = saved;
    return rc;
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
    if (client->profile != UD_PROFILE_MICRO) return ud_error(-EOPNOTSUPP);
    if (!client->local_args) return ud_error(-ENOTCONN);
    if (!pthread_equal(client->owner, pthread_self())) return ud_error(-EPERM);
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
        m->transitions != (call->procedure_id == 2 ?
                          ud_nested_transitions(client->nested_path) : 2U)) {
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
int ud_lrpc_close_checked(struct lrpc_handle *h)
{
    struct ud_client *client;
    int rc;
    if (!h) return ud_error(-EINVAL);
    client = h->astack_map;
    if (client && !pthread_equal(client->owner, pthread_self())) return ud_error(-EPERM);
    if (client && client->local_args) {
        if (munmap(client->local_args, client->map_size)) return -1;
        client->local_args = NULL;
    }
    if (h->fd >= 0) { rc = close(h->fd); h->fd = -1; if (rc) return -1; }
    if (client) {
        rc = lrpc_ubsm_close(&client->region, 0);
        /* Retain the region on failure so the owner may retry cleanup. */
        if (rc) return ud_error(rc);
        free(client);
    }
    memset(h, 0, sizeof(*h)); h->fd = -1;
    return 0;
}
void ud_lrpc_close(struct lrpc_handle *h)
{
    if (ud_lrpc_close_checked(h)) fprintf(stderr, "UB_DOMAIN_CLOSE_FAIL errno=%d\n", errno);
}
int ud_lrpc_connection_info(const struct lrpc_handle *h, struct ud_connection_info *info)
{
    const struct ud_client *client;
    if (!h || !info || h->fd < 0 || h->astack_offset != UD_HANDLE_MARKER || !h->astack_map)
        return ud_error(-EINVAL);
    client = h->astack_map;
    *info = client->info;
    return 0;
}
