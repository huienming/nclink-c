/* NC-Link core - platform services (time, sleep, entropy). */
#include "nclink/ncl_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(NCL_OS_WINDOWS)
#  include <process.h>
#  include <wincrypt.h>
#  pragma comment(lib, "advapi32.lib")
#else
#  include <errno.h>
#  include <pthread.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

struct ncl_mutex {
#if defined(NCL_OS_WINDOWS)
    CRITICAL_SECTION cs;
    bool             initialised;
#else
    pthread_mutex_t  handle;
#endif
};

ncl_mutex *ncl_mutex_create(void)
{
    ncl_mutex *m = (ncl_mutex *)calloc(1, sizeof(ncl_mutex));
    if (m == NULL) {
        return NULL;
    }
#if defined(NCL_OS_WINDOWS)
    InitializeCriticalSection(&m->cs);
    m->initialised = true;
#else
    if (pthread_mutex_init(&m->handle, NULL) != 0) {
        free(m);
        return NULL;
    }
#endif
    return m;
}

void ncl_mutex_destroy(ncl_mutex *m)
{
    if (m == NULL) {
        return;
    }
#if defined(NCL_OS_WINDOWS)
    if (m->initialised) {
        DeleteCriticalSection(&m->cs);
    }
#else
    pthread_mutex_destroy(&m->handle);
#endif
    free(m);
}

void ncl_mutex_lock(ncl_mutex *m)
{
    if (m == NULL) {
        return;
    }
#if defined(NCL_OS_WINDOWS)
    EnterCriticalSection(&m->cs);
#else
    pthread_mutex_lock(&m->handle);
#endif
}

void ncl_mutex_unlock(ncl_mutex *m)
{
    if (m == NULL) {
        return;
    }
#if defined(NCL_OS_WINDOWS)
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->handle);
#endif
}

/* ------------------------------------------------------- condition vars --- */

struct ncl_cond {
#if defined(NCL_OS_WINDOWS)
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t     handle;
#endif
};

ncl_cond *ncl_cond_create(void)
{
    ncl_cond *c = (ncl_cond *)calloc(1, sizeof(ncl_cond));
    if (c == NULL) {
        return NULL;
    }
#if defined(NCL_OS_WINDOWS)
    InitializeConditionVariable(&c->cv);
#else
    if (pthread_cond_init(&c->handle, NULL) != 0) {
        free(c);
        return NULL;
    }
#endif
    return c;
}

void ncl_cond_destroy(ncl_cond *c)
{
    if (c == NULL) {
        return;
    }
#if !defined(NCL_OS_WINDOWS)
    pthread_cond_destroy(&c->handle);
#endif
    free(c);
}

void ncl_cond_wait(ncl_cond *c, ncl_mutex *m)
{
    ncl_cond_wait_timeout(c, m, 0);
}

bool ncl_cond_wait_timeout(ncl_cond *c, ncl_mutex *m, unsigned timeout_ms)
{
    if (c == NULL || m == NULL) {
        return false;
    }
#if defined(NCL_OS_WINDOWS)
    return SleepConditionVariableCS(&c->cv, &m->cs,
                                    timeout_ms == 0 ? INFINITE : (DWORD)timeout_ms)
               ? true
               : false;
#else
    if (timeout_ms == 0) {
        return pthread_cond_wait(&c->handle, &m->handle) == 0;
    }
    {
        struct timespec ts;
        struct timeval now;
        int rc;
        gettimeofday(&now, NULL);
        ts.tv_sec = now.tv_sec + (time_t)(timeout_ms / 1000u);
        ts.tv_nsec = (long)(now.tv_usec * 1000) +
                     (long)(timeout_ms % 1000u) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        rc = pthread_cond_timedwait(&c->handle, &m->handle, &ts);
        return rc == 0;
    }
#endif
}

void ncl_cond_signal(ncl_cond *c)
{
    if (c == NULL) {
        return;
    }
#if defined(NCL_OS_WINDOWS)
    WakeConditionVariable(&c->cv);
#else
    pthread_cond_signal(&c->handle);
#endif
}

void ncl_cond_broadcast(ncl_cond *c)
{
    if (c == NULL) {
        return;
    }
#if defined(NCL_OS_WINDOWS)
    WakeAllConditionVariable(&c->cv);
#else
    pthread_cond_broadcast(&c->handle);
#endif
}

