/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - FANUC, as one file.
 *
 * This is the whole FANUC adapter: the connection, one dispatch function, and
 * the points the site's machine model is built from - the standard ones (册 32:
 * STATUS / PART_COUNT / PROGRAM / 五轴的 POSITION 与 SPEED) plus FANUC's own ODBST
 * bits, which the standard has no name for and which therefore carry a vendor
 * prefix. The point map
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

#include <stdio.h>
#include <string.h>

#include "focas/ncl_focas_driver.h"

/** How a point's value is produced. */
typedef enum {
    FOCAS_DATA = 0, /**< read the item and decode it                      */
    FOCAS_STATUS,   /**< the standard's three-state STATUS, derived (§5)  */
    FOCAS_PENDING,  /**< the FOCAS frame is not captured yet (01 册 §2.3) */
} focas_kind;

/** One declared point: the FOCAS item, the reply block, and how to read it. */
typedef struct {
    const char *area;   /**< item name, or "ITEM@<byte>" for a field inside it */
    int64_t     offset; /**< reply block index (0 based)                       */
    ncl_dtype   dtype;
    int         length; /**< elements; 36 for the eighteen byte long names     */
    focas_kind  kind;
    bool        as_text; /**< answer as a string even though the item is a number */
} focas_point;

/*
 * 标准项与私有项并存（32 册 §4.3/§5）：
 *
 *   /MACHINE/STATUS           标准里**唯一**的运行状态项，三态 running/free/holding
 *                             （表 8）；由 ODBST 的 RUN 与 EMERGENCY 位推导。
 *   /MACHINE/FANUC_ODBST@xxx  FANUC 私有位（ODBST，01 册 §2.3 实测的 30 字节位域）。
 *                             标准第 4 部分没有这些名字，所以带厂商前缀另起，
 *                             不能占用 STATUS（32 册 §5.3 记了这条待第 3 部分确认）。
 */
static const focas_point k_status        = {NULL, 0, NCL_DTYPE_STRING, 0, FOCAS_STATUS, false};

static const focas_point k_odbst_manual    = {"STATINFO@0",  0, NCL_DTYPE_INT16, 1, FOCAS_DATA, false};
static const focas_point k_odbst_run       = {"STATINFO@2",  0, NCL_DTYPE_INT16, 1, FOCAS_DATA, false};
static const focas_point k_odbst_edit      = {"STATINFO@4",  0, NCL_DTYPE_INT16, 1, FOCAS_DATA, false};
static const focas_point k_odbst_motion    = {"STATINFO@6",  0, NCL_DTYPE_INT16, 1, FOCAS_DATA, false};
static const focas_point k_odbst_mstb      = {"STATINFO@8",  0, NCL_DTYPE_INT16, 1, FOCAS_DATA, false};
static const focas_point k_odbst_emergency = {"STATINFO@10", 0, NCL_DTYPE_INT16, 1, FOCAS_DATA, false};
static const focas_point k_odbst_alarm     = {"STATINFO@12", 0, NCL_DTYPE_INT16, 1, FOCAS_DATA, false};
static const focas_point k_odbst_spindle   = {"STATINFO@14", 0, NCL_DTYPE_INT16, 1, FOCAS_DATA, false};
static const focas_point k_odbst_operator  = {"STATINFO@16", 0, NCL_DTYPE_INT16, 1, FOCAS_DATA, false};
/* Two more of the same item, taken from later reply blocks. */
static const focas_point k_odbst_dummy     = {"STATINFO",    1, NCL_DTYPE_INT16, 1, FOCAS_DATA, false};
static const focas_point k_odbst_auto      = {"STATINFO",    2, NCL_DTYPE_INT16, 1, FOCAS_DATA, false};

/* 表 7：PART_COUNT 是 **string**（加工件数），所以读回来要转成字符串；
 * PROGRAM 是主程序名（string），属于 CONTROLLER 组件，FOCAS 侧就是 EXEPRGNAME2。 */
static const focas_point k_part_count    = {"RDCOUNT",     0, NCL_DTYPE_INT32, 1, FOCAS_DATA, true};
static const focas_point k_program       = {"EXEPRGNAME2", 0, NCL_DTYPE_STRING, 36, FOCAS_DATA, false};

/* 表 6 的 WARNING（报警信息）：值是一个 JSON 对象（表 9：number 报警编号、
 * text 报警内容、key_value 自定义）。FOCAS 侧要靠 cnc_rdalmmsg2 一类调用取报警号
 * 与文本，**这个帧还没抓到**（01 册 §2.3 第 215 行：本地先失败，没进到协议层），
 * 所以先声明成 FOCAS_PENDING：读数返回明确错误，不给半截数据。
 * （FANUC 私有位里只有"有没有报警"这个标志：见 FANUC_ODBST@ALARM。） */
static const focas_point k_warning       = {"cnc_rdalmmsg2", 0, NCL_DTYPE_STRING, 0, FOCAS_PENDING, false};

