/*
 * Compile unchanged worker.c and dispatch_request.c together. Real pthread
 * mutexes protect state; selected calls return deterministic injected errors.
 * Storage/response/notification calls are observations, not a running database.
 * The worker loop is invoked synchronously with a modeled terminal wake; no
 * claim is made about real scheduler timing or naturally failing pthread APIs.
 */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

struct workctl;
static struct workctl * running_ctl;
static int fail_lock, fail_signal, fail_alloc, response_failure;
static int lock_calls, signal_calls, wait_calls, operation_calls, response_calls;
static int request_frees, buffer_frees, allocation_calls;
static void * watched_request;
static void * watched_buffer;
static uint64_t nextblock = 7;

static int injected_lock(pthread_mutex_t *);
static int injected_signal(pthread_cond_t *);
static int modeled_wait(pthread_cond_t *, pthread_mutex_t *);
static void * tracked_malloc(size_t);
static void tracked_free(void *);

#define pthread_mutex_lock(m) injected_lock(m)
#define pthread_cond_signal(c) injected_signal(c)
#define pthread_cond_wait(c, m) modeled_wait(c, m)
#include "worker.c"
#undef pthread_mutex_lock
#undef pthread_cond_signal
#undef pthread_cond_wait

/* Function-like macros do not rename the request union's r.free member. */
#define malloc(n) tracked_malloc(n)
#define free(p) tracked_free(p)
#include "dispatch_request.c"
#undef malloc
#undef free

static int
injected_lock(pthread_mutex_t * m)
{
    lock_calls++;
    if (fail_lock) {
        fail_lock = 0;
        return (EINVAL);
    }
    return (pthread_mutex_lock(m));
}

static int
injected_signal(pthread_cond_t * cv)
{
    signal_calls++;
    if (fail_signal) {
        fail_signal = 0;
        return (EINVAL);
    }
    return (pthread_cond_signal(cv));
}

static int
modeled_wait(pthread_cond_t * cv, pthread_mutex_t * m)
{
    (void)cv;
    (void)m;
    assert(running_ctl != NULL);
    wait_calls++;
    /* A terminal wake lets the synchronous observer stop without a timeout. */
    running_ctl->suicide = 1;
    return (0);
}

static void *
tracked_malloc(size_t n)
{
    allocation_calls++;
    if (fail_alloc) {
        fail_alloc = 0;
        return (NULL);
    }
    assert(watched_buffer == NULL);
    watched_buffer = malloc(n);
    assert(watched_buffer != NULL);
    return (watched_buffer);
}

static void
tracked_free(void * p)
{
    if (p != NULL && p == watched_request)
        request_frees++;
    if (p != NULL && p == watched_buffer)
        buffer_frees++;
    free(p);
}

uint64_t
storage_nextblock(struct storage_state * s)
{
    (void)s;
    return (nextblock);
}

int
storage_read(struct storage_state * s, uint64_t block, uint8_t * buf)
{
    (void)s; (void)block;
    assert(buf != NULL);
    operation_calls++;
    return (1);
}

int
storage_write(struct storage_state * s, uint64_t block, uint64_t n,
    uint8_t * buf)
{
    (void)s; (void)block; (void)n;
    assert(buf != NULL);
    operation_calls++;
    return (0);
}

int
storage_delete(struct storage_state * s, uint64_t block)
{
    (void)s; (void)block;
    operation_calls++;
    return (0);
}

ssize_t
noeintr_write(int fd, const void * buf, size_t n)
{
    (void)fd; (void)buf;
    assert(running_ctl != NULL);
    running_ctl->suicide = 1;
    return ((ssize_t)n);
}

int
proto_lbs_response_append(struct netbuf_write * q, uint64_t id,
    int status, uint64_t block)
{
    (void)q; (void)id;
    assert(status == 1 && block == UINT64_MAX);
    response_calls++;
    return (response_failure ? -1 : 0);
}

int
proto_lbs_response_free(struct netbuf_write * q, uint64_t id)
{
    (void)q; (void)id;
    response_calls++;
    return (response_failure ? -1 : 0);
}

static void
init_ctl(struct workctl * ctl)
{
    memset(ctl, 0, sizeof(*ctl));
    assert(pthread_mutex_init(&ctl->mtx, NULL) == 0);
    assert(pthread_cond_init(&ctl->cv, NULL) == 0);
    ctl->workdone = 7;
    ctl->op = 99;
    ctl->blkno = 111;
    ctl->nblks = 222;
    ctl->reqID = 333;
}

static void
destroy_ctl(struct workctl * ctl)
{
    assert(pthread_cond_destroy(&ctl->cv) == 0);
    assert(pthread_mutex_destroy(&ctl->mtx) == 0);
}

