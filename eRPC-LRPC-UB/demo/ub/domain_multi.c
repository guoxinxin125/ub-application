#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <lrpc/lrpc.h>
#include <lrpc/ub_nested_path.h>
#include "../../lib/ub_domain_client.h"

struct options {
    unsigned int cpus[UD_CLIENTS], workers, samples, warmup;
    const char *mode, *test, *path;
};
struct worker_result {
    unsigned int ready, measured, checked, done, error;
    struct ud_connection_info first, replacement;
    uint64_t minimum, p50, p99, maximum, total, calls;
};
struct campaign {
    struct worker_result worker[UD_CLIENTS];
    unsigned int start, check, closed, release, abort;
};
struct worker_arg { struct options *options; struct campaign *campaign; unsigned int id; };
static unsigned int load(const unsigned int *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void store(unsigned int *p, unsigned int v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &t)) return 0;
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static int wait_flag(struct campaign *c, unsigned int *flag)
{
    uint64_t begin = now_ns();
    while (!load(flag)) {
        if (load(&c->abort) || now_ns()-begin > 60000000000ULL) { errno = ETIMEDOUT; return -1; }
        usleep(100);
    }
    return load(&c->abort) ? -1 : 0;
}
static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x>y)-(x<y);
}
static int invoke(struct lrpc_handle *h, struct lrpc_astack *call,
                   const struct options *o, unsigned int id, unsigned int i)
{
    uint64_t input[2] = {((uint64_t)id+1)*1000000+i, 17+id}, expected, value;
    unsigned int procedure = !strcmp(o->test, "direct") ? 1 :
                             !strcmp(o->test, "nested") ? 2 : i%3+1;
    if (procedure == 3) { input[0] = 8; input[1] = 7; expected = 1007; }
    else expected = procedure == 1 ? input[0]+input[1]+100 : input[0]+2;
    call->abi = LRPC_ASTACK_ABI; call->procedure_id = procedure;
    call->request_size = procedure == 2 ? 8 : 16;
    memcpy(call->payload, input, call->request_size);
    if (lrpc_invoke(h, call)) return -1;
    memcpy(&value, call->payload, sizeof(value));
    if (call->response_size != 8 || call->status || value != expected ||
        call->caller_cpu_before != o->cpus[id] || call->caller_cpu_after != o->cpus[id]) {
        errno = EPROTO; return -1;
    }
    return 0;
}
static void *worker(void *pointer)
{
    struct worker_arg *a = pointer;
    struct options *o = a->options;
    struct campaign *c = a->campaign;
    struct worker_result *r = &c->worker[a->id];
    struct lrpc_handle h = {.fd = -1};
    struct lrpc_astack *call = calloc(1, sizeof(*call));
    uint64_t *samples = calloc(o->samples, sizeof(*samples));
    cpu_set_t set;
    unsigned int i;
    int bound = 0;
    if (!call || !samples) goto fail;
    CPU_ZERO(&set); CPU_SET(o->cpus[a->id], &set);
    if (sched_setaffinity(0, sizeof(set), &set)) goto fail;
    if (lrpc_bind(&h, "/dev/ub_lrpc_domain", 1, 1)) goto fail;
    bound = 1;
    if (ud_lrpc_connection_info(&h, &r->first) || !r->first.multi || !r->first.identity ||
        r->first.slot >= UD_CLIENTS || r->first.capacity != UD_CLIENTS) { errno = EPROTO; goto fail; }
    store(&r->ready, 1);
    if (wait_flag(c, &c->start)) goto fail;
    for (i = 0; i < o->warmup+o->samples; ++i) {
        uint64_t begin, elapsed;
        if (load(&c->abort)) goto fail;
        /* Includes request preparation and result verification, unlike the
         * single-worker benchmark's narrower lrpc_invoke-only scope. */
        begin = now_ns();
        if (invoke(&h, call, o, a->id, i)) goto fail;
        elapsed = now_ns()-begin; ++r->calls;
        if (i >= o->warmup) { samples[i-o->warmup] = elapsed; r->total += elapsed; }
    }
    qsort(samples, o->samples, sizeof(*samples), compare);
    r->minimum = samples[0]; r->maximum = samples[o->samples-1];
    r->p50 = samples[(o->samples-1)*50/100]; r->p99 = samples[(o->samples-1)*99/100];
    store(&r->measured, 1);
    if (wait_flag(c, &c->check)) goto fail;
    if (!a->id) {
        if (ud_lrpc_close_checked(&h)) goto fail;
        bound = 0;
        store(&c->closed, 1);
        if (lrpc_bind(&h, "/dev/ub_lrpc_domain", 1, 1)) goto fail;
        bound = 1;
        if (ud_lrpc_connection_info(&h, &r->replacement) || !r->replacement.multi ||
            r->replacement.slot != r->first.slot ||
            r->replacement.identity == r->first.identity ||
            r->replacement.generation != r->first.generation) { errno = EPROTO; goto fail; }
    } else if (wait_flag(c, &c->closed)) goto fail;
    /* Other connections remain open while worker zero closes/rebinds. */
    for (i = 0; i < 32; ++i) {
        if (invoke(&h, call, o, a->id, o->warmup+o->samples+i)) goto fail;
        ++r->calls;
    }
    store(&r->checked, 1);
    if (wait_flag(c, &c->release)) goto fail;
    goto out;
fail:
    fprintf(stderr, "UB_DOMAIN_MULTI_WORKER_FAIL worker=%u errno=%d\n", a->id, errno);
    store(&r->error, 1); store(&c->abort, 1);
out:
    if (bound && ud_lrpc_close_checked(&h)) {
        fprintf(stderr, "UB_DOMAIN_MULTI_CLOSE_FAIL worker=%u errno=%d\n", a->id, errno);
        store(&r->error, 1); store(&c->abort, 1);
    }
    free(call); free(samples); store(&r->done, 1);
    return NULL;
}
static int number(const char *s, unsigned int min, unsigned int max, unsigned int *out)
{
    char *end;
    unsigned long n;
    if (!s[0] || strspn(s, "0123456789") != strlen(s)) return -1;
    errno = 0; n = strtoul(s, &end, 10);
    if (errno || *end || n < min || n > max) return -1;
    *out = (unsigned int)n; return 0;
}
static int parse(int argc, char **argv, struct options *o)
{
    unsigned int path;
    int i;
    const char *cpus = NULL;
    cpu_set_t allowed;
    *o = (struct options){.samples=100, .warmup=10, .mode="threads", .test="mixed",
                          .path=getenv("LRPC_UB_NESTED_PATH")};
    if (!o->path) o->path = UD_NESTED_DEFAULT_NAME;
    for (i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--help")) return 1;
        if (++i == argc) return -1;
        if (!strcmp(key, "--cpus")) cpus = argv[i];
        else if (!strcmp(key, "--mode")) o->mode = argv[i];
        else if (!strcmp(key, "--test")) o->test = argv[i];
        else if (!strcmp(key, "--nested-path")) o->path = argv[i];
        else if (!strcmp(key, "--samples")) { if (number(argv[i],1,1000000,&o->samples)) return -1; }
        else if (!strcmp(key, "--warmup")) { if (number(argv[i],0,1000000,&o->warmup)) return -1; }
        else return -1;
    }
    if (!cpus || (strcmp(o->mode,"threads") && strcmp(o->mode,"processes")) ||
        (strcmp(o->test,"mixed") && strcmp(o->test,"direct") && strcmp(o->test,"nested")) ||
        ud_nested_path_parse(o->path,&path) || sched_getaffinity(0,sizeof(allowed),&allowed)) return -1;
    while (*cpus) {
        char item[16];
        size_t n = strcspn(cpus, ",");
        if (!n || n >= sizeof(item) || o->workers == UD_CLIENTS) return -1;
        memcpy(item, cpus, n); item[n] = 0;
        if (number(item,0,CPU_SETSIZE-1,&o->cpus[o->workers]) ||
            !CPU_ISSET(o->cpus[o->workers],&allowed)) return -1;
        ++o->workers; cpus += n;
        if (*cpus) { ++cpus; if (!*cpus) return -1; }
    }
    return o->workers >= 2 ? 0 : -1;
}
int main(int argc, char **argv)
{
    struct options o;
    struct campaign *c;
    struct worker_arg args[UD_CLIENTS];
    pthread_t threads[UD_CLIENTS];
    pid_t children[UD_CLIENTS];
    unsigned int i, j, created = 0, unique = 0;
    uint64_t calls = 0;
    int result = parse(argc,argv,&o), failed = 0;
    if (result) {
        fprintf(result < 0 ? stderr : stdout, "usage: %s --cpus N,N[,N...] [--mode threads|processes] "
                "[--test mixed|direct|nested] [--nested-path legacy|fused] [--samples N] [--warmup N]\n"
                "default nested path: fused\n",argv[0]);
        return result < 0 ? 2 : 0;
    }
    if (setenv("LRPC_EXECUTION_BACKEND","ub-remote-domain",1) ||
        setenv("LRPC_UB_NESTED_PATH",o.path,1)) return 1;
    setvbuf(stdout,NULL,_IOLBF,0);
    c = mmap(NULL,sizeof(*c),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if (c == MAP_FAILED) return 1;
    memset(c,0,sizeof(*c));
    /* No SDK state exists before fork. Forking an initialized SDK is unsupported. */
    for (i = 0; i < o.workers; ++i) {
        args[i] = (struct worker_arg){&o,c,i};
        if (!strcmp(o.mode,"threads")) {
            if (pthread_create(&threads[i],NULL,worker,&args[i])) { failed = 1; break; }
        } else {
            children[i] = fork();
            if (children[i] < 0) { failed = 1; break; }
            if (!children[i]) { worker(&args[i]); _exit(load(&c->worker[i].error) ? 1 : 0); }
        }
        ++created;
    }
    if (failed) store(&c->abort,1);
    for (i = 0; !failed && i < o.workers; ++i) if (wait_flag(c,&c->worker[i].ready)) failed = 1;
    for (i = 0; !failed && i < o.workers; ++i) {
        for (j = 0; j < i; ++j)
            if (c->worker[i].first.slot == c->worker[j].first.slot ||
                c->worker[i].first.identity == c->worker[j].first.identity ||
                c->worker[i].first.generation != c->worker[j].first.generation) failed = 1;
    }
    if (!failed) store(&c->start,1);
    for (i = 0; !failed && i < o.workers; ++i) if (wait_flag(c,&c->worker[i].measured)) failed = 1;
    if (!failed) store(&c->check,1);
    for (i = 0; !failed && i < o.workers; ++i) if (wait_flag(c,&c->worker[i].checked)) failed = 1;
    if (failed) store(&c->abort,1);
    store(&c->release,1);
    for (i = 0; i < created; ++i) {
        if (!strcmp(o.mode,"threads")) pthread_join(threads[i],NULL);
        else {
            int status = 0;
            pid_t waited;
            do { waited = waitpid(children[i],&status,0); } while (waited < 0 && errno == EINTR);
            if (waited != children[i] || !WIFEXITED(status) || WEXITSTATUS(status)) failed = 1;
        }
        if (load(&c->worker[i].error) || !load(&c->worker[i].done)) failed = 1;
    }
    for (i = 0; i < o.workers; ++i) {
        struct worker_result *r = &c->worker[i];
        for (j = 0; j < i && o.cpus[j] != o.cpus[i]; ++j) {}
        if (j == i) ++unique;
        calls += r->calls;
        if (r->calls != (uint64_t)o.samples+o.warmup+32 || !load(&r->checked)) failed = 1;
        printf("UB_DOMAIN_MULTI_RESULT worker=%u cpu=%u slot=%u identity=%" PRIu64
               " samples=%u warmup=%u min_ns=%" PRIu64 " p50_ns=%" PRIu64 " p99_ns=%" PRIu64
               " max_ns=%" PRIu64 " avg_ns=%.1f scope=invoke-and-verify nested_path=%s\n",
               i,o.cpus[i],r->first.slot,(uint64_t)r->first.identity,o.samples,o.warmup,
               r->minimum,r->p50,r->p99,r->maximum,(double)r->total/o.samples,o.path);
    }
    if (!failed) printf("UB_DOMAIN_MULTI_PASS mode=%s workers=%u unique_cpus=%u calls=%" PRIu64
                       " test=%s read_only=1 slots_unique=1 reconnect=1 peer_survived_close=1 nested_path=%s\n",
                       o.mode,o.workers,unique,calls,o.test,o.path);
    else fprintf(stderr,"UB_DOMAIN_MULTI_FAIL completed_workers=%u\n",created);
    munmap(c,sizeof(*c));
    return failed ? 1 : 0;
}
