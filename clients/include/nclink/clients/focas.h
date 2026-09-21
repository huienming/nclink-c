/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * FANUC FOCAS / Fwlib32 - 现场接口。
 *
 * 这一个头就是接一台 FANUC 机床要读的全部：ncl_focas_open() 拿一个会话，然后
 * ncl_focas_status() / ncl_focas_part_count() / ncl_focas_axis_position() … 这些
 * 名字就是它们读回来的东西。"哪个 item、哪一块、怎么由位域推成三态"这类知识都写在
 * client 里（clients/focas/focas_values.c），适配器（plugins/focas.c）只把函数绑到
 * 模型路径上。
 *
 * 协议层（PDU 帧、命令块、回复块、item 码表、raw 逃逸口）在
 * clients/focas/ncl_focas_pdu.h —— 写 client 的人、查抓包的人才需要读它。
 */
#ifndef NCL_FOCAS_H
#define NCL_FOCAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================== 现场接口（语义）== */

/*
 *      ncl_focas_config config;
 *      ncl_focas_config_default(&config);
 *      config.host = "192.168.1.100";
 *
 *      char *err = NULL;
 *      ncl_focas *focas = ncl_focas_open(&config, &err);
 *      if (focas == NULL) { ... err ... }
 *
 *      char state[32];
 *      if (ncl_focas_status(focas, state, sizeof(state)) == NCL_OK) { ... }
 *
 * 约定：
 *   - open() 不连机床：会话在第一次读时建立，所以机床没开机不影响设备程序启动；
 *   - 这些函数可以并发调用（内部串行化），采样通道与 REST 请求会同时用它们；
 *   - 失败返回 ncl_err，原因用 ncl_focas_last_error() 取（一句话，可以直接当
 *     NC-Link 应答里的 reason）；
 *   - 文本出参自己截断并保证 NUL 结尾。
 */
typedef struct ncl_focas ncl_focas;

/** 连接参数，和配置里的 "parameters" 一一对应；host 必填，其余有默认值。 */
typedef struct {
    const char *host;
    unsigned    port;               /**< 默认 8193（FOCAS over Ethernet）    */
    unsigned    timeout_ms;         /**< 一次请求的超时                       */
    unsigned    connect_timeout_ms;
    unsigned    retries;
    bool        negotiate;          /**< 先走 hello 再进命令模式（默认开）    */
} ncl_focas_config;

/** 轴序：与模型里 /MACHINE/AXIS@<轴>/... 的顺序一致。 */
typedef enum {
    NCL_FOCAS_AXIS_X = 0,
    NCL_FOCAS_AXIS_Y,
    NCL_FOCAS_AXIS_Z,
    NCL_FOCAS_AXIS_A,
    NCL_FOCAS_AXIS_C,
    NCL_FOCAS_AXIS_COUNT
} ncl_focas_axis;

/**
 * 主轴号的个数上限：`cnc_acts` / `cnc_rdspmeter` 按**主轴**编号取（不是轴号）。
 * FOCAS 的 MAX_SPINDLE 随系列不同（4 或 8），这里按 8 留够。
 */
#define NCL_FOCAS_SPINDLE_MAX 8

/** 填上默认值（host 留空，其余是驱动自己的默认）。 */
void ncl_focas_config_default(ncl_focas_config *config);

ncl_focas *ncl_focas_open(const ncl_focas_config *config, char **err);
void       ncl_focas_close(ncl_focas *focas);

/** 会话现在是通着的吗（读过一次之后才有意义）。 */
bool       ncl_focas_connected(const ncl_focas *focas);
/** 上一次失败的一句话原因；没失败过就是空串。 */
const char *ncl_focas_last_error(const ncl_focas *focas);

/* 语义：名字就是读回来的东西 ------------------------------------------------- */

/*
 * 每个函数对应一个 FOCAS 调用（注释里给的是 Fwlib64.h 的函数名与线上 item 码）。
 * 这些码与参数是按 FANUC 官方手册与 SDK 逐条核出来的（见 tools/site-probe/
 * focas_sdk_probe.*、01 册 §2.3）。
 *
 * **还没实现的调用回 NCL_ERR_UNAVAILABLE**：函数照样摆在这里、照样能绑到模型路径
 * 上 —— 那个点位在模型里看得见、客户端问它答"还读不了（UnavailableException）"、
 * 轮询与 §6 审计都不碰它、自检把它算成"待抓包"而不是失败（见 ncl_common.h 里这个
 * 码）。要抓哪一帧写在各自的注释里，ncl_focas_last_error() 里也带一句。抓包补上
 * 之后**改的就是那个函数的函数体**，适配器的点位表一行都不用动。
 */

