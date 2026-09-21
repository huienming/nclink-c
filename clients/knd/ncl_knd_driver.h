/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the KND driver.
 *
 *   "knd"   a 凯恩帝 controller's own HTTP/JSON interface (port 80 by default)
 *
 * Parameters:
 *   "host"                the controller (required)
 *   "port"                default 80
 *   "timeoutMs"           per request, default 3000
 *   "user", "password"    HTTP Basic authentication, when the controller wants
 *                         it (some firmwares do)
 *
 * A point names a **model item** (see ncl_knd.h), and the item table says which
 * endpoint holds it and how to scale it:
 *
 *   {"path": "/CNC/STATE",  "addr": "STATUS"}
 *   {"path": "/CNC/COUNT",  "addr": "/PART_COUNT"}
 *   {"path": "/CNC/X",      "addr": "/AXIS@0/SCREW/POSITION"}
 *
 * The offset is not used - every item is a single value - so the object form
 * with a literal area is the reliable way to write one.
 *
 * Read only: the delivered mapping layer registers `get_value` alone, so this
 * driver answers NCL_ERR_NOT_SUPPORTED for writes rather than inventing an
 * endpoint (解 09 册 §3 的表里没有写端点).
 */
#ifndef NCL_KND_DRIVER_H
#define NCL_KND_DRIVER_H

#include "nclink/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

ncl_driver *ncl_knd_create(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_KND_DRIVER_H */
