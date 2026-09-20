/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the Mitsubishi CNC M70/M80 (MELDAS) driver.
 *
 *   "meldas"   M700 series over GIOP, TCP 683
 *
 * Parameters:
 *   "host", "port"        target; port defaults to 683 (machine parameter #1929)
 *   "axisMode"            "bit" (default: X=1, Y=2, Z=4, per §5) or "index"
 *                         (the axes numbered 1..n, as the C++ samples do)
 *   "count"               elements per request, usually 1
 *   "connectTimeoutMs"    TCP connect (default 3000)
 *   "timeoutMs"           per request (default 1000; a CNC is slower than a PLC)
 *   "retries"             re-sends after a transport failure (default 0: the
 *                         session is exclusive, so a lost one needs a fresh
 *                         connection more often than a repeat)
 *
 * A point names the data it wants and the axis it wants it from, so **the area
 * is the command** (a name from §4, or the raw form "0x25/2") and **the offset
 * is the axis or I/O address**:
 *
 *   {"path": "/CNC/X",   "addr": {"area": "machine_position", "offset": 1,
 *                                 "dtype": "float64"}}
 *   {"path": "/CNC/LOAD","addr": {"area": "spindle_load", "offset": 0}}
 *   {"path": "/CNC/PART","addr": {"area": "part_count", "offset": 0}}
 *
 * Reads only: the delivered material documents the read frames byte for byte,
 * while mochaSetData (§4.2) has no captured layout, and §8.7 asks for writes to
 * be off by default. A write therefore reports NCL_ERR_NOT_SUPPORTED until a
 * capture says how the frame looks.
 *
 * The raw escape hatch takes the five little endian 32 bit fields a request
 * carries - command, subcode, count, address, expected type - so an undocumented
 * command can be tried against a real machine and its answer read back.
 */
#ifndef NCL_MELDAS_DRIVER_H
#define NCL_MELDAS_DRIVER_H

#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

ncl_driver *ncl_meldas_create(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MELDAS_DRIVER_H */
