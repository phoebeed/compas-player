/* Force-included (-include) into every file of the macOS simulator build
 * (`make host` on a Mac) and nowhere else: the device build and the Linux
 * simulator never see it. It fills the gaps between Linux and macOS that the
 * player's code relies on, so the code itself stays Linux-only.
 *
 * The stand-in headers for Linux-only includes (<sys/prctl.h>,
 * <linux/input.h>, ...) sit next to this file in include/. */
#ifndef COMPAS_HOST_MACOS_COMPAT_H
#define COMPAS_HOST_MACOS_COMPAT_H

#ifdef __APPLE__

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* struct stat: Linux names the timespec fields st_[amc]tim. */
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec

/* Time that keeps counting through sleep; the simulator never sleeps. */
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#endif

/* Socket flags: macOS has no type flags or MSG_NOSIGNAL. SIGPIPE is ignored
 * process-wide instead (see macos_compat.c). */
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Scheduler policies are accepted and ignored. */
#ifndef SCHED_BATCH
#define SCHED_BATCH 3
#endif
#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif
#ifndef SCHED_RESET_ON_FORK
#define SCHED_RESET_ON_FORK 0x40000000
#endif
int compas_mac_sched_setscheduler(pid_t pid, int policy, const struct sched_param * param);
#define sched_setscheduler compas_mac_sched_setscheduler

typedef off_t off64_t;

int compas_mac_pipe2(int fds[2], int flags);
#define pipe2 compas_mac_pipe2

/* pthread condition variables: macOS waits only on CLOCK_REALTIME. A deadline
 * the caller took from CLOCK_MONOTONIC is turned into a relative wait. */
int compas_mac_condattr_setclock(pthread_condattr_t * attr, clockid_t clock);
#define pthread_condattr_setclock compas_mac_condattr_setclock
int compas_mac_cond_timedwait(pthread_cond_t * cond, pthread_mutex_t * mutex, const struct timespec * abstime);
#define pthread_cond_timedwait compas_mac_cond_timedwait

/* Joins without the deadline: on a Mac the calls it bounds (DNS) return. */
int compas_mac_timedjoin_np(pthread_t thread, void ** result, const struct timespec * abstime);
#define pthread_timedjoin_np compas_mac_timedjoin_np

/* glibc's fopencookie(), on top of the BSD funopen(). */
typedef ssize_t cookie_read_function_t(void * cookie, char * buf, size_t size);
typedef ssize_t cookie_write_function_t(void * cookie, const char * buf, size_t size);
typedef int cookie_seek_function_t(void * cookie, off_t * offset, int whence);
typedef int cookie_close_function_t(void * cookie);
typedef struct {
    cookie_read_function_t * read;
    cookie_write_function_t * write;
    cookie_seek_function_t * seek;
    cookie_close_function_t * close;
} cookie_io_functions_t;
FILE * compas_mac_fopencookie(void * cookie, const char * mode, cookie_io_functions_t io);
#define fopencookie compas_mac_fopencookie

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */
#endif /* COMPAS_HOST_MACOS_COMPAT_H */
