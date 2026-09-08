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

// mutual exclusion and condition variables, enough to build a bounded work
// queue. all handles are heap-backed so the header stays dependency-free.
typedef struct {
    void *handle;
} Mutex;

typedef struct {
    void *handle;
} Cond;

// returns 0 on success
int mutex_init(Mutex *mu);
void mutex_lock(Mutex *mu);
void mutex_unlock(Mutex *mu);
// callers lock then destroy; no one may be waiting at that point
void mutex_destroy(Mutex *mu);

// returns 0 on success. cond_wait atomically drops mu while sleeping and
// re-acquires it before returning, so the caller can re-check its predicate.
int cond_init(Cond *c);
int cond_wait(Cond *c, Mutex *mu);
int cond_signal(Cond *c);
int cond_broadcast(Cond *c);
void cond_destroy(Cond *c);

#endif