/* 状态与模式 ---------------------------------------------------------------- */

/** 设备状态，标准的三态："running" / "free" / "holding"（`cnc_statinfo`，ODBST 位域）。 */
ncl_err ncl_focas_status(ncl_focas *focas, char *out, size_t cap);
/** 工作模式："auto" / "manual" / "other"（同一个 ODBST，aut / manual 两位）。 */
ncl_err ncl_focas_mode(ncl_focas *focas, char *out, size_t cap);
/** 急停位（ODBST.emergency）。 */
ncl_err ncl_focas_emergency(ncl_focas *focas, bool *on);

/* 报警 --------------------------------------------------------------------- */

/**
 * 报警状态位（`cnc_alarm2`，item 0x1a）：0 = 无报警；非 0 的每一位是哪一类报警，
 * 见 FOCAS 手册（P/S、OT、SV、IO、SP、MC、PC、EX…）。
 */
ncl_err ncl_focas_alarm_status(ncl_focas *focas, long long *bits);
/**
 * 报警消息（表 6 的 WARNING：{"number","text"}）。
 *
 * **还没实现**（帧待抓包）：要抓 `cnc_rdalmmsg2`（item 0x23，d = 报警类型、
 * e = 条数；应答按 ODBALMMSG2 数组切）。
 */
ncl_err ncl_focas_alarm(ncl_focas *focas, ncl_json **value);

/* 轴与主轴 ----------------------------------------------------------------- */

/**
 * 轴的实际进给速度 F（`cnc_actf`，item 0x24）：每轴一个 float（mm/min）。
 * 载荷里从第 `axis * 4` 字节开始就是这一根轴的值。
 */
ncl_err ncl_focas_axis_feedrate(ncl_focas *focas, ncl_focas_axis axis,
                                double *value);
/**
 * 主轴的实际转速 S（`cnc_acts`，item 0x25）：每个主轴一个 float（rpm）。
 * 注意是**主轴**号，不是轴号。
 */
ncl_err ncl_focas_spindle_speed(ncl_focas *focas, unsigned spindle,
                                double *value);
/**
 * 轴的绝对位置。走 `cnc_rdposition`（item `RDPOSITION`：一条请求 9 个块，绝对位置在
 * **下标 1**（第 2 个块，块号从 0 数）），每轴一个 `POSELM`（12 字节），值 =
 * `data / 10^dec`——NCGuide 上实测到 `dec = 3`、轴名 'X'（01 册 §2.5）。
 */
ncl_err ncl_focas_axis_position(ncl_focas *focas, ncl_focas_axis axis,
                                double *value);
/** 轴的机械坐标（同一条请求的下标 2）。 */
ncl_err ncl_focas_axis_position_machine(ncl_focas *focas, ncl_focas_axis axis,
                                        double *value);
/** 轴的相对坐标（下标 3）。 */
ncl_err ncl_focas_axis_position_relative(ncl_focas *focas, ncl_focas_axis axis,
                                         double *value);
/** 轴的剩余距离（下标 4）。 */
ncl_err ncl_focas_axis_distance(ncl_focas *focas, ncl_focas_axis axis,
                                double *value);
/**
 * 跟踪误差（= 伺服延迟量，`cnc_srvdelay`，item `SV_DELAY`：一条 `0x26`、d = 9、
 * e = ALL_AXES）。记录里带小数位，出门是 mm；机床静止时是 0。
 */
ncl_err ncl_focas_axis_srv_delay(ncl_focas *focas, ncl_focas_axis axis,
                                 double *value);
/**
 * 轴的目标位置（指令位置）= **实际位置 − 跟踪误差**。
 *
 * 现场口径是"跟踪误差 = 实际位置 − 指令位置"，所以指令位置是算得出来的：实际位置走
 * `cnc_rdposition`、跟踪误差走 `cnc_srvdelay`，两条相减（不是拿 `cnc_getfigure`
 * 那一套凑的）。静止时机床两个量相等，出门就是实际位置。
 */
ncl_err ncl_focas_axis_position_cmd(ncl_focas *focas, ncl_focas_axis axis,
                                    double *value);
