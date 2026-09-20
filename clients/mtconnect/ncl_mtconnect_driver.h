/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the MTConnect driver.
 *
 *   "mtconnect"   an MTConnect agent over HTTP (7878 by default)
 *
 * Parameters:
 *   "host", "port"        the agent; port defaults to 7878
 *   "basePath"            a prefix for agents behind a reverse proxy ("")
 *   "user", "password"    HTTP Basic authentication, when the agent wants it
 *   "timeoutMs"           per HTTP request (default 3000)
 *
 * Read only, as the standard is (§1): writes report NCL_ERR_NOT_SUPPORTED.
 *
 * A point names a data item by its id, which is a string, so **the area name is
 * the dataItemId** and the offset is always 0:
 *
 *   {"path": "/PLC1/XABS", "addr": {"area": "Xabs", "offset": 0}}
 *   {"path": "/PLC1/EXEC", "addr": "exec"}          # a letters-only id
 *
 * The driver reads /probe once per session (the data item table, so a category
 * is known rather than guessed) and /current on every read; an item the agent
 * reports as UNAVAILABLE becomes JSON null, and a CONDITION that turns into
 * Fault or Warning is pushed as an event.
 *
 * Not implemented yet: the streaming /sample endpoint (§5 recommends it for
 * high rates) and the /assets documents.
 */
#ifndef NCL_MTCONNECT_DRIVER_H
#define NCL_MTCONNECT_DRIVER_H

#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

ncl_driver *ncl_mtconnect_create(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MTCONNECT_DRIVER_H */
