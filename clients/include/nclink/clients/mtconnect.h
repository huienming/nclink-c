/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - MTConnect, the parts a driver needs (protocal/docs/
 * 16-MTCONNECT.md).
 *
 * MTConnect is a read-only standard interface: an Agent answers HTTP GET with
 * XML documents. Two small pieces make it usable here, and both are pure
 * enough to test without a machine:
 *
 *   ncl_mtconnect_get()      - a minimal HTTP/1.1 GET (Content-Length, chunked
 *                              and read-until-close bodies, optional Basic auth)
 *   ncl_mtconnect_parse_*    - the XML scan of /probe and /current
 *
 * A data item is addressed by its @c dataItemId, which is a string. This
 * library's unified address carries its area as a string, so for MTConnect
 * **the area name is the data item id** and the offset is always 0:
 *
 *   {"path": "/PLC1/XABS", "addr": {"area": "Xabs", "offset": 0}}
 *   {"path": "/PLC1/EXEC", "addr": "exec"}      # a letters-only id
 */
#ifndef NCL_MTCONNECT_H
#define NCL_MTCONNECT_H

#include <stdbool.h>
#include <stddef.h>

#include "nclink/ncl_json.h"
#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The HTTP GET the driver uses lives in drivers/http/ncl_http_client.h: it is
 * shared with the KND driver, which talks JSON instead of XML. */

/* ============================================================== XML scan == */

/** A slice of a document: borrowed, not NUL terminated. */
typedef struct {
    const char *text;
    size_t      len;
} ncl_mt_slice;

/**
 * Find the next element named @p name at or after @p from.
 * On success @p tag receives the tag itself (between the angle brackets),
 * @p content the text between the tags (empty for a self closing element) and
 * *next the offset just past the element.
 */
bool ncl_mt_next_element(const char *text, size_t len, const char *name,
                         const ncl_mt_slice *from, ncl_mt_slice *tag,
                         ncl_mt_slice *content, size_t *next);

/** Value of attribute @p name inside @p tag, decoded; false when absent. */
bool ncl_mt_attr(const ncl_mt_slice *tag, const char *name, char *out,
                 size_t out_len);

/** Text of @p content with the entities decoded and the blanks trimmed. */
char *ncl_mt_text(const ncl_mt_slice *content);

/* =========================================================== the documents */

/** What /probe knows about one data item. */
typedef struct {
    char *id;
    char *type;    /**< POSITION, EXECUTION, ALARM, ... */
    char *sub_type;
    char *category; /**< SAMPLE, EVENT, CONDITION        */
    char *units;
    char *name;
} ncl_mt_data_item;

typedef struct {
    ncl_mt_data_item *items;
    size_t            count;
    size_t            capacity;
    char             *device_name; /**< from <Device name="..."> */
    char             *device_uuid;
    char             *model; /**< from <Description>       */
} ncl_mt_probe;

void ncl_mt_probe_init(ncl_mt_probe *probe);
void ncl_mt_probe_free(ncl_mt_probe *probe);
/** Parse a /probe document. */
ncl_err ncl_mt_probe_parse(const char *xml, size_t len, ncl_mt_probe *out,
                           char *err, size_t err_len);
/** The data item of @p id, or NULL. */
const ncl_mt_data_item *ncl_mt_probe_find(const ncl_mt_probe *probe,
                                          const char *id);
/** The probe as JSON, for the "probe" method. */
ncl_json *ncl_mt_probe_to_json(const ncl_mt_probe *probe);

/** One reading of /current. */
typedef struct {
    char *item_id;
    char *type;      /**< the element name, e.g. "Position"      */
    char *category;  /**< SAMPLE / EVENT / CONDITION             */
    char *value;     /**< the text, decoded                      */
    char *timestamp; /**< UTC, as the agent wrote it             */
    long long sequence;
    char *native_code; /**< CONDITION only                       */
    char *severity;    /**< CONDITION only                       */
} ncl_mt_reading;

typedef struct {
    ncl_mt_reading *readings;
    size_t          count;
    size_t          capacity;
    long long       first_sequence;
    long long       last_sequence;
} ncl_mt_current;

void ncl_mt_current_init(ncl_mt_current *current);
void ncl_mt_current_free(ncl_mt_current *current);
/**
 * Parse a /current document. @p probe may be NULL; when it is given, the
 * category of each reading is taken from it, which is the only way to tell a
 * CONDITION from an EVENT without hard coding element names (§6.4).
 */
ncl_err ncl_mt_current_parse(const char *xml, size_t len,
                             const ncl_mt_probe *probe, ncl_mt_current *out,
                             char *err, size_t err_len);
/** The reading of @p id, or NULL. */
const ncl_mt_reading *ncl_mt_current_find(const ncl_mt_current *current,
                                          const char *id);

/** "UNAVAILABLE" and friends: the agent has no value for that item. */
bool ncl_mt_value_is_unavailable(const char *value);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MTCONNECT_H */
