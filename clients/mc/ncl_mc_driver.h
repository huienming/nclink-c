/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the Mitsubishi MC / SLMP driver.
 *
 *   "mc_tcp"   MC protocol over TCP (5534, or 5007 for SLMP, 5000/5001 FX5U)
 *
 * Parameters:
 *   "host", "port"        target; port defaults to 5534
 *   "frame"               "3e" (default) or "4e" (iQ-R, carries a serial number)
 *   "network", "plc", "station", "module", "timer"   header fields, usually
 *                         left at their defaults (0 / 0xFF / 0 / 0x03FF / 4 s)
 *   "connectTimeoutMs"    TCP connect (default 3000)
 *   "timeoutMs"           per request (default 1000)
 *   "retries"             re-sends after a transport failure (default 1)
 *   "mergeGap"            device points a read may skip to join two spans
 *                         (default 8)
 *
 * The point map names a device and a number: {"area":"D","offset":100},
 * {"area":"M","offset":10,"bit":3} or the shorthand "D100" / "M10.3".
 * A word device with a bit index is addressed as number * 16 + bit, as the
 * protocol wants (§8.2).
 *
 * Batch reads merge neighbours and split at 960 words (the protocol's own
 * limit). Not implemented yet: UDP (the core socket layer is TCP only) and the
 * ASCII encoding (its byte level has no primary sample in the spec book).
 */
#ifndef NCL_MC_DRIVER_H
#define NCL_MC_DRIVER_H

#include "nclink/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

ncl_driver *ncl_mc_tcp_create(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MC_DRIVER_H */
