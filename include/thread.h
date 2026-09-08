#ifndef CWEB_THREAD_H
#define CWEB_THREAD_H

#include <stdint.h>

// minimal threads over pthreads (POSIX) and Win32 threads. the framework only
// ever needs spawn, join and detach, so that is all this papers over.
typedef struct {
#ifdef _WIN32
    void *handle; // HANDLE
#else
    uint64_t id;  // pthread_t
#endif
} Thread;

// starts fn(arg) running in its own thread. returns 0 on success.
int thread_init(Thread *t, void (*fn)(void *), void *arg);
// block until the thread finished. returns 0 on success.
int thread_join(Thread *t);
// set the thread free: it cleans up after itself when fn returns.
int thread_detach(Thread *t);

// how many hardware threads the machine offers, for sizing pools (>= 1)
int thread_hardware_parallelism(void);

#endif