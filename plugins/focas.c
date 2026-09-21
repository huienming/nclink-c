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

#include <string.h>

#include "nclink/clients/focas.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_json.h"

/* 文件处理最后一段的实现（定义在下面），open() 里要它们的地址。 */
static ncl_err focas_file_push(void *user, const char *name, const char *path,
                               char **reason);
static ncl_err focas_file_pull(void *user, const char *name, const char *path,
                               char **reason);
static ncl_err focas_file_remove(void *user, const char *name, char **reason);

/* ---------------------------------------------------------------- 连接 ---- */

/**
 * 把配置里的 parameters 交给 client。会话在第一次读的时候才建，所以机床没开机
 * 不影响设备程序启动：谁问值，谁拿到一条明确的"读不到"。
 */
static void *focas_open(const ncl_json *params, char **err)
{
    /* 注册进文件工具的那份要活得比这次 open 长，所以放静态：一个进程一台机床。 */
    static ncl_file_backend backend;
    ncl_focas_config config;
    ncl_focas *focas;

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
    focas = ncl_focas_open(&config, err);
    if (focas == NULL) {
        return NULL;
    }
    /* 文件处理的最后一段（adapter → 机床）：把 FOCAS 的程序上下行交给文件工具，
     * 设备的 `/CONTROLLER/FILE` 从此多走这一段（见 nclink/ncl_file.h）。 */
    memset(&backend, 0, sizeof(backend));
    backend.user = focas;
    backend.push = focas_file_push;
    backend.pull = focas_file_pull;
    backend.remove = focas_file_remove;
    (void)ncl_file_tool_set_backend(&backend);
    return focas;
}

