
#include <process.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/timeb.h>
#include "Win32_PThread.h"
#include "Win32_ThreadControl.h"
#include "Win32_Error.h"

#define REDIS_THREAD_STACK_SIZE (1024*1024*4)
#ifndef STACK_SIZE_PARAM_IS_A_RESERVATION
#define STACK_SIZE_PARAM_IS_A_RESERVATION   0x00010000    // Threads only
#endif

#ifndef UNUSED
#define UNUSED(V) ((void) V)
#endif

typedef struct win32_thread_record {
    pthread_t id;
    HANDLE handle;
    void *result;
    int completed;
    int detached;
    int joined;
    struct win32_thread_record *next;
} win32_thread_record;

/* Proxy structure to pass func and arg to thread */
typedef struct thread_params {
    void *(*func)(void *);
    void *arg;
    win32_thread_record *record;
} thread_params;

static INIT_ONCE thread_registry_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION thread_registry_lock;
static win32_thread_record *thread_registry;
#ifdef __MINGW32__
static __thread pthread_t current_thread_id;
#else
static __declspec(thread) pthread_t current_thread_id;
#endif

static BOOL CALLBACK init_thread_registry(PINIT_ONCE once, PVOID parameter,
                                          PVOID *context) {
    UNUSED(once);
    UNUSED(parameter);
    UNUSED(context);
    InitializeCriticalSectionAndSpinCount(&thread_registry_lock, 0x80000400);
    return TRUE;
}

static void ensure_thread_registry(void) {
    InitOnceExecuteOnce(&thread_registry_once, init_thread_registry, NULL, NULL);
}

static win32_thread_record *find_thread_record(pthread_t id) {
    win32_thread_record *record;
    for (record = thread_registry; record != NULL; record = record->next) {
        if (record->id == id) return record;
    }
    return NULL;
}

static void remove_thread_record(win32_thread_record *record) {
    win32_thread_record **current = &thread_registry;
    while (*current != NULL) {
        if (*current == record) {
            *current = record->next;
            return;
        }
        current = &(*current)->next;
    }
}

static int pthread_win32_error(DWORD error) {
    int translated = translate_sys_error((int)error);
    return translated == -9999 ? EIO : translated;
}

/* Proxy function by windows thread requirements */
static unsigned __stdcall win32_proxy_threadproc(void *arg) {
    thread_params *p = (thread_params *)arg;
    win32_thread_record *record = p->record;
    void *result = NULL;
    int release_record = 0;

    current_thread_id = record->id;
    IncrementWorkerThreadCount();
#ifdef __MINGW32__
    result = p->func(p->arg);
    free(p);
    DecrementWorkerThreadCount();
#else
    __try {
        result = p->func(p->arg);
        free(p);
    }
    __finally {
        DecrementWorkerThreadCount();
    }
#endif

    ensure_thread_registry();
    EnterCriticalSection(&thread_registry_lock);
    record->result = result;
    record->completed = 1;
    if (record->detached) {
        remove_thread_record(record);
        release_record = 1;
    }
    LeaveCriticalSection(&thread_registry_lock);

    if (release_record) {
        CloseHandle(record->handle);
        free(record);
    }

    _endthreadex(0);
    return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                   void *(*start_routine)(void*), void *arg) {
    HANDLE handle;
    DWORD resume_result;
    size_t stack_size = REDIS_THREAD_STACK_SIZE;
    thread_params *params;
    win32_thread_record *record;

    if (thread == NULL || start_routine == NULL) return EINVAL;
    if (attributes != NULL) {
        if (*attributes < 0 || (uintmax_t)*attributes > UINT_MAX) return EINVAL;
        if (*attributes != 0) stack_size = (size_t)*attributes;
    }
    params = (thread_params *)malloc(sizeof(*params));
    record = (win32_thread_record *)calloc(1, sizeof(*record));
    if (params == NULL || record == NULL) {
        free(params);
        free(record);
        return ENOMEM;
    }
    params->func = start_routine;
    params->arg = arg;
    params->record = record;

    handle = (HANDLE)_beginthreadex(
        NULL, (unsigned int)stack_size, win32_proxy_threadproc, params,
        STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED, NULL);
    if (handle == NULL) {
        int error = errno != 0 ? errno : EAGAIN;
        free(params);
        free(record);
        return error;
    }

    /* Windows may recycle a numeric thread ID as soon as a thread exits,
     * even while a joinable handle is still retained. Use the live record's
     * address as the opaque pthread_t so an unjoined thread cannot collide
     * with a later thread. */
    record->id = (pthread_t)(uintptr_t)record;
    record->handle = handle;
    ensure_thread_registry();
    EnterCriticalSection(&thread_registry_lock);
    record->next = thread_registry;
    thread_registry = record;
    LeaveCriticalSection(&thread_registry_lock);

    resume_result = ResumeThread(handle);
    if (resume_result == (DWORD)-1) {
        DWORD error = GetLastError();
        EnterCriticalSection(&thread_registry_lock);
        remove_thread_record(record);
        LeaveCriticalSection(&thread_registry_lock);
        TerminateThread(handle, ERROR_OPERATION_ABORTED);
        CloseHandle(handle);
        free(params);
        free(record);
        return pthread_win32_error(error);
    }

    *thread = record->id;
    return 0;
}

