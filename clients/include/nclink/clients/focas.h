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
 *   - 会话是**两条 TCP**，分工写在 hello 的计数器上（01 册 §11.18）：计数器 1 那条
 *     只收传输帧（程序上下行），计数器 2 那条只收 `func 0x21` 的业务读写；发错那一条
 *     机床直接断连接。这些都在驱动里分好了，上层不用管；
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
    /**
     * 握手之后要不要再发那条会话探针（`func 0x21` 一个 `code 24` 的块，
     * 应答就是 ODBSYS）。默认开 —— 官方 SDK 也是这么发的（01 册 §2.8）。
     * 关掉只跳过它，两条 TCP 与两条 hello 照样走（那是会话本身）。
     */
    bool        negotiate;
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
/**
 * 轴的扭矩（`cnc_loadtorq`，item `TORQUE` = 0xfd：d = 电机号（0 = 伺服）、
 * e = 轴号（1 起））。**帧已核**（本机 `d=0 e=1` 回块返回码 0、载荷 4 字节；
 * `d=3 e=7` 回 EW_RANGE），**量纲没定标**：本机静止恒 0，值按载荷 @0 的 BE32 取，
 * 单位留给站点在 `get_attributes` 里写明。
 */
ncl_err ncl_focas_axis_torque(ncl_focas *focas, ncl_focas_axis axis,
                              double *value);
/**
 * 轴的电流（安培）：`cnc_rdsvmeter`（item `SVCURRENT` = 0x56，**d = 3**）。
 * 同一格里 `d = 1` 是负载表（%），见 `ncl_focas_axis_load()`。
 * 单位 = 安培（官方 `cnc_rdaxisdata(cls = 2, type = 2)` 就是这一格）。
 *
 * **轴的伺服温度没有这个接口**：FOCAS 里搜不到读轴温的调用（只有智能终端的高温
 * 报警码），所以模型里也没有那个点位列。
 */
