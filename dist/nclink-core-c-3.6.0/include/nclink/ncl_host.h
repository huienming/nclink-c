/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link tool layer - the host: a tool declaration plus a configuration file
 * in, a live NC-Link device out.
 *
 * This is what a device program uses: ncl_server (src/tool/main.c) is one, and
 * a site that compiles its adapter in rather than loading it writes its own
 * main() that calls ncl_host_run() the same way.
 *
 * The points are not in the configuration: a loaded adapter module declares
 * them (see ncl_tool.h), and the host turns that declaration into the model, the
 * sample channel and one operation per point on the device's server:
 *
 *   Query  get_value#<point path>  -> read the point from the device
 *   Set    set_value#<point path>  -> write it (only when the point is writable)
 *   Method call#<method path>      -> the tool's own operation
 *
 * Sampling needs no extra machinery: the core's sample tasks send ordinary
 * queries, so a channel over a point goes through the same binding.
 *
 * Configuration:
 *   {
 *     "sn": "V2A1B2C3D4E",
 *     "model": "conf/model.json",     // optional; generated from the points
 *     "device": { "type": "MACHINE", "id": "01", "name": "数控机床" },
 *     "sample": { "intervalMs": 1000, "uploadMs": 1000 },
 *     "mqtt": {                        // optional: without it the host is offline
 *       "url": "tcp://127.0.0.1:1883",
 *       "username": "", "password": "",
 *       "clientId": "V2A1B2C3D4E",     // defaults to the serial number
 *       "keepAliveSeconds": 60,
 *       "automaticReconnect": true,
 *       "reconnectDelayMs": 1000,
 *       "offline": false               // true = never touch the broker
 *     }
 *   }
 *
 * With an "mqtt" object the host owns the session: it connects, subscribes
 * this serial number's request topics, answers on them and publishes the
 * sample channels through the same client. A broker that is not up yet is not
 * fatal - the host loop calls ncl_host_broker_poll() to retry with a
 * backoff, so a collector comes up on a machine whose broker boots later.
 *
 * Generating the model keeps the device usable through REST alone: every point
 * becomes a data item whose path is exactly the point path, so a site can look
 * at (and talk to) a device before a broker is anywhere in sight.
 */
#ifndef NCL_HOST_H
#define NCL_HOST_H

#include "nclink/ncl_server.h"
#include "nclink/ncl_module.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ncl_host ncl_host;

/** Build an host from a parsed configuration document. */
ncl_host *ncl_host_create(const ncl_json *config, ncl_strbuf *err);

/**
 * Build an host from a configuration and the modules the host loaded.
 *
 * A module that declares its points (see ncl_tool.h) becomes the device: the
 * model and the bindings come from its declaration, and the configuration only
 * carries its "parameters" (a "tools" entry), the device block, the sampling
 * and the broker. There is no point map in the configuration any more.
 *
 * One device serves one declared tool: two of them is a configuration error,
 * because they would fight over the same device node and sample channel.
 *
 * The module set is borrowed and has to outlive the host (the declaration
 * lives inside the loaded module).
 */
ncl_host *ncl_host_create_with_modules(const ncl_json *config,
                                             const ncl_module_set *modules,
                                             ncl_strbuf *err);
/** Build an host from a configuration file. */
ncl_host *ncl_host_create_from_file(const char *path, ncl_strbuf *err);
void         ncl_host_free(ncl_host *host);

/**
 * Put an "mqtt" object into a configuration before it is handed to
 * ncl_host_create(). The order is: @p broker wins ("-" means offline),
 * then the configuration's own "mqtt" object (it may name its own broker),
 * then <conf>/mqtt.cfg (url, username, password). @p offline always means
 * offline. A missing or empty url leaves the host offline, so a box without
 * a broker still collects and serves REST.
 */
ncl_err ncl_host_config_set_broker(ncl_json *config, const char *broker,
                                      bool offline, ncl_strbuf *err);

/** Borrowed server, for the host to subscribe/serve REST/attach its own tools. */
ncl_server *ncl_host_server(ncl_host *host);