/** 轴的伺服负载（`cnc_rdsvmeter`，item 0x56 + 0x89）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_axis_load(ncl_focas *focas, ncl_focas_axis axis,
                            double *value);
/** 主轴的负载与转速（`cnc_rdspmeter`，item 0x40：d=4 负载 / d=5 转速）。**还没实现**。 */
ncl_err ncl_focas_spindle_load(ncl_focas *focas, unsigned spindle,
                               double *value);
/** 轴的扭矩（`cnc_loadtorq`，ODBLOAD 数组）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_axis_torque(ncl_focas *focas, ncl_focas_axis axis,
                              double *value);
/** 轴的电流（`cnc_rdaxisdata` 的一类数据）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_axis_current(ncl_focas *focas, ncl_focas_axis axis,
                               double *value);
/** 轴的伺服温度（`cnc_rdaxisdata` 的一类数据）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_axis_temperature(ncl_focas *focas, ncl_focas_axis axis,
                                   double *value);
/** 轴的种类（linear / rotary，表 7 的 TYPE）：**还没实现**，要读 `cnc_rdaxisname` /
 *  `cnc_rdaxisdata` 的轴属性。 */
ncl_err ncl_focas_axis_type(ncl_focas *focas, ncl_focas_axis axis,
                            char *out, size_t cap);
/** 合成进给速度（`cnc_rddynamic2` 的 DBDY2）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_feed_speed(ncl_focas *focas, double *value);
/** 进给倍率（`cnc_rddynamic2`，ODBDY2.feed_override）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_feed_override(ncl_focas *focas, double *value);
/** 主轴倍率（`cnc_rddynamic2`，ODBDY2.spindle_override）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_spindle_override(ncl_focas *focas, double *value);

/* 程序 --------------------------------------------------------------------- */

/** 当前主程序名（`cnc_exeprgname2`，item 0xfc）。 */
ncl_err ncl_focas_program_name(ncl_focas *focas, char *out, size_t cap);
/** 运行中的程序号（`cnc_rdprgnum`，item 0x1c，载荷 @2 的 BE16）。表 7 的 PROGRAM_NUMBER。 */
ncl_err ncl_focas_program_number(ncl_focas *focas, long long *value);
/** 主程序号（同一条应答的 @6）。 */
ncl_err ncl_focas_main_program_number(ncl_focas *focas, long long *value);
/** 子程序号（`cnc_rdexecprog3`，ODBEXEPRGINFO）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_subprogram_number(ncl_focas *focas, long long *value);
/**
 * 当前程序行号（`cnc_rdseqnum`，item 0x1d，载荷 @0 的 BE32），文本形式 ——
 * 表 7 的 LINE_NUMBER 是 string，所以这里直接给字符串（例如 "N1234"）。
 */
ncl_err ncl_focas_line_number(ncl_focas *focas, char *out, size_t cap);
/** 正在执行的程序段（`cnc_rdexecprog`）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_executed_block(ncl_focas *focas, char *out, size_t cap);
/** 程序目录（`cnc_rdprogdir3`，item 0x06，d = 0x13）。**还没实现**（帧待核对）。 */
ncl_err ncl_focas_program_directory(ncl_focas *focas, ncl_json **value);
/** 当前刀具号（模态 T 码，`cnc_rdgcode`）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_tool_number(ncl_focas *focas, long long *value);

/* 计数与计时 --------------------------------------------------------------- */

/** 加工件数（`cnc_rdcount`，item 0x8b，d = e = 0；表 7 的 PART_COUNT）。 */
ncl_err ncl_focas_part_count(ncl_focas *focas, long long *value);
/** 刀具组数（`cnc_rdngrp`，item 0x4a，载荷 @0 的 BE32）。 */
ncl_err ncl_focas_tool_group_count(ncl_focas *focas, long long *value);

/** `cnc_rdtimer` 的 type：先看哪一个时钟。 */
typedef enum {
    NCL_FOCAS_TIMER_POWER_ON = 0,  /**< 通电时间                       */
    NCL_FOCAS_TIMER_OPERATING = 1, /**< 运行时间（自动运行）           */
    NCL_FOCAS_TIMER_CUTTING = 2,   /**< 切削时间                       */
    NCL_FOCAS_TIMER_CYCLE = 3,     /**< 循环时间                       */
    NCL_FOCAS_TIMER_FREE = 4       /**< 自由用途                       */
} ncl_focas_timer_kind;

