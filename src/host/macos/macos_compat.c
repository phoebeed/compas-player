/* The functions macos_compat.h maps Linux calls onto. Built only by the
 * macOS simulator (`make host` on a Mac). */
#include "macos_compat.h"

#ifdef __APPLE__

#include <errno.h>
#include <signal.h>
#include <stdlib.h>

#undef pthread_cond_timedwait
#undef pthread_condattr_setclock
#undef sched_setscheduler
#undef pipe2
#undef fopencookie

/* Linux sends with MSG_NOSIGNAL; here a write to a closed socket or pipe must
 * not kill the simulator either. */
__attribute__((constructor)) static void compas_mac_ignore_sigpipe(void) {
    signal(SIGPIPE, SIG_IGN);
}

int compas_mac_sched_setscheduler(pid_t pid, int policy, const struct sched_param * param) {
    (void) pid;
    (void) policy;
    (void) param;
    return 0;
}

int compas_mac_pipe2(int fds[2], int flags) {
    if (pipe(fds) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        if (flags & O_CLOEXEC) fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        if (flags & O_NONBLOCK) fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
    }
    return 0;
}

int compas_mac_condattr_setclock(pthread_condattr_t * attr, clockid_t clock) {
    (void) attr;
    (void) clock;
    return 0;
}

/* A CLOCK_MONOTONIC deadline counts from boot, a CLOCK_REALTIME one from 1970:
 * anything before 2001 can only be monotonic. */
#define REALTIME_FLOOR_SEC 978307200

int compas_mac_cond_timedwait(pthread_cond_t * cond, pthread_mutex_t * mutex, const struct timespec * abstime) {
    if (!abstime || abstime->tv_sec >= REALTIME_FLOOR_SEC) return pthread_cond_timedwait(cond, mutex, abstime);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long left_ns = ((long long) abstime->tv_sec - now.tv_sec) * 1000000000LL + (abstime->tv_nsec - now.tv_nsec);
    if (left_ns <= 0) return ETIMEDOUT;
    struct timespec rel = { (time_t) (left_ns / 1000000000LL), (long) (left_ns % 1000000000LL) };
    return pthread_cond_timedwait_relative_np(cond, mutex, &rel);
}

int compas_mac_timedjoin_np(pthread_t thread, void ** result, const struct timespec * abstime) {
    (void) abstime;
    return pthread_join(thread, result);
}

typedef struct {
    void * cookie;
    cookie_io_functions_t io;
} compas_mac_cookie_t;

static int mac_cookie_read(void * opaque, char * buf, int size) {
    compas_mac_cookie_t * c = opaque;
    if (!c->io.read) return -1;
    return (int) c->io.read(c->cookie, buf, (size_t) size);
}

static int mac_cookie_write(void * opaque, const char * buf, int size) {
    compas_mac_cookie_t * c = opaque;
    if (!c->io.write) return -1;
    return (int) c->io.write(c->cookie, buf, (size_t) size);
}

static fpos_t mac_cookie_seek(void * opaque, fpos_t offset, int whence) {
    compas_mac_cookie_t * c = opaque;
    if (!c->io.seek) return -1;
    off_t pos = (off_t) offset;
    if (c->io.seek(c->cookie, &pos, whence) != 0) return -1;
    return (fpos_t) pos;
}

static int mac_cookie_close(void * opaque) {
    compas_mac_cookie_t * c = opaque;
    int rc = c->io.close ? c->io.close(c->cookie) : 0;
    free(c);
    return rc;
}

FILE * compas_mac_fopencookie(void * cookie, const char * mode, cookie_io_functions_t io) {
    compas_mac_cookie_t * c = malloc(sizeof(*c));
    if (!c) return NULL;
    c->cookie = cookie;
    c->io = io;
    int writable = mode && (mode[0] == 'w' || mode[0] == 'a' || (mode[0] && mode[1] == '+'));
    FILE * f = funopen(c, mac_cookie_read, writable ? mac_cookie_write : NULL, mac_cookie_seek, mac_cookie_close);
    if (!f) free(c);
    return f;
}

#endif /* __APPLE__ */
