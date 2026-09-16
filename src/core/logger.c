/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - logger implementation. */
#include "nclink/ncl_logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_common.h"
#include "nclink/ncl_platform.h"

#define NCL_LOG_FILE_MAX_BYTES (10 * 1024 * 1024) /* 10 MB, single file        */
#define NCL_LOG_LINE_MAX 4096

static FILE *g_log_file = NULL;
static char *g_log_path = NULL;
static long g_log_bytes = 0;
static bool g_log_console = true;
static ncl_log_level g_log_level = NCL_LOG_INFO;
static ncl_mutex *g_log_mutex = NULL;

static ncl_mutex *ncl_log_mutex(void)
{
    if (g_log_mutex == NULL) {
        g_log_mutex = ncl_mutex_create();
    }
    return g_log_mutex;
}

static const char *ncl_log_level_name(ncl_log_level level)
{
    switch (level) {
    case NCL_LOG_DEBUG: return "DEBUG";
    case NCL_LOG_INFO: return "INFO";
    case NCL_LOG_WARN: return "WARNING";
    case NCL_LOG_ERROR: return "SEVERE";
    default: return "INFO";
    }
}

static void ncl_log_format_timestamp(char *buf, size_t len)
{
#if defined(NCL_OS_WINDOWS)
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(buf, len, _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds);
#else
    time_t now = time(NULL);
    struct tm tmv;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&now, &tmv);
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tmv.tm_year + 1900,
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             (int)(ts.tv_nsec / 1000000));
#endif
}

bool ncl_log_init(const char *log_dir)
{
    const char *dir = log_dir != NULL ? log_dir : ncl_env_log_path();
    char *path = NULL;
    ncl_mutex *mtx = ncl_log_mutex();

    ncl_mutex_lock(mtx);
    if (g_log_file != NULL) {
        ncl_mutex_unlock(mtx);
        return true;
    }
    if (dir == NULL) {
        ncl_mutex_unlock(mtx);
        return false;
    }
    if (ncl_mkdir_p(dir) != NCL_OK) {
        ncl_mutex_unlock(mtx);
        return false;
    }
    if (ncl_asprintf(&path, "%s%cout.txt", dir, NCL_PATH_SEP) != NCL_OK) {
        ncl_mutex_unlock(mtx);
        return false;
    }
    /* Append mode; the file is rotated once it passes the limit. */
    g_log_file = fopen(path, "ab");
    if (g_log_file == NULL) {
        free(path);
        ncl_mutex_unlock(mtx);
        return false;
    }
    g_log_path = path;
    g_log_bytes = ftell(g_log_file);
    ncl_mutex_unlock(mtx);
    return true;
}

void ncl_log_shutdown(void)
{
    ncl_mutex *mtx = g_log_mutex;
    if (mtx == NULL) {
        return;
    }
    ncl_mutex_lock(mtx);
    if (g_log_file != NULL) {
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }
    free(g_log_path);
    g_log_path = NULL;
    g_log_bytes = 0;
    ncl_mutex_unlock(mtx);
    ncl_mutex_destroy(mtx);
    g_log_mutex = NULL;
}

void ncl_log_set_console(bool enabled)
{
    g_log_console = enabled;
}

void ncl_log_set_level(ncl_log_level level)
{
    g_log_level = level;
}

/* Rotate the single file once it passes the limit. */
static void ncl_log_maybe_rotate(void)
{
    char *rotated = NULL;
    if (g_log_file == NULL || g_log_bytes < NCL_LOG_FILE_MAX_BYTES) {
        return;
    }
    if (ncl_asprintf(&rotated, "%s.1", g_log_path) != NCL_OK) {
        return;
    }
    fclose(g_log_file);
    g_log_file = NULL;
    remove(rotated);
    if (rename(g_log_path, rotated) == 0) {
        g_log_file = fopen(g_log_path, "ab");
        g_log_bytes = 0;
    } else {
        g_log_file = fopen(g_log_path, "ab");
    }
    free(rotated);
}

void ncl_log_write(ncl_log_level level, const char *fmt, ...)
{
    char message[NCL_LOG_LINE_MAX];
    char line[NCL_LOG_LINE_MAX + 64];
    char timestamp[40];
    va_list ap;
    int written;

    if (fmt == NULL || level < g_log_level) {
        return;
    }

    va_start(ap, fmt);
    written = vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    if (written < 0) {
        return;
    }

    ncl_log_format_timestamp(timestamp, sizeof(timestamp));
    snprintf(line, sizeof(line), "%s %-7s [%ld] %s\n", timestamp,
             ncl_log_level_name(level), ncl_process_id(), message);

    {
        ncl_mutex *mtx = ncl_log_mutex();
        ncl_mutex_lock(mtx);
    if (g_log_file != NULL) {
        size_t n = fwrite(line, 1, strlen(line), g_log_file);
        g_log_bytes += (long)n;
        fflush(g_log_file);
        ncl_log_maybe_rotate();
    }
    if (g_log_console) {
        ncl_console_write(line);
    }
        ncl_mutex_unlock(mtx);
    }
}

void ncl_log_info(const char *fmt, ...)
{
    va_list ap;
    char message[NCL_LOG_LINE_MAX];
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    ncl_log_write(NCL_LOG_INFO, "%s", message);
}

void ncl_log_error(const char *fmt, ...)
{
    va_list ap;
    char message[NCL_LOG_LINE_MAX];
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    ncl_log_write(NCL_LOG_ERROR, "%s", message);
}

void ncl_log_warn(const char *fmt, ...)
{
    va_list ap;
    char message[NCL_LOG_LINE_MAX];
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    ncl_log_write(NCL_LOG_WARN, "%s", message);
}

void ncl_log_debug(const char *fmt, ...)
{
    va_list ap;
    char message[NCL_LOG_LINE_MAX];
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    ncl_log_write(NCL_LOG_DEBUG, "%s", message);
}
