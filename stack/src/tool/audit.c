/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the audit trail (see ncl_audit.h).
 *
 * Two halves: the log lines, which go through the library logger so they land
 * where every other line lands, and the counters, which answer "is it the
 * cable or the protocol" without reading the log.
 */

#include "nclink/ncl_audit.h"

#include <stdio.h>
#include <string.h>

#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"

#define NCL_AUDIT_CODES 32
#define NCL_AUDIT_WRITES 8
/** How much of a frame the raw line shows: 96 bytes of hex plus its NUL. */
#define NCL_AUDIT_HEX_MAX 96

typedef struct {
    int    code;
    size_t count;
} audit_code;

typedef struct {
    char     path[128];
    char     address[48];
    char     from[48];
    char     to[48];
    int      code;
    size_t   index;
} audit_write;

static ncl_audit_options g_options = {true, false, NULL};
static ncl_mutex        *g_lock;
static bool              g_stats_reset; /* options initialised? */
static size_t            g_requests;
static size_t            g_writes;
static size_t            g_tiers[4]; /**< 0 ok, 1 transport, 2 protocol, 3 business */
static size_t            g_sessions;
static size_t            g_reconnects;
static audit_code        g_codes[NCL_AUDIT_CODES];
static size_t            g_code_count;
static audit_write       g_write_log[NCL_AUDIT_WRITES];
static size_t            g_write_next;

static ncl_mutex *audit_lock(void)
{
    if (g_lock == NULL) {
        g_lock = ncl_mutex_create();
    }
    return g_lock;
}

void ncl_audit_options_default(ncl_audit_options *options)
{
    if (options == NULL) {
        return;
    }
    options->enabled = true;
    options->raw = false;
    options->operator_name = NULL;
}

void ncl_audit_init(const ncl_audit_options *options)
{
    ncl_audit_options defaults;

    ncl_audit_options_default(&defaults);
    ncl_mutex_lock(audit_lock());
    g_options = options != NULL ? *options : defaults;
    g_stats_reset = true;
    ncl_mutex_unlock(audit_lock());
}

bool ncl_audit_enabled(void)
{
    return g_options.enabled;
}

bool ncl_audit_wants_raw(void)
{
    return g_options.enabled && g_options.raw;
}

void ncl_audit_reset_stats(void)
{
    ncl_mutex_lock(audit_lock());
    g_requests = 0;
    g_writes = 0;
    memset(g_tiers, 0, sizeof(g_tiers));
    g_sessions = 0;
    g_reconnects = 0;
    g_code_count = 0;
    memset(g_codes, 0, sizeof(g_codes));
    g_write_next = 0;
    memset(g_write_log, 0, sizeof(g_write_log));
    ncl_mutex_unlock(audit_lock());
}

/** Record one outcome in the counters (caller holds the lock). */
static void note_code(int code)
{
    int tier = ncl_driver_error_tier(code);
    size_t i;

    if (tier > 0 && tier <= 3) {
        g_tiers[tier]++;
    }
    if (code == 0) {
        return;
    }
    for (i = 0; i < g_code_count; i++) {
        if (g_codes[i].code == code) {
            g_codes[i].count++;
            return;
        }
    }
    if (g_code_count < NCL_AUDIT_CODES) {
        g_codes[g_code_count].code = code;
        g_codes[g_code_count].count = 1;
        g_code_count++;
    }
}

/** Hex of the first @p len bytes, for the raw line. Returns the bytes written. */
static size_t hex_into(char *out, size_t out_len, const uint8_t *data,
                       size_t len)
{
    static const char kDigits[] = "0123456789abcdef";
    size_t i;
    size_t used = 0;

    for (i = 0; i < len && used + 2 < out_len; i++) {
        out[used++] = kDigits[(data[i] >> 4) & 0xF];
        out[used++] = kDigits[data[i] & 0xF];
    }
    out[used] = '\0';
    return used;
}

void ncl_audit_request(const char *link, const char *path, const char *address,
                       int code, int64_t micros, const ncl_driver_raw *raw)
{
    if (!g_options.enabled) {
        return;
    }
    ncl_mutex_lock(audit_lock());
    g_requests++;
    note_code(code);
    ncl_mutex_unlock(audit_lock());

    if (code == 0) {
        ncl_log_debug("AUDIT %s %s %s ok %lldus", link != NULL ? link : "-",
                      path != NULL ? path : "-",
                      address != NULL ? address : "-", (long long)micros);
    } else {
        ncl_log_warn("AUDIT %s %s %s %s(%d) %lldus",
                     link != NULL ? link : "-", path != NULL ? path : "-",
                     address != NULL ? address : "-",
                     ncl_driver_error_tier_name(code), code, (long long)micros);
    }
    if (g_options.raw && raw != NULL &&
        (raw->request_len > 0 || raw->reply_len > 0)) {
        char text[NCL_AUDIT_HEX_MAX * 2 + 1];
        const char *truncated;

        if (raw->request_len > 0) {
            truncated = hex_into(text, sizeof(text), raw->request,
                                 raw->request_len) < raw->request_len * 2u
                            ? "..."
                            : "";
            ncl_log_debug("AUDIT raw %s %s %s request %u bytes %s%s",
                          link != NULL ? link : "-", path != NULL ? path : "-",
                          address != NULL ? address : "-",
                          (unsigned)raw->request_len, text, truncated);
        }
        if (raw->reply_len > 0) {
            truncated = hex_into(text, sizeof(text), raw->reply,
                                 raw->reply_len) < raw->reply_len * 2u
                            ? "..."
                            : "";
            ncl_log_debug("AUDIT raw %s %s %s reply %u bytes %s%s",
                          link != NULL ? link : "-", path != NULL ? path : "-",
                          address != NULL ? address : "-",
                          (unsigned)raw->reply_len, text, truncated);
        }
    }
}

