/* macOS simulator stand-in for Linux <sys/prctl.h>: thread naming only. */
#pragma once
#include <pthread.h>
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define PR_SET_PDEATHSIG 1
#define PR_SET_TIMERSLACK 29
static inline int compas_mac_prctl(int option, unsigned long arg2) {
    if (option == PR_SET_NAME && arg2) pthread_setname_np((const char *) arg2);
    return 0;
}
#define prctl(option, arg2, ...) compas_mac_prctl((option), (unsigned long) (arg2))
