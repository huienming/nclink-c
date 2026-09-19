/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the daemon: a configuration file in, a live NC-Link device
 * out.
 *
 * The adapter reads a driver configuration, brings up one NC-Link server per
 * device and turns every configured point into an operation of that device:
 *
 *   Query  get_value#<point path>  -> read the point from the device
 *   Set    set_value#<point path>  -> write it (only when the point is writable)
 *   Method call#<method path>      -> the driver's own operation
 *
 * Sampling needs no extra machinery: the core's sample tasks send ordinary
 * queries, so a channel over a point goes through the same binding.
 *
 * Configuration:
 *   {
 *     "sn": "V2A1B2C3D4E",
 *     "driverDir": "conf/driver",     // or "driverFile", or inline "drivers"
 *     "model": "conf/model.json",     // optional; generated from the points
 *     "device": { "type": "MACHINE", "id": "01", "name": "数控机床" },
 *     "sample": { "intervalMs": 1000, "uploadMs": 1000 }
 *   }
 *
 * Generating the model keeps the adapter usable with nothing but a point map:
 * every point becomes a data item whose path is exactly the point path, so the
 * configuration and the model cannot drift apart.
 */
#ifndef NCL_ADAPTER_H
#define NCL_ADAPTER_H

#include "nclink/ncl_server.h"
#include "nclink_adapter/ncl_driver_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ncl_adapter ncl_adapter;

/** Build an adapter from a parsed configuration document. */
ncl_adapter *ncl_adapter_create(const ncl_json *config, ncl_strbuf *err);
/** Build an adapter from a configuration file. */
ncl_adapter *ncl_adapter_create_from_file(const char *path, ncl_strbuf *err);
void         ncl_adapter_free(ncl_adapter *adapter);

/** Borrowed server, for the host to subscribe/serve REST/attach its own tools. */
ncl_server *ncl_adapter_server(ncl_adapter *adapter);
ncl_driver_manager *ncl_adapter_drivers(ncl_adapter *adapter);

/** Serial number the adapter answers for. */
const char *ncl_adapter_sn(const ncl_adapter *adapter);

size_t ncl_adapter_point_count(const ncl_adapter *adapter);
/** Path of point @p index, or NULL. */
const char *ncl_adapter_point_path(const ncl_adapter *adapter, size_t index);
size_t ncl_adapter_method_count(const ncl_adapter *adapter);

/**
 * Read every point once and refresh the model's values. Sampling does not need
 * this (it goes through the bindings), but a host that reads the model - REST,
 * a status page - wants the values to be current.
 *
 * A point that cannot be read leaves its previous value in place and is
 * reported through @p err; the first failure is the return value.
 */
ncl_err ncl_adapter_poll(ncl_adapter *adapter, ncl_strbuf *err);

#ifdef __cplusplus
}
#endif

#endif /* NCL_ADAPTER_H */