/** One JSON scalar as text, for the audit record. */
static void value_text(const ncl_json *value, char *out, size_t out_len)
{
    char *text;

    if (out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (value == NULL) {
        snprintf(out, out_len, "-");
        return;
    }
    text = ncl_json_write_string(value);
    if (text == NULL) {
        snprintf(out, out_len, "?");
        return;
    }
    snprintf(out, out_len, "%s", text);
    ncl_free_safe(text);
}

void ncl_audit_write(const char *link, const char *path, const char *address,
                     const ncl_json *old_value, const ncl_json *new_value,
                     int code)
{
    audit_write *record;
    char from[48];
    char to[48];

    if (!g_options.enabled) {
        return;
    }
    value_text(old_value, from, sizeof(from));
    value_text(new_value, to, sizeof(to));
    ncl_mutex_lock(audit_lock());
    g_writes++;
    /* The tier counters are kept by ncl_audit_request(), which runs once per
     * manager call; counting here as well would double a failed write. */
    record = &g_write_log[g_write_next % NCL_AUDIT_WRITES];
    g_write_next++;
    snprintf(record->path, sizeof(record->path), "%s", path != NULL ? path : "-");
    snprintf(record->address, sizeof(record->address), "%s",
             address != NULL ? address : "-");
    snprintf(record->from, sizeof(record->from), "%s", from);
    snprintf(record->to, sizeof(record->to), "%s", to);
    record->code = code;
    record->index = g_write_next;
    ncl_mutex_unlock(audit_lock());

    /* §6: a write is always logged in full, not only when the debug level is on */
    ncl_log_info("AUDIT write %s %s %s %s -> %s by %s（%s）",
                 link != NULL ? link : "-", path != NULL ? path : "-",
                 address != NULL ? address : "-", from, to,
                 g_options.operator_name != NULL ? g_options.operator_name : "?",
                 code == 0 ? "ok" : ncl_driver_error_tier_name(code));
}

void ncl_audit_session(const char *link, const char *event, const char *detail)
{
    if (!g_options.enabled) {
        return;
    }
    ncl_mutex_lock(audit_lock());
    if (event != NULL && strcmp(event, "reconnect") == 0) {
        g_reconnects++;
    } else {
        g_sessions++;
    }
    ncl_mutex_unlock(audit_lock());
    ncl_log_info("AUDIT session %s %s%s%s", link != NULL ? link : "-",
                 event != NULL ? event : "-", detail != NULL ? " " : "",
                 detail != NULL ? detail : "");
}

ncl_json *ncl_audit_stats(void)
{
    ncl_json *root = ncl_json_new_object();
    ncl_json *codes = ncl_json_new_array();
    ncl_json *writes = ncl_json_new_array();
    size_t i;

    if (root == NULL || codes == NULL || writes == NULL) {
        ncl_json_free(root);
        ncl_json_free(codes);
        ncl_json_free(writes);
        return NULL;
    }
    ncl_mutex_lock(audit_lock());
    (void)ncl_json_obj_set_int(root, "requests", (long long)g_requests);
    (void)ncl_json_obj_set_int(root, "writes", (long long)g_writes);
    (void)ncl_json_obj_set_int(root, "sessions", (long long)g_sessions);
    (void)ncl_json_obj_set_int(root, "reconnects", (long long)g_reconnects);
    (void)ncl_json_obj_set_int(root, "transport", (long long)g_tiers[1]);
    (void)ncl_json_obj_set_int(root, "protocol", (long long)g_tiers[2]);
    (void)ncl_json_obj_set_int(root, "business", (long long)g_tiers[3]);
    for (i = 0; i < g_code_count; i++) {
        ncl_json *entry = ncl_json_new_object();

        if (entry == NULL) {
            continue;
        }
        (void)ncl_json_obj_set_int(entry, "code", g_codes[i].code);
        (void)ncl_json_obj_set_int(entry, "count", (long long)g_codes[i].count);
        (void)ncl_json_obj_set_string(entry, "tier",
                                      ncl_driver_error_tier_name(g_codes[i].code));
        (void)ncl_json_arr_push(codes, entry);
    }
    for (i = 0; i < NCL_AUDIT_WRITES; i++) {
        const audit_write *record = &g_write_log[i];
        ncl_json *entry;

        if (record->index == 0) {
            continue;
        }
        entry = ncl_json_new_object();
        if (entry == NULL) {
            continue;
        }
        (void)ncl_json_obj_set_int(entry, "seq", (long long)record->index);
        (void)ncl_json_obj_set_string(entry, "path", record->path);
        (void)ncl_json_obj_set_string(entry, "address", record->address);
        (void)ncl_json_obj_set_string(entry, "from", record->from);
        (void)ncl_json_obj_set_string(entry, "to", record->to);
        (void)ncl_json_obj_set_int(entry, "code", record->code);
        (void)ncl_json_arr_push(writes, entry);
    }
    ncl_mutex_unlock(audit_lock());
    (void)ncl_json_obj_set(root, "errors", codes);
    (void)ncl_json_obj_set(root, "lastWrites", writes);
    return root;
}
