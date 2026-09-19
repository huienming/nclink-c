/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - 凯恩帝 KND, the controller's own HTTP/JSON interface.
 *
 * Unlike every other protocol in this library, KND needs no frame codec: the
 * controller runs a small REST service and answers one JSON object per
 * endpoint. What the driver needs is therefore a *table* - which model item
 * lives on which endpoint, under which field, with what scaling.
 *
 * The table below is not a guess: it comes from the mapping layer of the
 * delivered gateway (`app1/nclink-service/lua/lua_mod/knd_mod.lua` and
 * `func_mod.lua`, both driving their base class with a `base_url`), which is
 * the field implementation that actually runs. See
 * `protocal/docs/09-KND-凯恩帝.md` §3 and `protocal/docs/28-交付包现场API清单.md`.
 *
 * Two shapes worth knowing:
 *   - the overrides arrive as a 0..2 fraction and the field wants 0..200 %,
 *     so they are multiplied by 100;
 *   - `run-status` is 0/1/2 and means free/holding/running, not a number.
 */
#ifndef NCL_KND_H
#define NCL_KND_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How a field becomes the value of a model item. */
typedef enum {
    NCL_KND_NUMBER = 0,   /**< a number as it comes                       */
    NCL_KND_INTEGER,      /**< a number rounded to an integer              */
    NCL_KND_TEXT,         /**< a number rendered as text (a program number)*/
    NCL_KND_PERCENT,      /**< 0..2 fraction, reported in percent (×100)   */
    NCL_KND_BIT0,         /**< bit 0 of the number, as a boolean           */
    NCL_KND_RUN_STATUS,   /**< 0 free, 1 holding, 2 running                */
    NCL_KND_FIRST,        /**< the document is an array: take element 0    */
    NCL_KND_ALARMS,       /**< the document is the alarm map (see below)   */
    NCL_KND_AXIS,         /**< the field name is the axis letter           */
} ncl_knd_shape;

/** One model item, and where the controller keeps it. */
typedef struct {
    const char  *item;  /**< the model item, "/STATUS", "/PART_COUNT", ...  */
    const char  *path;  /**< the endpoint on the controller ("/getValue")   */
    const char  *field; /**< the JSON key; NULL for the array/alarm shapes  */
    ncl_knd_shape shape;
    int          axis;  /**< NCL_KND_AXIS: index into "XYZABCUVW", else -1  */
} ncl_knd_item;

/**
 * The item *name* of an axis reading, as the point map spells it:
 * `/AXIS@2/SCREW/POSITION`. Both `SCREW` and `MOTOR` name the same number
 * (the delivered mapping layer reads both from `/coors/machine`).
 */
#define NCL_KND_AXIS_LETTERS "XYZABCUVW"
#define NCL_KND_AXIS_COUNT 9

/**
 * Look a model item up. Case is ignored and a leading '/' is optional, so
 * "STATUS", "/status" and "/STATUS" are the same item; axis readings accept
 * any index 0..8 ("/AXIS@0/SCREW/POSITION").
 * Returns NULL when the item is not one of the mapped ones.
 */
const ncl_knd_item *ncl_knd_item_lookup(const char *name);

/**
 * The alarm classes of `/CONTROLLER/WARNING`, in the order the delivered
 * mapping layer numbers them (`100%02d`, so "prm-switch" is alarm 10001).
 * A class whose text is empty is not reported.
 */
size_t ncl_knd_alarm_class_count(void);
const char *ncl_knd_alarm_class(size_t index);

#ifdef __cplusplus
}
#endif

#endif /* NCL_KND_H */
