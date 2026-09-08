#include "thread_pool.h"

#include "xmem.h"

typedef struct Thread_Pool_Job {
    void *arg;
    struct Thread_Pool_Job *next;
} Thread_Pool_Job;

static void worker_loop(void *arg)
{
    Thread_Pool *p = arg;
    for (;;) {
        mutex_lock(&p->mu);
        while (p->head == NULL && !p->stop) {
            cond_wait(&p->work, &p->mu);
        }
        if (p->head == NULL) {
            // stop was set and the queue is drained: this worker is done
            mutex_unlock(&p->mu);
            break;
        }
        Thread_Pool_Job *job = p->head;
        p->head = job->next;
        if (p->head == NULL) {
            p->tail = NULL;
        }
        p->queued--;
        p->active++;
        mutex_unlock(&p->mu);

        p->fn(job->arg);
        xfree(job);

        mutex_lock(&p->mu);
        p->active--;
        if (p->queued == 0 && p->active == 0) {
            cond_signal(&p->idle);
        }
        mutex_unlock(&p->mu);
    }
}

int thread_pool_init(Thread_Pool *p, size_t n_workers, Thread_Pool_Fn fn)
{
    if (n_workers == 0) {
        n_workers = (size_t)thread_hardware_parallelism();
    }
    p->worker_count = 0; // set once we know how many actually started
    p->fn = fn;
    p->head = NULL;
    p->tail = NULL;
    p->queued = 0;
    p->active = 0;
    p->stop = 0;
    if (mutex_init(&p->mu) != 0) {
        return -1;
    }
    if (cond_init(&p->work) != 0) {
        mutex_destroy(&p->mu);
        return -1;
    }
    if (cond_init(&p->idle) != 0) {
        cond_destroy(&p->work);
        mutex_destroy(&p->mu);
        return -1;
    }

    p->workers = xmalloc(sizeof(Thread) * n_workers);
    size_t started = 0;
    for (; started < n_workers; started++) {
        if (thread_init(&p->workers[started], worker_loop, p) != 0) {
            break;
        }
    }
    if (started == 0) {
        // not a single worker made it: unwind without leaving half a pool
        xfree(p->workers);
        cond_destroy(&p->idle);
        cond_destroy(&p->work);
        mutex_destroy(&p->mu);
        return -1;
    }
    p->worker_count = started;
    return 0;
}

int thread_pool_submit(Thread_Pool *p, void *arg)
{
    // free() stamps the pool dead; locking a torn-down mutex would be UB, so
    // the sentinel is checked before anything else. concurrent submit while
    // free() runs is the caller's problem, as the docs say.
    if (p->workers == NULL) {
        return -1;
    }
    Thread_Pool_Job *job = xmalloc(sizeof *job);
    job->arg = arg;
    job->next = NULL;

    mutex_lock(&p->mu);
    if (p->stop) {
        mutex_unlock(&p->mu);
        xfree(job);
        return -1;
    }
    if (p->tail != NULL) {
        p->tail->next = job;
    } else {
        p->head = job;
    }
    p->tail = job;
    p->queued++;
    cond_signal(&p->work);
    mutex_unlock(&p->mu);
    return 0;
}

void thread_pool_wait(Thread_Pool *p)
{
    mutex_lock(&p->mu);
    while (p->queued > 0 || p->active > 0) {
        cond_wait(&p->idle, &p->mu);
    }
    mutex_unlock(&p->mu);
}

void thread_pool_free(Thread_Pool *p)
{
    mutex_lock(&p->mu);
    p->stop = 1;
    cond_broadcast(&p->work);
    mutex_unlock(&p->mu);

    for (size_t i = 0; i < p->worker_count; i++) {
        thread_join(&p->workers[i]);
    }

    // workers only leave through the drained queue, so in theory nothing is
    // left; keeping the drain makes bailing out before wait() harmless
    while (p->head != NULL) {
        Thread_Pool_Job *job = p->head;
        p->head = job->next;
        xfree(job);
    }

    xfree(p->workers);
    p->workers = NULL; // the post-free sentinel submit() refuses on
    p->worker_count = 0;
    cond_destroy(&p->idle);
    cond_destroy(&p->work);
    mutex_destroy(&p->mu);
}