/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - FANUC, as one file.
 *
 * This is the whole FANUC adapter: the connection, one dispatch function, and
 * the points the site's machine model is built from. **Every name here comes
 * from the data dictionary** (册 32 第 4 部分): STATUS / PART_COUNT / WARNING /
 * PROGRAM / POSITION / SPEED. FANUC's own ODBST bit field is not in the model -
 * the dictionary has no name for those bits - so the model carries only what
 * the dictionary names, and a derived item (STATUS) is spelled with a
 * dictionary name rather than a vendor one. The point map
 * used to live in conf/fanuc.json; it is here now, next to the protocol call it
 * belongs to, so a compiler checks it and no one has to keep two files in step.
 *
 * 默认采样通道按现场口径只放四样：设备状态、加工计件、程序名称、报警。位置、速度
 * 一律按需读 —— 要上报就把那一行的 NCL_POINT_* 换成 NCL_POINT_SAMPLED_*。
 * 报警与五个目标位置现在还没有抓包（NCL_POINT_PENDING*）：模型里有、问起来有明确
 * 答复，但取不到值。
 *
 * Everything the points need - which FOCAS item, which reply block, which type
 * - is the `addr` object the configuration used to carry, moved into the point
 * table below (see ncl_focas_driver.h for what `area`/`offset` mean):
 *
 *   "ACTF@8"       item ACTF, byte 8 of the block payload        (轴 Z 的位置)
 *   offset         the reply block to take the value out of
 *
 * Five items carry the whole table, and all five have a verified layout
 * (01 册 §2.3): STATINFO (the ODBST bit field STATUS is derived from), ACTF and
 * ACTS (per axis float), RDCOUNT (the counter) and EXEPRGNAME2 (the program
 * name). The rest of the two dozen items in the book stay out of the table
 * until someone can check them against a machine.
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
 * 名字一律取标准第 4 部分（32 册）的数据项名 —— 模型里出现的每一个 type 都要能在
 * 那一册里查到。FANUC 自己的 ODBST 位域（01 册 §2.3 实测的 30 字节）**不进模型**：
 * 标准里没有这些名字，派生出来的东西只能用标准里的名字（下面的 STATUS 就是 RUN 与
 * EMERGENCY 两位推出来的三态）。要看原始位就用方法 `focas/ITEMS`（驱动自己的项表）。
 *
 *   /MACHINE/STATUS  表 6 的 STATUS（表 8 的取值 running/free/holding）
 */
static const focas_point k_status        = {NULL, 0, NCL_DTYPE_STRING, 0, FOCAS_STATUS, false};

/* 表 7：PART_COUNT 是 **string**（加工件数），所以读回来要转成字符串；
 * PROGRAM 是主程序名（string），属于 CONTROLLER 组件，FOCAS 侧就是 EXEPRGNAME2。 */
static const focas_point k_part_count    = {"RDCOUNT",     0, NCL_DTYPE_INT32, 1, FOCAS_DATA, true};
static const focas_point k_program       = {"EXEPRGNAME2", 0, NCL_DTYPE_STRING, 36, FOCAS_DATA, false};

/*
 * 五轴（表 3 的轴名：X/Y/Z 线性、A/C 旋转），每轴两个位置：实际（REAL）与目标（CMD）。
 *
 *   实际位置 REAL：`ACTF@4k`（01 册 §3.2：cnc_actf = 全部轴绝对位置 float 数组）。
 *   目标位置 CMD ：在声明表里，但**帧还没抓到** —— 01 册 §2.3 的 Cb 码表里没有
 *                  坐标类调用（cnc_absolute / cnc_rdposition 都没进到协议层），
 *                  所以那边用 NCL_POINT_PENDING_NAMED 声明：模型里有它，问它会
 *                  拿到"待抓包"，不给假值（见 31 册待真机清单）。
 *
 * 机床没有的轴：驱动在载荷不足时直接报错（focas_read_one：偏移超出载荷 ->
 * NCL_ERR_RANGE，长度不够 -> NCL_FOCAS_ERR_LENGTH），不会给出 0 或别的假值。
 */
