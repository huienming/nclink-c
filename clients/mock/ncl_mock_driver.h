/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the "mock" protocol.
 *
 * An in-process memory model of a device: no wire, no sockets, but the same
 * address model, the same three error tiers and the same event hook as a real
 * driver. It is what the adapter's own tests and the offline development of a
 * point map run against (protocal/docs/00-通用-实现约定.md §8).
 */
#ifndef NCL_MOCK_DRIVER_H
#define NCL_MOCK_DRIVER_H

#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Factory for the "mock" protocol.
 *
 * Parameters (all optional):
 *   "latency_ms":  number - sleep this long in every operation
 *   "unmapped":    "zero" (default) | "error" - reading an unwritten address
 *   "fail":        {"code":245, "count":1, "message":"..."} - the next
 *                  "count" operations fail with that code (-1 = forever)
 *   "points":      [{"area":"D","offset":100,"dtype":"int16","value":7}]
 *
 * Operations of call():
 *   "echo"       - returns the params unchanged (protocol sanity check)
 *   "sleep"      - {"ms":N} waits and returns {"slept":N}
 *   "raiseEvent" - {"id":"alarm:1201","event":{...}} fires the event hook
 *   "fail"       - {"code":..,"message":".."} returns that code
 */
ncl_driver *ncl_mock_driver_create(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MOCK_DRIVER_H */
