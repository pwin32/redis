
#include <process.h>
#include <stdint.h>
#include "Win32_PThread.h"
#include "Win32_ThreadControl.h"

#define REDIS_THREAD_STACK_SIZE (1024*1024*4)
#ifndef STACK_SIZE_PARAM_IS_A_RESERVATION
#define STACK_SIZE_PARAM_IS_A_RESERVATION   0x00010000    // Threads only
#endif

#ifndef UNUSED
#define UNUSED(V) ((void) V)
#endif

/* Proxy structure to pass func and arg to thread */
typedef struct thread_params {
    void *(*func)(void *);
    void * arg;
} thread_params;

/* Proxy function by windows thread requirements */
static unsigned __stdcall win32_proxy_threadproc(void *arg) {
    IncrementWorkerThreadCount();
#ifdef __MINGW32__
    thread_params *p = (thread_params *) arg;
    p->func(p->arg);

    /* Dealocate params */
    free(p);
    DecrementWorkerThreadCount();
#else
    __try {
        thread_params *p = (thread_params *) arg;
        p->func(p->arg);

        /* Dealocate params */
        free(p);
    }
    __finally {
        DecrementWorkerThreadCount();
    }
#endif

    _endthreadex(0);
    return 0;
}

int pthread_create(pthread_t *thread, const void *unused, void *(*start_routine)(void*), void *arg) {
    HANDLE h;
    thread_params *params = (thread_params *) malloc(sizeof(thread_params));
    UNUSED(unused);

    params->func = start_routine;
    params->arg = arg;

    h = (HANDLE) _beginthreadex(NULL,                              /* Security not used */
                                REDIS_THREAD_STACK_SIZE,           /* Set custom stack size */
                                win32_proxy_threadproc,            /* calls win32 stdcall proxy */
                                params,                            /* real threadproc is passed as paremeter */
                                STACK_SIZE_PARAM_IS_A_RESERVATION, /* reserve stack */
                                thread                             /* returned thread id */
                                );

    if (!h)
        return errno;

    CloseHandle(h);
    return 0;
}

/* Noop in Windows */
int pthread_detach(pthread_t thread) {
    UNUSED(thread);
    return 0;
}

pthread_t pthread_self(void) {
    return GetCurrentThreadId();
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oset) {
    UNUSED(set);
    UNUSED(oset);
    switch (how) {
        case SIG_BLOCK:
        case SIG_UNBLOCK:
        case SIG_SETMASK:
            break;
        default:
            errno = EINVAL;
            return -1;
    }

    errno = ENOSYS;
    return 0;
}

int pthread_join(pthread_t thread, void **value_ptr) {
    int result;
    HANDLE h = OpenThread(SYNCHRONIZE, FALSE, thread);
    UNUSED(value_ptr);

    if (h == NULL)
        return GetLastError();

    switch (WaitForSingleObject(h, INFINITE)) {
        case WAIT_OBJECT_0:
            result = 0;
            break;
        case WAIT_ABANDONED:
            result = EINVAL;
            break;
        default:
            result = GetLastError();
    }

    CloseHandle(h);
    return result;
}

int pthread_cond_init(pthread_cond_t *cond, const void *unused) {
    UNUSED(unused);
    cond->waiters = 0;
    cond->was_broadcast = 0;

    InitializeCriticalSection(&cond->waiters_lock);

    cond->sema = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (!cond->sema) {
        errno = GetLastError();
        return -1;
    }

    cond->continue_broadcast = CreateEvent(NULL,    /* security */
                                           FALSE,   /* auto-reset */
                                           FALSE,   /* not signaled */
                                           NULL);   /* name */
    if (!cond->continue_broadcast) {
        errno = GetLastError();
        return -1;
    }

    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    CloseHandle(cond->sema);
    CloseHandle(cond->continue_broadcast);
    DeleteCriticalSection(&cond->waiters_lock);
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    int last_waiter;

    EnterCriticalSection(&cond->waiters_lock);
    cond->waiters++;
    LeaveCriticalSection(&cond->waiters_lock);

    /*
    * Unlock external mutex and wait for signal.
    * NOTE: we've held mutex locked long enough to increment
    * waiters count above, so there's no problem with
    * leaving mutex unlocked before we wait on semaphore.
    */
    LeaveCriticalSection(mutex);

    /* Let's wait - ignore return value */
    WaitForSingleObject(cond->sema, INFINITE);

    /*
    * Decrease waiters count. If we are the last waiter, then we must
    * notify the broadcasting thread that it can continue.
    * But if we continued due to cond_signal, we do not have to do that
    * because the signaling thread knows that only one waiter continued.
    */
    EnterCriticalSection(&cond->waiters_lock);
    cond->waiters--;
    last_waiter = cond->was_broadcast && cond->waiters == 0;
    LeaveCriticalSection(&cond->waiters_lock);

    if (last_waiter) {
        /*
        * cond_broadcast was issued while mutex was held. This means
        * that all other waiters have continued, but are contending
        * for the mutex at the end of this function because the
        * broadcasting thread did not leave cond_broadcast, yet.
        * (This is so that it can be sure that each waiter has
        * consumed exactly one slice of the semaphor.)
        * The last waiter must tell the broadcasting thread that it
        * can go on.
        */
        SetEvent(cond->continue_broadcast);
        /*
        * Now we go on to contend with all other waiters for
        * the mutex. Auf in den Kampf!
        */
    }
    /* Lock external mutex again */
    EnterCriticalSection(mutex);

    return 0;
}