/**
 * The declaration the device is built from, or NULL when no loaded module
 * declared one - ncl_host_create() refuses that configuration, so a host that
 * exists always has a tool here. Read through it for the point paths, the
 * periods and the module that serves them.
 */
const ncl_tool_decl *ncl_host_tool(const ncl_host *host);

/**
 * The model document this device publishes: either the one built from the
 * declaration (one data item per declared point, one sample channel over the
 * sampled ones) or the file the configuration named. Borrowed, owned by the
 * host - a host that wants to show it or hand it to a site writes it out with
 * ncl_json_write_pretty().
 */
const ncl_json *ncl_host_model(const ncl_host *host);

/** Serial number the host answers for. */
const char *ncl_host_sn(const ncl_host *host);

size_t ncl_host_point_count(const ncl_host *host);
/** Path of point @p index, or NULL. */
const char *ncl_host_point_path(const ncl_host *host, size_t index);
/**
 * Value the model currently holds for point @p index, or NULL. It is refreshed
 * by ncl_host_poll_one() / ncl_host_poll*(); a host that wants the value
 * without going through the server (a status page, a self check) reads it here.
 */
const ncl_json *ncl_host_point_value(const ncl_host *host,
                                        size_t index);

/**
 * True for a point that this build cannot read yet: it is in the model and it
 * answers a clear "还读不了", because its client function has no protocol call
 * for it (NCL_ERR_UNAVAILABLE - the frame has not been captured).
 *
 * The state is **learned from the point's own answer**, not declared: it is
 * false until the point has been read once, and it cannot change while the
 * process runs ("this build has no call for it" is not something a machine
 * does and undoes). The polling rounds leave such a point alone from then on -
 * it cannot answer, and a round is not where that should be learned once per
 * second - and ncl_host_poll_one() on it answers NCL_ERR_UNAVAILABLE without
 * asking the machine.
 */
bool ncl_host_point_unavailable(const ncl_host *host, size_t index);

/* Broker ---------------------------------------------------------------- */

/** Broker URL the configuration named, or NULL when the host is offline. */
const char *ncl_host_broker_url(const ncl_host *host);

/** True while the MQTT session to the broker is up. */
bool ncl_host_online(const ncl_host *host);

/**
 * Keep the broker session up: retry a connection that has not come up yet
 * (with the configured backoff) and subscribe once it is up. The host has
 * no thread of its own, so the host's loop calls this - once per round is
 * enough.
 *
 * Returns NCL_OK when there is nothing to do (offline, or the session is up);
 * NCL_ERR_CLOSED while the broker is configured but not reachable yet;
 * NCL_ERR_INVALID_ARG only for a NULL host.
 */
ncl_err ncl_host_broker_poll(ncl_host *host, ncl_strbuf *err);

/**
 * Read every point once and refresh the model's values. Sampling does not need
 * this (it goes through the bindings), but a host that reads the model - REST,
 * a status page - wants the values to be current.
 *
 * A point that cannot be read leaves its previous value in place and is
 * reported through @p err; the first failure is the return value. A point that
 * turns out to be unreadable in this build (NCL_ERR_UNAVAILABLE, see
 * ncl_host_point_unavailable()) is not a failure: it is left alone from here on
 * and only ncl_host_poll_one() reports it.
 */
ncl_err ncl_host_poll(ncl_host *host, ncl_strbuf *err);

/**
 * Read one point and refresh its model value.
 *
 * A host that must stay responsive when the machine is down walks the points
 * itself and stops the round on the first transport error (tier 1 of
 * ncl_driver_error_tier()), instead of paying one connect timeout per point.
 */
ncl_err ncl_host_poll_one(ncl_host *host, const char *path,
                             ncl_strbuf *err);

/**
 * One polling round with that policy built in: read the points in order,
 * refresh the model, and stop at the first transport error (a dead machine
 * must cost one timeout per round, not one per point). *failed, when not NULL,
 * receives how many points did not answer - the points after the break
 * included. Returns the first error, or NCL_OK when every point answered.
 */
ncl_err ncl_host_poll_round(ncl_host *host, size_t *failed,
                               ncl_strbuf *err);

#ifdef __cplusplus
}
#endif

#endif /* NCL_HOST_H */
