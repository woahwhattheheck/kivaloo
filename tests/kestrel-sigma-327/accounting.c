/* LLM-authored (KESTREL-SIGMA) isolated validation of existing PR327.
 * Includes unchanged production code; queued completions are deterministic
 * observations, not a real S3 service, HTTP stack, or concurrent daemon.
 */
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int alloc_fail, enqueue_fail, allocs, frees, attempts;
static void * observed_malloc(size_t n);
static void observed_free(void * p);
#define malloc(n) observed_malloc(n)
#define free(p) observed_free(p)
#include "s3state.c"
#undef malloc
#undef free

struct queued {
    int (*put)(void *, int);
    int (*get)(void *, int, size_t, const uint8_t *);
    void * cookie;
};
static struct queued queued[8];
static int queued_count, application_calls, app_error, saw_null;
static struct s3state * observed_state;
static size_t seen_pending;
static uint64_t seen_next;
static uint8_t data[32];

static void *
observed_malloc(size_t n)
{
    void * p;
    attempts++;
    if (alloc_fail) {
        alloc_fail = 0;
        return (NULL);
    }
    p = malloc(n);
    assert(p != NULL);
    allocs++;
    return (p);
}

static void
observed_free(void * p)
{
    assert(p != NULL);
    frees++;
    free(p);
}

const char *
objmap(uint64_t n)
{
    (void)n;
    return ("synthetic-object");
}

int
proto_s3_request_put(struct wire_requestqueue * q, const char * bucket,
    const char * object, size_t length, const uint8_t * buf,
    int (*callback)(void *, int), void * cookie)
{
    (void)q;
    assert(strcmp(bucket, "synthetic-bucket") == 0);
    assert(strcmp(object, "synthetic-object") == 0);
    assert(length == sizeof(data) && buf == data);
    if (enqueue_fail) return (-1);
    assert(queued_count < 8);
    queued[queued_count].put = callback;
    queued[queued_count++].cookie = cookie;
    return (0);
}

int
proto_s3_request_range(struct wire_requestqueue * q, const char * bucket,
    const char * object, uint32_t offset, uint32_t length,
    int (*callback)(void *, int, size_t, const uint8_t *), void * cookie)
{
    (void)q;
    assert(strcmp(bucket, "synthetic-bucket") == 0);
    assert(strcmp(object, "synthetic-object") == 0);
    assert(offset == 64 && length == 16);
    if (enqueue_fail) return (-1);
    assert(queued_count < 8);
    queued[queued_count].get = callback;
    queued[queued_count++].cookie = cookie;
    return (0);
}

static int
append_application(void * cookie, struct proto_lbs_request * request,
    uint64_t next)
{
    assert(cookie == observed_state && request != NULL);
    application_calls++;
    seen_pending = observed_state->npending;
    seen_next = next;
    return (app_error ? -1 : 0);
}

static int
get_application(void * cookie, struct proto_lbs_request * request,
    const uint8_t * buf, size_t length)
{
    assert(cookie == observed_state && request != NULL);
    assert(length == 16 || length == 15);
    application_calls++;
    seen_pending = observed_state->npending;
    saw_null = buf == NULL;
    assert(buf == NULL || buf == data);
    return (app_error ? -1 : 0);
}

