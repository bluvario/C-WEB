#include "thread.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <unistd.h>
#endif

#include "xmem.h"

// the arg handed to the OS thread is owned by the stub, which frees it once
// the user function returns, join or detach notwithstanding
typedef struct {
    void (*fn)(void *);
    void *arg;
} Thread_Fn_Arg;

#ifdef _WIN32
static DWORD WINAPI win_stub(LPVOID data)
{
    Thread_Fn_Arg *f = data;
    f->fn(f->arg);
    xfree(f);
    return 0;
}
#else
static void *posix_stub(void *data)
{
    Thread_Fn_Arg *f = data;
    f->fn(f->arg);
    xfree(f);
    return NULL;
}
#endif

int thread_init(Thread *t, void (*fn)(void *), void *arg)
{
    Thread_Fn_Arg *f = xmalloc(sizeof *f);
    f->fn = fn;
    f->arg = arg;
#ifdef _WIN32
    t->handle = CreateThread(NULL, 0, win_stub, f, 0, NULL);
    if (t->handle == NULL) {
        xfree(f);
        return -1;
    }
#else
    if (pthread_create(&t->id, NULL, posix_stub, f) != 0) {
        xfree(f);
        return -1;
    }
#endif
    return 0;
}

int thread_join(Thread *t)
{
#ifdef _WIN32
    if (WaitForSingleObject(t->handle, INFINITE) != WAIT_OBJECT_0) {
        return -1;
    }
    CloseHandle(t->handle);
    t->handle = NULL;
    return 0;
#else
    return pthread_join(t->id, NULL);
#endif
}

int thread_detach(Thread *t)
{
#ifdef _WIN32
    CloseHandle(t->handle);
    t->handle = NULL;
    return 0;
#else
    return pthread_detach(t->id);
#endif
}

int thread_hardware_parallelism(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n > 0 ? (int)n : 1;
}