/* --------------------------------------------------------------- threads -- */

struct ncl_thread {
#if defined(NCL_OS_WINDOWS)
    HANDLE handle;
#else
    pthread_t handle;
#endif
    ncl_thread_fn fn;
    void         *arg;
};

#if defined(NCL_OS_WINDOWS)
static unsigned __stdcall ncl_thread_trampoline(void *param)
{
    ncl_thread *t = (ncl_thread *)param;
    t->fn(t->arg);
    return 0;
}
#else
static void *ncl_thread_trampoline(void *param)
{
    ncl_thread *t = (ncl_thread *)param;
    t->fn(t->arg);
    return NULL;
}
#endif

ncl_thread *ncl_thread_start(ncl_thread_fn fn, void *arg)
{
    ncl_thread *t;

    if (fn == NULL) {
        return NULL;
    }
    t = (ncl_thread *)calloc(1, sizeof(ncl_thread));
    if (t == NULL) {
        return NULL;
    }
    t->fn = fn;
    t->arg = arg;

#if defined(NCL_OS_WINDOWS)
    t->handle = (HANDLE)_beginthreadex(NULL, 0, ncl_thread_trampoline, t, 0, NULL);
    if (t->handle == NULL) {
        free(t);
        return NULL;
    }
#else
    if (pthread_create(&t->handle, NULL, ncl_thread_trampoline, t) != 0) {
        free(t);
        return NULL;
    }
#endif
    return t;
}

ncl_err ncl_thread_join(ncl_thread *t)
{
    if (t == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
#if defined(NCL_OS_WINDOWS)
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
#else
    pthread_join(t->handle, NULL);
#endif
    free(t);
    return NCL_OK;
}

void ncl_thread_detach(ncl_thread *t)
{
    if (t == NULL) {
        return;
    }
#if defined(NCL_OS_WINDOWS)
    CloseHandle(t->handle);
#else
    pthread_detach(t->handle);
#endif
    free(t);
}

int64_t ncl_time_millis(void)
{
#if defined(NCL_OS_WINDOWS)
    FILETIME ft;
    ULARGE_INTEGER li;
    /* 100 ns ticks between 1601-01-01 and 1970-01-01. */
    const uint64_t epoch_delta = 116444736000000000ULL;

    GetSystemTimeAsFileTime(&ft);
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    if (li.QuadPart < epoch_delta) {
        return 0;
    }
    return (int64_t)((li.QuadPart - epoch_delta) / 10000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
#endif
}

int64_t ncl_time_monotonic_millis(void)
{
#if defined(NCL_OS_WINDOWS)
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
#endif
}

void ncl_sleep_millis(unsigned ms)
{
#if defined(NCL_OS_WINDOWS)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        /* resume the remaining interval */
    }
#endif
}

bool ncl_random_bytes(void *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return false;
    }
#if defined(NCL_OS_WINDOWS)
    {
        HCRYPTPROV prov = 0;
        BOOL ok;
        if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL,
                                  CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
            return false;
        }
        ok = CryptGenRandom(prov, (DWORD)len, (BYTE *)buf);
        CryptReleaseContext(prov, 0);
        return ok ? true : false;
    }
#else
    {
        FILE *fp = fopen("/dev/urandom", "rb");
        size_t got;
        if (fp != NULL) {
            got = fread(buf, 1, len, fp);
            fclose(fp);
            if (got == len) {
                return true;
            }
        }
        /* Fallback: mix clock and process id, good enough for UUIDs on a
         * system without /dev/urandom. */
        {
            uint8_t *p = (uint8_t *)buf;
            uint64_t seed = (uint64_t)ncl_time_monotonic_millis() ^
                            ((uint64_t)ncl_time_millis() << 21) ^
                            ((uint64_t)ncl_process_id() << 3);
            for (size_t i = 0; i < len; i++) {
                /* xorshift64* */
                seed ^= seed >> 12;
                seed ^= seed << 25;
                seed ^= seed >> 27;
                p[i] = (uint8_t)((seed * 2685821657736338717ULL) >> 33);
            }
            return true;
        }
    }
#endif
}

long ncl_process_id(void)
{
#if defined(NCL_OS_WINDOWS)
    return (long)_getpid();
#else
    return (long)getpid();
#endif
}
