/* Test-only: includes the entire unmodified target translation unit.
 * Only external cleanup dependencies and close/free are instrumented.
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include LATTICE_SOURCE

static int target_fd = -1, target_calls, all_calls, warnings;
static int mode, donor_fd = -1, worker_calls, worker_error;
static int cancel_calls, read_frees, write_frees, state_frees, array_frees;
static void * state_ptr, * workers_ptr, * idle_ptr;

int __real_close(int);
void __real_free(void *);

void __wrap_free(void * p) {
    if (p && p == state_ptr) state_frees++;
    if (p && (p == workers_ptr || p == idle_ptr)) array_frees++;
    __real_free(p);
}

static void report(int status, int guard) {
    int alive = fcntl(target_fd, F_GETFD) != -1;
    printf("{\"status\":%d,\"guard\":%d,\"target_calls\":%d,\"all_calls\":%d,"
        "\"fd_open\":%d,\"warnings\":%d,\"worker_calls\":%d,\"cancel_calls\":%d,"
        "\"read_frees\":%d,\"write_frees\":%d,\"state_frees\":%d,\"array_frees\":%d}\n",
        status,guard,target_calls,all_calls,alive,warnings,worker_calls,cancel_calls,
        read_frees,write_frees,state_frees,array_frees);
    fflush(stdout);
}

int __wrap_close(int fd) {
    all_calls++;
    if (fd != target_fd) return __real_close(fd);
    target_calls++;
    if (target_calls > 8) {
        report(-999, 1); /* Bounded repetition guard, not an application exit. */
        _Exit(90);
    }
    if (mode == 1 && target_calls == 1) {
        if (__real_close(fd)) _Exit(93);
        errno = EIO; return -1;
    }
    if (mode == 2 && target_calls == 1) {
        /* Model a completed close followed by reuse in another thread. */
        if (__real_close(fd) || dup2(donor_fd, fd) != fd) _Exit(91);
        errno = EINTR;
        return -1;
    }
    if (mode == 3 && target_calls == 1) {
        /* Explicit separate model: descriptor remains owned on EINTR. */
        errno = EINTR;
        return -1;
    }
    if (mode == 4 && target_calls == 1) {
        if (__real_close(fd)) _Exit(92);
        errno = EINPROGRESS;
        return -1;
    }
    if (mode == 5 && target_calls == 1) {
        /* Present a genuinely invalid descriptor to the tested call. */
        if (__real_close(fd)) _Exit(93);
    }
    /* Stale errno on success must not be mistaken for a close failure. */
    if (mode == 6) errno = EINTR;
    return __real_close(fd);
}

void __wrap_warnp(const char * fmt, ...) { (void)fmt; warnings++; }
void netbuf_read_free(struct netbuf_read * p) { (void)p; read_frees++; }
void netbuf_write_free(struct netbuf_write * p) { (void)p; write_frees++; }
#if LATTICE_LBS
int worker_kill(struct workctl * p) { (void)p; worker_calls++; return worker_error; }
void network_read_cancel(void * p) { (void)p; cancel_calls++; }
#endif

int main(int argc, char ** argv) {
    struct dispatch_state * D;
    int fds[2], rc;
    if (argc != 4) return 94;
    mode = atoi(argv[1]);
    int site = atoi(argv[2]);
    worker_error = atoi(argv[3]);
    if (pipe(fds) || (donor_fd = open("/dev/null", O_RDONLY)) < 0) return 95;
    D = calloc(1, sizeof(*D));
    if (!D) return 96;
    state_ptr = D;
#if LATTICE_LBS
    if (site != 2) {
        D->nreaders = 1;
        workers_ptr = D->workers = calloc(3, sizeof(*D->workers));
        idle_ptr = D->readers_idle = calloc(1, sizeof(*D->readers_idle));
        if (!D->workers || !D->readers_idle) return 97;
        D->spair[0] = fds[0]; D->spair[1] = fds[1];
        target_fd = fds[site];
        rc = dispatch_done(D);
    } else {
        D->sconn = target_fd = fds[0];
        rc = dispatch_close(D);
    }
#else
    (void)site;
    D->sconn = target_fd = fds[0];
    rc = dispatch_done(D);
#endif
    report(rc, 0);
    /* Process isolation contains any deliberately demonstrated retained fd
     * or state. Do not accidentally hide cleanup behavior before reporting. */
    if (!state_frees) __real_free(D);
    __real_close(fds[0]); __real_close(fds[1]); __real_close(donor_fd);
    return 0;
}
