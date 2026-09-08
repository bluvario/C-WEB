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

    if (fails == 0) {
        printf("thread ok\n");
    }
    return fails != 0;
}