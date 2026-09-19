/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the SYNTEC RemoteCNC driver.
 *
 *   "syntec"   M700/M80 (and the OpenCNC family) over TCP, port 8000
 *
 * Parameters:
 *   "host", "port"      target; port defaults to 8000
 *   "funcId"            override for the uFuncID written into the function
 *                       header; 0 (default) means "the command number itself"
 *   "serial"            the first uSerial (default 1, incrementing)
 *   "connectTimeoutMs"  TCP connect (default 3000)
 *   "timeoutMs"         per request (default 3000; a control is slower than a
 *                       PLC, and the reply carries a whole structure)
 *   "retries"           re-sends after a transport failure (default 0)
 *
 * A point names **the command and the code it wants**, so the driver never has
 * to guess business codes that are still unverified:
 *
 *   {"path": "/CNC/PART", "addr": {"area": "KrnlAPI", "offset": 0x0001,
 *                                  "length": 4, "dtype": "int32"}}
 *   {"path": "/CNC/X",    "addr": {"area": "200", "offset": 20,
 *                                  "length": 8, "dtype": "float64"}}
 *
 * The area is the command number (§10.7: one numbering space for the whole
 * controller) - a name from §10.6 or a bare number - and the offset is the
 * `dwCode` the command carries; `length` is the reply size asked for.
 *
 * §10.9: the controller dispatches on the function header's **uFuncID** and
 * writes `CmdID = uFuncID` into its answers, so both fields carry the same
 * number; `"funcId"` only exists for the case where a machine wants another.
 * Those two numbers come from the machine's own documentation or from a
 * capture, which is what keeps this driver honest: §10.8 shows the controller
 * hands `dwCode` straight to its native Krnl API, so the codes are the native
 * ones and were not recoverable from the delivered material.
 *
 * Reads only. The box's own driver layer lists no write endpoint for SYNTEC
 * either (i-BOX 设备 API 清单 §4.5), so a write reports NCL_ERR_NOT_SUPPORTED.
 *
 * The raw escape hatch takes `CmdID u2 | funcId u2 | body bytes` and answers
 * with the reply body, which is how an undocumented command is tried.
 */
#ifndef NCL_SYNTEC_DRIVER_H
#define NCL_SYNTEC_DRIVER_H

#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

ncl_driver *ncl_syntec_create(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_SYNTEC_DRIVER_H */
