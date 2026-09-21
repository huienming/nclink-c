/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the Modbus drivers.
 *
 * One engine, three transports: values turn into registers either way, only the
 * framing and the carrier differ.
 *
 *   "modbus_tcp"      MBAP over TCP (port 502)
 *   "modbus_rtu"      address + PDU + CRC16 over RS-485/RS-232
 *   "modbus_rtu_tcp"  RTU frames over TCP (serial gateway / Moxa style)
 *
 * Parameters (all optional unless noted):
 *   "host", "port"         TCP / RTU-over-TCP target
 *   "serial"              "COM3" or "/dev/ttyUSB0" for RTU
 *   "baud", "parity", "dataBits", "stopBits", "interFrameMs"
 *   "unit"                slave / unit id (default 1; 0 = broadcast write)
 *   "connectTimeoutMs"    TCP connect (default 3000)
 *   "timeoutMs"           per request (default 1000)
 *   "retries"             re-sends after a transport failure (default 1 TCP,
 *                         2 RTU, as 15-MODBUS.md §5 suggests)
 *   "mergeGap"            registers a read may skip to join two spans (default 8)
 *   "wordOrder"           ABCD (default) / CDAB / BADC / DCBA
 *   "base1"               true when the point map uses 40001-style numbers
 *                         (default false: the protocol's 0-based addresses)
 */
#ifndef NCL_MODBUS_DRIVER_H
#define NCL_MODBUS_DRIVER_H

#include "nclink/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

ncl_driver *ncl_modbus_tcp_create(void);
ncl_driver *ncl_modbus_rtu_create(void);
ncl_driver *ncl_modbus_rtu_tcp_create(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MODBUS_DRIVER_H */