/**
 * 机床的时钟（`cnc_rdtimer`，item 0x120）：载荷 @0 = 分钟、@4 = 毫秒（都是 BE32）。
 * *seconds 收到的是**合计秒数**（分钟 × 60 + 毫秒 / 1000）。
 */
ncl_err ncl_focas_timer(ncl_focas *focas, ncl_focas_timer_kind kind,
                        long long *seconds);

/* 刀具、参数、工件坐标（都还没抓到帧）-------------------------------------- */

/** 刀具表（表 7 的 TOOL，list）：**还没实现**，要抓 `cnc_rdtooldata` / `cnc_rdtoolrng`。 */
ncl_err ncl_focas_tool_list(ncl_focas *focas, ncl_json **value);
/** 一条刀补（`cnc_rdtofs`，item 0x08）。**还没实现**（帧待核对：形状/磨损 × 长度/半径）。 */
ncl_err ncl_focas_tool_offset(ncl_focas *focas, long long index,
                              ncl_json **value);
/** 刀具寿命计数（`cnc_rdlife`，item 0x8b，d = e = 1）。**还没实现**（载荷待核）。 */
ncl_err ncl_focas_tool_life(ncl_focas *focas, long long group,
                            long long *value);
/** 一个用户宏变量（`cnc_rdmacro`，item 0x15）。**还没实现**（帧待核对）。 */
ncl_err ncl_focas_macro_variable(ncl_focas *focas, long long number,
                                 ncl_json **value);
/** 一段宏变量（`cnc_rdmacror`）：表 7 的 VARIABLE（list）就是它。**还没实现**（帧待核对）。 */
ncl_err ncl_focas_macro_variables(ncl_focas *focas, long long first,
                                  long long count, ncl_json **value);
/** 一个 CNC 参数（`cnc_rdparam`，item 0x0e）。**还没实现**（帧待核对）。 */
ncl_err ncl_focas_parameter(ncl_focas *focas, long long number,
                            ncl_json **value);
/** 一套刀具参数（表 7 的 TOOLPARAM）：刀补 `cnc_rdtofs` + 寿命 `cnc_rdlife` 拼出来。
 *  **还没实现**（帧待核对）。 */
ncl_err ncl_focas_tool_param(ncl_focas *focas, long long index,
                             ncl_json **value);
/** 整张刀具参数表（表 7 的 TOOLPARAM，JSON 对象）：刀补 + 寿命逐条拼。
 *  **还没实现**（帧待核对）。 */
ncl_err ncl_focas_tool_param_table(ncl_focas *focas, ncl_json **value);
/** 整张参数表（表 6 的 PARAMETER，dict）：`cnc_rdparanum` + `cnc_rdparar`。
 *  **还没实现**（帧待核对）。 */
ncl_err ncl_focas_parameter_table(ncl_focas *focas, ncl_json **value);
/** 宏变量表（表 7 的 VARIABLE，list）：`cnc_rdmacror` 按段读。
 *  **还没实现**（帧待核对）。 */
ncl_err ncl_focas_variable_table(ncl_focas *focas, ncl_json **value);
/** 工件坐标系（`cnc_rdwkcdshft` 一族，G54…）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_work_offset(ncl_focas *focas, const char *name,
                              ncl_json **value);
/** 整套工件坐标系（表 7 的 COORDINATE，JSON 对象 → 表 9 的 x/y/z…）。
 *  **还没实现**（帧待抓包）。 */
ncl_err ncl_focas_work_offsets(ncl_focas *focas, ncl_json **value);
/** 当前模态（T/B/S/F 等，`cnc_rdgcode`）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_modal(ncl_focas *focas, ncl_json **value);
/** 系统信息（型号/系列/轴数，`cnc_sysinfo`）：**还没实现** —— 这一条的数据在会话
 *  握手（`func 01`/`func 21` 的应答）里，不在数据帧里，要先解那段记录。 */
ncl_err ncl_focas_system(ncl_focas *focas, ncl_json **value);
/** 机床型号（`cnc_rdmodel` / `cnc_sysinfo` 的记录）。**还没实现**。 */
ncl_err ncl_focas_model(ncl_focas *focas, char *out, size_t cap);
/** 系统软件版本（`cnc_sysinfo` 的 series/version）。**还没实现**。 */
ncl_err ncl_focas_version(ncl_focas *focas, char *out, size_t cap);
/** 厂商（表 6 的 MANUFACTURER）：**不用读机床** —— 这一份 client 接的就是 FANUC，
 *  直接回 "FANUC"。 */
