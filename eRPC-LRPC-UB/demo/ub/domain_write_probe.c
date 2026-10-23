#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <lrpc/ub_social_client.h>

#define WRITE_MAX_CELLS 64U

struct options {
    unsigned int cpus[UD_CLIENTS], workers, calls, rewrites, observe_ms;
};
struct worker_result {
    unsigned int ready, done, error, closed, transitions;
    struct ud_connection_info info;
    uint64_t expected[WRITE_MAX_CELLS + 1];
};
struct campaign {
    struct options options;
    struct worker_result worker[UD_CLIENTS];
    unsigned int start, release, abort;
};
struct worker_arg { struct campaign *campaign; unsigned int id; };

static unsigned int load(const unsigned int *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static void store(unsigned int *p, unsigned int value)
{
    __atomic_store_n(p, value, __ATOMIC_RELEASE);
}
static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static int wait_flag(struct campaign *c, const unsigned int *flag)
{
    const uint64_t begin = now_ns();
    while (!load(flag)) {
        if (load(&c->abort)) { errno = ECANCELED; return -1; }
        if (!begin || now_ns() - begin > UINT64_C(60000000000)) {
            errno = ETIMEDOUT; return -1;
        }
        usleep(1000);
    }
    return 0;
}
static int number(const char *s, unsigned int low, unsigned int high,
                  unsigned int *value)
{
    char *end;
    unsigned long result;
    if (!s[0] || strspn(s, "0123456789") != strlen(s)) return -1;
    errno = 0; result = strtoul(s, &end, 10);
    if (errno || *end || result < low || result > high) return -1;
    *value = (unsigned int)result;
    return 0;
}
static int parse_cpus(const char *text, struct options *o, const cpu_set_t *allowed)
{
    while (*text) {
        char item[16];
        size_t length = strcspn(text, ",");
        unsigned int cpu, i;
        if (!length || length >= sizeof(item) || o->workers == UD_CLIENTS) return -1;
        memcpy(item, text, length); item[length] = 0;
        if (number(item, 0, CPU_SETSIZE - 1, &cpu) || !CPU_ISSET(cpu, allowed)) return -1;
        for (i = 0; i < o->workers; ++i) if (o->cpus[i] == cpu) return -1;
        o->cpus[o->workers++] = cpu;
        text += length;
        if (*text == ',') { ++text; if (!*text) return -1; }
    }
    return o->workers ? 0 : -1;
}
static int parse(int argc, char **argv, struct options *o)
{
    const char *cpus = NULL;
    cpu_set_t allowed;
    int i;
    *o = (struct options){.calls = 1, .observe_ms = 2000};
    for (i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--help")) return 1;
        if (++i == argc) return -1;
        if (!strcmp(key, "--cpus")) cpus = argv[i];
        else if (!strcmp(key, "--calls")) {
            if (number(argv[i], 1, WRITE_MAX_CELLS, &o->calls)) return -1;
        } else if (!strcmp(key, "--rewrites")) {
            if (number(argv[i], 0, 10000, &o->rewrites)) return -1;
        } else if (!strcmp(key, "--observe-ms")) {
            if (number(argv[i], 0, 30000, &o->observe_ms)) return -1;
        } else return -1;
    }
    if (sched_getaffinity(0, sizeof(allowed), &allowed)) return -1;
    if (cpus) return parse_cpus(cpus, o, &allowed);
    if (CPU_COUNT(&allowed) != 1) return -1;
    for (i = 0; i < CPU_SETSIZE; ++i) if (CPU_ISSET(i, &allowed)) {
        o->cpus[0] = (unsigned int)i; o->workers = 1; return 0;
    }
    return -1;
}
static uint64_t write_value(unsigned int worker, unsigned int sequence)
{
    /* The kernel accepts only nonzero 32-bit values. Keep workers distinct. */
    return 41U + (uint64_t)worker * 1000000U + sequence;
}
static int invoke(struct lrpc_handle *handle, unsigned int worker,
                  unsigned int cpu, unsigned int index, unsigned int sequence,
                  unsigned int *transitions)
{
    struct us_call call = {0};
    const uint64_t value = write_value(worker, sequence);
    call.abi = US_ABI; call.direction = US_FORWARD;
    call.message.source = worker + 1;
    call.message.request = index;
    call.message.value = value;
    call.message.type = US_WRITE_REQ;
    if (us_client_call(handle, &call)) return -1;
    if (call.message.source != worker + 1 || call.message.request != index ||
        call.message.value != value || call.message.type != US_WRITE_REQ ||
        call.message.ticket != value || call.message.status ||
        call.message.action != US_ACTION_RPC || call.message.target != 104 ||
        call.fault_vector ||
        call.entries != 2 || call.domain[0] != 1 || call.domain[1] != 2 ||
        call.cpu != cpu || (call.transitions != 3 && call.transitions != 4)) {
        errno = EPROTO; return -1;
    }
    *transitions = call.transitions;
    return 0;
}
static void *worker(void *pointer)
{
    struct worker_arg *a = pointer;
    struct campaign *c = a->campaign;
    const struct options *o = &c->options;
    struct worker_result *r = &c->worker[a->id];
    struct lrpc_handle handle = {.fd = -1};
    cpu_set_t cpus;
    unsigned int i;
    int opened = 0;
    CPU_ZERO(&cpus); CPU_SET(o->cpus[a->id], &cpus);
    if (sched_setaffinity(0, sizeof(cpus), &cpus)) goto fail;
    if (us_client_open_write(&handle)) goto fail;
    opened = 1;
    if (us_client_info(&handle, &r->info) || !r->info.multi ||
        r->info.slot >= UD_CLIENTS || !r->info.identity ||
        r->info.capacity != UD_CLIENTS) { errno = EPROTO; goto fail; }
    store(&r->ready, 1);
    if (wait_flag(c, &c->start)) goto fail;
    for (i = 1; i <= o->calls; ++i) {
        if (load(&c->abort)) { errno = ECANCELED; goto fail; }
        if (invoke(&handle, a->id, o->cpus[a->id], i, i,
                   &r->transitions)) goto fail;
        r->expected[i] = write_value(a->id, i);
    }
    for (i = 1; i <= o->rewrites; ++i) {
        const unsigned int sequence = o->calls + i;
        if (load(&c->abort)) { errno = ECANCELED; goto fail; }
        if (invoke(&handle, a->id, o->cpus[a->id], 1,
                   sequence, &r->transitions)) goto fail;
        r->expected[1] = write_value(a->id, sequence);
    }
    store(&r->done, 1);
    if (wait_flag(c, &c->release)) goto fail;
    goto out;
fail:
    fprintf(stderr, "UB_DOMAIN_WRITE_WORKER_FAIL worker=%u cpu=%u errno=%d\n",
            a->id, o->cpus[a->id], errno);
    store(&r->error, 1); store(&c->abort, 1);
out:
    if (opened && us_client_close(&handle)) {
        fprintf(stderr, "UB_DOMAIN_WRITE_CLOSE_FAIL worker=%u errno=%d\n", a->id, errno);
        store(&r->error, 1); store(&c->abort, 1);
    }
    store(&r->closed, 1);
    return NULL;
}
int main(int argc, char **argv)
{
    struct campaign c = {0};
    struct worker_arg args[UD_CLIENTS];
    pthread_t threads[UD_CLIENTS];
    unsigned int i, j, created = 0;
    int result = parse(argc, argv, &c.options), failed = 0;
    if (result) {
        fprintf(result < 0 ? stderr : stdout,
                "usage: %s [--cpus N[,N...]] [--calls 1..64] "
                "[--rewrites 0..10000] [--observe-ms 0..30000]\n"
                "without --cpus, first pin the process with taskset -c N\n", argv[0]);
        return result < 0 ? 2 : 0;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (i = 0; i < c.options.workers; ++i) {
        int rc;
        args[i] = (struct worker_arg){&c, i};
        rc = pthread_create(&threads[i], NULL, worker, &args[i]);
        if (rc) { errno = rc; failed = 1; break; }
        ++created;
    }
    if (failed) store(&c.abort, 1);
    for (i = 0; !failed && i < c.options.workers; ++i)
        if (wait_flag(&c, &c.worker[i].ready)) failed = 1;
    for (i = 0; !failed && i < c.options.workers; ++i)
        for (j = 0; j < i; ++j)
            if (c.worker[i].info.slot == c.worker[j].info.slot ||
                c.worker[i].info.identity == c.worker[j].info.identity ||
                c.worker[i].info.generation != c.worker[j].info.generation) failed = 1;
    if (failed) store(&c.abort, 1);
    else store(&c.start, 1);
    for (i = 0; !failed && i < c.options.workers; ++i)
        if (wait_flag(&c, &c.worker[i].done)) failed = 1;
    if (failed) store(&c.abort, 1);
    else {
        for (i = 0; i < c.options.workers; ++i) {
            const struct worker_result *r = &c.worker[i];
            for (j = 1; j <= c.options.calls; ++j)
                printf("UB_DOMAIN_WRITE_A_PASS worker=%u cpu=%u slot=%u identity=%llu "
                       "index=%u value=%llu ticket=%llu transitions=%u "
                       "b_observation=check_owner_log\n",
                       i, c.options.cpus[i], r->info.slot,
                       (unsigned long long)r->info.identity, j,
                       (unsigned long long)r->expected[j],
                       (unsigned long long)r->expected[j], r->transitions);
        }
        /* B polls asynchronously. Keep every connection/page alive for its scan. */
        for (i = 0; i < c.options.observe_ms && !load(&c.abort); ++i)
            usleep(1000);
    }
    store(&c.release, 1);
    for (i = 0; i < created; ++i) {
        pthread_join(threads[i], NULL);
        if (load(&c.worker[i].error) || !load(&c.worker[i].closed)) failed = 1;
    }
    if (!failed)
        printf("UB_DOMAIN_WRITE_CAMPAIGN_PASS workers=%u calls_per_worker=%u "
               "rewrites_per_worker=%u final_cells=%u b_observation=check_owner_log\n",
               c.options.workers, c.options.calls, c.options.rewrites,
               c.options.workers * c.options.calls);
    else fprintf(stderr, "UB_DOMAIN_WRITE_CAMPAIGN_FAIL workers_created=%u\n", created);
    return failed ? 1 : 0;
}