static const focas_point k_axis_x_position_real = {"ACTF@0",  0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
static const focas_point k_axis_y_position_real = {"ACTF@4",  0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
static const focas_point k_axis_z_position_real = {"ACTF@8",  0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
static const focas_point k_axis_a_position_real = {"ACTF@12", 0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
static const focas_point k_axis_c_position_real = {"ACTF@16", 0, NCL_DTYPE_FLOAT32, 1, FOCAS_DATA, false};
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
    /* 默认采样通道只放四样（现场口径）：设备状态、加工计件、程序名称、报警。
     * 其余（位置/速度）一律按需读 —— 要上报就把对应的 NCL_POINT_* 换成
     * NCL_POINT_SAMPLED_*。 */
    NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS", focas_dispatch, &k_status)
    NCL_POINT_SAMPLED_ARG("/MACHINE/PART_COUNT", focas_dispatch, &k_part_count)
    /* PROGRAM 属于 CONTROLLER 组件（表 2：组件对象）*/
    NCL_POINT_SAMPLED_ARG("/MACHINE/CONTROLLER/PROGRAM", focas_dispatch,
                          &k_program)
    /* 报警（表 6 的 WARNING）：进默认采样通道，但帧还没抓到（01 册 §2.3 的码表里
     * 没有 cnc_rdalmmsg2），所以先占着通道、取值为 null，自检把它算成"待抓包"。
     * 抓包补上之后换成 NCL_POINT_SAMPLED_ARG("/MACHINE/WARNING", …, &k_warning)。 */
    NCL_POINT_PENDING_SAMPLED("/MACHINE/WARNING",
                              "报警：帧待抓包（cnc_rdalmmsg2，01 册 §2.3 / 31 册）")
    /* 五轴的位置：每轴两个 —— 实际（REAL，读得到）与目标（CMD，待抓包），都按需读。
     * 路径尾段重名，所以显式给名字：调用地址形如 focas/AXIS_X.POSITION_REAL。 */
    NCL_POINT_NAMED("/MACHINE/AXIS@X/POSITION@REAL", focas_dispatch,
                    &k_axis_x_position_real, "AXIS_X.POSITION_REAL")
    NCL_POINT_NAMED("/MACHINE/AXIS@Y/POSITION@REAL", focas_dispatch,
                    &k_axis_y_position_real, "AXIS_Y.POSITION_REAL")
    NCL_POINT_NAMED("/MACHINE/AXIS@Z/POSITION@REAL", focas_dispatch,
                    &k_axis_z_position_real, "AXIS_Z.POSITION_REAL")
    NCL_POINT_NAMED("/MACHINE/AXIS@A/POSITION@REAL", focas_dispatch,
                    &k_axis_a_position_real, "AXIS_A.POSITION_REAL")
    NCL_POINT_NAMED("/MACHINE/AXIS@C/POSITION@REAL", focas_dispatch,
                    &k_axis_c_position_real, "AXIS_C.POSITION_REAL")
    NCL_POINT_PENDING_NAMED("/MACHINE/AXIS@X/POSITION@CMD", "AXIS_X.POSITION_CMD",
                            "目标位置：帧待抓包（cnc_rdposition）")
    NCL_POINT_PENDING_NAMED("/MACHINE/AXIS@Y/POSITION@CMD", "AXIS_Y.POSITION_CMD",
                            "目标位置：帧待抓包（cnc_rdposition）")
    NCL_POINT_PENDING_NAMED("/MACHINE/AXIS@Z/POSITION@CMD", "AXIS_Z.POSITION_CMD",
                            "目标位置：帧待抓包（cnc_rdposition）")
    NCL_POINT_PENDING_NAMED("/MACHINE/AXIS@A/POSITION@CMD", "AXIS_A.POSITION_CMD",
                            "目标位置：帧待抓包（cnc_rdposition）")
    NCL_POINT_PENDING_NAMED("/MACHINE/AXIS@C/POSITION@CMD", "AXIS_C.POSITION_CMD",
                            "目标位置：帧待抓包（cnc_rdposition）")
    NCL_POINT_NAMED("/MACHINE/AXIS@X/SPEED", focas_dispatch, &k_axis_x_speed,
                    "AXIS_X.SPEED")
    NCL_POINT_NAMED("/MACHINE/AXIS@Y/SPEED", focas_dispatch, &k_axis_y_speed,
                    "AXIS_Y.SPEED")
    NCL_POINT_NAMED("/MACHINE/AXIS@Z/SPEED", focas_dispatch, &k_axis_z_speed,
                    "AXIS_Z.SPEED")
    NCL_POINT_NAMED("/MACHINE/AXIS@A/SPEED", focas_dispatch, &k_axis_a_speed,
                    "AXIS_A.SPEED")
    NCL_POINT_NAMED("/MACHINE/AXIS@C/SPEED", focas_dispatch, &k_axis_c_speed,
                    "AXIS_C.SPEED")
    /* 方法：会话状态与数据项清单（现场调试用，不进模型、不参与采样）。 */
    NCL_METHOD_NAMED("/MACHINE/SESSION", focas_dispatch, &k_session, "SESSION")
    NCL_METHOD_NAMED("/MACHINE/ITEMS", focas_dispatch, &k_items, "ITEMS")
NCL_TOOL_END_WITH_RAW(focas_last_raw)

NCL_TOOL_MODULE("1.3.0", "FANUC FOCAS / Fwlib32 over TCP, read only (01 册 §2.1-§2.3，32 册数据项)")