static int
worker_case(const char * name, int op)
{
    struct workctl ctl;
    uint8_t * buf = malloc(512);
    int rc, before, done_before;

    assert(buf != NULL && op >= 0 && op <= 2);
    memset(buf, 0x5a, 512);
    init_ctl(&ctl);
    fail_lock = strcmp(name, "worker_lock_fail") == 0;
    fail_signal = strcmp(name, "worker_signal_fail") == 0 ||
        strcmp(name, "worker_consume_failed") == 0 ||
        strcmp(name, "worker_retry") == 0;
    rc = worker_assign(&ctl, op, 7, 1, buf, 101);
    before = ctl.haswork;
    done_before = ctl.workdone;
    /* Both error exits must leave the mutex available. */
    assert(pthread_mutex_trylock(&ctl.mtx) == 0);
    assert(pthread_mutex_unlock(&ctl.mtx) == 0);
    if (strcmp(name, "worker_consume_failed") == 0) {
        assert(rc == -1);
        running_ctl = &ctl;
        workthread(&ctl);
        running_ctl = NULL;
    }
    if (strcmp(name, "worker_retry") == 0) {
        assert(rc == -1);
        /* Base aborts at the real haswork assertion; head accepts the retry. */
        rc = worker_assign(&ctl, op, 8, 1, buf, 102);
    }
    printf("{\"ret\":%d,\"haswork_before\":%d,\"workdone_before\":%d,"
        "\"haswork\":%d,\"workdone\":%d,\"op\":%d,\"blkno\":%" PRIu64 ","
        "\"reqID\":%" PRIu64 ",\"signal_calls\":%d,\"wait_calls\":%d,"
        "\"operation_calls\":%d}\n", rc, before, done_before,
        ctl.haswork, ctl.workdone, ctl.op, ctl.blkno, ctl.reqID,
        signal_calls, wait_calls, operation_calls);
    destroy_ctl(&ctl);
    free(buf);
    return (0);
}

static int
dispatch_case(const char * name)
{
    struct workctl reader, writer, deleter, * ctl;
    struct workctl * workers[3];
    struct dispatch_state ds;
    struct proto_lbs_request * req;
    struct readq * node;
    size_t reader_id = 0;
    int rc, is_read = strncmp(name, "read_", 5) == 0;
    int is_free = strncmp(name, "free_", 5) == 0;

    init_ctl(&reader); init_ctl(&writer); init_ctl(&deleter);
    memset(&ds, 0, sizeof(ds));
    ds.blocklen = 512;
    ds.npending = 4;
    ds.workers = workers;
    ds.nreaders = is_read ? 1 : 0;
    if (is_read) {
        workers[0] = &reader; workers[1] = &writer; workers[2] = &deleter;
    } else {
        workers[0] = &writer; workers[1] = &deleter; workers[2] = NULL;
    }
    ctl = is_read ? &reader : (is_free ? &deleter : &writer);
    fail_lock = strstr(name, "lock_fail") != NULL;
    fail_signal = strstr(name, "signal_fail") != NULL;
    fail_alloc = strstr(name, "alloc_fail") != NULL;
    response_failure = strstr(name, "response_fail") != NULL;
    if (strstr(name, "nextblock_fail") != NULL)
        nextblock = UINT64_MAX;
    if (strstr(name, "busy") != NULL || strstr(name, "active") != NULL) {
        if (is_free) ds.deleter_busy = 1;
        else ds.writer_busy = 1;
        ctl->haswork = 1;
        ctl->workdone = 0;
    }
    if (is_read) {
        node = calloc(1, sizeof(*node));
        assert(node != NULL);
        watched_request = node;
        node->reqID = 101; node->blkno = 7;
        ds.readq_head = node; ds.readq_tail = &node->next;
        ds.nreaders_idle = 1; ds.readers_idle = &reader_id;
        rc = dispatch_request_pokereadq(&ds);
    } else {
        req = calloc(1, sizeof(*req));
        assert(req != NULL);
        watched_request = req;
        req->ID = 101;
        if (is_free) {
            req->type = PROTO_LBS_FREE;
            req->r.free.blkno = 7;
            rc = dispatch_request_free(&ds, req);
        } else {
            assert(strncmp(name, "append_", 7) == 0);
            req->type = PROTO_LBS_APPEND;
            req->r.append.blkno = strstr(name, "wrongblock") != NULL ? 8 : 7;
            req->r.append.nblks = 1; req->r.append.blklen = 512;
            req->r.append.buf = malloc(512);
            assert(req->r.append.buf != NULL);
            memset(req->r.append.buf, 0x5a, 512);
            watched_buffer = req->r.append.buf;
            rc = dispatch_request_append(&ds, req);
        }
    }
    printf("{\"ret\":%d,\"haswork\":%d,\"writer_busy\":%d,"
        "\"deleter_busy\":%d,\"npending\":%zu,\"signal_calls\":%d,"
        "\"response_calls\":%d,\"request_frees\":%d,\"buffer_frees\":%d,"
        "\"nreaders_idle\":%zu,\"readq_retained\":%d,\"allocation_calls\":%d}\n",
        rc, ctl->haswork, ds.writer_busy, ds.deleter_busy, ds.npending,
        signal_calls, response_calls, request_frees, buffer_frees,
        ds.nreaders_idle, ds.readq_head != NULL, allocation_calls);
    /* Release only fixture-owned objects, after the observed ownership counts. */
    if (watched_buffer != NULL && buffer_frees == 0) free(watched_buffer);
    if (watched_request != NULL && request_frees == 0) free(watched_request);
    destroy_ctl(&reader); destroy_ctl(&writer); destroy_ctl(&deleter);
    return (0);
}

int
main(int argc, char ** argv)
{
    assert(argc >= 2);
    if (strncmp(argv[1], "worker_", 7) == 0) {
        assert(argc == 3);
        return (worker_case(argv[1], atoi(argv[2])));
    }
    assert(argc == 2);
    return (dispatch_case(argv[1]));
}
