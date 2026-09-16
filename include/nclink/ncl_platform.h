/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - platform abstraction (Windows / POSIX).
 * Part of the NC-Link middleware (GB/T 41970-2022).
 */
#ifndef NCL_PLATFORM_H
#define NCL_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ncl_err is used by the thread helpers below; ncl_common.h has no dependency
 * on this header, so including it here cannot create a cycle. */
#include "nclink/ncl_common.h"

#if defined(_WIN32) || defined(_WIN64)
#  define NCL_OS_WINDOWS 1
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  define NCL_PATH_SEP '\\'
#  define ncl_strcasecmp _stricmp
#  define ncl_strncasecmp _strnicmp
#else
#  define NCL_OS_POSIX 1
#  include <strings.h>
#  define NCL_PATH_SEP '/'
#  define ncl_strcasecmp strcasecmp
#  define ncl_strncasecmp strncasecmp
#endif

/** Largest file system path the library builds on its own stack buffers. */
#define NCL_PATH_MAX_BUF 4096

#ifdef __cplusplus
extern "C" {
#endif

/** Milliseconds since the Unix epoch (wall clock). */
int64_t ncl_time_millis(void);

/** Monotonic milliseconds, suitable for measuring intervals. */
int64_t ncl_time_monotonic_millis(void);

/** Sleep for the given number of milliseconds. */
void ncl_sleep_millis(unsigned ms);

/**
 * Fill @p buf with @p len cryptographically-seeded random bytes.
 * Returns true on success.
 */
bool ncl_random_bytes(void *buf, size_t len);

/** Process id, used for diagnostics. */
long ncl_process_id(void);

/**
 * 把一段 UTF-8 文本写到 stderr（日志的控制台镜像走这里）。
 *
 * 库里所有文案都是 UTF-8。Windows 上"控制台"有两种形态，解码规则不一样，
 * 所以这里分开处理，中文才不会变成乱码：
 *
 *   - 真控制台（cmd / PowerShell / VS Code 终端 / ConPTY）：转宽字符走
 *     WriteConsoleW，跟当前代码页无关，中文在任何 CP 下都正确；
 *   - 管道（VS Code 调试控制台、`> file` 重定向）：上游按**系统 ANSI 代码页**
 *     解码，所以先转成本地 ANSI 再写。
 *
 * 两处都用 `NCL_CONSOLE_ENCODING` 环境变量兜底：置为 `utf8` 时强制按 UTF-8
 * 原样写（默认 `auto`）。POSIX 上就是 `fputs(text, stderr)`。
 */
void ncl_console_write(const char *text);

/* --------------------------------------------------------------- mutexes -- */

/** Recursive-safe mutual exclusion primitive used across the library. */
typedef struct ncl_mutex ncl_mutex;

ncl_mutex *ncl_mutex_create(void);
void       ncl_mutex_destroy(ncl_mutex *m);
void       ncl_mutex_lock(ncl_mutex *m);
void       ncl_mutex_unlock(ncl_mutex *m);

/* --------------------------------------------------------- condition vars - */

typedef struct ncl_cond ncl_cond;

ncl_cond *ncl_cond_create(void);
void      ncl_cond_destroy(ncl_cond *c);
/** Wait until signalled; @p m must be held and is re-acquired on return. */
void      ncl_cond_wait(ncl_cond *c, ncl_mutex *m);
/** Wait at most @p timeout_ms (0 means "no limit"). Returns true when signalled. */
bool      ncl_cond_wait_timeout(ncl_cond *c, ncl_mutex *m, unsigned timeout_ms);
void      ncl_cond_signal(ncl_cond *c);
void      ncl_cond_broadcast(ncl_cond *c);

/* --------------------------------------------------------------- threads -- */

typedef struct ncl_thread ncl_thread;
typedef void (*ncl_thread_fn)(void *arg);

/** Start a thread running @p fn. Returns NULL on failure. */
ncl_thread *ncl_thread_start(ncl_thread_fn fn, void *arg);

/** Block until the thread finishes, then release the handle. */
ncl_err ncl_thread_join(ncl_thread *t);

/** Release the handle without waiting (the thread keeps running). */
void ncl_thread_detach(ncl_thread *t);

#ifdef __cplusplus
}
#endif

#endif /* NCL_PLATFORM_H */
