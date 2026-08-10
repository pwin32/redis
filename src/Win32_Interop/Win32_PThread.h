#ifndef __WIN32_PTHREAD_H_
#define __WIN32_PTHREAD_H_

#include <windows.h>
#include <errno.h>
#include <stdint.h>

#ifdef __MINGW32__
#include <sys/types.h>
#ifndef sigset_t
#define sigset_t _sigset_t
#endif
#else
#ifndef _SIGSET_T_
#define _SIGSET_T_
typedef size_t _sigset_t;
#define sigset_t _sigset_t
#endif /* _SIGSET_T_ */
#endif

#ifndef SIG_SETMASK
#define SIG_SETMASK (0)
#define SIG_BLOCK   (1)
#define SIG_UNBLOCK (2)
#endif /* SIG_SETMASK */

/* threads avoiding pthread.h */
#define pthread_mutex_t CRITICAL_SECTION
#define pthread_attr_t ssize_t

static __inline int win32_pthread_mutex_init(CRITICAL_SECTION *mutex,
                                             const void *attributes) {
    (void)attributes;
    return InitializeCriticalSectionAndSpinCount(mutex, 0x80000400) ? 0 : ENOMEM;
}

static __inline int win32_pthread_mutex_destroy(CRITICAL_SECTION *mutex) {
    DeleteCriticalSection(mutex);
    return 0;
}

static __inline int win32_pthread_mutex_lock(CRITICAL_SECTION *mutex) {
    EnterCriticalSection(mutex);
    return 0;
}

static __inline int win32_pthread_mutex_unlock(CRITICAL_SECTION *mutex) {
    LeaveCriticalSection(mutex);
    return 0;
}

static __inline int win32_pthread_attr_init(ssize_t *attributes) {
    *attributes = 0;
    return 0;
}

static __inline int win32_pthread_attr_getstacksize(const ssize_t *attributes,
                                                     size_t *stacksize) {
    *stacksize = (size_t)*attributes;
    return 0;
}

static __inline int win32_pthread_attr_setstacksize(ssize_t *attributes,
                                                     size_t stacksize) {
    if (stacksize > (size_t)INTPTR_MAX) return EINVAL;
    *attributes = (ssize_t)stacksize;
    return 0;
}

#define pthread_mutex_init(a,b) win32_pthread_mutex_init((a), (b))
#define pthread_mutex_destroy(a) win32_pthread_mutex_destroy((a))
#define pthread_mutex_lock(a) win32_pthread_mutex_lock((a))
#define pthread_mutex_unlock(a) win32_pthread_mutex_unlock((a))
#define pthread_mutex_trylock(a) (TryEnterCriticalSection((a)) ? 0 : EBUSY)

#define pthread_equal(t1, t2) ((t1) == (t2))

#define pthread_attr_init(x) win32_pthread_attr_init((x))
#define pthread_attr_getstacksize(x, y) win32_pthread_attr_getstacksize((x), (y))
#define pthread_attr_setstacksize(x, y) win32_pthread_attr_setstacksize((x), (y))

#define pthread_t uintptr_t

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                   void *(*start_routine)(void*), void *arg);
int pthread_join(pthread_t thread, void **value_ptr);

pthread_t pthread_self(void);

typedef struct {
    CRITICAL_SECTION waiters_lock;
    LONG waiters;
    int was_broadcast;
    HANDLE sema;
    HANDLE continue_broadcast;
} win32_pthread_cond_t;

/* Keep Redis' condition-variable layout private.  MinGW's static libstdc++
 * calls the public pthread_cond_* ABI with its own intptr_t objects, so using
 * the same symbols for both layouts corrupts startup synchronization. */
#define pthread_cond_t win32_pthread_cond_t
#define pthread_cond_init win32_pthread_cond_init
#define pthread_cond_destroy win32_pthread_cond_destroy
#define pthread_cond_wait win32_pthread_cond_wait
#define pthread_cond_signal win32_pthread_cond_signal
#define pthread_cond_broadcast win32_pthread_cond_broadcast

int pthread_cond_init(pthread_cond_t *cond, const void *unused);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

int pthread_detach(pthread_t thread);
#ifdef pthread_sigmask
#undef pthread_sigmask
#endif
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);

#endif
