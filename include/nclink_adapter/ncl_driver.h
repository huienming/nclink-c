/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the vendor protocol driver interface.
 *
 * Every driver speaks one machine/PLC protocol (Modbus, MC, FINS, S7, LSV2,
 * FOCAS, ...) and exposes it through this interface, which is the C shape of
 * the abstract session in protocal/docs/00-通用-实现约定.md:
 *
 *   - one unified address model (area / offset / bit / length / dtype),
 *   - batch read and batch write,
 *   - a raw escape hatch (read_raw / write_raw) for protocol specific work,
 *   - call() for method-like operations (start program, MDI, tool offset, ...),
 *   - an event hook (alarms, program end, ...).
 *
 * Errors are tiered exactly as the spec asks: transport (0x1...), protocol
 * (0x2...) and business (0x3...) are kept apart so the caller can decide
 * whether to reconnect, map a protocol code or just report bad data.
 */
#ifndef NCL_DRIVER_H
#define NCL_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_common.h"
#include "nclink/ncl_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================ data types == */

/** The seven unified types of the spec (no protocol specific types leak out). */
typedef enum {
    NCL_DTYPE_BIT = 0,
    NCL_DTYPE_BYTE,
    NCL_DTYPE_INT16,
    NCL_DTYPE_INT32,
    NCL_DTYPE_FLOAT32,
    NCL_DTYPE_FLOAT64,
    NCL_DTYPE_STRING
} ncl_dtype;

/** "bit" / "byte" / "int16" / "int32" / "float32" / "float64" / "string". */
const char *ncl_dtype_name(ncl_dtype dtype);
bool ncl_dtype_parse(const char *text, ncl_dtype *out);

/** One unified address: <area> + offset [+ bit], @p length elements of @p dtype. */
typedef struct {
    /**
     * M/D/X/Y/DB/CIO/... , compared case-insensitively by the drivers; NULL
     * when the protocol has no areas. ncl_address_from_json() owns the string
     * it puts here, so release the address with ncl_address_clear().
     */
    const char *area;
    int64_t     offset; /**< word offset or item number                     */
    int         bit;    /**< bit index, -1 = the whole word                 */
    int         length; /**< number of elements, >= 1                       */
    ncl_dtype   dtype;
} ncl_address;

/* ================================================================ errors == */

/**
 * Tier bits (00 通用约定 §2): the low 28 bits are the driver's own code.
 * 0 is success everywhere; anything else is one of these tiers.
 */
#define NCL_DRV_ERR_TRANSPORT(code) (0x10000000 | ((int)(code) & 0x0FFFFFFF))
#define NCL_DRV_ERR_PROTOCOL(code)  (0x20000000 | ((int)(code) & 0x0FFFFFFF))
#define NCL_DRV_ERR_BUSINESS(code)  (0x30000000 | ((int)(code) & 0x0FFFFFFF))

/**
 * Tier of @p code: 0 success, 1 transport, 2 protocol, 3 business. A negative
 * code is a plain library error (NCL_ERR_*), not a driver tier: those report
 * -1 so a caller can tell "the driver says the link is down" from "the caller
 * passed a bad argument".
 */
int ncl_driver_error_tier(int code);
/** Human readable tier name, for logs. */
const char *ncl_driver_error_tier_name(int code);

/* =============================================================== results == */

/** Unified response envelope (spec §1): code + value + message + raw reply. */
typedef struct {
    int       code;      /**< 0 = success, otherwise a tiered code           */
    bool      success;   /**< convenience: code == 0                         */
    ncl_json *value;     /**< owned; scalar / array / object                 */
    char      message[160];
    uint8_t  *raw;       /**< owned raw reply, for the audit log             */
    size_t    raw_len;
} ncl_driver_result;

void ncl_driver_result_init(ncl_driver_result *result);
/** Releases value/raw and zeroes the struct. */
void ncl_driver_result_free(ncl_driver_result *result);
/**
 * Take ownership of @p value and mark the result successful. The raw reply is
 * left alone, so set_raw() may be called before or after this.
 */
void ncl_driver_result_ok(ncl_driver_result *result, ncl_json *value);
/**
 * Set a tiered failure code with a printf style message. Releases the value;
 * keeps the raw reply, so a failed exchange still reaches the audit log.
 */
void ncl_driver_result_fail(ncl_driver_result *result, int code,
                            const char *fmt, ...);
/** Copy @p len bytes of @p data into result->raw, for the audit log. */
void ncl_driver_result_set_raw(ncl_driver_result *result, const void *data,
                               size_t len);

/**
 * The two frames of one exchange, borrowed from the driver: the request that
 * went out and the reply that came back (either may be NULL). See
 * ncl_driver_ops::last_raw.
 */
typedef struct {
    const uint8_t *request;
    size_t         request_len;
    const uint8_t *reply;
    size_t         reply_len;
} ncl_driver_raw;

/* ============================================================= addresses == */

/** Release the strings @p address owns. Safe on a zeroed struct; idempotent. */
void ncl_address_clear(ncl_address *address);

/** Human readable address ("D100.3 x4 int16"), for logs and audit lines. */
char *ncl_address_to_text(const ncl_address *address);

/* ================================================================ driver == */

typedef struct ncl_driver ncl_driver;

/** Event hook: @p event is borrowed (alarms, program end, tool life, ...). */
typedef void (*ncl_driver_event_fn)(void *user, const char *event_id,
                                    const ncl_json *event);

/**
 * What a protocol driver implements. All callbacks are optional except
 * read_batch; a missing one answers NCL_ERR_NOT_SUPPORTED.
 */
