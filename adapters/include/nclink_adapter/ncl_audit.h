/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the audit trail the specification asks for.
 *
 * 00-通用-实现约定.md §6 wants four things from an adapter that is wired to a
 * real machine, and this module gives all four:
 *
 *   1. session lifecycle      - open / close / reconnect, with the reason
 *   2. every request          - path, address, outcome, duration, and the raw
 *                               bytes when the driver can hand them over
 *   3. a code histogram       - how many failures of each tier and code, so a
 *                               cable problem can be told from a protocol one
 *   4. write auditing         - which address, the old value, the new value and
 *                               who asked for it
 *
 * Lines go through the library logger, so they land in <root>/log/out.txt and
 * on the console like everything else. §7 asks for writes to be opt-in and the
 * daemon is what refuses a write to a point that was not declared "writable";
 * every write that does reach the driver - accepted or refused by it - is
 * recorded here in full, old value and all.
 */
#ifndef NCL_AUDIT_H
#define NCL_AUDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_json.h"
#include "nclink_adapter/ncl_driver.h" /* the tiered error codes it counts */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool        enabled;  /**< default true: the trail is the point of it */
    bool        raw;      /**< also log frame bytes when a driver offers them */
    const char *operator_name; /**< who is driving, for the write audit */
} ncl_audit_options;

/** Fill @p options with the defaults (enabled, no raw bytes, no operator). */
void ncl_audit_options_default(ncl_audit_options *options);
/** Install the options. Passing NULL restores the defaults. */
void ncl_audit_init(const ncl_audit_options *options);
/** True when the trail is on. */
bool ncl_audit_enabled(void);
/** True when raw bytes are wanted as well. */
bool ncl_audit_wants_raw(void);
/** Drop the counters (the log file is not touched). */
void ncl_audit_reset_stats(void);

/**
 * One request: @p code is the tiered outcome, @p micros how long it took and
 * @p raw the frames the driver exchanged, when it has any to show and the
 * options asked for them (borrowed; may be NULL).
 */
void ncl_audit_request(const char *link, const char *path, const char *address,
                       int code, int64_t micros, const ncl_driver_raw *raw);

/**
 * One write, in full: the old value (NULL when it could not be read), the new
 * one and the outcome (§6: "改了哪个地址、旧值、新值、操作者").
 */
void ncl_audit_write(const char *link, const char *path, const char *address,
                     const ncl_json *old_value, const ncl_json *new_value,
                     int code);

/** One session event: "open", "close", "reconnect", plus a reason. */
void ncl_audit_session(const char *link, const char *event, const char *detail);

/**
 * The counters as JSON: totals, one counter per error tier, the code histogram
 * and the last few writes. This is what a REST endpoint or a `--stats` run
 * prints, and what the tests assert on.
 */
ncl_json *ncl_audit_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_AUDIT_H */