/*
 * 五轴（表 3 的轴名：X/Y/Z 线性、A/C 旋转），每轴两个位置：实际（REAL）与目标（CMD）。
 *
 *   实际位置 REAL：`ACTF@4k`（01 册 §3.2：cnc_actf = 全部轴绝对位置 float 数组）。
 *   目标位置 CMD ：**还没抓到帧** —— 01 册 §2.3 的 Cb 码表里没有坐标类调用
 *                  （cnc_absolute / cnc_rdposition 都没进到协议层），所以这里
 *                  声明成 FOCAS_PENDING：读数返回明确的"未抓包"，不给假值，
 *                  也不进采样通道（见 31 册待真机清单）。
 *
 * 机床没有的轴：驱动在载荷不足时直接报错（focas_read_one：偏移超出载荷 ->
 * NCL_ERR_RANGE，长度不够 -> NCL_FOCAS_ERR_LENGTH），不会给出 0 或别的假值。
 */
static const focas_point k_axis_x_position_real = {"ACTF@0",  0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
static const focas_point k_axis_y_position_real = {"ACTF@4",  0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
static const focas_point k_axis_z_position_real = {"ACTF@8",  0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
static const focas_point k_axis_a_position_real = {"ACTF@12", 0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
static const focas_point k_axis_c_position_real = {"ACTF@16", 0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
static const focas_point k_axis_position_cmd    = {"cnc_rdposition", 0, NCL_DTYPE_FLOAT32, 1, FOCAS_PENDING, false};
static const focas_point k_axis_x_speed    = {"ACTS@0",  0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
static const focas_point k_axis_y_speed    = {"ACTS@4",  0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
static const focas_point k_axis_z_speed    = {"ACTS@8",  0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
static const focas_point k_axis_a_speed    = {"ACTS@12", 0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
static const focas_point k_axis_c_speed    = {"ACTS@16", 0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};

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
    if (point->kind == FOCAS_PENDING) {
        /* 帧还没抓到（01 册 §2.3 的码表里没有这个调用）：明确报错，不给假值。 */
        return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                             "%s：FOCAS 侧还没抓到这个调用的帧（%s，见 31 册）",
                             self->path, point->area);
    }
    if (point->kind == FOCAS_STATUS) {
        /* 标准 STATUS（32 册表 6/表 8）：一次读 STATINFO 的前十个 int16
         * （= ODBST 的前 20 字节），用 RUN（载荷偏移 2）与 EMERGENCY（偏移 10）
         * 两个位决定三态。三次读并成一次，报文不多花。 */
        ncl_address bits_address;
        ncl_json *bits = NULL;
        const ncl_json *run;
        const ncl_json *stop;
        long long running = 0;
        long long holding = 0;

        memset(&bits_address, 0, sizeof(bits_address));
        bits_address.area = "STATINFO@0";
        bits_address.offset = 0;
        bits_address.bit = -1;
        bits_address.length = 10;
        bits_address.dtype = NCL_DTYPE_INT16;
        rc = ncl_driver_read_one(driver, &bits_address, &bits);
        if (rc != NCL_OK) {
            ncl_json_free(bits);
            return ncl_tool_fail(reason, rc, "读取 %s 失败（%s）", self->path,
                                 ncl_driver_error_tier_name(rc));
        }
        run = ncl_json_arr_get(bits, 1);   /* STATINFO@2  */
        stop = ncl_json_arr_get(bits, 5);  /* STATINFO@10 */
        (void)ncl_json_as_int(run, &running);
        (void)ncl_json_as_int(stop, &holding);
        ncl_json_free(bits);
        return ncl_tool_reply_text(result,
                                   holding != 0   ? "holding"
                                   : running != 0 ? "running"
                                                  : "free");
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
    if (point->as_text && *result != NULL) {
        /* 表 7 说 PART_COUNT 是 string：把读到的数转成字符串再交出去。 */
        long long number = 0;

        if (ncl_json_as_int(*result, &number)) {
            char text[32];

            snprintf(text, sizeof(text), "%lld", number);
            ncl_json_free(*result);
            *result = NULL;
            return ncl_tool_reply_text(result, text);
        }
    }
    return NCL_OK;
}

NCL_TOOL_BEGIN("focas", "FANUC FOCAS / Fwlib32 over TCP, read only", 1000, 1000,
               focas_open, focas_close)
    /* 标准项（32 册：表 6 的 STATUS、表 7 的 PART_COUNT/PROGRAM、表 4 的位置与速度） */
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS", focas_dispatch, &k_status)
    NCL_POINT_SAMPLED_ARG("/MACHINE/PART_COUNT", focas_dispatch, &k_part_count)
    /* PROGRAM 属于 CONTROLLER 组件（表 2：组件对象）*/
    NCL_POINT_SAMPLED_ARG("/MACHINE/CONTROLLER/PROGRAM", focas_dispatch,
                          &k_program)
    /* 报警（表 6 的 WARNING）：待抓包，按需读 —— 报警不是每秒都要上报的东西，
     * 现场轮询/事件通道需要时再读。 */
    NCL_POINT_ARG("/MACHINE/WARNING", focas_dispatch, &k_warning)
    /* 五轴的位置：每轴两个 —— 实际（REAL，读得到）与目标（CMD，待抓包，按需读）。
     * 路径尾段重名，所以显式给名字：调用地址形如 focas/AXIS_X.POSITION_REAL。 */
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@X/POSITION@REAL", focas_dispatch,
                            &k_axis_x_position_real, "AXIS_X.POSITION_REAL")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@Y/POSITION@REAL", focas_dispatch,
                            &k_axis_y_position_real, "AXIS_Y.POSITION_REAL")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@Z/POSITION@REAL", focas_dispatch,
                            &k_axis_z_position_real, "AXIS_Z.POSITION_REAL")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@A/POSITION@REAL", focas_dispatch,
                            &k_axis_a_position_real, "AXIS_A.POSITION_REAL")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@C/POSITION@REAL", focas_dispatch,
                            &k_axis_c_position_real, "AXIS_C.POSITION_REAL")
    NCL_POINT_NAMED("/MACHINE/AXIS@X/POSITION@CMD", focas_dispatch,
                    &k_axis_position_cmd, "AXIS_X.POSITION_CMD")
    NCL_POINT_NAMED("/MACHINE/AXIS@Y/POSITION@CMD", focas_dispatch,
                    &k_axis_position_cmd, "AXIS_Y.POSITION_CMD")
    NCL_POINT_NAMED("/MACHINE/AXIS@Z/POSITION@CMD", focas_dispatch,
                    &k_axis_position_cmd, "AXIS_Z.POSITION_CMD")
    NCL_POINT_NAMED("/MACHINE/AXIS@A/POSITION@CMD", focas_dispatch,
                    &k_axis_position_cmd, "AXIS_A.POSITION_CMD")
    NCL_POINT_NAMED("/MACHINE/AXIS@C/POSITION@CMD", focas_dispatch,
                    &k_axis_position_cmd, "AXIS_C.POSITION_CMD")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@X/SPEED", focas_dispatch,
                            &k_axis_x_speed, "AXIS_X.SPEED")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@Y/SPEED", focas_dispatch,
                            &k_axis_y_speed, "AXIS_Y.SPEED")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@Z/SPEED", focas_dispatch,
                            &k_axis_z_speed, "AXIS_Z.SPEED")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@A/SPEED", focas_dispatch,
                            &k_axis_a_speed, "AXIS_A.SPEED")
    NCL_POINT_SAMPLED_NAMED("/MACHINE/AXIS@C/SPEED", focas_dispatch,
                            &k_axis_c_speed, "AXIS_C.SPEED")
    /* 私有项：FANUC 的 ODBST 位域（现场调试用；标准第 4 部分没有这些名字，
     * 所以带 FANUC_ 前缀，不占用 STATUS）。 */
    NCL_POINT_SAMPLED_ARG("/MACHINE/FANUC_ODBST@MANUAL", focas_dispatch,
                          &k_odbst_manual)
    NCL_POINT_SAMPLED_ARG("/MACHINE/FANUC_ODBST@RUN", focas_dispatch,
                          &k_odbst_run)
    NCL_POINT_SAMPLED_ARG("/MACHINE/FANUC_ODBST@EDIT", focas_dispatch,
                          &k_odbst_edit)
    NCL_POINT_SAMPLED_ARG("/MACHINE/FANUC_ODBST@MOTION", focas_dispatch,
                          &k_odbst_motion)
    NCL_POINT_SAMPLED_ARG("/MACHINE/FANUC_ODBST@MSTB", focas_dispatch,
                          &k_odbst_mstb)
    NCL_POINT_SAMPLED_ARG("/MACHINE/FANUC_ODBST@EMERGENCY", focas_dispatch,
                          &k_odbst_emergency)
    NCL_POINT_SAMPLED_ARG("/MACHINE/FANUC_ODBST@ALARM", focas_dispatch,
                          &k_odbst_alarm)
    NCL_POINT_SAMPLED_ARG("/MACHINE/FANUC_ODBST@SPINDLE", focas_dispatch,
                          &k_odbst_spindle)
    NCL_POINT_SAMPLED_ARG("/MACHINE/FANUC_ODBST@OPERATOR", focas_dispatch,
                          &k_odbst_operator)
    NCL_POINT_SAMPLED_ARG("/MACHINE/FANUC_ODBST@DUMMY", focas_dispatch,
                          &k_odbst_dummy)
    NCL_POINT_SAMPLED_ARG("/MACHINE/FANUC_ODBST@AUTO", focas_dispatch,
                          &k_odbst_auto)
    /* 方法：会话状态与数据项清单（现场调试用，不进模型、不参与采样）。 */
    NCL_METHOD_NAMED("/MACHINE/SESSION", focas_dispatch, &k_session, "SESSION")
    NCL_METHOD_NAMED("/MACHINE/ITEMS", focas_dispatch, &k_items, "ITEMS")
NCL_TOOL_END_WITH_RAW(focas_last_raw)

NCL_TOOL_MODULE("1.1.0", "FANUC FOCAS / Fwlib32 over TCP, read only (01 册 §2.1-§2.3，32 册数据项)")