typedef struct ncl_driver_ops {
    /** Protocol name used by the configuration ("mock", "modbus_tcp", ...). */
    const char *protocol;
    /** Configure from the "parameters" object of the driver config. */
    ncl_err (*create)(ncl_driver *self, const ncl_json *parameters);
    /** Establish the session (connect + handshake). Called on demand. */
    ncl_err (*open)(ncl_driver *self);
    /** Drop the session. Idempotent. */
    void (*close)(ncl_driver *self);
    bool (*is_connected)(const ncl_driver *self);
    /**
     * Read @p count addresses. *values receives an array of @p count entries,
     * each the scalar of that address or, when the address asks for
     * length > 1, an array of that many scalars. The driver owns *values.
     */
    ncl_err (*read_batch)(ncl_driver *self, const ncl_address *addresses,
                          size_t count, ncl_json **values);
    /**
     * Write @p count values to the addresses. @p values holds @p count
     * entries, each the scalar for that address or, for an address with
     * length > 1, an array of that many scalars.
     */
    ncl_err (*write_batch)(ncl_driver *self, const ncl_address *addresses,
                           const ncl_json *values, size_t count);
    /** Escape hatch: send a raw frame and take the raw reply (both owned). */
    ncl_err (*read_raw)(ncl_driver *self, const void *frame, size_t frame_len,
                        ncl_driver_result *out);
    ncl_err (*write_raw)(ncl_driver *self, const void *frame, size_t frame_len,
                         ncl_driver_result *out);
    /** Method-like operation ("call" / "status" / "result" / "cancel"). */
    ncl_err (*call)(ncl_driver *self, const char *operation,
                    const ncl_json *params, ncl_json **result);
    /** Register the event hook (at most one; NULL clears it). */
    void (*attach_event)(ncl_driver *self, ncl_driver_event_fn fn, void *user);
    /** Release the private state; the driver struct itself is freed here. */
    void (*destroy)(ncl_driver *self);
    /**
     * The bytes of the last exchange, for the audit trail (§6 of the spec asks
     * for the raw frame of every request). Optional: a driver whose protocol
     * has no frame of its own - the mock, or MTConnect's XML documents - leaves
     * it out and the caller logs no bytes. It comes last so a driver that does
     * not have one keeps its initialiser list untouched.
     *
     * The driver keeps the buffers until its next exchange, so a caller copies
     * what it wants to keep; either frame may be NULL.
     */
    void (*last_raw)(const ncl_driver *self, ncl_driver_raw *out);
} ncl_driver_ops;

struct ncl_driver {
    const ncl_driver_ops *ops; /**< never NULL once created */
    void                 *ctx; /**< driver private state    */
};

/**
 * Allocate a driver shell around @p ops and @p ctx (both borrowed by the
 * driver; the destroy callback releases them). Returns NULL on OOM. Drivers
 * use this in their factory so the shape of ncl_driver stays private.
 */
ncl_driver *ncl_driver_new(const ncl_driver_ops *ops, void *ctx);

/** The operations of @p driver, or NULL. */
const ncl_driver_ops *ncl_driver_ops_of(const ncl_driver *driver);
/** Protocol name of @p driver ("?" when unknown). */
const char *ncl_driver_protocol(const ncl_driver *driver);

/**
 * The bytes of the last exchange of @p driver, zeroed when there is nothing to
 * report (no callback, or no exchange yet). Safe to call on any driver.
 */
void ncl_driver_last_raw(const ncl_driver *driver, ncl_driver_raw *out);

/**
 * Read one address as a JSON scalar, opening the session on demand. Helper for
 * drivers and for the adapter's point map.
 */
ncl_err ncl_driver_read_one(ncl_driver *driver, const ncl_address *address,
                            ncl_json **value);
/** Write one JSON scalar. */
ncl_err ncl_driver_write_one(ncl_driver *driver, const ncl_address *address,
                             const ncl_json *value);

/** Parse {"area":..,"offset":..,"bit":..,"length":..,"dtype":".."}. */
ncl_err ncl_address_from_json(const ncl_json *object, ncl_address *out);
/**
 * Read the dtype named by @p key into @p out.
 * Returns NCL_OK, NCL_ERR_NOT_FOUND when the key is absent (the caller keeps
 * its own default), or NCL_ERR_INVALID_DATA_TYPE when it is not a type name.
 */
ncl_err ncl_dtype_from_object(const ncl_json *object, const char *key,
                              ncl_dtype *out);

/* ============================================================= factories == */

/** A protocol driver factory (registered per protocol name). */
typedef ncl_driver *(*ncl_driver_factory)(void);

/**
 * Register @p factory under @p protocol (the built in set is registered by
 * ncl_driver_register_builtin()). Returns NCL_ERR_EXISTS when taken.
 */
ncl_err ncl_driver_register_protocol(const char *protocol,
                                     ncl_driver_factory factory);
/** Register the drivers built into this library (mock today, more in P1). */
void ncl_driver_register_builtin(void);
/**
 * Create a driver for @p protocol (NULL when unknown or out of memory). The
 * driver comes up with its defaults; hand it its "parameters" object with
 * ops->create() before the first read.
 */
ncl_driver *ncl_driver_create(const char *protocol);
/** Number of registered protocols. */
size_t ncl_driver_protocol_count(void);
/**
 * True when @p protocol is registered (a built-in or one a module handed
 * over). A host checks this before building a device so that "the module is
 * not in the plugin directory" is said out loud instead of surfacing as
 * "unknown protocol".
 */
bool ncl_driver_protocol_known(const char *protocol);

#ifdef __cplusplus
}
#endif

#endif /* NCL_DRIVER_H */
