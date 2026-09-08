#ifndef CWEB_THREAD_POOL_H
#define CWEB_THREAD_POOL_H

#include <stddef.h>

#include "thread.h"

// bounded worker pool: a fixed set of threads pulls jobs off a FIFO queue.
// the workers are up and waiting at init time, so the marginal cost of a job
// is one enqueue and one signal, not a thread spawn.
typedef struct Thread_Pool_Job Thread_Pool_Job;

typedef void (*Thread_Pool_Fn)(void *arg);

typedef struct {
    Thread *workers;
    size_t worker_count;
    Thread_Pool_Fn fn;
    Mutex mu;
    Cond work; // a job arrived, or stop was set
    Cond idle; // queue empty and every worker resting
    Thread_Pool_Job *head;
    Thread_Pool_Job *tail;
    size_t queued;
    size_t active;
    int stop;
} Thread_Pool;

// starts n_workers threads all running fn. n_workers == 0 means "as many as
// the machine has cores". returns 0 on success.
int thread_pool_init(Thread_Pool *p, size_t n_workers, Thread_Pool_Fn fn);
// queue a job. returns 0 on success, -1 once the pool has been shut down.
int thread_pool_submit(Thread_Pool *p, void *arg);
// block until every submitted job has run to completion. must not be called
// from inside a job, the caller would wait on itself forever.
void thread_pool_wait(Thread_Pool *p);
// let workers drain what is queued, then join them and free the pool. submits
// after this fail with -1.
void thread_pool_free(Thread_Pool *p);

#endif