int
main(int argc, char ** argv)
{
    struct s3state state, * s = &state;
    struct proto_lbs_request request, second, original;
    const char * name;
    size_t initial, expected_pending;
    int fixed, get, reject, rc, completed = 0, teardown;

    assert(argc == 4);
    name = argv[1];
    fixed = atoi(argv[2]);
    initial = (size_t)strtoul(argv[3], NULL, 10);
    assert(fixed == 0 || fixed == 1);
    teardown = strcmp(name, "failed_append_teardown") == 0;
    if (teardown) {
        assert(initial == 0);
        s = calloc(1, sizeof(*s));
        assert(s != NULL);
    }
    memset(s, 0, sizeof(*s));
    s->bucket = teardown ? strdup("synthetic-bucket") : (char *)"synthetic-bucket";
    assert(s->bucket != NULL);
    s->blklen = 16; s->nextblk = 123; s->lastblk = 122; s->npending = initial;
    observed_state = s;
    memset(&request, 0, sizeof(request));
    memset(data, 0x5a, sizeof(data));
    request.ID = 101;
    get = strncmp(name, "get_", 4) == 0;
    if (get) {
        request.type = PROTO_LBS_GET; request.r.get.blkno = 4;
    } else {
        request.type = PROTO_LBS_APPEND;
        request.r.append.blkno = 0; request.r.append.nblks = 2;
        request.r.append.blklen = 16; request.r.append.buf = data;
    }
    original = request;
    alloc_fail = strstr(name, "alloc_fail") != NULL;
    enqueue_fail = strstr(name, "enqueue_fail") != NULL;
    reject = alloc_fail || enqueue_fail;
    app_error = strstr(name, "callback_error") != NULL;
    rc = get ? s3state_get(s, &request, get_application, s) :
        s3state_append(s, &request, append_application, s);
    assert(rc == (reject ? -1 : 0));
    assert(s->npending == initial + (reject ? 0 : 1));
    expected_pending = initial;
    if (strcmp(name, "mixed_completion") == 0) {
        second = request;
        second.ID = 102; second.r.append.blkno = BLKSPEROBJECT;
        assert(s3state_append(s, &second, append_application, s) == 0);
        assert(s->npending == initial + 2 && queued_count == 2);
        assert(queued[1].put(queued[1].cookie, 1) == -1);
        assert(s->npending == initial + (fixed ? 1 : 2));
        assert(application_calls == 0 && s->nextblk == 123 && s->lastblk == 122);
        assert(queued[0].put(queued[0].cookie, 0) == 0);
        assert(application_calls == 1 && seen_next == BLKSPEROBJECT);
        assert(s->lastblk == 1 && s->nextblk == BLKSPEROBJECT);
        expected_pending += fixed ? 0 : 1;
        completed = 2;
    } else if (!reject) {
        if (get) {
            int failed = strcmp(name, "get_failed") == 0;
            size_t length = strcmp(name, "get_short") == 0 ? 15 : 16;
            rc = queued[0].get(queued[0].cookie, failed, length, data);
            assert(rc == (app_error ? -1 : 0));
            assert(application_calls == 1);
            assert(saw_null == (failed || length != 16));
            assert(seen_pending == initial + 1);
            assert(s->nextblk == 123 && s->lastblk == 122);
        } else {
            int failed = strcmp(name, "append_failed") == 0 || teardown;
            rc = queued[0].put(queued[0].cookie, failed);
            assert(rc == (failed || app_error ? -1 : 0));
            assert(application_calls == (failed ? 0 : 1));
            if (failed) {
                expected_pending += fixed ? 0 : 1;
                assert(s->nextblk == 123 && s->lastblk == 122);
            } else {
                assert(s->nextblk == BLKSPEROBJECT && s->lastblk == 1);
                assert(seen_next == BLKSPEROBJECT && seen_pending == initial + 1);
            }
        }
        completed = 1;
    } else {
        assert(queued_count == 0 && application_calls == 0);
        assert(s->nextblk == 123 && s->lastblk == 122);
    }
    assert(s->npending == expected_pending);
    assert(memcmp(&request, &original, sizeof(request)) == 0);
    assert(allocs == frees);
    assert(allocs == (reject ? (enqueue_fail ? 1 : 0) : completed));
    assert(attempts == (strcmp(name, "mixed_completion") == 0 ? 2 : 1));
    if (teardown) {
        /* Original source aborts at its real pending-count assertion. */
        s3state_free(s);
        assert(fixed && frees == 3);
    }
    printf("{\"pending\":%zu,\"application_calls\":%d,\"allocs\":%d,\"frees\":%d,"
        "\"completed\":%d,\"saw_null\":%d,\"teardown\":%d}\n",
        expected_pending, application_calls, allocs, frees, completed, saw_null, teardown);
    return (0);
}
