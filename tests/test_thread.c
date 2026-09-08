#include <stdatomic.h>
#include <stdio.h>

#include "thread.h"

static atomic_int g_counter;
static atomic_int g_calls;

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

// one thread per array slot, each adds its index to the shared counter; the
// main thread must observe all four increments after joining
static void bump(void *arg)
{
    int which = *(int *)arg;
    for (int i = 0; i < 1000; i++) {
        atomic_fetch_add(&g_calls, 1);
        atomic_fetch_add(&g_counter, which);
    }
}

// a counter protected by a mutex: losing the lock would lose increments
static Mutex g_mu;
static int g_locked_sum;

static void bump_locked(void *arg)
{
    int which = *(int *)arg;
    for (int i = 0; i < 1000; i++) {
        mutex_lock(&g_mu);
        g_locked_sum += which;
        mutex_unlock(&g_mu);
    }
}

// waiters sleep on the condvar until the gate opens; the woken count only
// reaches the full three if broadcast lights every sleeper
typedef struct {
    Mutex mu;
    Cond cv;
    int release;
    int woken;
} Gate;

static void gated_worker(void *arg)
{
    Gate *g = arg;
    mutex_lock(&g->mu);
    while (!g->release) {
        cond_wait(&g->cv, &g->mu);
    }
    g->woken++;
    mutex_unlock(&g->mu);
}

static void gate_open(void *arg)
{
    Gate *g = arg;
    mutex_lock(&g->mu);
    g->release = 1;
    cond_broadcast(&g->cv);
    mutex_unlock(&g->mu);
}

int main(void)
{
    int fails = 0;

    int slots[4] = {1, 1, 2, 5};
    Thread threads[4];
    for (int i = 0; i < 4; i++) {
        if (thread_init(&threads[i], bump, &slots[i]) != 0) {
            fprintf(stderr, "thread %d failed to start\n", i);
            return 1;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (thread_join(&threads[i]) != 0) {
            fprintf(stderr, "thread %d failed to join\n", i);
            return 1;
        }
    }

    // 1 + 1 + 2 + 5 = 9, times a thousand iterations each
    if (atomic_load(&g_counter) != 9000) {
        fprintf(stderr, "counter wrong: %d\n", atomic_load(&g_counter));
        return 1;
    }
    fails += check("threads ran and joined", atomic_load(&g_calls) == 4000);

    // every machine claims at least one core
    fails += check("hardware parallelism is positive",
                   thread_hardware_parallelism() >= 1);

    fails += check("mutex init", mutex_init(&g_mu) == 0);
    for (int i = 0; i < 4; i++) {
        if (thread_init(&threads[i], bump_locked, &slots[i]) != 0) {
            fprintf(stderr, "locked thread %d failed to start\n", i);
            return 1;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (thread_join(&threads[i]) != 0) {
            fprintf(stderr, "locked thread %d failed to join\n", i);
            return 1;
        }
    }
    fails += check("mutex kept the sum exact", g_locked_sum == 9000);
    mutex_destroy(&g_mu);

    Gate gate;
    fails += check("mutex init (gate)", mutex_init(&gate.mu) == 0);
    fails += check("cond init", cond_init(&gate.cv) == 0);
    for (int i = 0; i < 3; i++) {
        if (thread_init(&threads[i], gated_worker, &gate) != 0) {
            fprintf(stderr, "waiter %d failed to start\n", i);
            return 1;
        }
    }
    Thread opener;
    if (thread_init(&opener, gate_open, &gate) != 0) {
        fprintf(stderr, "opener failed to start\n");
        return 1;
    }
    for (int i = 0; i < 3; i++) {
        if (thread_join(&threads[i]) != 0) {
            fprintf(stderr, "waiter %d failed to join\n", i);
            return 1;
        }
    }
    if (thread_join(&opener) != 0) {
        fprintf(stderr, "opener failed to join\n");
        return 1;
    }
    fails += check("broadcast woke every waiter", gate.woken == 3);
    mutex_destroy(&gate.mu);
    cond_destroy(&gate.cv);

    if (fails == 0) {
        printf("thread ok\n");
    }
    return fails != 0;
}