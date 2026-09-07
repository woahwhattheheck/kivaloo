#define _GNU_SOURCE 1
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include LATTICE_SOURCE

static int mode, calls, warnings, dir_fd = -1;
static DIR * seen;
int __real_closedir(DIR *);
void __wrap_warnp(const char * fmt, ...) { (void)fmt; warnings++; }
int __wrap_closedir(DIR * d) {
    calls++;
    if (seen == d) {
        printf("{\"guard\":1,\"calls\":%d,\"warnings\":%d,\"fd_open\":%d}\n",
            calls,warnings,fcntl(dir_fd,F_GETFD) != -1);
        fflush(stdout);
        _Exit(90); /* Prevent an actual double-free of the released DIR. */
    }
    seen = d;
    dir_fd = dirfd(d);
    int rc = __real_closedir(d);
    if (rc) _Exit(91);
    if (mode == 1) { errno = EINTR; return -1; }
    if (mode == 2) { errno = EIO; return -1; }
    return 0;
}
int main(int argc, char ** argv) {
    struct elasticqueue * Q;
    if (argc != 3) return 94;
    mode = atoi(argv[1]);
    Q = storage_findfiles(argv[2]);
    printf("{\"guard\":0,\"calls\":%d,\"warnings\":%d,\"result_nonnull\":%d,\"fd_open\":%d}\n",
        calls,warnings,Q != NULL,fcntl(dir_fd,F_GETFD) != -1);
    if (Q) elasticqueue_free(Q);
    return 0;
}
