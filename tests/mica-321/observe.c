/* PR321 exact-object observer. The actual storage/queue/disk code executes.
 * A joined writer/deleter thread runs at two precise reader boundaries.
 * No stale pointer is dereferenced by the observer; ASan failures, when
 * expected, originate in the original storage_read implementation itself. */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "disk.h"
#include "elasticqueue.h"
#include "proto_lbs.h"
#include "storage.h"
#include "storage_internal.h"
#include "storage_util.h"

static struct storage_state *store;
static pthread_t reader_thread;
static const char *mode, *boundary;
static int reading, armed, mutating, joined, interleaves, reallocations, moves;
static int nfiles, selected_index, within_file, read_calls, path_calls, sleep_calls;
static uint64_t block, original_next, observed_fnum;
static uintptr_t selected_pointer, tracked_buffer;
static off_t observed_offset;
static size_t files_after;
static uint64_t minimum_after, next_after;
static const uint64_t FIRST = 100;

int __real_storage_util_unlock(struct storage_state *);
char *__real_storage_util_mkpath(struct storage_state *, uint64_t);
void *__real_elasticqueue_get(struct elasticqueue *, size_t);
void *__real_realloc(void *, size_t);
int __real_disk_read(const char *, off_t, size_t, uint8_t *);
int __real_nanosleep(const struct timespec *, struct timespec *);

