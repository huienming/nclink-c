/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - logger.
 *
 * One file, <root>/log/out.txt, rotated at 10 MB, plus optional console output.
 * Every line is prefixed with the local time and the level.
 */
#ifndef NCL_LOGGER_H
#define NCL_LOGGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NCL_LOG_DEBUG = 0,
    NCL_LOG_INFO,
    NCL_LOG_WARN,
    NCL_LOG_ERROR,
    NCL_LOG_NONE
} ncl_log_level;

/**
 * Start writing to <log_dir>/out.txt. When @p log_dir is NULL the default
 * from ncl_env_log_path() is used. Safe to call more than once.
 * Returns false when the file could not be opened (console output still works).
 */
bool ncl_log_init(const char *log_dir);

/** Stop writing to the file and flush. */
void ncl_log_shutdown(void);

/** Enable or disable the stderr mirror (default: enabled). */
void ncl_log_set_console(bool enabled);

/** Minimum level written to the file and console (default: NCL_LOG_INFO). */
void ncl_log_set_level(ncl_log_level level);

void ncl_log_write(ncl_log_level level, const char *fmt, ...);

/** Informational line. */
void ncl_log_info(const char *fmt, ...);

/** Error line. */
void ncl_log_error(const char *fmt, ...);

/** Warning and debug lines; useful for MQTT tracing. */
void ncl_log_warn(const char *fmt, ...);
void ncl_log_debug(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* NCL_LOGGER_H */
