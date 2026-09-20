/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - FANUC, as one file.
 *
 * This is the whole FANUC adapter: the connection, one dispatch function, and
 * the nineteen points the site's machine model is built from. The point map
 * used to live in conf/fanuc.json; it is here now, next to the protocol call it
 * belongs to, so a compiler checks it and no one has to keep two files in step.
 *
 * Everything the points need - which FOCAS item, which reply block, which type
 * - is the `addr` object the configuration used to carry, moved into the point
 * table below (see ncl_focas_driver.h for what `area`/`offset` mean):
 *
 *   "STATINFO@12"  item STATINFO, byte 12 of the block payload   (ODBST.alarm)
 *   offset         the reply block to take the value out of
 *
 * Only three items have a verified field layout (01 册 §2.3): STATINFO's ODBST
 * split, ACTF/ACTS' float arrays and RDCOUNT's counter, plus EXEPRGNAME2's
 * name. The rest of the two dozen items in the book stay out of the table until
 * someone can check them against a machine.
 *
 * Read only: the delivered box reads FOCAS through its own C++ SDK and no write
 * endpoint was ever captured, so a Set on any of these points is refused with
 * NCL_ERR_NOT_SUPPORTED (the host never calls an operation the point did not
 * declare, so this is belt and braces).
 */
#include "nclink/ncl_tool.h"

#include <string.h>

#include "focas/ncl_focas_driver.h"

/** One declared point: the FOCAS item, the reply block, and how to read it. */
typedef struct {
    const char *area;   /**< item name, or "ITEM@<byte>" for a field inside it */
    int64_t     offset; /**< reply block index (0 based)                       */
    ncl_dtype   dtype;
    int         length; /**< elements; 36 for the eighteen byte long names     */
} focas_point;

/* Machine status: ODBST, a 30 byte bitfield the capture in 01 册 §2.3 splits
 * into these sixteen bit fields, one per declared point. */
static const focas_point k_status_manual    = {"STATINFO@0",  0, NCL_DTYPE_INT16, 1};
static const focas_point k_status_run       = {"STATINFO@2",  0, NCL_DTYPE_INT16, 1};
static const focas_point k_status_edit      = {"STATINFO@4",  0, NCL_DTYPE_INT16, 1};
static const focas_point k_status_motion    = {"STATINFO@6",  0, NCL_DTYPE_INT16, 1};
static const focas_point k_status_mstb      = {"STATINFO@8",  0, NCL_DTYPE_INT16, 1};
static const focas_point k_status_emergency = {"STATINFO@10", 0, NCL_DTYPE_INT16, 1};
static const focas_point k_status_alarm     = {"STATINFO@12", 0, NCL_DTYPE_INT16, 1};
static const focas_point k_status_spindle   = {"STATINFO@14", 0, NCL_DTYPE_INT16, 1};
static const focas_point k_status_operator  = {"STATINFO@16", 0, NCL_DTYPE_INT16, 1};
/* Two more of the same item, taken from later reply blocks. */
static const focas_point k_status_dummy     = {"STATINFO",    1, NCL_DTYPE_INT16, 1};
static const focas_point k_status_auto      = {"STATINFO",    2, NCL_DTYPE_INT16, 1};

static const focas_point k_part_count       = {"RDCOUNT",     0, NCL_DTYPE_INT32, 1};
static const focas_point k_program_name     = {"EXEPRGNAME2", 0, NCL_DTYPE_STRING, 36};

static const focas_point k_axis0_position   = {"ACTF@0", 0, NCL_DTYPE_FLOAT32, 1};
static const focas_point k_axis1_position   = {"ACTF@4", 0, NCL_DTYPE_FLOAT32, 1};
static const focas_point k_axis2_position   = {"ACTF@8", 0, NCL_DTYPE_FLOAT32, 1};
static const focas_point k_axis0_speed      = {"ACTS@0", 0, NCL_DTYPE_FLOAT32, 1};
static const focas_point k_axis1_speed      = {"ACTS@4", 0, NCL_DTYPE_FLOAT32, 1};
static const focas_point k_axis2_speed      = {"ACTS@8", 0, NCL_DTYPE_FLOAT32, 1};

/** A method-like point: the FOCAS operation the host calls by name. */
typedef struct {
    const char *operation;
} focas_method;

/* Two commissioning aids the driver has always had: what the session looks like
 * and what the item table knows. They answer as Method calls to
 * "focas/SESSION" and "focas/ITEMS". */
static const focas_method k_session = {"session"};
static const focas_method k_items   = {"items"};

/* -------------------------------------------------------------- the tool -- */

/**
 * Open the box: build the FOCAS client and hand it the configuration's
 * "parameters" (host, port, timeouts, negotiate). The session itself is opened
 * on the first read, so a machine that is down does not keep the adapter from
 * coming up - it reports a dead link when someone asks for a value.
 */
static void *focas_open(const ncl_json *params, char **err)
{
    ncl_driver *driver = ncl_focas_create();
    const ncl_driver_ops *ops;
    ncl_err rc;

    if (driver == NULL) {
        if (err != NULL) {
            *err = ncl_strdup("无法创建 FOCAS 客户端");
        }
        return NULL;
    }
    ops = ncl_driver_ops_of(driver);
    rc = ops != NULL && ops->create != NULL ? ops->create(driver, params)
                                            : NCL_ERR_NOT_SUPPORTED;
    if (rc != NCL_OK) {
        if (err != NULL) {
            *err = ncl_strdup("FOCAS 参数不合法");
        }
        if (ops != NULL && ops->destroy != NULL) {
            ops->destroy(driver);
        }
        return NULL;
    }
    return driver;
}