int pthread_detach(pthread_t thread) {
    win32_thread_record *record;
    int release_record = 0;

    ensure_thread_registry();
    EnterCriticalSection(&thread_registry_lock);
    record = find_thread_record(thread);
    if (record == NULL) {
        LeaveCriticalSection(&thread_registry_lock);
        return ESRCH;
    }
    if (record->detached || record->joined) {
        LeaveCriticalSection(&thread_registry_lock);
        return EINVAL;
    }
    record->detached = 1;
    if (record->completed) {
        remove_thread_record(record);
        release_record = 1;
    }
    LeaveCriticalSection(&thread_registry_lock);

    if (release_record) {
        CloseHandle(record->handle);
        free(record);
    }
    return 0;
}

pthread_t pthread_self(void) {
    if (current_thread_id != 0) return current_thread_id;
    /* Threads not created through pthread_create (the process main thread and
     * the SCM worker) still need a stable opaque identity. Keep that namespace
     * separate from aligned record pointers. */
    return ((pthread_t)GetCurrentThreadId() << 1) | 1;
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
            return EINVAL;
    }

    /* Windows has no process-compatible per-thread signal mask. Redis only
     * uses this to block signals in worker threads, so the portable contract
     * is a successful no-op without contaminating the caller's errno. */
    return 0;
}