ncl_err ncl_focas_axis_current(ncl_focas *focas, ncl_focas_axis axis,
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

/* 刀具、参数（帧 2026-09 全部核过，见 01 册 §11.13）---------------------- */

/**
 * 刀具表（表 7 的 TOOL，list）：逐号读刀补（0x08），从 1 号读到
 * `cnc_rdtofsinfo`（0x0a）给的 `use_no`（本机 400，实现上最多读到
 * `FOCAS_TOOL_TABLE_MAX` = 64 号）。元素形状 = `ncl_focas_tool_param()`。
 * 空号跳过；一个号都没有时回 `NCL_ERR_NOT_FOUND`（不交空表）。
 */
ncl_err ncl_focas_tool_list(ncl_focas *focas, ncl_json **value);
/** 刀补号的上限（`cnc_rdtofsinfo` = 0x0a 的 `use_no`；本机 400）。 */
ncl_err ncl_focas_tool_offset_count(ncl_focas *focas, long long *count);
/** 一条刀补（`cnc_rdtofs`，item 0x08）：`{"number":n,"value":v}`，type 0（半径磨损）。 */
ncl_err ncl_focas_tool_offset(ncl_focas *focas, long long index,
                              ncl_json **value);
/** 带类型的一条刀补：type 1 = 半径、3 = 长度、0 = 半径磨损、2 = 长度磨损。 */
ncl_err ncl_focas_tool_offset_typed(ncl_focas *focas, long long index,
                                    long long type, ncl_json **value);
/** 刀具寿命计数（`cnc_rdlife`，item 0x8b）：**这台机床回 EW_NOOPT=6**（寿命管理选项
 *  没开），所以如实回"机床不提供"。 */
ncl_err ncl_focas_tool_life(ncl_focas *focas, long long group,
                            long long *value);
/** 一个用户宏变量（`cnc_rdmacro`，item 0x15）：`{"number":n,"value":v}`。
 *  **这台机床没开用户宏变量**（回 EW_NOOPT=6）。 */
ncl_err ncl_focas_macro_variable(ncl_focas *focas, long long number,
                                 ncl_json **value);
/** 一段宏变量（`cnc_rdmacror`）：表 7 的 VARIABLE（list）就是它。**机床不提供**。 */
ncl_err ncl_focas_macro_variables(ncl_focas *focas, long long first,
                                  long long count, ncl_json **value);
/**
 * 读一个 CNC 参数（`cnc_rdparam`，item `RDPARAM` = 0x8d）。@p number 是无轴参数的
 * 号；带轴参数（1320/1420/1825…）用 `ncl_focas_parameter_axis`。
 *
 * 出门的对象：`{"number","axis","type","value","raw"}` —— `type` 是机床的属性字
 * （prm_type：低位 = 类型 0 bit / 1 byte / 2 word / 3 2字，bit2 = 带轴，bit5 = 写保护）。
 */
ncl_err ncl_focas_parameter(ncl_focas *focas, long long number, ncl_json **value);
/** 读一个**带轴** CNC 参数：@p axis = 1..n（0 只对无轴参数有效，给错机床不收）。 */
ncl_err ncl_focas_parameter_axis(ncl_focas *focas, long long number, long long axis,
                                 ncl_json **value);
/** 一套刀具参数（表 7 的 TOOLPARAM）：`{"id","kind","radius","length",
 *  "radius_wear","length_wear"}`（`kind` 这条路上没有来源，固定 0）。 */
ncl_err ncl_focas_tool_param(ncl_focas *focas, long long index,
                             ncl_json **value);
/** 写一套刀具参数（每个给出的字段一条 0x09）：**只写给出的字段**，其余不动。 */
ncl_err ncl_focas_tool_param_write(ncl_focas *focas, long long index,
                                   const ncl_json *fields);
/** 整张刀具参数表（表 7 的 TOOLPARAM，JSON 对象）：号 → 参数。 */
ncl_err ncl_focas_tool_param_table(ncl_focas *focas, ncl_json **value);
/** 整张参数表（表 6 的 PARAMETER，dict）：逐号读（本实现读到
 *  `FOCAS_PARAM_TABLE_MAX` = 64 号为止），一条都读不到就回错。 */
ncl_err ncl_focas_parameter_table(ncl_focas *focas, ncl_json **value);
/** 宏变量表（表 7 的 VARIABLE，list）：`cnc_rdmacror` 按段读。
 *  **还没实现**（帧待核对）。 */
ncl_err ncl_focas_variable_table(ncl_focas *focas, ncl_json **value);
/**
 * 读一段 **PMC**（FANUC 的 PLC 就叫 PMC）：`pmc_rdpmcrng`（item `PMCRNG` = 0x8001）。
 * @p family 是族字母（`G`/`F`/`Y`/`X`/`A`/`R`/`T`/`K`/`C`/`D`），@p start/@p count
 * 以**该族自己的单位**计（X/Y/R 这些是字节号，D 是字号），@p width：0 字节 / 1 字。
 * 出门是一个数组，每点一个值（大端解析）。
 *
 * 位读用 `ncl_focas_pmc_bit()`：梯形图地址是"字节.位"，@p bit 是扁平位号
 * （`字节 × 8 + 位`），与 `/CONTROLLER/REGISTER@X` 那类点位的号一致。
 */
ncl_err ncl_focas_pmc_read(ncl_focas *focas, char family, long long start,
                           long long count, int width, ncl_json **value);
ncl_err ncl_focas_pmc_bit(ncl_focas *focas, char family, long long bit,
                          bool *on);
/**
 * PMC 参数区的**控制数据**（数据表 `D` / 扩展继电器）：`pmc_rdcntldata` = 0x8004
 * （@p exrelay = false）或 `pmc_rdcntlexrelay` = 0x8057（true）。组号**从 1 起**，
 * 出门 `{"1":{"tableParam":…,"size":10000,"address":0}, …}`。
 */
ncl_err ncl_focas_pmc_control_table(ncl_focas *focas, bool exrelay,
                                    ncl_json **value);
/**
 * **PMC 自己的报警文本**（`pmc_rdalmmsg` = 0x8010，`type` 用 1、起始号从 1 起）：
 * 出门 `[{"number":…,"text":"…"}]`；没有报警回空数组。与 `cnc_alarm`（CNC 侧）不是一回事。
 * ⚠️ 文本切法没在真机上核过（这台没有 PMC 报警）。
 */
ncl_err ncl_focas_pmc_alarm(ncl_focas *focas, long long start, long long count,
                            ncl_json **value);
/** 族字母 → PMC 的 adr_type（0..9；认不出回 -1）。给适配器算号段用。 */
int ncl_focas_pmc_adr_type(char family);
/**
 * 写一段 PMC（`pmc_wrpmcrng` = item `PMCWR` = **0x8002**）：同一族的 `count` 个点，
 * 值在 @p values 里（单位与读一致：位族按字节、`D` 按字）。
 *
 * **不是所有族都能写**（`X`/`F` 是机床/CNC 驱动的信号，spec 也说有些区不能写）——
 * 机床不收就如实回错，不假装成功。
 */
ncl_err ncl_focas_pmc_write(ncl_focas *focas, char family, long long start,
                            const long long *values, size_t count, int width);
/** 写一个 PMC 位（位号 = 字节 × 8 + 位）：读回所在字节、改那一位、写回去。 */
ncl_err ncl_focas_pmc_bit_write(ncl_focas *focas, char family, long long bit,
                                bool on);

/**
 * 一个工件坐标系（工件零点偏移）：`cnc_rdzofs`（item `RDZOFS` = **0x0b**）。
 * @p name 收 `"EXT"`（外部）、`"G54"`…`"G59"`、`"G54.1P3"` 这种。
 * 出门 `{"number":1,"x":12.345,"y":0,"z":0}` —— 键是**机床自己报的轴名**（小写）。
 */
ncl_err ncl_focas_work_offset(ncl_focas *focas, const char *name,
                              ncl_json **value);
/** 整套工件坐标系（表 7 的 COORDINATE）：外部 + G54…G59，键就是名字。 */
ncl_err ncl_focas_work_offsets(ncl_focas *focas, ncl_json **value);
/**
 * 写一个轴的工件零点偏移（`cnc_wrzofs` = item `WRZOFS` = **0x0c**）。
 * @p axis_number 是**机床的轴号**（1 = X、2 = Y、…），@p value 是实际值（mm/deg，
 * 本实现按机床报的小数位换算成"最低输入单位"的整数发下去）。写完**读回来复核**，
 * 没落到位就回 `NCL_ERR_UNAVAILABLE`（不假装成功）。
 */
ncl_err ncl_focas_work_offset_write(ncl_focas *focas, const char *name,
                                    long long axis_number, double value);
/** 当前模态（T/B/S/F 等，`cnc_rdgcode`）。**还没实现**（帧待抓包）。 */
ncl_err ncl_focas_modal(ncl_focas *focas, ncl_json **value);
/**
 * 系统信息（型号/系列/轴数，`cnc_sysinfo`）：会话探针那条 `code 24` 的应答载荷
 * ODBSYS（18 字节）拆出来的 —— addinfo / maxAxis / cncType / machineType /
 * series / version / axes。2026-09 真机实测（01 册 §2.8）。
 */
ncl_err ncl_focas_system(ncl_focas *focas, ncl_json **value);
/** 机床型号：ODBSYS 里的 `cnc_type` + `mt_type` + `series`（如 `"0M D4G3"`）。 */
ncl_err ncl_focas_model(ncl_focas *focas, char *out, size_t cap);
/** 系统软件版本（ODBSYS 的 `version`，如 `"28.0"`）。 */
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
 * 把机床上的某个程序**当文件读回来**（`type` = 0 = NC 程序；别的类型回
 * `NCL_ERR_UNAVAILABLE`）。
 *
 * 走的是 **`cnc_rdpdf_line`（Cb `0xf0`）**：`d` = 起始行号、`e` = 一次读几行、
 * 载荷 = **256 字节**的程序路径；应答体就是程序正文。一次读
 * `FOCAS_PDF_LINES_PER_CALL` 行、按回来的行数往后接着读，末行没有 `'\n'` 或机床
 * 报错就收尾；`name` 可以给完整路径（`//CNC_MEM/USER/PATH1/O0001`），也可以只给
 * 文件名（自动补默认文件夹 `//CNC_MEM/USER/PATH1/`）。读不到就如实回
 * `NCL_ERR_NOT_FOUND`（不编内容）。
 *
 * 2026-09-23 对 NCGuide 0i-MF 实测：读 O3001 拿回 384 字节正文（`O3001(SUBPOCKET)
 * … M99 %`）。⚠️ 官方手册把 `cnc_rdpdf_line` 标成 **HSSB 专用**（以太网列是 `-`），
 * 这台模拟器的以太网照答，**真机未必** —— 换机器时先核一次（01 册 §11.16）。
 */
ncl_err ncl_focas_program_upload(ncl_focas *focas, long long type,
                                 const char *name, char **program,
                                 size_t *len);

/**
 * 在机床的程序区**建一个程序文件（或文件夹）**（`cnc_pdf_add` = Cb `0xb5`）。
 *
 * 2026-09-23 实测（模拟器）：`//CNC_MEM/USER/PATH1/O1234` 建出来 `rc=0`，机床还会把
 * 程序号那一行（`O1234`）自动写进去；路径要"盘名 + 路径 + 文件名"。
 * `folder` = true 就建文件夹（本机没核过）。
 */
ncl_err ncl_focas_program_create(ncl_focas *focas, const char *name,
                                 bool folder);

/**
 * 把机床上的某个程序选成主程序（`cnc_pdf_slctmain`）。**还没实现**（帧待抓包）。
 */
ncl_err ncl_focas_program_select_main(ncl_focas *focas, const char *name);
/**
 * 删掉机床上的某个程序（`cnc_delete` = 一个 0x05，d = 程序号）。
 *
 * 帧是从官方 SDK 上抄的（`cnc_delete(h, 1)` → 0x05、d=1）。**这台模拟器回
 * EW_ATTRIB=5** 不收这一条（`cnc_pdf_del` 的 0xb6 也一样，见 01 册 §11.13），
 * 所以这里会如实报模块错、不假装删掉了。`name` 收 "O0001" / "1" / "0001"；
 * 带路径的名字（`//CNC_MEM/...`）这条路不走，回参数错。
 * 删正在执行的程序机床会拒，这是机床侧的保护。
 */
ncl_err ncl_focas_program_delete(ncl_focas *focas, const char *name);
/**
 * 写一个 CNC 参数（`cnc_wrparam` 的第二条帧，item `WRPARAM2` = **0x8e**）：
 * 照机床自己那条 264 字节读应答回填、只换值那一格，块头四格全 0、tag1 = 264。
 * 2026-09 在 NCGuide 0i-MF 上**写进去又读回核过**（01 册 §11.26）。
 *
 * @p number 是无轴参数的号；带轴参数用 `ncl_focas_parameter_write_axis`（只动那一轴）。
 * 机床上"参数写入允许（PWE）"没开 / 参数是锁的（>9000 一类）→ 机床回 EW_PROT=7，
 * 这里翻成 `NCL_ERR_UNAVAILABLE`。**写完一律读回复核**，对不上也回
 * `NCL_ERR_UNAVAILABLE`，不假成功。
 *
 * **风险高**：参数写错会让机床行为不对，站点用之前先确认权限与备份（权限在外面控）。
 */
ncl_err ncl_focas_parameter_write(ncl_focas *focas, long long number,
                                  const char *value);
/** 写一个**带轴** CNC 参数：@p axis = 1..n，只动那一轴，其余轴不碰。 */
ncl_err ncl_focas_parameter_write_axis(ncl_focas *focas, long long number,
                                       long long axis, const char *value);
/**
 * 写一条刀补（`cnc_wrtofs` = item `WRTOFS` = **0x09**）：帧 2026-09 对模拟器
 * **写进去又读回来核过**（写 0x3333 → 读回 13.107mm，01 册 §11.13）。写的是
 * type 0（半径磨损）那一格 —— 与 `ncl_focas_tool_offset()` 读的同一格；
 * 值按机床自己的小数位换算（先读一次打底拿 dec）。**改刀补会导致撞刀**。
 */
ncl_err ncl_focas_tool_offset_write(ncl_focas *focas, long long index,
                                    const char *value);
/** 写一条带类型的刀补（type 1 = 半径、3 = 长度、0 = 半径磨损、2 = 长度磨损）。 */
ncl_err ncl_focas_tool_offset_write_typed(ncl_focas *focas, long long index,
                                          long long type, double value);
/**
 * 写一个宏变量（`cnc_wrmacro` = item `WRMACRO` = 0x16，载荷形状同刀补）。
 * **这台机床没开用户宏变量**（读 0x15 回 EW_NOOPT=6），写这条同样被拒 ——
 * 帧照发，机床怎么答就如实往上报。
 */
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