ncl_err ncl_focas_manufacturer(ncl_focas *focas, char *out, size_t cap);

/* 程序上下行（不是数据对象，是动作）---------------------------------------- */

/**
 * 把一个 NC 程序（或别的 NC 数据）**下发**给机床：`cnc_dwnstart4` →
 * 分块 `cnc_download4` → `cnc_dwnend4`（帧见 01 册 §2.4，官方 SDK 实测）。
 *
 * @param type     数据种类：0 NC 程序 / 1 刀补 / 2 参数 / 3 螺距误差 /
 *                 4 宏变量 / 5 工件零点偏置（官方手册的取值）
 * @param dir      目标目录或程序名，可 NULL（NC 程序可以给个目标目录）
 * @param program  程序文本（NUL 结尾），内部按 1400 字节一块发
 *
 * 一块发完不等应答（官方库就是这么发的）；**错误在最后那条 end 帧才回**
 * （数据错/内存溢出一类），所以返回 NCL_OK 才算真的落地。中途失败会把机床侧的
 * 传输状态留着，下一次 start 会把它冲掉，但最好别在传输中途放弃。
 */
ncl_err ncl_focas_program_download(ncl_focas *focas, long long type,
                                   const char *dir, const char *program);

/**
 * 把机床上的程序**取回来**（`cnc_upstart4` → `cnc_upload4` → `cnc_upend4`）。
 *
 * **还没实现**：请求码已经核出来了（0x15 / 0x18，数据请求 8 字节体、dir 4），
 * 差的是**应答里程序文本的切法**（SDK 在内部函数里解，2026-09 反汇编到那一层没
 * 再往下；真机抓一次就知道前缀/长度字段在哪）。现在回 NCL_ERR_UNAVAILABLE。
 */
ncl_err ncl_focas_program_upload(ncl_focas *focas, long long type,
                                 const char *name, char **program,
                                 size_t *len);

/**
 * 把机床上的某个程序选成主程序（`cnc_pdf_slctmain`）。**还没实现**（帧待抓包）。
 */
ncl_err ncl_focas_program_select_main(ncl_focas *focas, const char *name);
/**
 * 删掉机床上的某个程序（`cnc_delete` / `cnc_pdf_del`）。**还没实现**（帧待抓包）。
 * 删正在执行的程序机床会拒，这是机床侧的保护。
 */
ncl_err ncl_focas_program_delete(ncl_focas *focas, const char *name);
/**
 * 写一个 CNC 参数（`cnc_wrparam`）。**还没实现**（帧待抓包），而且**风险高**：
 * 参数写错会让机床行为不对，站点用之前先确认权限与备份。
 */
ncl_err ncl_focas_parameter_write(ncl_focas *focas, long long number,
                                  const char *value);
/** 写一条刀补（`cnc_wrtofs`）。**还没实现**（帧待抓包）——**改刀补会导致撞刀**。 */
ncl_err ncl_focas_tool_offset_write(ncl_focas *focas, long long index,
                                    const char *value);
/** 写一个宏变量（`cnc_wrmacro`）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_macro_write(ncl_focas *focas, long long number,
                              double value);

/* 底层：给"覆盖"和排障用 --------------------------------------------------- */

/**
 * 读一个 item 的某一块。绑定时一般用不到；需要自己的解释（把两个 item 凑成一个
 * 量、把位域推成三态、算单位换算）时，覆盖档里就用它。
 */
ncl_err ncl_focas_read_item(ncl_focas *focas, const char *item, long long block,
                            int length, ncl_dtype dtype, ncl_json **value);
/** 调一个驱动自己的操作（诊断用，例如 "session" / "items"）。 */
ncl_err ncl_focas_call(ncl_focas *focas, const char *operation,
                       const ncl_json *params, ncl_json **result);
/** 最近一次交换的原始报文，给 §6 的审计轨迹用。 */
void    ncl_focas_last_raw(ncl_focas *focas, ncl_driver_raw *out);

/*
 * 协议层不在这里：PDU 帧格式、命令/回复块、item 码表、以及驱动构造函数与 raw 逃逸口
 * 都在 clients/focas/ncl_focas_pdu.h（-Iclients 才到得了，现场不需要）。
 */#ifdef __cplusplus
}
#endif

#endif /* NCL_FOCAS_H */
