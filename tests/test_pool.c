#include <stdio.h>

#include "thread.h"
#include "thread_pool.h"

static Mutex g_mu;
static long g_done;

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

static void count_job(void *arg)
{
    (void)arg;
    mutex_lock(&g_mu);
    g_done++;
    mutex_unlock(&g_mu);
}

int main(void)
{
    int fails = 0;
    mutex_init(&g_mu);

    Thread_Pool pool;
    fails += check("pool starts four workers",
                   thread_pool_init(&pool, 4, count_job) == 0);
    int submits_ok = 0;
    for (int i = 0; i < 1000; i++) {
        if (thread_pool_submit(&pool, NULL) == 0) {
            submits_ok++;
        }
    }
    fails += check("all submits accepted", submits_ok == 1000);
    thread_pool_wait(&pool);
    fails += check("every submitted job ran", g_done == 1000);

    // a pool stays usable after wait()
    if (thread_pool_submit(&pool, NULL) == 0) {
        thread_pool_wait(&pool);
    }
    fails += check("pool accepts work after wait", g_done == 1001);

    thread_pool_free(&pool);
    fails += check("submit refuses after free",
                   thread_pool_submit(&pool, NULL) == -1);

    // draining a pool that still has queued jobs when freed must not crash
    Thread_Pool drained;
    fails += check("pool starts two workers",
                   thread_pool_init(&drained, 2, count_job) == 0);
    for (int i = 0; i < 50; i++) {
        thread_pool_submit(&drained, NULL);
    }
    thread_pool_free(&drained);

    mutex_destroy(&g_mu);

    if (fails == 0) {
        printf("pool ok\n");
    }
    return fails != 0;
}