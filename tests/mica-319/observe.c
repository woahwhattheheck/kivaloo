/* PR319 observer: links the actual normally built lbs/disk.o. Real temporary
 * files, reads/writes/fsync and descriptor identities are retained. A joined
 * second thread deterministically reuses a released descriptor when requested.
 * Injected close semantics are models, not claims of naturally observed EINTR. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "disk.h"

static const char *operation, *close_mode, *fault, *target_path, *sentinel_path;
static int active, target_fd = -1, open_calls, close_calls, read_calls;
static int write_calls, fsync_calls, seek_calls, fault_fired, close_injected;
static int reused_by_thread, replacement_fd = -1, joined, cleanup_closes;
static struct stat original_stat, replacement_stat;

int __real_open(const char *, int, ...);
int __real_close(int);
ssize_t __real_read(int, void *, size_t);
ssize_t __real_write(int, const void *, size_t);
int __real_fsync(int);
off_t __real_lseek(int, off_t, int);

static int same_file(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}
static int is_fault(const char *name) { return strcmp(fault, name) == 0; }

int __wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    int fd;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (active && strcmp(path, target_path) == 0) {
        open_calls++;
        if (is_fault("open-eintr") && open_calls == 1) {
            fault_fired++; errno = EINTR; return -1;
        }
        if (is_fault("open-eacces")) {
            fault_fired++; errno = EACCES; return -1;
        }
    }
    fd = flags & O_CREAT ? __real_open(path, flags, mode) : __real_open(path, flags);
    if (active && strcmp(path, target_path) == 0 && fd >= 0) {
        assert(target_fd == -1);
        target_fd = fd;
        assert(fstat(fd, &original_stat) == 0);
    }
    return fd;
}
static void *reuse_descriptor(void *arg)
{
    int fd;
    (void)arg;
    fd = __real_open(sentinel_path, O_RDWR);
    assert(fd >= 0);
    if (fd != target_fd) {
        assert(dup2(fd, target_fd) == target_fd);
        assert(__real_close(fd) == 0);
    }
    replacement_fd = target_fd;
    assert(fstat(replacement_fd, &replacement_stat) == 0);
    assert(!same_file(&original_stat, &replacement_stat));
    reused_by_thread = 1;
    return NULL;
}
int __wrap_close(int fd)
{
    pthread_t worker;
    int injected_errno;
    if (!active || fd != target_fd) return __real_close(fd);
    close_calls++;
    assert(close_calls <= 2);
    if (close_calls != 1 || strcmp(close_mode, "normal") == 0)
        return __real_close(fd);
    close_injected = 1;
    if (strcmp(close_mode, "retained-eintr") == 0) {
        errno = EINTR; return -1;
    }
    assert(__real_close(fd) == 0);
    if (strcmp(close_mode, "reuse-eintr") == 0) {
        assert(pthread_create(&worker, NULL, reuse_descriptor, NULL) == 0);
        assert(pthread_join(worker, NULL) == 0);
        joined = 1;
        errno = EINTR; return -1;
    }
    if (strcmp(close_mode, "released-eintr") == 0) injected_errno = EINTR;
    else if (strcmp(close_mode, "released-eio") == 0) injected_errno = EIO;
    else if (strcmp(close_mode, "released-enospc") == 0) injected_errno = ENOSPC;
    else { assert(strcmp(close_mode, "released-einprogress") == 0); injected_errno = EINPROGRESS; }
    errno = injected_errno;
    return -1;
}
ssize_t __wrap_read(int fd, void *p, size_t n)
{
    if (active && fd == target_fd) {
        read_calls++;
        if (is_fault("io-eio")) { fault_fired++; errno = EIO; return -1; }
        if (is_fault("io-eof")) { fault_fired++; return 0; }
        if (is_fault("io-eintr") && read_calls == 1) { fault_fired++; errno = EINTR; return -1; }
        if (is_fault("partial") && n > 7) { fault_fired++; n = 7; }
    }
    return __real_read(fd, p, n);
}
ssize_t __wrap_write(int fd, const void *p, size_t n)
{
    if (active && fd == target_fd) {
        write_calls++;
        if (is_fault("io-eio")) { fault_fired++; errno = EIO; return -1; }
        if (is_fault("io-eintr") && write_calls == 1) { fault_fired++; errno = EINTR; return -1; }
        if (is_fault("partial") && n > 7) { fault_fired++; n = 7; }
    }
    return __real_write(fd, p, n);
}
int __wrap_fsync(int fd)
{
    if (active && fd == target_fd) {
        fsync_calls++;
        if (is_fault("fsync-eio")) { fault_fired++; errno = EIO; return -1; }
        if (is_fault("fsync-eintr") && fsync_calls == 1) { fault_fired++; errno = EINTR; return -1; }
    }
    return __real_fsync(fd);
}
off_t __wrap_lseek(int fd, off_t offset, int whence)
{
    if (active && fd == target_fd) {
        seek_calls++;
        if (is_fault("seek-eio")) { fault_fired++; errno = EIO; return -1; }
    }
    return __real_lseek(fd, offset, whence);
}
int main(int argc, char **argv)
{
    uint8_t data[32], result[32];
    int rc, saved_errno, i, identity = 0, readable = 0, writable = 0;
    struct stat now;
    char byte;
    if (argc != 6) return 2;
    operation = argv[1]; close_mode = argv[2]; fault = argv[3];
    target_path = argv[4]; sentinel_path = argv[5];
    for (i = 0; i < 32; i++) data[i] = (uint8_t)(i*17+3);
    memset(result, 0xa5, sizeof(result));
    active = 1;
    errno = 0;
    if (strcmp(operation, "syncdir") == 0) rc = disk_syncdir(target_path);
    else if (strcmp(operation, "read") == 0 || strcmp(operation, "read-zero") == 0)
        rc = disk_read(target_path, 7, strcmp(operation, "read-zero") == 0 ? 0 : 32, result);
    else {
        int create = strncmp(operation, "create", 6) == 0;
        int nosync = strstr(operation, "nosync") != NULL;
        assert(create || strncmp(operation, "append", 6) == 0);
        rc = disk_write(target_path, create, sizeof(data), data, nosync);
    }
    saved_errno = errno;
    active = 0;
    if (target_fd >= 0 && fstat(target_fd, &now) == 0) {
        if (same_file(&now, &original_stat)) identity = 1;
        else { assert(reused_by_thread && same_file(&now, &replacement_stat)); identity = 2; }
    }
    if (identity == 2) {
        readable = pread(target_fd, &byte, 1, 0) == 1 && byte == 'M';
        byte = 'M';
        writable = pwrite(target_fd, &byte, 1, 0) == 1;
    }
    /* Preserve observed state first; clean test-owned live descriptors after. */
    if (identity) { assert(__real_close(target_fd) == 0); cleanup_closes++; }
    printf("{\"returncode\":%d,\"errno\":%d,\"target_opened\":%d,\"open_calls\":%d,"
        "\"close_calls\":%d,\"close_injected\":%d,\"read_calls\":%d,\"write_calls\":%d,"
        "\"fsync_calls\":%d,\"seek_calls\":%d,\"fault_fired\":%d,\"descriptor_identity\":%d,"
        "\"reused_by_second_thread\":%d,\"replacement_readable\":%d,\"replacement_writable\":%d,"
        "\"joined\":%d,\"cleanup_closes\":%d,\"read_buffer\":\"",
        rc, saved_errno, target_fd >= 0, open_calls, close_calls, close_injected,
        read_calls, write_calls, fsync_calls, seek_calls, fault_fired, identity,
        reused_by_thread, readable, writable, joined, cleanup_closes);
    for (i = 0; i < 32; i++) printf("%02x", result[i]);
    printf("\"}\n");
    return 0;
}
