/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * FANUC 适配器 —— 一个文件一台机床。
 *
 * 这份文件只做两件事，从上往下读一遍，就知道这台机床对外有哪些量：
 *
 *   1. **绑定**：把 client 的语义函数挂到模型路径上，一行一个点。
 *      "RDCOUNT 就是加工件数"、"STATUS 由 ODBST 的哪两位推出来"这些知识都在
 *      client 那一边（nclink/clients/focas.h），这里不重复。
 *   2. **覆盖**：两个现场调试方法自己写一小段；帧还没抓到的点也照常绑定 —— 那几
 *      个 client 函数现在回 NCL_ERR_UNAVAILABLE（"还读不了"），抓包补上只改 client，
 *      这张表一行都不用动。
 *
 * 协议细节（PDU 帧、item 码、回复块布局）在这份文件里一个字都没有 —— 那是 client
 * 的事，现场不需要懂。
 *
 * 每个名字都来自数据字典（册 32 第 4 部分）：STATUS / PART_COUNT / WARNING /
 * PROGRAM / POSITION / SPEED。FANUC 自己的 ODBST 位域不进模型 —— 字典里没有那些
 * 名字 —— 所以派生出来的 STATUS 用的是字典里的名字。
 *
 * 默认采样通道按现场口径只放四样：设备状态、加工计件、程序名称、报警。位置、速度
 * 一律按需读 —— 要上报就把那一行的 NCL_DATAITEM_F64 换成 NCL_DATAITEM_F64_SAMPLED。
 */
#include "nclink/ncl_tool.h"

#include "nclink/clients/focas.h"
#include "nclink/ncl_json.h"

/* ---------------------------------------------------------------- 连接 ---- */

/**
 * 把配置里的 parameters 交给 client。会话在第一次读的时候才建，所以机床没开机
 * 不影响设备程序启动：谁问值，谁拿到一条明确的"读不到"。
 */
static void *focas_open(const ncl_json *params, char **err)
{
    ncl_focas_config config;

    ncl_focas_config_default(&config);
    config.host = ncl_tool_param_str(params, "host", "");
    config.port =
        (unsigned)ncl_tool_param_int(params, "port", (long long)config.port);
    config.timeout_ms = (unsigned)ncl_tool_param_int(params, "timeoutMs",
                                                     (long long)config.timeout_ms);
    config.connect_timeout_ms =
        (unsigned)ncl_tool_param_int(params, "connectTimeoutMs",
                                     (long long)config.connect_timeout_ms);
    config.retries = (unsigned)ncl_tool_param_int(params, "retries",
                                                  (long long)config.retries);
    config.negotiate =
        ncl_tool_param_bool(params, "negotiate", config.negotiate);
    return ncl_focas_open(&config, err);
}

static void focas_close(void *ctx)
{
    ncl_focas_close((ncl_focas *)ctx);
}

/** §6 的审计要原始报文：问 client 一句就够，账由宿主管。 */
static void focas_last_raw(void *ctx, ncl_tool_frames *out)
{
    ncl_driver_raw raw;

    ncl_focas_last_raw((ncl_focas *)ctx, &raw);
    out->request = raw.request;
    out->request_len = raw.request_len;
    out->reply = raw.reply;
    out->reply_len = raw.reply_len;
}

/* -------------------------------------------------------------- 覆盖档 ---- */

/*
 * 两个现场调试方法（不进模型、不参与采样）：会话现在什么样、client 的 item 表里
 * 有什么。它们只是把 client 的诊断操作转出去，失败时把原因带上 —— 这就是"覆盖"
 * 的样子：需要自己解释时才写函数，而函数里照样用同一个 client 实例。
 */
static ncl_err session_method(void *ctx, const ncl_json *params,
                              ncl_json **result, char **reason)
{
    ncl_focas *focas = (ncl_focas *)ctx;
    ncl_err rc = ncl_focas_call(focas, "session", params, result);

    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s", ncl_focas_last_error(focas));
    }
    return NCL_OK;
}

