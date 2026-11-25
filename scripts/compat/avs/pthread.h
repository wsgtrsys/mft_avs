/*
 * compat/avs/pthread.h - minimal pthreads emulation for MSVC / clang-cl.
 *
 * The AVS/AVS+ decoder core (libavcodec/libcavs.c) is written against the
 * POSIX threads API.  MinGW gets that from winpthreads, MSVC has nothing, so
 * this header provides just the subset libcavs.c uses:
 *
 *   pthread_t / pthread_create / pthread_join / pthread_self
 *   pthread_attr_{init,destroy}
 *   pthread_mutex_{init,destroy,lock,unlock}
 *   pthread_cond_{init,destroy,wait,signal,broadcast}
 *   pthread_getschedparam / pthread_setschedparam + struct sched_param + SCHED_OTHER
 *   pthread_num_processors_np()
 *
 * Everything is static so the header can be included from a single TU without
 * link-time symbols.  Implemented on top of SRWLOCK / CONDITION_VARIABLE /
 * _beginthreadex, i.e. no extra runtime dependency.
 */

#ifndef AVS_COMPAT_PTHREAD_H
#define AVS_COMPAT_PTHREAD_H

#include <process.h>
#include <windows.h>
#include <stdlib.h>
#include <errno.h>

/* --- scheduler emulation (only used by the SYS_WINDOWS thread-priority hint) --- */
#define SCHED_OTHER 0
struct sched_param { int sched_priority; };

typedef void *pthread_t;
typedef struct { int dummy; } pthread_attr_t;
typedef struct { SRWLOCK lock; } pthread_mutex_t;
typedef struct { CONDITION_VARIABLE cond; } pthread_cond_t;

/* thread bookkeeping so that pthread_join() can deliver a return value */
typedef struct avs_thread {
    HANDLE handle;
    void *(*start)(void *);
    void *arg;
    void *ret;
} avs_thread;

static unsigned __stdcall avs_thread_trampoline(void *p)
{
    avs_thread *t = (avs_thread *)p;
    t->ret = t->start(t->arg);
    return 0;
}

static inline int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                void *(*start)(void *), void *arg)
{
    avs_thread *t;
    unsigned tid;
    uintptr_t h;

    (void)attr;
    t = (avs_thread *)calloc(1, sizeof(*t));
    if (!t)
        return ENOMEM;
    t->start = start;
    t->arg   = arg;
    h = _beginthreadex(NULL, 0, avs_thread_trampoline, t, 0, &tid);
    if (h == 0) {
        free(t);
        return EAGAIN;
    }
    t->handle = (HANDLE)h;
    *thread = (pthread_t)t;
    return 0;
}

static inline int pthread_join(pthread_t thread, void **retval)
{
    avs_thread *t = (avs_thread *)thread;
    if (!t)
        return EINVAL;
    WaitForSingleObject(t->handle, INFINITE);
    if (retval)
        *retval = t->ret;
    CloseHandle(t->handle);
    free(t);
    return 0;
}

static inline pthread_t pthread_self(void)
{
    return (pthread_t)GetCurrentThread();
}

static inline int pthread_attr_init(pthread_attr_t *a)      { (void)a; return 0; }
static inline int pthread_attr_destroy(pthread_attr_t *a)   { (void)a; return 0; }

static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
    (void)attr;
    InitializeSRWLock(&m->lock);
    return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m)    { AcquireSRWLockExclusive(&m->lock); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m)  { ReleaseSRWLockExclusive(&m->lock); return 0; }

static inline int pthread_cond_init(pthread_cond_t *c, const void *attr)
{
    (void)attr;
    InitializeConditionVariable(&c->cond);
    return 0;
}
static inline int pthread_cond_destroy(pthread_cond_t *c)   { (void)c; return 0; }
static inline int pthread_cond_signal(pthread_cond_t *c)    { WakeConditionVariable(&c->cond); return 0; }
static inline int pthread_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable(&c->cond); return 0; }
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    SleepConditionVariableSRW(&c->cond, &m->lock, INFINITE, 0);
    return 0;
}

/*
 * Priority mapping: the decoder calls cavs_lower_thread_priority(n) to give its
 * worker threads a "nice" penalty.  We keep that penalty in a per-thread value
 * and translate it into a SetThreadPriority() delta.
 */
static __declspec(thread) int avs_nice = 0;   /* emulated sched_priority, per thread */

static inline int pthread_getschedparam(pthread_t th, int *policy, struct sched_param *sp)
{
    (void)th;
    *policy = SCHED_OTHER;
    sp->sched_priority = avs_nice;
    return 0;
}

static inline int pthread_setschedparam(pthread_t th, int policy, const struct sched_param *sp)
{
    int new_nice = sp->sched_priority;
    int delta = new_nice - avs_nice;

    (void)th;
    if (policy != SCHED_OTHER)
        return EINVAL;
    avs_nice = new_nice;
    if (delta) {
        int cur = GetThreadPriority(GetCurrentThread());
        if (cur != THREAD_PRIORITY_ERROR_RETURN) {
            int np = cur - delta;   /* a higher "nice" means a lower priority */
            if (np < THREAD_PRIORITY_LOWEST)
                np = THREAD_PRIORITY_LOWEST;
            if (np > THREAD_PRIORITY_TIME_CRITICAL)
                np = THREAD_PRIORITY_TIME_CRITICAL;
            SetThreadPriority(GetCurrentThread(), np);
        }
    }
    return 0;
}

static inline int pthread_num_processors_np(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

#endif /* AVS_COMPAT_PTHREAD_H */
