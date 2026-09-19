/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - driver configuration and path dispatch.
 *
 * The manager is the bridge between a configuration file and the driver
 * interface: one entry per device link, each with a protocol, the "parameters"
 * object of that protocol and a point map from NC-Link model paths to unified
 * addresses.
 *
 *   {
 *     "id":   "plc1",
 *     "path": "/PLC1",              // the model sub-tree this link answers for
 *     "type": "modbus_tcp",         // protocol name in the driver registry
 *     "parameters": { "host": "10.0.0.5", "port": 502, "unit": 1 },
 *     "points": [
 *       { "path": "/PLC1/STATUS", "addr": "D100" },
 *       { "id": "POWER", "addr": {"area":"D","offset":101}, "dtype": "float32" }
 *     ]
 *   }
 *
 * Dispatch follows the reference adapter: the driver whose @c path is the
 * longest prefix of the requested path wins, and a driver registered on "/"
 * answers everything no other driver claims. A point's own path may be absolute
 * ("/PLC1/STATUS") or relative to the driver ("/STATUS" / "STATUS"); both are
 * stored in the same relative form, so the lookup is one string compare.
 */
#ifndef NCL_DRIVER_MANAGER_H
#define NCL_DRIVER_MANAGER_H

#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ncl_driver_manager ncl_driver_manager;

/** Create an empty manager with the built-in protocols registered. */
ncl_driver_manager *ncl_driver_manager_create(void);
void ncl_driver_manager_free(ncl_driver_manager *manager);

/**
 * Add every driver described by one JSON document. The document is a single
 * driver object, or {"drivers":[...]}, or a bare array. On failure @p err
 * receives a diagnostic message when it is not NULL.
 */
ncl_err ncl_driver_manager_add_json(ncl_driver_manager *manager,
                                    const ncl_json *document, ncl_strbuf *err);

/** Load one configuration file. */
ncl_err ncl_driver_manager_load_file(ncl_driver_manager *manager,
                                     const char *path, ncl_strbuf *err);

/**
 * Load every "*.json" in @p directory, in name order. A missing directory is
 * not an error (an adapter with no drivers configured is a valid state), it is
 * reported through @p err and answers NCL_ERR_NOT_FOUND.
 */
ncl_err ncl_driver_manager_load_dir(ncl_driver_manager *manager,
                                    const char *directory, ncl_strbuf *err);

/* ------------------------------------------------------------- contents -- */

size_t ncl_driver_manager_count(const ncl_driver_manager *manager);
const char *ncl_driver_manager_id_at(const ncl_driver_manager *manager, size_t index);
const char *ncl_driver_manager_path_at(const ncl_driver_manager *manager, size_t index);
const char *ncl_driver_manager_protocol_at(const ncl_driver_manager *manager,
                                           size_t index);
/** Borrowed driver of entry @p index, or NULL. */
ncl_driver *ncl_driver_manager_driver_at(ncl_driver_manager *manager, size_t index);

/** Entry answering for @p path: longest matching prefix, then "/". NULL when
 *  nothing matches. */
ncl_driver *ncl_driver_manager_driver_of(ncl_driver_manager *manager,
                                         const char *path);

/** Address of the point behind @p path (borrowed), or NULL. */
const ncl_address *ncl_driver_manager_address_of(ncl_driver_manager *manager,
                                                 const char *path);

/* --------------------------------------------------------------- points -- */

/*
 * The point map of entry @p index, for the daemon that has to turn every point
 * into an operation of the NC-Link device. A point is writable only when the
 * configuration says so (00-通用-实现约定 §7: read-only unless opened up), and
 * sampled unless it opts out.
 */

size_t ncl_driver_manager_point_count(const ncl_driver_manager *manager,
                                      size_t index);
/** Absolute model path of point @p point of entry @p index, or NULL. */
const char *ncl_driver_manager_point_path(const ncl_driver_manager *manager,
                                          size_t index, size_t point);
/** Address of that point (borrowed), or NULL. */
const ncl_address *ncl_driver_manager_point_address(
    const ncl_driver_manager *manager, size_t index, size_t point);
bool ncl_driver_manager_point_writable(const ncl_driver_manager *manager,
                                       size_t index, size_t point);
bool ncl_driver_manager_point_sampled(const ncl_driver_manager *manager,
                                      size_t index, size_t point);

/* -------------------------------------------------------------- traffic -- */

/** Read the point behind @p path. *value is owned by the caller. */
ncl_err ncl_driver_manager_read(ncl_driver_manager *manager, const char *path,
                                ncl_json **value);
/** Write the point behind @p path. */
ncl_err ncl_driver_manager_write(ncl_driver_manager *manager, const char *path,
                                 const ncl_json *value);
/**
 * Run a method-like operation on the driver that owns @p path. @p operation is
 * the driver's own name for it ("startProgram", "toolOffset", "echo", ...).
 */
ncl_err ncl_driver_manager_call(ncl_driver_manager *manager, const char *path,
                                const char *operation, const ncl_json *params,
                                ncl_json **result);

/** Attach one event hook to every driver (alarms, program end, ...). */
void ncl_driver_manager_attach_event(ncl_driver_manager *manager,
                                     ncl_driver_event_fn fn, void *user);

/** Open every session (the daemon calls this once at start-up). */
ncl_err ncl_driver_manager_open_all(ncl_driver_manager *manager, ncl_strbuf *err);
/** Close every session; idempotent. */
void ncl_driver_manager_close_all(ncl_driver_manager *manager);

#ifdef __cplusplus
}
#endif

#endif /* NCL_DRIVER_MANAGER_H */