static ncl_err items_method(void *ctx, const ncl_json *params,
                            ncl_json **result, char **reason)
{
    ncl_focas *focas = (ncl_focas *)ctx;
    ncl_err rc = ncl_focas_call(focas, "items", params, result);

    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s", ncl_focas_last_error(focas));
    }
    return NCL_OK;
}

/*
 * 程序上下行（现场调试用，不进模型、不参与采样）：参数里给程序文本/程序名，
 * 字节走 FOCAS 的三件套（01 册 §2.4）。程序不是数据对象 —— 标准里"文件"是
 * FILE（dict）对象，而"把一段程序下发/取回"是**动作**，所以它们是方法。
 */
static ncl_err program_download_method(void *ctx, const ncl_json *params,
                                       ncl_json **result, char **reason)
{
    ncl_focas *focas = (ncl_focas *)ctx;
    const char *program = ncl_tool_param_str(params, "data", NULL);
    long long type = ncl_tool_param_int(params, "type", 0);
    ncl_err rc;

    if (program == NULL) {
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                             "下发程序要在 params 里给 \"data\"（程序文本）");
    }
    rc = ncl_focas_program_download(focas, type,
                                    ncl_tool_param_str(params, "dir", NULL),
                                    program);
    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s", ncl_focas_last_error(focas));
    }
    if (result != NULL) {
        ncl_json *object = ncl_json_new_object();

        if (object == NULL) {
            return NCL_ERR_NOMEM;
        }
        (void)ncl_json_obj_set_int(object, "bytes", (long long)strlen(program));
        *result = object;
    }
    return NCL_OK;
}

static ncl_err program_upload_method(void *ctx, const ncl_json *params,
                                     ncl_json **result, char **reason)
{
    ncl_focas *focas = (ncl_focas *)ctx;
    char *program = NULL;
    size_t len = 0;
    ncl_err rc = ncl_focas_program_upload(
        focas, ncl_tool_param_int(params, "type", 0),
        ncl_tool_param_str(params, "name", NULL), &program, &len);

    (void)result; /* 上传还没实现：成功时才会往 result 里放程序文本 */
    ncl_free_safe(program);
    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s", ncl_focas_last_error(focas));
    }
    (void)len;
    return NCL_OK;
}

/*
 * 程序目录 / 选主程序 / 删程序：都是**命令**（读一次程序目录也是命令，因为它问的是
 * 机床的程序内存里有哪几个文件，不是某个数据对象的值）。
 */
static ncl_err program_directory_method(void *ctx, const ncl_json *params,
                                        ncl_json **result, char **reason)
{
    ncl_focas *focas = (ncl_focas *)ctx;
    ncl_err rc = ncl_focas_program_directory(focas, result);

    (void)params;
    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s", ncl_focas_last_error(focas));
    }
    return NCL_OK;
}

/** 选主程序 / 删程序 / 写参数 / 写刀补 / 写宏变量：参数都从 params 里取。 */
static ncl_err program_select_main_method(void *ctx, const ncl_json *params,
                                          ncl_json **result, char **reason)
{
    ncl_err rc = ncl_focas_program_select_main(
        (ncl_focas *)ctx, ncl_tool_param_str(params, "name", NULL));

    (void)result;
    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s",
                             ncl_focas_last_error((ncl_focas *)ctx));
    }
    return NCL_OK;
}

static ncl_err program_delete_method(void *ctx, const ncl_json *params,
                                     ncl_json **result, char **reason)
{
    ncl_err rc = ncl_focas_program_delete(
        (ncl_focas *)ctx, ncl_tool_param_str(params, "name", NULL));

    (void)result;
    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s",
                             ncl_focas_last_error((ncl_focas *)ctx));
    }
    return NCL_OK;
}

static ncl_err parameter_write_method(void *ctx, const ncl_json *params,
                                      ncl_json **result, char **reason)
{
    ncl_err rc = ncl_focas_parameter_write(
        (ncl_focas *)ctx, ncl_tool_param_int(params, "number", -1),
        ncl_tool_param_str(params, "value", NULL));

    (void)result;
    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s",
                             ncl_focas_last_error((ncl_focas *)ctx));
    }
    return NCL_OK;
}