int pthread_join(pthread_t thread, void **value_ptr) {
    win32_thread_record *record;
    DWORD wait_result;
    int result = 0;

    if (thread == pthread_self()) return EDEADLK;
    ensure_thread_registry();
    EnterCriticalSection(&thread_registry_lock);
    record = find_thread_record(thread);
    if (record == NULL) {
        LeaveCriticalSection(&thread_registry_lock);
        return ESRCH;
    }
    if (record->detached || record->joined) {
        LeaveCriticalSection(&thread_registry_lock);
        return EINVAL;
    }
    record->joined = 1;
    LeaveCriticalSection(&thread_registry_lock);

    wait_result = WaitForSingleObject(record->handle, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
        result = wait_result == WAIT_FAILED ?
                 pthread_win32_error(GetLastError()) : EINVAL;
        EnterCriticalSection(&thread_registry_lock);
        record->joined = 0;
        LeaveCriticalSection(&thread_registry_lock);
        return result;
    }

    EnterCriticalSection(&thread_registry_lock);
    if (value_ptr != NULL) *value_ptr = record->result;
    remove_thread_record(record);
    LeaveCriticalSection(&thread_registry_lock);
    CloseHandle(record->handle);
    free(record);
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, const void *unused) {
    UNUSED(unused);
    cond->waiters = 0;
    cond->was_broadcast = 0;

    InitializeCriticalSection(&cond->waiters_lock);

    cond->sema = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (!cond->sema) {
        int error = pthread_win32_error(GetLastError());
        DeleteCriticalSection(&cond->waiters_lock);
        return error;
    }

    cond->continue_broadcast = CreateEvent(NULL,    /* security */
                                           FALSE,   /* auto-reset */
                                           FALSE,   /* not signaled */
                                           NULL);   /* name */
    if (!cond->continue_broadcast) {
        int error = pthread_win32_error(GetLastError());
        CloseHandle(cond->sema);
        cond->sema = NULL;
        DeleteCriticalSection(&cond->waiters_lock);
        return error;
    }

    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    int result = 0;
    if (cond->waiters != 0) return EBUSY;
    if (cond->sema != NULL && !CloseHandle(cond->sema))
        result = pthread_win32_error(GetLastError());
    if (cond->continue_broadcast != NULL && !CloseHandle(cond->continue_broadcast) && result == 0)
        result = pthread_win32_error(GetLastError());
    cond->sema = NULL;
    cond->continue_broadcast = NULL;
    DeleteCriticalSection(&cond->waiters_lock);
    return result;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    int last_waiter;
    int result = 0;
    DWORD wait_result;

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

    wait_result = WaitForSingleObject(cond->sema, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
        result = wait_result == WAIT_FAILED ?
                 pthread_win32_error(GetLastError()) : EINVAL;
    }

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
        if (!SetEvent(cond->continue_broadcast) && result == 0)
            result = pthread_win32_error(GetLastError());
        /*
        * Now we go on to contend with all other waiters for
        * the mutex. Auf in den Kampf!
        */
    }
    /* Lock external mutex again */
    EnterCriticalSection(mutex);

    return result;
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
        0 : pthread_win32_error(GetLastError());
    else
        return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    LONG waiters;
    DWORD wait_result;

    EnterCriticalSection(&cond->waiters_lock);
    waiters = cond->waiters;
    if (waiters > 0) {
        cond->was_broadcast = 1;
        if (!ReleaseSemaphore(cond->sema, waiters, NULL)) {
            int error = pthread_win32_error(GetLastError());
            cond->was_broadcast = 0;
            LeaveCriticalSection(&cond->waiters_lock);
            return error;
        }
    }
    LeaveCriticalSection(&cond->waiters_lock);

    if (waiters == 0) return 0;
    wait_result = WaitForSingleObject(cond->continue_broadcast, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
        EnterCriticalSection(&cond->waiters_lock);
        cond->was_broadcast = 0;
        LeaveCriticalSection(&cond->waiters_lock);
        return wait_result == WAIT_FAILED ?
               pthread_win32_error(GetLastError()) : EINVAL;
    }
    EnterCriticalSection(&cond->waiters_lock);
    cond->was_broadcast = 0;
    LeaveCriticalSection(&cond->waiters_lock);
    return 0;
}

#ifdef __MINGW32__
#undef pthread_mutex_t
#undef pthread_mutex_init
#undef pthread_mutex_destroy
#undef pthread_mutex_lock
#undef pthread_mutex_unlock
#undef pthread_cond_t
#undef pthread_cond_init
#undef pthread_cond_destroy
#undef pthread_cond_wait
#undef pthread_cond_signal
#undef pthread_cond_broadcast

typedef intptr_t win32_pthread_abi_mutex_t;
typedef intptr_t win32_pthread_abi_cond_t;
typedef long win32_pthread_abi_once_t;
typedef unsigned win32_pthread_abi_key_t;

typedef struct win32_pthread_abi_cond_state {
    CONDITION_VARIABLE condition;
} win32_pthread_abi_cond_state;

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

    return *mutex > 0 ? (CRITICAL_SECTION *) (uintptr_t) *mutex : NULL;
}

