/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the HEIDENHAIN LSV2 driver.
 *
 *   "lsv2"   iTNC 530 / TNC 7 over TCP 19000
 *
 * Parameters:
 *   "host", "port"        target; port defaults to 19000
 *   "user"                login name (INSPECT / FILE / MONITOR / DIAGNOSTICS /
 *                         PLCDEBUG, §4.1); empty means "do not log in"
 *   "password"            optional
 *   "count"               bytes one R_MB read asks for (default 1)
 *   "connectTimeoutMs"    TCP connect (default 3000)
 *   "timeoutMs"           per request (default 3000; a control is slower than a
 *                         PLC, and R_VR can take a moment)
 *   "retries"             re-sends after a transport failure (default 0)
 *
 * A point names the command and its selector, so **the area is what to read**
 * and the offset is the address or index it applies to:
 *
 *   {"path": "/TNC/VER",  "addr": {"area": "version",       "offset": 0}}
 *   {"path": "/TNC/ST",   "addr": {"area": "remote_status", "offset": 0}}
 *   {"path": "/TNC/M100", "addr": {"area": "plc_memory",    "offset": 100,
 *                                  "dtype": "int16"}}
 *
 * Reads only, and deliberately: §7.6 lists C_EK (simulated key presses) and
 * C_MC (machine parameters) as things to keep behind a permission wall, and the
 * captured material does not show their payloads. A write reports
 * NCL_ERR_NOT_SUPPORTED.
 *
 * What §7 leaves open, and this driver therefore does not pretend to know: the
 * 16 bit selector of R_RI (the polling main command) and the layout of its S_RI
 * answer. Those need a capture; everything that is documented - the framing, the
 * login, the version, the remote status, the PLC memory read and raw access - is
 * implemented.
 */
#ifndef NCL_LSV2_DRIVER_H
#define NCL_LSV2_DRIVER_H

#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

ncl_driver *ncl_lsv2_create(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_LSV2_DRIVER_H */
