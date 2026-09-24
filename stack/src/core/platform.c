/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - platform services (time, sleep, entropy). */
#include "nclink/ncl_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(NCL_OS_WINDOWS)
#  include <mmsystem.h>
#  include <process.h>
#  include <wincrypt.h>
#  pragma comment(lib, "advapi32.lib")
#  pragma comment(lib, "winmm.lib")
#else
#  include <errno.h>
#  include <pthread.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

/* 每线程变量：Windows/MSVC 用 __declspec(thread)，POSIX 用 C11 的 _Thread_local。 */
#if defined(NCL_OS_WINDOWS)
#  define NCL_THREAD_LOCAL __declspec(thread)
#else
#  define NCL_THREAD_LOCAL _Thread_local
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
    ncl_mutex *m = (ncl_mutex *)ncl_mem_calloc(1, sizeof(ncl_mutex));
    if (m == NULL) {
        return NULL;
    }
#if defined(NCL_OS_WINDOWS)
    InitializeCriticalSection(&m->cs);
    m->initialised = true;
#else
    if (pthread_mutex_init(&m->handle, NULL) != 0) {
        ncl_mem_free(m);
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
    ncl_mem_free(m);
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

/* ------------------------------------------------- Windows 高精度等待 --- */

#if defined(NCL_OS_WINDOWS)

/*
 * Windows 默认的时钟粒度是 ~15.6 ms：`Sleep(1)`、`SleepConditionVariableCS(_, _, 1)`
 * 实测都要 14~15 ms，于是 1 ms 的采样槽位变成 15 ms 一跳。这里让小等待改走两条路，
 * 优先第一条：
 *
 *   1. **高精度可等待计时器**（Win10 1803+）：`CreateWaitableTimerEx` 带
 *      `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`，精度 ~0.5 ms，不动系统时钟；
 *   2. **回退**：把系统时钟粒度提到 1 ms（`timeBeginPeriod(1)`）再用普通等待 ——
 *      精度约 1.7~1.9 ms，够 1 ms 槽位用了（老系统上才走这条）。
 *
 * 计时器对象自带"到期时间"状态，多线程共用会互相踩，所以按线程持有
 * （`NCL_THREAD_LOCAL`），线程退出时在 `ncl_thread_trampoline()` 里收掉。
 * 只有 ≤ NCL_HR_WAIT_MAX_MS 的等待走这条路；更长的等待仍交给系统 wait（省电，也
 * 不必一直吊着高精度计时器）。
 */
#define NCL_HR_WAIT_MAX_MS 100u

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#  define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static NCL_THREAD_LOCAL HANDLE t_hr_timer;   /* 本线程的高精度计时器，惰性创建 */
static bool g_hr_timer_probed = false;       /* 是否探测过系统支持 */
static bool g_hr_timer_usable = false;       /* 支持则为 true，否则走回退路径 */
static volatile LONG g_timer_period_raised = 0;

/** 回退路径：把系统时钟粒度提到 1 ms，只提一次（进程退出时系统自己复位）。 */
static void ncl_win_timer_period_raise(void)
{
    if (InterlockedCompareExchange(&g_timer_period_raised, 1, 0) == 0) {
        timeBeginPeriod(1);
    }
}

static HANDLE ncl_win_hr_timer_create(void)
{
    return CreateWaitableTimerExW(NULL, NULL,
                                  CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                  TIMER_ALL_ACCESS);
}

/** 本线程的高精度计时器；系统不支持时返回 NULL（调用方走回退路径）。 */
static HANDLE ncl_win_hr_timer(void)
{
    if (!g_hr_timer_probed) {
        g_hr_timer_probed = true;
        t_hr_timer = ncl_win_hr_timer_create();
        g_hr_timer_usable = t_hr_timer != NULL;
        if (!g_hr_timer_usable) {
            ncl_win_timer_period_raise();
        }
    }
    if (!g_hr_timer_usable) {
        return NULL;
    }
    if (t_hr_timer == NULL) {
        t_hr_timer = ncl_win_hr_timer_create();
        if (t_hr_timer == NULL) {
            ncl_win_timer_period_raise();
        }
    }
    return t_hr_timer;
}

/** 线程收尾：把本线程的计时器句柄还掉（线程退出不会自动关句柄）。 */
static void ncl_win_hr_timer_thread_cleanup(void)
{
    if (t_hr_timer != NULL) {
        CloseHandle(t_hr_timer);
        t_hr_timer = NULL;
    }
}

/** 等 @p millis 毫秒，尽量准（高精度计时器 → 回退到提高粒度后的 Sleep）。 */
static void ncl_win_wait_millis(unsigned millis)
{
    HANDLE timer = millis > 0 && millis <= NCL_HR_WAIT_MAX_MS
                       ? ncl_win_hr_timer()
                       : NULL;

    if (timer != NULL) {
        LARGE_INTEGER due;
        /* 负值 = 相对时间，单位 100 ns */
        due.QuadPart = -(LONGLONG)millis * 10000;
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) &&
            WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) {
            return;
        }
        ncl_win_timer_period_raise();
    }
    if (millis > 0 && millis <= NCL_HR_WAIT_MAX_MS) {
        ncl_win_timer_period_raise();
    }
    Sleep((DWORD)millis);
}