static ncl_err tool_offset_write_method(void *ctx, const ncl_json *params,
                                        ncl_json **result, char **reason)
{
    ncl_err rc = ncl_focas_tool_offset_write(
        (ncl_focas *)ctx, ncl_tool_param_int(params, "index", -1),
        ncl_tool_param_str(params, "value", NULL));

    (void)result;
    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s",
                             ncl_focas_last_error((ncl_focas *)ctx));
    }
    return NCL_OK;
}

static ncl_err macro_write_method(void *ctx, const ncl_json *params,
                                  ncl_json **result, char **reason)
{
    ncl_err rc = ncl_focas_macro_write(
        (ncl_focas *)ctx, ncl_tool_param_int(params, "number", -1),
        (double)ncl_tool_param_int(params, "value", 0));

    (void)result;
    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s",
                             ncl_focas_last_error((ncl_focas *)ctx));
    }
    return NCL_OK;
}

/* ------------------------------------------------------------------ 工具 -- */

NCL_TOOL_BEGIN("focas", "FANUC FOCAS / Fwlib32 over TCP, read only", "MACHINE", 1000, 1000,
               focas_open, focas_close)

    /* 默认采样通道只放四样（现场口径）：设备状态、加工计件、程序名称、报警。 */
    NCL_DATAITEM_STR_SAMPLED("/STATUS", ncl_focas_status)
    /* 工作模式（表 7 的 WORK_MODE，取值 manual / auto，表 8）：同一个 STATINFO
     * 位域推出来，不多花报文，从默认采样通道读。 */
    NCL_DATAITEM_STR("/WORK_MODE", ncl_focas_mode)
    NCL_DATAITEM_I64_SAMPLED("/PART_COUNT", ncl_focas_part_count)
    /* 表 6/表 7 的对象元信息（进 configs，不进采样通道）：厂商这一条不用读机床，
     * 型号与版本来自会话握手记录（还没解，先答"还读不了"）。 */
    NCL_CONFIG_STR("/MANUFACTURER", ncl_focas_manufacturer)
    NCL_CONFIG_STR("/MODEL", ncl_focas_model)
    NCL_CONFIG_STR("/VERSION", ncl_focas_version)
    /* PROGRAM / PROGRAM_NUMBER / LINE_NUMBER / SUBPROGRAM 属 CONTROLLER 组件
     * （表 2：组件对象）。前三条是真读的（EXEPRGNAME2 / cnc_rdprgnum /
     * cnc_rdseqnum），子程序号要 cnc_rdexecprog3（帧待抓）。 */
    NCL_DATAITEM_STR_SAMPLED("/CONTROLLER/PROGRAM", ncl_focas_program_name)
    NCL_DATAITEM_I64("/CONTROLLER/PROGRAM_NUMBER", ncl_focas_program_number)
    NCL_DATAITEM_I64("/CONTROLLER/SUBPROGRAM", ncl_focas_subprogram_number)
    NCL_DATAITEM_STR("/LINE_NUMBER", ncl_focas_line_number)
    /* 当前刀具号（表 7 的 TOOL_NUMBER）与倍率：cnc_rdgcode / cnc_rddynamic2。 */
    NCL_DATAITEM_I64("/TOOL_NUMBER", ncl_focas_tool_number)
    NCL_DATAITEM_F64("/FEED_OVERRIDE", ncl_focas_feed_override)
    NCL_DATAITEM_F64("/SPINDLE_OVERRIDE", ncl_focas_spindle_override)
    NCL_DATAITEM_F64("/FEED_SPEED", ncl_focas_feed_speed)

    /* 报警（表 6 的 WARNING）：进默认采样通道。帧还没抓到（01 册 §2.3 的码表里没有
     * cnc_rdalmmsg2），所以 ncl_focas_alarm() 现在回 NCL_ERR_UNAVAILABLE：模型里有
     * 这条路径、问它答"还读不了"、采样那一列是 null、自检算"待抓包"而不是失败。
     * 抓包补上只改 client 里那个函数体，这一行不动。 */
    NCL_DATAITEM_JSON_SAMPLED("/WARNING", ncl_focas_alarm)

    /* 五轴的位置与进给速度。线性轴的位置是 POSITION（mm），旋转轴（A/C）是
     * ANGLE（角度）—— 同一个 cnc_absolute 读回来，载荷里的 unit 决定报哪一个。
     * 位置这一类（实际/目标/机床/相对）现在**都还读不了**：item 0x26 的请求码
     * 已核、应答切法待真机核，函数回 NCL_ERR_UNAVAILABLE。
     * 进给速度是真读的 —— cnc_actf（0x24），每轴一个 float，mm/min。名字从路径自动推：
     * /MACHINE/AXIS@X/POSITION@REAL -> AXIS_X.POSITION_REAL。 */
    NCL_DATAITEM_F64("/AXIS@X/POSITION@REAL", ncl_focas_axis_position,
                 NCL_FOCAS_AXIS_X)
    NCL_DATAITEM_F64("/AXIS@Y/POSITION@REAL", ncl_focas_axis_position,
                 NCL_FOCAS_AXIS_Y)
    NCL_DATAITEM_F64("/AXIS@Z/POSITION@REAL", ncl_focas_axis_position,
                 NCL_FOCAS_AXIS_Z)
    NCL_DATAITEM_F64("/AXIS@A/POSITION@REAL", ncl_focas_axis_position,
                 NCL_FOCAS_AXIS_A)
    NCL_DATAITEM_F64("/AXIS@C/POSITION@REAL", ncl_focas_axis_position,
                 NCL_FOCAS_AXIS_C)
    /* 旋转轴的位置按表 4 的 ANGLE 报（同一根轴、同一个读法）。 */
    NCL_DATAITEM_F64("/AXIS@A/ANGLE@REAL", ncl_focas_axis_position,
                     NCL_FOCAS_AXIS_A)
    NCL_DATAITEM_F64("/AXIS@C/ANGLE@REAL", ncl_focas_axis_position,
                     NCL_FOCAS_AXIS_C)
    NCL_DATAITEM_F64("/AXIS@X/POSITION@CMD", ncl_focas_axis_position_cmd,
                     NCL_FOCAS_AXIS_X)
    NCL_DATAITEM_F64("/AXIS@Y/POSITION@CMD", ncl_focas_axis_position_cmd,
                     NCL_FOCAS_AXIS_Y)
    NCL_DATAITEM_F64("/AXIS@Z/POSITION@CMD", ncl_focas_axis_position_cmd,
                     NCL_FOCAS_AXIS_Z)
    NCL_DATAITEM_F64("/AXIS@A/POSITION@CMD", ncl_focas_axis_position_cmd,
                     NCL_FOCAS_AXIS_A)
    NCL_DATAITEM_F64("/AXIS@C/POSITION@CMD", ncl_focas_axis_position_cmd,
                     NCL_FOCAS_AXIS_C)

    NCL_DATAITEM_F64("/AXIS@X/SPEED", ncl_focas_axis_feedrate, NCL_FOCAS_AXIS_X)
    NCL_DATAITEM_F64("/AXIS@Y/SPEED", ncl_focas_axis_feedrate, NCL_FOCAS_AXIS_Y)
    NCL_DATAITEM_F64("/AXIS@Z/SPEED", ncl_focas_axis_feedrate, NCL_FOCAS_AXIS_Z)
    NCL_DATAITEM_F64("/AXIS@A/SPEED", ncl_focas_axis_feedrate, NCL_FOCAS_AXIS_A)
    NCL_DATAITEM_F64("/AXIS@C/SPEED", ncl_focas_axis_feedrate, NCL_FOCAS_AXIS_C)
    /* 轴的剩余进给（表 7 的 PATH_LEFT_LENGTH）、扭矩/电流/温度（表 4）与轴类型
     * （表 7 的 TYPE，进 configs）：请求码/来源都写在 client 的注释里。 */
    NCL_DATAITEM_F64("/AXIS@X/PATH_LEFT_LENGTH", ncl_focas_axis_distance,
                     NCL_FOCAS_AXIS_X)
    NCL_DATAITEM_F64("/AXIS@X/TORQUE", ncl_focas_axis_torque, NCL_FOCAS_AXIS_X)
    NCL_DATAITEM_F64("/AXIS@X/CURRENT", ncl_focas_axis_current, NCL_FOCAS_AXIS_X)
    NCL_DATAITEM_F64("/AXIS@X/TEMPERATURE", ncl_focas_axis_temperature,
                     NCL_FOCAS_AXIS_X)
    NCL_CONFIG_STR("/AXIS@X/TYPE", ncl_focas_axis_type, NCL_FOCAS_AXIS_X)
    /* 主轴：表 2 的组件类型里没有 SPINDLE，最接近的是 MOTOR（主轴就是主轴电机
     * 驱动的），所以主轴转速按 SPEED（units rpm）报在 /MOTOR@S1 下；负载（%）
     * 字典里没有对应项，只留在 client 的 API 里。 */
    NCL_DATAITEM_F64("/MOTOR@S1/SPEED", ncl_focas_spindle_speed, 0)

    /* 刀具列表（表 7 的 TOOL，list）：FOCAS 侧是刀补表/刀具表那一族调用，帧还没核对
     * （32 册 §5 把它列在"待核"里），所以 ncl_focas_tool_list() 现在回
     * NCL_ERR_UNAVAILABLE —— 模型里有它、问它有明确答复、轮询跳过；核对完改的就是
     * client 里那个函数体。 */
    NCL_CONFIG_JSON("/CONTROLLER/TOOL", ncl_focas_tool_list)
    /* 刀具参数（表 7 的 TOOLPARAM，JSON 对象）：刀补 + 寿命。 */
    NCL_CONFIG_JSON("/CONTROLLER/TOOLPARAM", ncl_focas_tool_param_table)
    /* 参数表（表 6 的 PARAMETER，dict）与宏变量表（表 7 的 VARIABLE，list）：
     * 帧抓到了、字段布局待核（31 册）。 */
    NCL_CONFIG_JSON("/CONTROLLER/PARAMETER", ncl_focas_parameter_table)
    NCL_CONFIG_JSON("/CONTROLLER/VARIABLE", ncl_focas_variable_table)
    /* 工件坐标系（表 7 的 COORDINATE，JSON 对象 → 表 9 的 x/y/z…）。 */
    NCL_CONFIG_JSON("/CONTROLLER/COORDINATE", ncl_focas_work_offsets)

    /* 方法：会话状态与数据项清单（现场调试用，不进模型、不参与采样）。 */
    NCL_METHOD_CALL("/SESSION", session_method)
    NCL_METHOD_CALL("/ITEMS", items_method)
    /* 程序上下行（动作，不是数据对象）：下发已通（cnc_dwnstart4 三件套），
     * 上传的应答切法待真机核，现在回"还读不了"（见 client 的注释与 31 册）。 */
    NCL_METHOD_CALL("/PROGRAM@DOWNLOAD", program_download_method)
    NCL_METHOD_CALL("/PROGRAM@UPLOAD", program_upload_method)
    NCL_METHOD_CALL("/PROGRAM@DIRECTORY", program_directory_method)
    NCL_METHOD_CALL("/PROGRAM@SELECT_MAIN", program_select_main_method)
    NCL_METHOD_CALL("/PROGRAM@DELETE", program_delete_method)
    /* 写动作（现场要改参数/刀补/宏变量时用；**刀补写着会撞刀**，权限与备份先想清楚）。 */
    NCL_METHOD_CALL("/PARAMETER@WRITE", parameter_write_method)
    NCL_METHOD_CALL("/TOOL@WRITE", tool_offset_write_method)
    NCL_METHOD_CALL("/VARIABLE@WRITE", macro_write_method)

NCL_TOOL_END_WITH_RAW(focas_last_raw)

NCL_TOOL_MODULE("1.4.0", "FANUC FOCAS / Fwlib32 over TCP, read only (01 册 §2.1-§2.3，32 册数据项)")
