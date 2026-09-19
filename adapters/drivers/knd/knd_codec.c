/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the KND item table (see ncl_knd.h).
 *
 * Pure data plus lookups: no I/O, so the table can be tested on its own.
 */

#include "nclink_adapter/ncl_knd.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "nclink/ncl_common.h"

/*
 * The mapping of the delivered gateway, one row per model item. `path` and
 * `field` are what `lua_mod/knd_mod.lua` registers; the shapes are what its
 * `data_process` functions do to the value before it reaches the model.
 */
static const ncl_knd_item kItems[] = {
    {"/STATUS", "/getValue", "run-status", NCL_KND_RUN_STATUS, -1},
    {"/VARIABLE@ESP_STATE", "/status", "not-ready-reason", NCL_KND_BIT0, -1},
    {"/CONTROLLER/PROGRAM", "/progs/cur", "number", NCL_KND_TEXT, -1},
    {"/CONTROLLER/SUBPROGRAM", "/progs/cur", "number", NCL_KND_TEXT, -1},
    {"/CONTROLLER/LINE_NUMBER", "/progs/exec-status", "P", NCL_KND_NUMBER, -1},
    {"/PART_COUNT", "/workcounts/total", "count", NCL_KND_INTEGER, -1},
    {"/VARIABLE@PLANNED_COUNT", "/workcountgoals/total", "count",
     NCL_KND_NUMBER, -1},
    {"/VARIABLE@RUN_TIME", "/cycletime", "total", NCL_KND_NUMBER, -1},
    {"/VARIABLE@CUT_TIME", "/cycletime", "cur", NCL_KND_NUMBER, -1},
    {"/FEED_OVERRIDE", "/overrides/feed", "ov", NCL_KND_PERCENT, -1},
    {"/RAPID_OVERRIDE", "/overrides/rapid", "ov", NCL_KND_PERCENT, -1},
    {"/JOG_OVERRIDE", "/overrides/jog", "ov", NCL_KND_PERCENT, -1},
    {"/HANDWHEEL_OVERRIDE", "/overrides/handle", "ov", NCL_KND_PERCENT, -1},
    {"/SPINDLE_OVERRIDE", "/sp/overrides/1", "ov", NCL_KND_PERCENT, -1},
    {"/SPINDLE_SPEED", "/sp/speeds/1", "speed", NCL_KND_NUMBER, -1},
    {"/TOOL_NUMBER", "/plc/vm/TL0", NULL, NCL_KND_FIRST, -1},
    {"/CONTROLLER/WARNING", "/alarms/", NULL, NCL_KND_ALARMS, -1},
};

/** The two axis readings, both from the one machine-coordinate document. */
static const struct {
    const char *suffix;
    const char *item; /**< what a caller gets back as the canonical name */
} kAxisItems[] = {
    {"/SCREW/POSITION", "/AXIS@%d/SCREW/POSITION"},
    {"/MOTOR/POSITION", "/AXIS@%d/MOTOR/POSITION"},
};

static size_t axis_item_count(void)
{
    return sizeof(kAxisItems) / sizeof(kAxisItems[0]);
}

static const char *const kAlarmClasses[] = {
    "prm-switch", "reboot",     "plc",     "ps",         "over-travel",
    "over-heat",  "mem",        "servo",   "servo-bus",  "over-workarea",
    "io-bus",     "io-module",  "manufacture", "forbid-move",
};

/** Case insensitive compare that ignores a leading '/'. */
static bool item_name_same(const char *left, const char *right)
{
    if (left != NULL && left[0] == '/') {
        left++;
    }
    if (right != NULL && right[0] == '/') {
        right++;
    }
    if (left == NULL || right == NULL) {
        return false;
    }
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

const ncl_knd_item *ncl_knd_item_lookup(const char *name)
{
    /*
     * The axis rows are one item per (axis, reading), and there are 18 of them
     * for 2 readings; they all live on the same document, so they answer from
     * a small pool of static records filled in on demand. Single threaded per
     * caller (a driver holds one address at a time), and the pool is big enough
     * that a caller that keeps a few handles around does not see them change.
     */
    static ncl_knd_item pool[NCL_KND_AXIS_COUNT * 2];
    static char names[NCL_KND_AXIS_COUNT * 2][40];
    static size_t used;
    static const char kPrefix[] = "/AXIS@";
    const char *rest;
    size_t i;
    int axis;

    if (ncl_str_is_blank(name)) {
        return NULL;
    }
    for (i = 0; i < sizeof(kItems) / sizeof(kItems[0]); i++) {
        if (item_name_same(name, kItems[i].item)) {
            return &kItems[i];
        }
    }
    if (name[0] != '/' || strncmp(name, kPrefix, sizeof(kPrefix) - 1u) != 0) {
        return NULL;
    }
    rest = name + sizeof(kPrefix) - 1u;
    if (*rest < '0' || *rest > '0' + (NCL_KND_AXIS_COUNT - 1)) {
        return NULL;
    }
    axis = *rest - '0';
    rest++;
    for (i = 0; i < axis_item_count(); i++) {
        ncl_knd_item *slot;

        if (!item_name_same(rest, kAxisItems[i].suffix)) {
            continue;
        }
        slot = &pool[used % (NCL_KND_AXIS_COUNT * 2)];
        (void)snprintf(names[used % (NCL_KND_AXIS_COUNT * 2)],
                       sizeof(names[0]), "/AXIS@%d%s", axis,
                       kAxisItems[i].suffix);
        slot->item = names[used % (NCL_KND_AXIS_COUNT * 2)];
        slot->path = "/coors/machine";
        slot->field = NULL;
        slot->shape = NCL_KND_AXIS;
        slot->axis = axis;
        used++;
        return slot;
    }
    return NULL;
}

size_t ncl_knd_alarm_class_count(void)
{
    return sizeof(kAlarmClasses) / sizeof(kAlarmClasses[0]);
}

const char *ncl_knd_alarm_class(size_t index)
{
    return index < ncl_knd_alarm_class_count() ? kAlarmClasses[index] : NULL;
}