static int main_reader(void) { return reading && pthread_equal(pthread_self(), reader_thread); }
static int is_mode(const char *s) { return strcmp(mode, s) == 0; }
static void fill_block(uint8_t *p, uint64_t number)
{
    size_t i;
    for (i = 0; i < store->blocklen; i++) p[i] = (uint8_t)(number*31 + i*7);
}
void *__wrap_elasticqueue_get(struct elasticqueue *q, size_t index)
{
    void *p = __real_elasticqueue_get(q, index);
    if (main_reader() && q == store->files && p) selected_pointer = (uintptr_t)p;
    return p;
}
void *__wrap_realloc(void *p, size_t n)
{
    uintptr_t before = (uintptr_t)p;
    void *result = __real_realloc(p, n);
    if (mutating && before == tracked_buffer && before != 0) {
        assert(result);
        reallocations++;
        moves += (uintptr_t)result != before;
        tracked_buffer = (uintptr_t)result;
    }
    return result;
}
static void *mutate(void *arg)
{
    uint8_t *data;
    uint64_t limit;
    (void)arg;
    if (is_mode("grow") || is_mode("append-inplace")) {
        data = malloc(store->blocklen);
        assert(data);
        fill_block(data, original_next);
        assert(storage_write(store, original_next, 1, data) == 0);
        free(data);
    } else {
        assert(is_mode("delete-no-move") || is_mode("delete-shift") || is_mode("delete-shrink"));
        limit = FIRST + (is_mode("delete-no-move") ? 2 : (uint64_t)(nfiles-1)*2);
        assert(storage_delete(store, limit) == 0);
    }
    return NULL;
}
static void interleave(void)
{
    pthread_t worker;
    assert(armed && selected_pointer);
    armed = 0;
    mutating = 1;
    assert(pthread_create(&worker, NULL, mutate, NULL) == 0);
    assert(pthread_join(worker, NULL) == 0);
    mutating = 0;
    joined = 1;
    interleaves++;
    files_after = elasticqueue_getlen(store->files);
    minimum_after = store->minblk;
    next_after = store->nextblk;
    /* This receipt survives an expected ASan abort in the OLD reader. */
    printf("{\"event\":\"interleave\",\"joined\":%d,\"reallocations\":%d,\"moves\":%d,"
           "\"files_after\":%zu,\"minimum_after\":%" PRIu64 ",\"next_after\":%" PRIu64 "}\n",
           joined, reallocations, moves, files_after, minimum_after, next_after);
    fflush(stdout);
}
int __wrap_storage_util_unlock(struct storage_state *s)
{
    int rc = __real_storage_util_unlock(s);
    if (rc == 0 && main_reader() && s == store && armed && selected_pointer && strcmp(boundary, "unlock") == 0)
        interleave();
    return rc;
}
char *__wrap_storage_util_mkpath(struct storage_state *s, uint64_t fileno)
{
    if (main_reader() && s == store) {
        path_calls++;
        observed_fnum = fileno;
        if (armed && strcmp(boundary, "path") == 0) interleave();
        if (is_mode("path-error")) { errno = ENOMEM; return NULL; }
    }
    return __real_storage_util_mkpath(s, fileno);
}
int __wrap_disk_read(const char *path, off_t offset, size_t n, uint8_t *data)
{
    if (main_reader()) {
        read_calls++;
        observed_offset = offset;
        if (is_mode("disk-error")) { errno = EIO; return -1; }
    }
    return __real_disk_read(path, offset, n, data);
}
int __wrap_nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (main_reader()) {
        sleep_calls++;
        assert(req->tv_sec == 0 && req->tv_nsec == store->latency);
    }
    return __real_nanosleep(req, rem);
}
int main(int argc, char **argv)
{
    uint8_t *data, *expected, *appended;
    int rc, saved_errno, matches, appended_ok = -1, queue_unchanged;
    size_t size;
    uint64_t expected_fnum;
    if (argc == 2 && strcmp(argv[1], "--blocklen") == 0) {
        printf("%d\n", PROTO_LBS_BLKLEN_MIN); return 0;
    }
    if (argc != 7) return 2;
    mode = argv[1]; boundary = argv[2]; nfiles = atoi(argv[3]);
    selected_index = atoi(argv[4]); within_file = atoi(argv[5]);
    assert(nfiles > 0 && (within_file == 0 || within_file == 1));
    reader_thread = pthread_self();
    store = storage_init(argv[6], PROTO_LBS_BLKLEN_MIN, is_mode("latency") ? 2000000 : 0, 1);
    assert(store && elasticqueue_getlen(store->files) == (size_t)nfiles);
    assert(store->minblk == FIRST && store->nextblk == FIRST + 2*(uint64_t)nfiles);
    original_next = store->nextblk;
    size = store->blocklen;
    data = malloc(size); expected = malloc(size); appended = malloc(size);
    assert(data && expected && appended);
    block = selected_index == -1 ? FIRST-1 : selected_index == nfiles ? original_next : FIRST + 2*(uint64_t)selected_index + within_file;
    expected_fnum = selected_index >= 0 && selected_index < nfiles ? FIRST + 2*(uint64_t)selected_index : UINT64_MAX;
    fill_block(expected, block);
    memset(data, 0xa5, size);
    observed_fnum = UINT64_MAX; observed_offset = -1;
    tracked_buffer = (uintptr_t)__real_elasticqueue_get(store->files, 0);
    armed = is_mode("grow") || is_mode("append-inplace") || strncmp(mode, "delete-", 7) == 0;
    reading = 1;
    errno = 0;
    rc = storage_read(store, block, data);
    saved_errno = errno;
    reading = 0;
    matches = memcmp(data, expected, size) == 0;
    queue_unchanged = elasticqueue_getlen(store->files) == (size_t)nfiles;
    if (is_mode("grow") || is_mode("append-inplace")) {
        assert(storage_read(store, original_next, appended) == 1);
        fill_block(expected, original_next);
        appended_ok = memcmp(appended, expected, size) == 0;
    }
    if (!interleaves) {
        files_after = elasticqueue_getlen(store->files);
        minimum_after = store->minblk;
        next_after = store->nextblk;
    }
    assert(storage_done(store) == 0);
    free(data); free(expected); free(appended);
    printf("{\"event\":\"result\",\"returncode\":%d,\"errno\":%d,\"blocklen\":%zu,"
           "\"block\":%" PRIu64 ",\"expected_fnum\":%" PRIu64 ",\"observed_fnum\":%" PRIu64 ","
           "\"observed_offset\":%jd,\"data_matches\":%d,\"appended_block_matches\":%d,"
           "\"path_calls\":%d,\"disk_read_calls\":%d,\"sleep_calls\":%d,"
           "\"interleaves\":%d,\"joined\":%d,\"reallocations\":%d,\"moves\":%d,"
           "\"queue_length_unchanged\":%d,\"files_after\":%zu,\"minimum_after\":%" PRIu64 ",\"next_after\":%" PRIu64 "}\n",
           rc, saved_errno, size, block, expected_fnum, observed_fnum, (intmax_t)observed_offset,
           matches, appended_ok, path_calls, read_calls, sleep_calls, interleaves, joined,
           reallocations, moves, queue_unchanged, files_after, minimum_after, next_after);
    return 0;
}
