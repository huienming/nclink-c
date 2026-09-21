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

/** 填上默认值（host 留空，其余是驱动自己的默认）。 */
void ncl_focas_config_default(ncl_focas_config *config);

ncl_focas *ncl_focas_open(const ncl_focas_config *config, char **err);
void       ncl_focas_close(ncl_focas *focas);

/** 会话现在是通着的吗（读过一次之后才有意义）。 */
bool       ncl_focas_connected(const ncl_focas *focas);
/** 上一次失败的一句话原因；没失败过就是空串。 */
const char *ncl_focas_last_error(const ncl_focas *focas);

/* 语义：名字就是读回来的东西 ------------------------------------------------- */

/** 设备状态，标准的三态："running" / "free" / "holding"（由 ODBST 位域推出）。 */
ncl_err ncl_focas_status(ncl_focas *focas, char *out, size_t cap);
/** 加工件数（RDCOUNT，int32；表 7 的 PART_COUNT 是数值）。 */
ncl_err ncl_focas_part_count(ncl_focas *focas, long long *value);
/** 当前主程序名。 */
ncl_err ncl_focas_program_name(ncl_focas *focas, char *out, size_t cap);
/** 轴的实际位置（mm / deg）。 @p axis 见 ncl_focas_axis。 */
ncl_err ncl_focas_axis_position(ncl_focas *focas, ncl_focas_axis axis,
                                double *value);
/** 轴的转速/进给速度。 */
ncl_err ncl_focas_axis_speed(ncl_focas *focas, ncl_focas_axis axis,
                             double *value);

/*
 * 下面三条的协议调用还没抓帧（有的在 01 册 §2.3 的码表里就没有，有的是 32 册 §5
 * 列在"待核"里的）。函数照样摆在这里、照样能绑到模型路径上：在帧补上之前它们回
 * **NCL_ERR_UNAVAILABLE**，也就是"这一份 client 还没有它要的协议调用" —— 模型里
 * 有这条路径、客户端问它有明确答复、轮询与 §6 审计都不碰它、自检把它算成"待抓包"
 * 而不是失败（见 ncl_common.h 里这个码）。抓包补上之后**改的就是这三个函数的函数
 * 体**：适配器那张点位表一行都不用动。
 *
 * 要哪一帧，写在各自的注释里（ncl_focas_last_error() 里也带一句，排障时看得到）。
 */

/** 报警（表 6 的 WARNING）：cnc_rdalmmsg2 还没抓到帧，所以现在回 NCL_ERR_UNAVAILABLE。 */
ncl_err ncl_focas_alarm(ncl_focas *focas, ncl_json **value);
/** 轴的目标位置：cnc_rdposition 还没抓到帧，所以现在回 NCL_ERR_UNAVAILABLE。 */
ncl_err ncl_focas_axis_position_cmd(ncl_focas *focas, ncl_focas_axis axis,
                                    double *value);
/** 刀具表（表 7 的 TOOL，list）：帧待核对，所以现在回 NCL_ERR_UNAVAILABLE。 */
ncl_err ncl_focas_tool_list(ncl_focas *focas, ncl_json **value);

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