static win32_pthread_abi_cond_state *pthread_abi_get_cond(
    win32_pthread_abi_cond_t *cond)
{
    win32_pthread_abi_cond_state *state;

    if (*cond > 0)
        return (win32_pthread_abi_cond_state *) (uintptr_t) *cond;

    pthread_abi_ensure_init();
    EnterCriticalSection(&pthread_abi_lock);
    if (*cond <= 0) {
        state = (win32_pthread_abi_cond_state *) malloc(sizeof(*state));
        if (state != NULL) {
            InitializeConditionVariable(&state->condition);
            *cond = (win32_pthread_abi_cond_t) (uintptr_t) state;
        }
    }
    LeaveCriticalSection(&pthread_abi_lock);

    return *cond > 0 ?
           (win32_pthread_abi_cond_state *) (uintptr_t) *cond : NULL;
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

int pthread_cond_init(win32_pthread_abi_cond_t *cond, const void *unused) {
    win32_pthread_abi_cond_state *state;
    UNUSED(unused);

    state = (win32_pthread_abi_cond_state *) malloc(sizeof(*state));
    if (state == NULL) return ENOMEM;
    InitializeConditionVariable(&state->condition);
    *cond = (win32_pthread_abi_cond_t) (uintptr_t) state;
    return 0;
}

int pthread_cond_destroy(win32_pthread_abi_cond_t *cond) {
    if (*cond > 0)
        free((void *) (uintptr_t) *cond);
    *cond = 0;
    return 0;
}

int pthread_cond_signal(win32_pthread_abi_cond_t *cond) {
    win32_pthread_abi_cond_state *state = pthread_abi_get_cond(cond);
    if (state == NULL) return ENOMEM;
    WakeConditionVariable(&state->condition);
    return 0;
}

int pthread_cond_broadcast(win32_pthread_abi_cond_t *cond) {
    win32_pthread_abi_cond_state *state = pthread_abi_get_cond(cond);
    if (state == NULL) return ENOMEM;
    WakeAllConditionVariable(&state->condition);
    return 0;
}

static int pthread_abi_cond_wait(win32_pthread_abi_cond_t *cond,
                                 win32_pthread_abi_mutex_t *mutex,
                                 DWORD timeout)
{
    win32_pthread_abi_cond_state *state = pthread_abi_get_cond(cond);
    CRITICAL_SECTION *cs = pthread_abi_get_mutex(mutex);
    DWORD error;

    if (state == NULL || cs == NULL) return ENOMEM;
    if (SleepConditionVariableCS(&state->condition, cs, timeout)) return 0;
    error = GetLastError();
    if (error == ERROR_TIMEOUT) return ETIMEDOUT;
    return pthread_win32_error(error);
}

int pthread_cond_wait(win32_pthread_abi_cond_t *cond,
                      win32_pthread_abi_mutex_t *mutex)
{
    return pthread_abi_cond_wait(cond, mutex, INFINITE);
}

static DWORD pthread_abi_timeout_from_timespec64(
    const struct _timespec64 *abstime)
{
    const ULONGLONG windows_epoch = 116444736000000000ULL;
    FILETIME file_time;
    ULARGE_INTEGER now;
    ULONGLONG target;
    ULONGLONG difference;
    ULONGLONG milliseconds;

    if (abstime == NULL || abstime->tv_sec < 0 ||
        abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L)
        return 0;
    if ((ULONGLONG)abstime->tv_sec >
        (ULLONG_MAX - windows_epoch) / 10000000ULL)
        return INFINITE - 1;

    target = windows_epoch +
             (ULONGLONG)abstime->tv_sec * 10000000ULL +
             (ULONGLONG)abstime->tv_nsec / 100ULL;
    GetSystemTimeAsFileTime(&file_time);
    now.LowPart = file_time.dwLowDateTime;
    now.HighPart = file_time.dwHighDateTime;
    if (target <= now.QuadPart) return 0;

    difference = target - now.QuadPart;
    milliseconds = (difference + 9999ULL) / 10000ULL;
    return milliseconds >= INFINITE ? INFINITE - 1 : (DWORD)milliseconds;
}

int pthread_cond_timedwait64(win32_pthread_abi_cond_t *cond,
                             win32_pthread_abi_mutex_t *mutex,
                             const struct _timespec64 *abstime)
{
    if (abstime == NULL || abstime->tv_sec < 0 ||
        abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L)
        return EINVAL;
    return pthread_abi_cond_wait(
        cond, mutex, pthread_abi_timeout_from_timespec64(abstime));
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

    tls_key = FlsAlloc((PFLS_CALLBACK_FUNCTION) destructor);
    if (tls_key == TLS_OUT_OF_INDEXES) {
        return EAGAIN;
    }

    *key = (win32_pthread_abi_key_t) tls_key;
    return 0;
}

int pthread_key_delete(win32_pthread_abi_key_t key) {
    return FlsFree((DWORD) key) ? 0 : EINVAL;
}

void *pthread_getspecific(win32_pthread_abi_key_t key) {
    return FlsGetValue((DWORD) key);
}

int pthread_setspecific(win32_pthread_abi_key_t key, const void *value) {
    return FlsSetValue((DWORD) key, (LPVOID) value) ? 0 : EINVAL;
}
#endif