static void focas_close(void *ctx)
{
    ncl_driver *driver = (ncl_driver *)ctx;

    if (driver != NULL && driver->ops != NULL &&
        driver->ops->destroy != NULL) {
        driver->ops->destroy(driver);
    }
}

/**
 * The frames of the last exchange, for the host's §6 trail. This is the only
 * audit related line an adapter author writes: the host does the accounting.
 */
static void focas_last_raw(void *ctx, ncl_tool_frames *out)
{
    ncl_driver *driver = (ncl_driver *)ctx;
    ncl_driver_raw raw;

    if (driver == NULL) {
        return;
    }
    ncl_driver_last_raw(driver, &raw);
    out->request = raw.request;
    out->request_len = raw.request_len;
    out->reply = raw.reply;
    out->reply_len = raw.reply_len;
}

/**
 * One function for all nineteen points: `self->arg` is the FOCAS address of
 * this point, `op` is what the client asked for. The address is borrowed from
 * the table above, so it is never cleared.
 */
static ncl_err focas_dispatch(void *ctx, const ncl_tool_point *self,
                              ncl_operation op, const ncl_json *params,
                              ncl_json **result, char **reason)
{
    const focas_point *point = (const focas_point *)self->arg;
    ncl_driver *driver = (ncl_driver *)ctx;
    ncl_address address;
    ncl_err rc;

    if (op == NCL_OP_FUNC_CALL) {
        const focas_method *method = (const focas_method *)self->arg;

        if (driver == NULL || method == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_STATE, "%s 没有方法参数",
                                 self->path);
        }
        rc = driver->ops->call != NULL
                 ? driver->ops->call(driver, method->operation, params, result)
                 : NCL_ERR_NOT_SUPPORTED;
        if (rc != NCL_OK) {
            return ncl_tool_fail(reason, rc, "%s 失败（%s）", self->path,
                                 ncl_driver_error_tier_name(rc));
        }
        return NCL_OK;
    }
    if (op != NCL_OP_GET_VALUE) {
        return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                             "FOCAS 适配器只读：%s", self->path);
    }
    if (driver == NULL || point == NULL) {
        return ncl_tool_fail(reason, NCL_ERR_STATE, "%s 没有点位参数",
                             self->path);
    }
    memset(&address, 0, sizeof(address));
    address.area = point->area;
    address.offset = point->offset;
    address.bit = -1; /* a field inside a block is part of the item name */
    address.length = point->length;
    address.dtype = point->dtype;

    rc = ncl_driver_read_one(driver, &address, result);
    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "读取 %s 失败（%s）", self->path,
                             ncl_driver_error_tier_name(rc));
    }
    return NCL_OK;
}

NCL_TOOL_BEGIN("focas", "FANUC FOCAS / Fwlib32 over TCP, read only", 1000, 1000,
               focas_open, focas_close)
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS@MANUAL", focas_dispatch,
                          &k_status_manual)
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS@RUN", focas_dispatch, &k_status_run)
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS@EDIT", focas_dispatch, &k_status_edit)
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS@MOTION", focas_dispatch,
                          &k_status_motion)
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS@MSTB", focas_dispatch, &k_status_mstb)
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS@EMERGENCY", focas_dispatch,
                          &k_status_emergency)
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS@ALARM", focas_dispatch, &k_status_alarm)
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS@SPINDLE", focas_dispatch,
                          &k_status_spindle)
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS@OPERATOR", focas_dispatch,
                          &k_status_operator)
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS@DUMMY", focas_dispatch, &k_status_dummy)
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS@AUTO", focas_dispatch, &k_status_auto)
    NCL_POINT_SAMPLED_ARG("/MACHINE/PART_COUNT", focas_dispatch, &k_part_count)
    NCL_POINT_SAMPLED_ARG("/MACHINE/PROGRAM@NAME", focas_dispatch, &k_program_name)
    /* Three axes, two values each: the path tail would collide six ways, so
     * these carry their own name and answer as "focas/AXIS0.POSITION". */
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@0/POSITION", focas_dispatch,
                            &k_axis0_position, "AXIS0.POSITION")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@1/POSITION", focas_dispatch,
                            &k_axis1_position, "AXIS1.POSITION")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@2/POSITION", focas_dispatch,
                            &k_axis2_position, "AXIS2.POSITION")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@0/SPEED", focas_dispatch,
                            &k_axis0_speed, "AXIS0.SPEED")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@1/SPEED", focas_dispatch,
                            &k_axis1_speed, "AXIS1.SPEED")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@2/SPEED", focas_dispatch,
                            &k_axis2_speed, "AXIS2.SPEED")
    /* 方法：会话状态与数据项清单（现场调试用，不进模型、不参与采样）。 */
    NCL_METHOD_NAMED("/MACHINE/SESSION", focas_dispatch, &k_session, "SESSION")
    NCL_METHOD_NAMED("/MACHINE/ITEMS", focas_dispatch, &k_items, "ITEMS")
NCL_TOOL_END_WITH_RAW(focas_last_raw)

NCL_TOOL_MODULE("1.0.0", "FANUC FOCAS / Fwlib32 over TCP, read only (01 册 §2.1-§2.3)")