#else

#define NCL_HR_WAIT_MAX_MS 100u

#endif /* NCL_OS_WINDOWS */

/* ------------------------------------------------------- condition vars --- */

struct ncl_cond {
#if defined(NCL_OS_WINDOWS)
    /* 不用 CONDITION_VARIABLE 的原因：它的超时精度是系统时钟粒度（~15.6 ms），
     * 而这里的等待要能和"高精度计时器"一起等（WaitForMultipleObjects），所以改成
     * 计数信号量 + 等人数：signal 放 1 个、broadcast 放 waiters 个，等待方回到
     * 用户锁后再由调用方复查谓词。语义与 condvar 一致（没有等的人时信号同样丢弃）。 */
    HANDLE           sem;
    CRITICAL_SECTION lock;
    unsigned         waiters;
#else
    pthread_cond_t   handle;
#endif
};

ncl_cond *ncl_cond_create(void)
{
    ncl_cond *c = (ncl_cond *)ncl_mem_calloc(1, sizeof(ncl_cond));
    if (c == NULL) {
        return NULL;
    }
#if defined(NCL_OS_WINDOWS)
    c->sem = CreateSemaphoreW(NULL, 0, 0x7fffffffL, NULL);
    if (c->sem == NULL) {
        ncl_mem_free(c);
        return NULL;
    }
    InitializeCriticalSection(&c->lock);
#else
    if (pthread_cond_init(&c->handle, NULL) != 0) {
        ncl_mem_free(c);
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
#if defined(NCL_OS_WINDOWS)
    DeleteCriticalSection(&c->lock);
    CloseHandle(c->sem);
#else
    pthread_cond_destroy(&c->handle);
#endif
    ncl_mem_free(c);
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
    {
        bool signalled = false;
        HANDLE timer = (timeout_ms > 0 && timeout_ms <= NCL_HR_WAIT_MAX_MS)
                           ? ncl_win_hr_timer()
                           : NULL;

        EnterCriticalSection(&c->lock);
        c->waiters++;
        LeaveCriticalSection(&c->lock);
        LeaveCriticalSection(&m->cs);

        if (timer != NULL) {
            LARGE_INTEGER due;
            HANDLE waits[2];

            /* 负值 = 相对时间，单位 100 ns */
            due.QuadPart = -(LONGLONG)timeout_ms * 10000;
            waits[0] = c->sem;      /* 0：被 signal 唤醒 */
            waits[1] = timer;       /* 1：等够时间（高精度） */
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
                signalled = WaitForMultipleObjects(2, waits, FALSE, INFINITE) ==
                            WAIT_OBJECT_0;
            } else {
                ncl_win_timer_period_raise();
                signalled = WaitForSingleObject(c->sem, (DWORD)timeout_ms) ==
                            WAIT_OBJECT_0;
            }
        } else {
            if (timeout_ms > 0 && timeout_ms <= NCL_HR_WAIT_MAX_MS) {
                ncl_win_timer_period_raise();
            }
            signalled = WaitForSingleObject(c->sem,
                                            timeout_ms == 0 ? INFINITE
                                                            : (DWORD)timeout_ms) ==
                        WAIT_OBJECT_0;
        }

        EnterCriticalSection(&c->lock);
        c->waiters--;
        LeaveCriticalSection(&c->lock);
        EnterCriticalSection(&m->cs);
        return signalled;
    }
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
    EnterCriticalSection(&c->lock);
    if (c->waiters > 0) {
        ReleaseSemaphore(c->sem, 1, NULL);
    }
    LeaveCriticalSection(&c->lock);
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
    EnterCriticalSection(&c->lock);
    if (c->waiters > 0) {
        ReleaseSemaphore(c->sem, (LONG)c->waiters, NULL);
    }
    LeaveCriticalSection(&c->lock);
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
    ncl_win_hr_timer_thread_cleanup();
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
    t = (ncl_thread *)ncl_mem_calloc(1, sizeof(ncl_thread));
    if (t == NULL) {
        return NULL;
    }
    t->fn = fn;
    t->arg = arg;

#if defined(NCL_OS_WINDOWS)
    t->handle = (HANDLE)_beginthreadex(NULL, 0, ncl_thread_trampoline, t, 0, NULL);
    if (t->handle == NULL) {
        ncl_mem_free(t);
        return NULL;
    }
#else
    if (pthread_create(&t->handle, NULL, ncl_thread_trampoline, t) != 0) {
        ncl_mem_free(t);
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
    ncl_mem_free(t);
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
    ncl_mem_free(t);
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

int64_t ncl_time_monotonic_us(void)
{
#if defined(NCL_OS_WINDOWS)
    LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0 ||
        !QueryPerformanceCounter(&now)) {
        return (int64_t)GetTickCount64() * 1000; /* 退回到毫秒钟 */
    }
    return (int64_t)((now.QuadPart / freq.QuadPart) * 1000000LL +
                     ((now.QuadPart % freq.QuadPart) * 1000000LL) / freq.QuadPart);
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)(ts.tv_nsec / 1000);
#endif
}