/*
* IMPORTANT: This implementation requires that pthread_cond_signal
* is called while the mutex is held that is used in the corresponding
* pthread_cond_wait calls!
*/
int pthread_cond_signal(pthread_cond_t *cond) {
    int have_waiters;

    EnterCriticalSection(&cond->waiters_lock);
    have_waiters = cond->waiters > 0;
    LeaveCriticalSection(&cond->waiters_lock);

    /* Signal only when there are waiters */
    if (have_waiters)
        return ReleaseSemaphore(cond->sema, 1, NULL) ?
        0 : GetLastError();
    else
        return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    //TODO
    return 0;
}

#ifdef __MINGW32__
#undef pthread_mutex_t
#undef pthread_mutex_init
#undef pthread_mutex_destroy
#undef pthread_mutex_lock
#undef pthread_mutex_unlock

typedef intptr_t win32_pthread_abi_mutex_t;
typedef long win32_pthread_abi_once_t;
typedef unsigned win32_pthread_abi_key_t;

static INIT_ONCE pthread_abi_init_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION pthread_abi_lock;

static BOOL CALLBACK pthread_abi_init(PINIT_ONCE init_once, PVOID parameter, PVOID *context) {
    UNUSED(init_once);
    UNUSED(parameter);
    UNUSED(context);
    InitializeCriticalSectionAndSpinCount(&pthread_abi_lock, 0x80000400);
    return TRUE;
}

static void pthread_abi_ensure_init(void) {
    InitOnceExecuteOnce(&pthread_abi_init_once, pthread_abi_init, NULL, NULL);
}

static CRITICAL_SECTION *pthread_abi_get_mutex(win32_pthread_abi_mutex_t *mutex) {
    CRITICAL_SECTION *cs;

    if (*mutex > 0) {
        return (CRITICAL_SECTION *) (uintptr_t) *mutex;
    }

    pthread_abi_ensure_init();
    EnterCriticalSection(&pthread_abi_lock);
    if (*mutex <= 0) {
        cs = (CRITICAL_SECTION *) malloc(sizeof(*cs));
        if (cs != NULL) {
            InitializeCriticalSectionAndSpinCount(cs, 0x80000400);
            *mutex = (win32_pthread_abi_mutex_t) (uintptr_t) cs;
        }
    }
    LeaveCriticalSection(&pthread_abi_lock);

    return (CRITICAL_SECTION *) (uintptr_t) *mutex;
}

int pthread_mutex_init(win32_pthread_abi_mutex_t *mutex, const void *unused) {
    CRITICAL_SECTION *cs;
    UNUSED(unused);

    cs = (CRITICAL_SECTION *) malloc(sizeof(*cs));
    if (cs == NULL) {
        return ENOMEM;
    }

    InitializeCriticalSectionAndSpinCount(cs, 0x80000400);
    *mutex = (win32_pthread_abi_mutex_t) (uintptr_t) cs;
    return 0;
}

int pthread_mutex_destroy(win32_pthread_abi_mutex_t *mutex) {
    if (*mutex > 0) {
        CRITICAL_SECTION *cs = (CRITICAL_SECTION *) (uintptr_t) *mutex;
        DeleteCriticalSection(cs);
        free(cs);
    }
    *mutex = 0;
    return 0;
}

int pthread_mutex_lock(win32_pthread_abi_mutex_t *mutex) {
    CRITICAL_SECTION *cs = pthread_abi_get_mutex(mutex);
    if (cs == NULL) {
        return ENOMEM;
    }
    EnterCriticalSection(cs);
    return 0;
}

int pthread_mutex_unlock(win32_pthread_abi_mutex_t *mutex) {
    if (*mutex <= 0) {
        return EINVAL;
    }
    LeaveCriticalSection((CRITICAL_SECTION *) (uintptr_t) *mutex);
    return 0;
}

int pthread_once(win32_pthread_abi_once_t *once_control, void (*init_routine)(void)) {
    volatile LONG *state = (volatile LONG *) once_control;
    LONG old_state = InterlockedCompareExchange(state, 1, 0);

    if (old_state == 0) {
        init_routine();
        InterlockedExchange(state, 2);
        return 0;
    }

    while (*state != 2) {
        Sleep(0);
    }
    return 0;
}

int pthread_key_create(win32_pthread_abi_key_t *key, void (*destructor)(void *)) {
    DWORD tls_key;
    UNUSED(destructor);

    tls_key = TlsAlloc();
    if (tls_key == TLS_OUT_OF_INDEXES) {
        return EAGAIN;
    }

    *key = (win32_pthread_abi_key_t) tls_key;
    return 0;
}

int pthread_key_delete(win32_pthread_abi_key_t key) {
    return TlsFree((DWORD) key) ? 0 : EINVAL;
}

void *pthread_getspecific(win32_pthread_abi_key_t key) {
    return TlsGetValue((DWORD) key);
}

int pthread_setspecific(win32_pthread_abi_key_t key, const void *value) {
    return TlsSetValue((DWORD) key, (LPVOID) value) ? 0 : EINVAL;
}
#endif
