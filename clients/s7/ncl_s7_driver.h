/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the Siemens S7 driver.
 *
 *   "s7_tcp"   S7comm over ISO-on-TCP (port 102)
 *
 * Parameters:
 *   "host", "port"       target; port defaults to 102
 *   "rack", "slot"       0/2 for S7-300/400, 0/1 for S7-1200/1500 (§7.6)
 *   "pduSize"            the PDU size to ask for, 240..960 (default 960)
 *   "connectTimeoutMs"   TCP connect (default 3000)
 *   "timeoutMs"          per request (default 1000)
 *   "retries"            re-sends after a transport failure (default 1)
 *
 * The point map names an area and a byte offset:
 *   {"area":"M","offset":10,"bit":3}   M10.3        (the shorthand "M10.3")
 *   {"area":"DB1","offset":0,"dtype":"float32"}     DB1.DBD0
 *   {"area":"DB1","offset":20,"dtype":"int16"}      DB1.DBW20
 *   {"area":"MB","offset":10}                       MB10 (B/W/D set the type)
 * A DB number is part of the area name ("DB1", "DB100"), and a string point
 * gets S7's two byte header written for it.
 *
 * One Read Var carries as many items as the negotiated PDU size allows, so a
 * batch is a single request rather than one per point; writes go one address
 * per request because a failed write must be attributable.
 *
 * Remember §7.1: an S7-1200/1500 refuses PUT/GET unless the project allows it,
 * and a DB with "optimized block access" has no absolute addresses.
 */
#ifndef NCL_S7_DRIVER_H
#define NCL_S7_DRIVER_H

#include "nclink/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

ncl_driver *ncl_s7_tcp_create(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_S7_DRIVER_H */
