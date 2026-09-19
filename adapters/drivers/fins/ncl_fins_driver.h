/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the Omron FINS driver.
 *
 *   "fins_tcp"   FINS over TCP (9600 by default, CJ2/NJ built-in Ethernet)
 *
 * Parameters:
 *   "host", "port"       target; port defaults to 9600
 *   "clientNode"         the node number to ask for in the handshake (1)
 *   "da1"                destination node; default: the one the PLC assigned
 *   "sa1"                source node; default: the one the handshake returned
 *   "dna", "sna"         network addresses (0x00 = local)
 *   "sid"                service id, echoed back (0x01)
 *   "connectTimeoutMs"   TCP connect (default 3000)
 *   "timeoutMs"          per request (default 1000)
 *   "retries"            re-sends after a transport failure (default 1)
 *   "mergeGap"           words a read may skip to join two spans (default 8)
 *
 * The point map names an area and a word: {"area":"D","offset":100},
 * {"area":"CIO","offset":10,"bit":3} or the shorthand "D100" / "CIO10.3".
 * Areas are CIO/W/H/A/D/P/C/T/TS/CS/CF/IR/DR/TK and the EM banks "E0_0".."E1_15".
 * A point whose type is bit - or that carries a bit index - is read in bit
 * access, everything else as words; the wire is big endian.
 *
 * A TCP link runs the node address allocation handshake once, right after
 * connecting (§8.1): without it the PLC refuses the FINS frames.
 *
 * Not implemented yet: FINS/UDP and HostLink/C-Mode.
 */
#ifndef NCL_FINS_DRIVER_H
#define NCL_FINS_DRIVER_H

#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

ncl_driver *ncl_fins_tcp_create(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_FINS_DRIVER_H */
