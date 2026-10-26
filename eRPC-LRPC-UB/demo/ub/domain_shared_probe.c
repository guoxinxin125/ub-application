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

struct campaign {
    unsigned int cpus[UD_CLIENTS], workers, rounds, increments, observe_ms;
    unsigned int phase, written, reads, atomic_start, final_start, release, abort;
    struct worker {
        unsigned int ready, atomic_done, final_done, closed, error;
        unsigned int returned_successes, failed_request, atomic_call_failed;
        unsigned int counter_snapshot_valid;
        int atomic_errno, counter_snapshot_errno;
        struct ud_connection_info info;
        struct us_call failed_call;
        uint64_t *tickets, last_old, final_counter, counter_snapshot;
    } worker[UD_CLIENTS];
};
struct worker_arg { struct campaign *campaign; unsigned int id; };

static unsigned int get(const unsigned int *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void set(unsigned int *p, unsigned int v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static uint64_t now_ns(void)
{
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? 0 :
           (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
static int await(struct campaign *c, const unsigned int *flag, unsigned int minimum)
{
    uint64_t start = now_ns();
    while (get(flag) < minimum) {
        if (get(&c->abort)) { errno = ECANCELED; return -1; }
        if (!start || now_ns() - start > UINT64_C(60000000000)) {
            errno = ETIMEDOUT; return -1;
        }
        usleep(1000);
    }
    return 0;
}
static int number(const char *s, unsigned int low, unsigned int high, unsigned int *out)
{
    char *end;
    unsigned long n;
    if (!*s || strspn(s, "0123456789") != strlen(s)) return -1;
    errno = 0; n = strtoul(s, &end, 10);
    if (errno || *end || n < low || n > high) return -1;
    *out = (unsigned int)n; return 0;
}
static int options(int argc, char **argv, struct campaign *c)
{
    cpu_set_t allowed;
    const char *cpus = NULL;
    int i;
    c->rounds = 4; c->increments = 1000; c->observe_ms = 2000;
    for (i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--help")) return 1;
        if (++i == argc) return -1;
        if (!strcmp(key, "--cpus")) cpus = argv[i];
        else if (!strcmp(key, "--rounds")) {
            if (number(argv[i], 2, 64, &c->rounds)) return -1;
        } else if (!strcmp(key, "--increments")) {
            if (number(argv[i], 1, 10000, &c->increments)) return -1;
        } else if (!strcmp(key, "--observe-ms")) {
            if (number(argv[i], 0, 30000, &c->observe_ms)) return -1;
        } else return -1;
    }
    if (!cpus || sched_getaffinity(0, sizeof(allowed), &allowed)) return -1;
    while (*cpus) {
        char item[16];
        size_t length = strcspn(cpus, ",");
        unsigned int cpu, j;
        if (!length || length >= sizeof(item) || c->workers == UD_CLIENTS) return -1;
        memcpy(item, cpus, length); item[length] = 0;
        if (number(item, 0, CPU_SETSIZE - 1, &cpu) || !CPU_ISSET(cpu, &allowed)) return -1;
        for (j = 0; j < c->workers; ++j) if (c->cpus[j] == cpu) return -1;
        c->cpus[c->workers++] = cpu;
        cpus += length;
        if (*cpus == ',') { ++cpus; if (!*cpus) return -1; }
    }
    return c->workers >= 2 ? 0 : -1;
}
static uint64_t token(unsigned int writer, unsigned int round)
{
    return 41U + writer * 1000U + round;
}
static int invoke(struct lrpc_handle *h, unsigned int id, unsigned int cpu,
                  unsigned int type, unsigned int index, uint64_t value,
                  uint64_t *ticket, struct us_call *observed)
{
    struct us_call call = {0};
    int rc, saved;
    call.abi = US_ABI; call.direction = US_FORWARD;
    call.message.source = id + 1; call.message.type = type;
    call.message.request = index; call.message.value = value;
    rc = us_client_call(h, &call); saved = errno;
    if (observed) *observed = call;
    if (rc) { errno = saved; return -1; }
    if (call.message.source != id + 1 || call.message.type != type ||
        call.message.request != index || call.message.value != value ||
        call.message.action != US_ACTION_RPC || call.message.target != 105 ||
        call.message.status || call.fault_vector || call.entries != 2 ||
        call.domain[0] != 1 || call.domain[1] != 2 || call.cpu != cpu ||
        (call.transitions != 3 && call.transitions != 4)) {
        errno = EPROTO; return -1;
    }
    *ticket = call.message.ticket;
    return 0;
}
static void *run_worker(void *pointer)
{
    struct worker_arg *a = pointer;
    struct campaign *c = a->campaign;
    struct worker *w = &c->worker[a->id];
    struct lrpc_handle h = {.fd = -1};
    cpu_set_t affinity;
    unsigned int round, i, cpu = c->cpus[a->id];
    uint64_t ticket;
    int opened = 0;
    CPU_ZERO(&affinity); CPU_SET(cpu, &affinity);
    if (sched_setaffinity(0, sizeof(affinity), &affinity) ||
        us_client_open_shared(&h)) goto fail;
    opened = 1;
    if (us_client_info(&h, &w->info) || !w->info.multi ||
        w->info.slot >= UD_CLIENTS || !w->info.identity ||
        w->info.capacity != UD_CLIENTS) { errno = EPROTO; goto fail; }
    if (a->id == 0) {
        if (invoke(&h, a->id, cpu, US_SHARED_READ_REQ,
                   US_SHARED_VALUE_INDEX, 1, &ticket, NULL)) goto fail;
        if (ticket) { errno = EBUSY; goto fail; }
        if (invoke(&h, a->id, cpu, US_SHARED_READ_REQ,
                   US_SHARED_COUNTER_INDEX, 1, &ticket, NULL)) goto fail;
        if (ticket) { errno = EBUSY; goto fail; }
    }
    set(&w->ready, 1);
    for (round = 1; round <= c->rounds; ++round) {
        unsigned int writer = (round - 1) % c->workers;
        uint64_t expected = token(writer, round);
        if (await(c, &c->phase, round)) goto fail;
        if (writer == a->id) {
            if (invoke(&h, a->id, cpu, US_SHARED_WRITE_REQ,
                       US_SHARED_VALUE_INDEX, expected, &ticket, NULL)) goto fail;
            if (ticket != expected) { errno = EPROTO; goto fail; }
            set(&c->written, round);
        }
        if (await(c, &c->written, round) ||
            invoke(&h, a->id, cpu, US_SHARED_READ_REQ,
                   US_SHARED_VALUE_INDEX, 1, &ticket, NULL)) goto fail;
        if (ticket != expected) { errno = EPROTO; goto fail; }
        __atomic_fetch_add(&c->reads, 1, __ATOMIC_RELEASE);
    }
    if (await(c, &c->atomic_start, 1)) goto fail;
    for (i = 0; i < c->increments; ++i) {
        struct us_call observed;
        if (get(&c->abort)) { errno = ECANCELED; goto fail; }
        if (invoke(&h, a->id, cpu, US_SHARED_FETCH_ADD_REQ,
                   US_SHARED_COUNTER_INDEX, 1, &ticket, &observed)) {
            int saved = errno;
            w->atomic_call_failed = 1;
            w->failed_request = i + 1;
            w->atomic_errno = saved;
            w->failed_call = observed;
            set(&c->abort, 1);
            /* EAGAIN leaves the session usable. This is a racing snapshot,
             * not the final counter: the peer may have a call in flight. */
            if (saved == EAGAIN && !observed.fault_vector) {
                if (invoke(&h, a->id, cpu, US_SHARED_READ_REQ,
                           US_SHARED_COUNTER_INDEX, 1, &w->counter_snapshot, NULL))
                    w->counter_snapshot_errno = errno;
                else
                    w->counter_snapshot_valid = 1;
            }
            errno = saved;
            goto fail;
        }
        w->tickets[i] = ticket;
        w->last_old = ticket;
        w->returned_successes = i + 1;
    }
    set(&w->atomic_done, 1);
    if (a->id == 0) {
        if (await(c, &c->final_start, 1) ||
            invoke(&h, a->id, cpu, US_SHARED_READ_REQ,
                   US_SHARED_COUNTER_INDEX, 1, &w->final_counter, NULL)) goto fail;
        set(&w->final_done, 1);
    }
    if (await(c, &c->release, 1)) goto fail;
    goto out;
fail:
    fprintf(stderr, "UB_DOMAIN_SHARED_WORKER_FAIL worker=%u cpu=%u errno=%d\n",
            a->id, cpu, errno);
    set(&w->error, 1); set(&c->abort, 1);
out:
    if (opened && us_client_close(&h)) {
        fprintf(stderr, "UB_DOMAIN_SHARED_CLOSE_FAIL worker=%u errno=%d\n", a->id, errno);
        set(&w->error, 1); set(&c->abort, 1);
    }
    set(&w->closed, 1);
    return NULL;
}
int main(int argc, char **argv)
{
    struct campaign c = {0};
    struct worker_arg args[UD_CLIENTS];
    pthread_t threads[UD_CLIENTS];
    unsigned char *seen = NULL;
    unsigned int i, j, created = 0, total;
    int rc = options(argc, argv, &c), failed = 0;
    if (rc) {
        fprintf(rc < 0 ? stderr : stdout,
                "usage: %s --cpus N,N[,N...] [--rounds 2..64] "
                "[--increments 1..10000] [--observe-ms 0..30000]\n"
                "pin the process with taskset -c N,N[,N...] first\n", argv[0]);
        return rc < 0 ? 2 : 0;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    total = c.workers * c.increments;
    seen = calloc(total, 1);
    if (!seen) return 1;
    for (i = 0; i < c.workers; ++i) {
        c.worker[i].tickets = calloc(c.increments, sizeof(uint64_t));
        if (!c.worker[i].tickets) { failed = 1; break; }
    }
    for (i = 0; !failed && i < c.workers; ++i) {
        args[i] = (struct worker_arg){&c, i};
        rc = pthread_create(&threads[i], NULL, run_worker, &args[i]);
        if (rc) { errno = rc; failed = 1; break; }
        ++created;
    }
    if (failed) set(&c.abort, 1);
    for (i = 0; !failed && i < c.workers; ++i)
        if (await(&c, &c.worker[i].ready, 1)) failed = 1;
    for (i = 0; !failed && i < c.workers; ++i)
        for (j = 0; j < i; ++j)
            if (c.worker[i].info.slot == c.worker[j].info.slot ||
                c.worker[i].info.identity == c.worker[j].info.identity ||
                c.worker[i].info.generation != c.worker[j].info.generation) failed = 1;
    if (!failed) for (i = 1; i <= c.rounds; ++i) {
        set(&c.phase, i);
        if (await(&c, &c.reads, i * c.workers)) { failed = 1; break; }
    }
    if (!failed) {
        printf("UB_DOMAIN_SHARED_READ_PASS workers=%u rounds=%u generation=%llu final_cell=%llu\n",
               c.workers, c.rounds, (unsigned long long)c.worker[0].info.generation,
               (unsigned long long)token((c.rounds - 1) % c.workers, c.rounds));
        printf("UB_DOMAIN_SHARED_ATOMIC_START workers=%u increments_per_worker=%u "
               "expected_total=%u generation=%llu\n",
               c.workers, c.increments, total,
               (unsigned long long)c.worker[0].info.generation);
        set(&c.atomic_start, 1);
    }
    for (i = 0; !failed && i < c.workers; ++i)
        if (await(&c, &c.worker[i].atomic_done, 1)) failed = 1;
    if (!failed) {
        for (i = 0; i < c.workers; ++i) for (j = 0; j < c.increments; ++j) {
            uint64_t old = c.worker[i].tickets[j];
            if (old >= total || seen[old]) { failed = 1; break; }
            seen[old] = 1;
        }
    }
    if (!failed) {
        set(&c.final_start, 1);
        if (await(&c, &c.worker[0].final_done, 1) || c.worker[0].final_counter != total)
            failed = 1;
    }
    if (!failed) {
        printf("UB_DOMAIN_SHARED_ATOMIC_PASS workers=%u increments_per_worker=%u "
               "unique_old_values=%u final_counter=%llu\n",
               c.workers, c.increments, total,
               (unsigned long long)c.worker[0].final_counter);
        for (i = 0; i < c.observe_ms && !get(&c.abort); ++i) usleep(1000);
    } else set(&c.abort, 1);
    set(&c.release, 1);
    for (i = 0; i < created; ++i) {
        pthread_join(threads[i], NULL);
        if (get(&c.worker[i].error) || !get(&c.worker[i].closed)) failed = 1;
    }
    if (failed && get(&c.atomic_start)) {
        unsigned int returned = 0, unique = 0, duplicates = 0, out_of_range = 0;
        memset(seen, 0, total);
        for (i = 0; i < created; ++i) {
            const struct worker *w = &c.worker[i];
            for (j = 0; j < w->returned_successes; ++j) {
                uint64_t old = w->tickets[j];
                ++returned;
                if (old >= total) ++out_of_range;
                else if (seen[old]) ++duplicates;
                else { seen[old] = 1; ++unique; }
            }
            fprintf(stderr, "UB_DOMAIN_SHARED_WORKER_DIAG worker=%u cpu=%u "
                    "returned_successes=%u last_old=%llu atomic_call_failed=%u "
                    "failed_request=%u "
                    "atomic_errno=%d status=%u failed_ticket=%llu entries=%u "
                    "transitions=%u fault_vector=%u counter_snapshot_valid=%u "
                    "counter_snapshot=%llu counter_snapshot_errno=%d\n",
                    i, c.cpus[i], w->returned_successes,
                    (unsigned long long)w->last_old, w->atomic_call_failed,
                    w->failed_request,
                    w->atomic_errno, w->failed_call.message.status,
                    (unsigned long long)w->failed_call.message.ticket,
                    w->failed_call.entries, w->failed_call.transitions,
                    w->failed_call.fault_vector, w->counter_snapshot_valid,
                    (unsigned long long)w->counter_snapshot,
                    w->counter_snapshot_errno);
        }
        fprintf(stderr, "UB_DOMAIN_SHARED_ATOMIC_DIAG expected_total=%u "
                "returned_successes=%u unique_old=%u duplicate_old=%u "
                "out_of_range_old=%u\n",
                total, returned, unique, duplicates, out_of_range);
    }
    if (!failed)
        printf("UB_DOMAIN_SHARED_CAMPAIGN_PASS workers=%u rounds=%u increments_per_worker=%u "
               "generation=%llu final_cell=%llu final_counter=%u b_observation=check_owner_log\n",
               c.workers, c.rounds, c.increments,
               (unsigned long long)c.worker[0].info.generation,
               (unsigned long long)token((c.rounds - 1) % c.workers, c.rounds), total);
    else fprintf(stderr, "UB_DOMAIN_SHARED_CAMPAIGN_FAIL workers_created=%u\n", created);
    for (i = 0; i < c.workers; ++i) free(c.worker[i].tickets);
    free(seen);
    return failed ? 1 : 0;
}