void ncl_sleep_millis(unsigned ms)
{
#if defined(NCL_OS_WINDOWS)
    if (ms > 0 && ms <= NCL_HR_WAIT_MAX_MS) {
        ncl_win_wait_millis(ms);
        return;
    }
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

/* --------------------------------------------------------------- console -- */

#if defined(NCL_OS_WINDOWS)

static bool ncl_console_is_terminal(HANDLE handle)
{
    DWORD mode = 0;

    return handle != NULL && handle != INVALID_HANDLE_VALUE &&
           GetConsoleMode(handle, &mode) != 0;
}

/** UTF-8 -> UTF-16；返回的缓冲调用方 free，*out_len 含结尾 NUL。 */
static wchar_t *ncl_utf8_to_wide(const char *text, int *out_len)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t *wide;

    if (len <= 1) { /* 空串或转换失败 */
        return NULL;
    }
    wide = (wchar_t *)ncl_mem_alloc((size_t)len * sizeof(wchar_t));
    if (wide == NULL) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, len) <= 0) {
        ncl_mem_free(wide);
        return NULL;
    }
    *out_len = len;
    return wide;
}

/** UTF-8 -> 本地 ANSI 代码页（调试器/重定向按它解码）。 */
static bool ncl_wide_to_ansi(const wchar_t *wide, int wide_len)
{
    int len = WideCharToMultiByte(CP_ACP, 0, wide, wide_len, NULL, 0, NULL, NULL);
    char *ansi;

    if (len <= 0) {
        return false;
    }
    ansi = (char *)ncl_mem_alloc((size_t)len);
    if (ansi == NULL) {
        return false;
    }
    if (WideCharToMultiByte(CP_ACP, 0, wide, wide_len, ansi, len, NULL, NULL) <= 0) {
        ncl_mem_free(ansi);
        return false;
    }
    fwrite(ansi, 1, (size_t)len, stderr);
    fflush(stderr);
    ncl_mem_free(ansi);
    return true;
}

#endif /* NCL_OS_WINDOWS */

void ncl_console_write(const char *text)
{
    if (text == NULL) {
        return;
    }
#if defined(NCL_OS_WINDOWS)
    {
        HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
        const char *mode = getenv("NCL_CONSOLE_ENCODING");
        bool force_utf8 = mode != NULL && ncl_strcasecmp(mode, "utf8") == 0;

        if (!force_utf8) {
            int wide_len = 0;
            wchar_t *wide = ncl_utf8_to_wide(text, &wide_len);

            if (wide != NULL) {
                if (ncl_console_is_terminal(handle)) {
                    DWORD written = 0;

                    /* 真控制台：宽字符直写，任何代码页下中文都对 */
                    WriteConsoleW(handle, wide, (DWORD)(wide_len - 1), &written,
                                  NULL);
                    ncl_mem_free(wide);
                    return;
                }
                if (ncl_wide_to_ansi(wide, wide_len - 1)) {
                    ncl_mem_free(wide);
                    return;
                }
                ncl_mem_free(wide);
            }
        }
        fputs(text, stderr);
        fflush(stderr);
    }
#else
    fputs(text, stderr);
#endif
}