static void focas_close(void *ctx)
{
    /* 最后一段随连接一起撤（文件工具退回"只到本地目录"）。 */
    (void)ncl_file_tool_set_backend(NULL);
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

/* ------------------------------------------------------ 文件处理的最后一段 -- */

/*
 * client → adapter → 机床 这条链的**最后一段**：文件在设备本地落地之后，由这里用
 * FOCAS 把它送进机床（`cnc_dwnstart4` 三件套），或从机床取回来（`cnc_upstart4`
 * 三件套）、删掉机床上的那份。注册进文件工具（`ncl_file_tool_set_backend()`），
 * 于是 `/CONTROLLER/FILE` 的 write / read / delete 就多走这一段（见 ncl_file.h）。
 *
 * 帧与体长按官方 SDK 实测（01 册 §2.4）；上行（取回）的应答切法还没核，所以
 * pull 现在会明确回"还读不了"（NCL_ERR_UNAVAILABLE）。
 */
static ncl_err focas_file_push(void *user, const char *name, const char *path,
                               char **reason)
{
    ncl_focas *focas = (ncl_focas *)user;
    char *bytes = NULL;
    ncl_err rc;

    rc = ncl_file_read_all(path, &bytes, NULL);
    if (rc != NCL_OK) {
        if (reason != NULL) {
            *reason = ncl_strdup("读本地文件失败");
        }
        return rc;
    }
    rc = ncl_focas_program_download(focas, 0, name, bytes);
    ncl_free_safe(bytes);
    if (rc != NCL_OK && reason != NULL) {
        *reason = ncl_strdup(ncl_focas_last_error(focas));
    }
    return rc;
}

static ncl_err focas_file_pull(void *user, const char *name, const char *path,
                               char **reason)
{
    ncl_focas *focas = (ncl_focas *)user;
    char *bytes = NULL;
    size_t len = 0;
    ncl_err rc = ncl_focas_program_upload(focas, 0, name, &bytes, &len);

    (void)path;
    ncl_free_safe(bytes);
    if (rc != NCL_OK && reason != NULL) {
        *reason = ncl_strdup(ncl_focas_last_error(focas));
    }
    return rc;
}

static ncl_err focas_file_remove(void *user, const char *name, char **reason)
{
    ncl_focas *focas = (ncl_focas *)user;
    ncl_err rc = ncl_focas_program_delete(focas, name);

    if (rc != NCL_OK && reason != NULL) {
        *reason = ncl_strdup(ncl_focas_last_error(focas));
    }
    return rc;
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
    /* 表 6/表 7 的对象元信息（进 configs，不进采样通道）：厂商这一条不用读机床；
     * 型号与版本来自会话探针那条 `code 24` 的 ODBSYS（真机实测，01 册 §2.8）——
     * 本机出门是 "0M D4G3" / "28.0"。 */
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
    /* 当前刀具号（表 7 的 TOOL_NUMBER）：**还没找到可靠来源**（模态那条 0x96 只报
     * G 组；cnc_rdexecprog 在这台机器上回的是整段程序，里面 T 码出现多次），所以
     * 这一格如实答"读不到"，不编数（01 册 §2.8.5）。
     * 倍率在操作面板信号（0x5d）里：进给倍率真机读得到；**主轴倍率那一格现代系列
     * 没有**，且这台机器的 0x5d 载荷是桩（32 字节里除 @2=0xffff 全是 0）。 */
    NCL_DATAITEM_I64("/TOOL_NUMBER", ncl_focas_tool_number)
    NCL_DATAITEM_F64("/FEED_OVERRIDE", ncl_focas_feed_override)
    NCL_DATAITEM_F64("/SPINDLE_OVERRIDE", ncl_focas_spindle_override)
    NCL_DATAITEM_F64("/FEED_SPEED", ncl_focas_feed_speed)

    /* 报警（表 6 的 WARNING）：进默认采样通道。真机实测（01 册 §2.8.2）：没报警时
     * 机床回 0 字节载荷 → 这里给**空数组**；有报警时给 `{"number":75,"type":3,
     * "text":"保护"}`（机床的文本是 GB2312，client 出门前过 ncl_gb2312_to_utf8）。
     * 注意 `0x23` 那两个暗格（arg2=2 / arg3=64）不填就永远拿不到文本。 */
    NCL_DATAITEM_JSON_SAMPLED("/WARNING", ncl_focas_alarm)

    /* 五轴的位置与进给速度。线性轴的位置是 POSITION（mm），旋转轴（A/C）是
     * ANGLE（角度）。位置这一类都是**真读**的（真机实测，01 册 §2.8.1）：
     *   @REAL  实际位置 —— cnc_rdposition（一条 8 块，下标 1 = 绝对），每轴一条
     *          **8 字节记录**：data(BE32)@0 + dec(BE16)@6，值 = data / 10^dec；
     *   @CMD   指令位置 —— 现场口径"跟踪误差 = 实际 − 指令"，所以指令 = 实际 −
     *          cnc_srvdelay（0x26 d=9）；机床静止时两条相等。
     * 进给速度也是真读的 —— cnc_actf（0x24，每轴一条 8 字节记录），mm/min。
     * 名字从路径自动推：/MACHINE/AXIS@X/POSITION@REAL -> AXIS_X.POSITION_REAL。 */
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

    /* 刀具列表（表 7 的 TOOL，list）：真机上 cnc_rdtooldata rc=1、cnc_rdtoolrng rc=3
     * —— **机床不提供**（官方 SDK 同样被拒，01 册 §2.8.6），所以这一格答"读不到"。 */
    NCL_CONFIG_JSON("/CONTROLLER/TOOL", ncl_focas_tool_list)
    /* 刀具参数（表 7 的 TOOLPARAM，JSON 对象）：刀补 + 寿命。 */
    NCL_CONFIG_JSON("/CONTROLLER/TOOLPARAM", ncl_focas_tool_param_table)
    /* 参数表（表 6 的 PARAMETER，dict）与宏变量表（表 7 的 VARIABLE，list）：
     * **单条**读得到（`cnc_rdparam` 0x8d / `cnc_rdmacro` 0x15，见 client），整表的
     * 范围调用（rdparanum/rdparar/rdmacror）在这台机器上被拒（§2.8.6）——哪天有机器
     * 支持整表，范围从 rdtofsinfo/rdmacroinfo 拿（§2.8.7）。 */
    NCL_CONFIG_JSON("/CONTROLLER/PARAMETER", ncl_focas_parameter_table)
    NCL_CONFIG_JSON("/CONTROLLER/VARIABLE", ncl_focas_variable_table)
    /* 工件坐标系（表 7 的 COORDINATE，JSON 对象 → 表 9 的 x/y/z…）：
     * `cnc_rdwkcdshft` type 0..20 全试过，这台机器一律 rc=1 —— 机床不提供。 */
    NCL_CONFIG_JSON("/CONTROLLER/COORDINATE", ncl_focas_work_offsets)

    /* 方法：会话状态与数据项清单（现场调试用，不进模型、不参与采样）。 */
    NCL_METHOD_CALL("/SESSION", session_method)
    NCL_METHOD_CALL("/ITEMS", items_method)

    /*
     * **文件/程序不在这里声明。** 文件的门面只有一个：文件工具声明的
     * `/CONTROLLER/FILE`（dict，进 configs），它的操作走标准那一套（`get_value` /
     * `get_attributes` / `get_keys` / `add` / `delete` / `call`），**传输的最后
     * 一段（adapter → 机床）**才由这份工具的 client 用 FOCAS 去搬
     * （`cnc_dwnstart4` 三件套一族 / `cnc_upstart4` 一族，见 clients/focas）。
     * 给 FOCAS 工具另开 `/PROGRAM@DOWNLOAD` 这种方法，等于把"文件"做成两套流程
     * —— 现场那家的门面是 `/CONTROLLER/{CONSOLE,FILE,PROGRAM_DATA}`，没有一条
     * 挂在设备节点上。
     */

NCL_TOOL_END_WITH_RAW(focas_last_raw)

NCL_TOOL_MODULE("1.5.0", "FANUC FOCAS / Fwlib32 over TCP, read only (会话与报文形状按真机实测：01 册 §2.8；点位对照 32 册数据项)")
