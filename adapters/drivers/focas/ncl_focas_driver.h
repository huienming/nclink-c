/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the FANUC FOCAS / Fwlib32 driver.
 *
 *   "focas"   FANUC 0i/16i/18i/21i/30i/31i/32i over TCP, port 8193
 *
 * Parameters:
 *   "host", "port"      target; port defaults to 8193 (§2.1)
 *   "negotiate"         send the capability probe of §2.3 on open (default
 *                       true - it is what the vendor SDK does, and a machine
 *                       may expect it before it answers data reads)
 *   "helloCounter"      the 2 byte body of the `func 1` hello (default 1)
 *   "connectTimeoutMs"  TCP connect (default 3000)
 *   "timeoutMs"         per exchange (default 3000)
 *   "retries"           re-sends after a transport failure (default 0)
 *
 * A point names **a FOCAS item and the reply block it wants**:
 *
 *   {"path": "/CNC/PART_COUNT",
 *    "addr": {"area": "RDCOUNT", "offset": 0, "length": 1, "dtype": "int32"}}
 *   {"path": "/CNC/X",
 *    "addr": {"area": "ACTF", "offset": 0, "length": 1, "dtype": "float32"}}
 *   {"path": "/CNC/STATUS",
 *    "addr": {"area": "STATINFO", "offset": 1, "length": 1, "dtype": "int16"}}
 *
 * `area` is an item name from §2.3 (STATINFO, ACTF, ACTS, RDCOUNT, RDLIFE,
 * RDMACRO, RDPARAM, RDTOFS, RDPROGDIR3, EXEPRGNAME2) or a bare command code
 * (`"36"`, `"0x24"`, `"CB:0x24"`) for one the table does not name; `offset` is
 * the **reply block index** (0 based); `bit` is an optional byte offset inside
 * that block's payload (`-1` or absent means 0); `length` and `dtype` say how
 * to read it. Every field on this wire is big endian.
 *
 * That shape is deliberate: the capture in §2.3 gives us the request frames
 * and the block layout, but only three data items have a verified field
 * mapping (STATINFO's ODBST split, ACTF/ACTS' float arrays, RDCOUNT's
 * counter), so the point map - not the driver - says which block is which
 * field. `call("items")` lists what the table knows while commissioning.
 *
 * Reads only: the delivered box reads FOCAS through its own C++ SDK, and no
 * write endpoint was captured, so a write reports NCL_ERR_NOT_SUPPORTED.
 *
 * The raw escape hatch takes `func u1 | body bytes` and answers with the reply
 * body as hex, which is how an undocumented function is tried.
 */
#ifndef NCL_FOCAS_DRIVER_H
#define NCL_FOCAS_DRIVER_H

#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

ncl_driver *ncl_focas_create(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_FOCAS_DRIVER_H */
