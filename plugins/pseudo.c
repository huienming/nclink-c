/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * 伪机床（pseudo）：不接硬件的数控机床，点位模型照 plugins/syntec.c 摆。
 *
 * **为什么要它**：整条链路的验证（声明 → 模型 → 采样通道 → 绑定 → 请求应答 →
 * 上位机）平时要等一台真机床。这个模块把"机床"换成内置的模拟器：同一个设备类型、
 * 同一套路径与类型、同一个采样周期、同样的四张配置表与操作位，客户端不需要知道
 * 对面是真是假 —— 拿现场那套上位机/工具直接指过来就能跑通。
 *
 * **它不是什么**：不是 client。clients/ 里的协议实现跟这里没有关系 —— 这个模块
 * 一个字节都不往线上发，也没有 socket（"协议字节一个字都不进适配器"那条规矩，
 * 在这里是自然成立的）。因此它也不实现 last_raw 钩子：审计里如实"无帧"，而不是
 * 编一串 JSON 冒充报文。
 *
 * 形状（逐条对齐 plugins/syntec.c 的声明）：
 *
 *   采样 4 项（1000 ms 采样 / 1000 ms 上报）
 *     /STATUS                     string   "running" / "holding" / "free"
 *     /PART_COUNT                 i64      加工件数
 *     /CONTROLLER/PROGRAM         string   程序名
 *     /CONTROLLER/WARNING         list     报警（空表 = 没有报警）
 *   覆盖量
 *     /CONTROLLER/LINE_NUMBER     string
 *     /FEED_OVERRIDE              i64      进给倍率 %
 *     /SPINDLE_OVERRIDE           i64      主轴倍率 %
 *     /FEED_SPEED                 f64      进给速度 mm/min
 *     /SPINDLE_SPEED              f64      主轴转速 rpm
 *   轴：X/Y/Z/A/B/C/U/V/W 九个字母都占位，每轴六格（九个字母、六个格子，
 *       一共 54 条；没列的轴照新代的规矩回 NOT_FOUND，不给数）
 *     /AXIS@<轴>/SCREW/POSITION              实际位置（机械坐标）
 *     /AXIS@<轴>/SERVO_DRIVER/POSITION       指令位置（略超前于实际）
 *     /AXIS@<轴>/MOTOR/POSITION              机械坐标（与 SCREW 同源）
 *     /AXIS@<轴>/MOTOR/VARIABLE@ABSOLUTE     绝对（工件坐标系）
 *     /AXIS@<轴>/MOTOR/VARIABLE@RELATIVE     相对（当前程序段起点）
 *     /AXIS@<轴>/MOTOR/VARIABLE@DISTANCE     剩余（到程序段终点）
 *   配置（都是"表"：参数 HASH、刀具/寄存器/变量 LIST）
 *     /CONTROLLER/PARAMETER       HASH  get_keys / get_value / get_attributes / set_value
 *     /CONTROLLER/TOOL            LIST  get_length / get_value / set_value / get_attributes
 *     /CONTROLLER/REGISTER@R|I|O|C|S|A  LIST  同新代：R/I/C/S 可写，O/A 只读
 *     /CONTROLLER/VARIABLE        LIST  get_length / get_value / set_value / get_attributes
 *   方法
 *     /SESSION                    会话 + 模拟器状态，也是这台假机床的遥控器（见下）
 *
 * 值从哪来：一条**相位时钟**。每个循环 `cycleMs` 毫秒，循环内进度 p ∈ [0,1)：
 *
 *   - 轴位置：三角波（p 与轴序号、seed 错开），所以采样帧画出来是连续的曲线，
 *     不是一堆常数；ABSOLUTE 加工件零点偏置、RELATIVE 相对程序段起点、
 *     DISTANCE 是到段终点的剩余量；指令位置取 p 稍靠前的那一点（伺服跟着指令走）。
 *   - 状态：p < 0.9 running、< 0.95 holding、否则 free（就是新代 §3.2 那几个字面量）。
 *   - 计件：每个循环走完 +1；程序名在每个循环换一个。
 *   - 报警：`alarmEvery` / `alarmFor` 决定"每几个循环报一次、报几个循环"（报在
 *     每组循环的末尾，所以刚开机是干净的）。
 *   - 写进去的东西（倍率、参数、刀补、寄存器、变量）落在模拟器的内存里，
 *     下一次读回来就是新值 —— 写后复核那条路能验。
 *
 * 配置（tools[].parameters，全都可省）：
 *
 *   cycleMs       一个加工循环多长（ms，默认 8000）
 *   axes          这台"机床"有哪几个轴（默认 "XYZC"）
 *   seed          轨迹错开量（默认 0）；同一份配置 + 同一个 seed = 同一条轨迹
 *   frozen        true：把时钟钉在 phase=0.5、第 4 个循环上，取值与时间无关
 *                 （用例要断言精确值就用它）
 *   programs      程序名表（默认 ["O0001"]）
 *   partCount     起始计件（默认 0）
 *   feedRate      100% 时的进给 mm/min（默认 800）
 *   spindleRpm    100% 时的转速 rpm（默认 1200）
 *   feedOverride / spindleOverride   初始倍率 %（默认 100 / 100）
 *   status        "running"/"holding"/"free"：把状态钉死（默认跟着相位走）
 *   alarmEvery / alarmFor / alarms    报警节奏与条目
 *                 alarms 是 [{"number":1201,"text":"..."}, ...]
 *   params        参数表：{"1001":{"title":"...","value":1}} 或
 *                 [{"no":1001,"title":"...","value":1}]
 *   tools         刀补条数（默认 8；1..tools 号）
 *   registers     {"R":64,"I":32,"O":32,"C":32,"S":32,"A":16}
 *   variables     变量个数（默认 64；#1..#8 默认有值，其余按"空号"答）
 *   latencyMs     每次取值先睡这么久（默认 0；喂超时/丢拍那条路）
 *   fail          {"code":-5,"count":2,"message":"..."} 前 count 次取值失败
 *                 （-1 = 一直失败）
 *   unavailable   ["/CONTROLLER/LINE_NUMBER", ...] 这些路径照实回"还读不了"
 *
 * 与新代**有意不同**的两处（都是为了"假机床能做真机床做不了的事"）：
 *
 *   1. /AXIS@<轴>/SERVO_DRIVER/POSITION 新代那边还"待抓包"（回 NCL_ERR_UNAVAILABLE），
 *      伪机给值 —— 客户端要验的是"拿到指令位置怎么用"，不是"这台机床抓没抓到帧"。
 *   2. /FEED_OVERRIDE、/SPINDLE_OVERRIDE 新代只读，伪机**可写** —— 上位机能通过
 *      协议本身把倍率拨到任意值，不用人去改配置。权限仍然在适配器外面控。
 */
#include "nclink/ncl_tool.h"

#include <stdio.h>
#include <string.h>

#include "nclink/ncl_json.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_platform.h"

/* ------------------------------------------------------------------ 上限 ---- */

/** 一次读多少条：跟新代同一个口径，别让人一口气点几千次。 */
#define PSEUDO_BATCH_MAX 64u
/** 翻页取元数据时一页多少条。 */
#define PSEUDO_PARAM_PAGE 16u
/** 刀补的长度几何/磨损组数：册 4 说的是 12 组。 */
#define PSEUDO_TOOL_LENGTHS 12

#define PSEUDO_MAX_PARAMS 64
#define PSEUDO_MAX_TOOLS 32
#define PSEUDO_MAX_REGS 256
#define PSEUDO_MAX_BITS 128
#define PSEUDO_MAX_VARS 256
#define PSEUDO_MAX_PROGRAMS 8
#define PSEUDO_MAX_MUTED 8
/** 位族的个数：I/O/C/S/A（R 是寄存器，单独一格）。 */
#define PSEUDO_BIT_FAMILIES 5

/* --------------------------------------------------------------- 控制器内存 -- */

/** 一条系统参数：号、标题、元数据两格、值。 */
typedef struct {
    long long no;
    char      title[48];
    long long flags;
    long long fallback;
    long long value;
} pseudo_param;

/** 一条刀补（§11.7 的形状：id/kind/radius/length 在前，12 组长度在里）。 */
typedef struct {
    long long kind;
    double    radius;
    double    length_geometry[PSEUDO_TOOL_LENGTHS];
    double    length_wear[PSEUDO_TOOL_LENGTHS];
    double    radius_wear;
    double    tool_angle;
} pseudo_tool;

/** 一个变量：控制器说"空"的号是 type 0，不硬凑一个 0 出来。 */
typedef struct {
    long long type; /* 0 空、1 整数、2 浮点 */
    long long int_value;
    double    double_value;
} pseudo_variable;

/** 哪个位族。R 不是位，单独走 regs[]。 */
enum { PSEUDO_BIT_I = 0, PSEUDO_BIT_O, PSEUDO_BIT_C, PSEUDO_BIT_S, PSEUDO_BIT_A };

typedef struct {
    ncl_mutex *lock;
    int64_t    t0;

    /* 机器的性子 */
    unsigned   cycle_ms;
    double     seed_phase;
    bool       frozen;
    char       axes[16];
    long long  part_base;
    char       programs[PSEUDO_MAX_PROGRAMS][24];
    size_t     program_count;

    /* 运行量 */
    long long  feed_override;
    long long  spindle_override;
    double     feed_rate;
    double     spindle_rpm;
    char       forced_status[16]; /* 空串 = 跟着相位走 */

    /* 报警：节奏来自配置，注入的那条来自 /SESSION */
    long long  alarm_every;
    long long  alarm_for;
    long long  alarm_no;
    char       alarm_text[64];
    bool       inj_alarm;
    long long  inj_alarm_no;
    char       inj_alarm_text[64];

    /* 故障注入 */
    long long  latency_ms;
    long long  fail_code;
    long long  fail_left; /* -1 = 一直失败 */
    char       fail_text[64];
    char       muted[PSEUDO_MAX_MUTED][48];
    size_t     muted_count;
    long long  reads;
    char       last_error[96]; /* /SESSION 的 lastError：上一次注入失败的原话 */

    /* 模拟出来的"控制器内存" */
    pseudo_param    params[PSEUDO_MAX_PARAMS];
    size_t          param_count;
    pseudo_tool     tools[PSEUDO_MAX_TOOLS];
    size_t          tool_count;
    uint32_t        regs[PSEUDO_MAX_REGS];
    size_t          reg_count;
    bool            bits[PSEUDO_BIT_FAMILIES][PSEUDO_MAX_BITS];
    size_t          bit_count[PSEUDO_BIT_FAMILIES];
    pseudo_variable vars[PSEUDO_MAX_VARS];
    size_t          var_count;
} pseudo_machine;

/* ------------------------------------------------------------------ 小数 ---- */

/**
 * @p path 是不是 @p relative 那条路径。
 *
 * 声明里写的是相对路径（"/STATUS"），运行时交到点位函数手上的是拼过设备段的
 * 绝对路径（"/MACHINE/STATUS"），所以按"尾部相同"比 —— 两种写法都认。
 */
static bool pseudo_path_is(const char *relative, const char *path)
{
    size_t want;
    size_t have;

    if (relative == NULL || path == NULL) {
        return false;
    }
    want = strlen(relative);
    have = strlen(path);
    return have >= want && strcmp(path + (have - want), relative) == 0;
}

/** u 的小数部分，负数也回 [0,1)。 */
static double pseudo_frac(double u)
{
    double whole = (double)(long long)u;

    u -= whole;
    if (u < 0.0) {
        u += 1.0;
    }
    return u;
}

/** 三角波：0→1→0，周期 1。没有 libm，也就不挑编译器。 */
static double pseudo_tri(double u)
{
    u = pseudo_frac(u);
    return u < 0.5 ? 2.0 * u : 2.0 * (1.0 - u);
}

/** 每根轴走多远（mm）：行程各不相同，画出来才分得开。 */
static double pseudo_travel(size_t index)
{
    return 100.0 + 40.0 * (double)index;
}

/** 每根轴的工件零点偏置（mm）：ABSOLUTE 那一格用它。 */
static double pseudo_work_offset(size_t index)
{
    return -100.0 + 10.0 * (double)index;
}

/** 第 index 根轴在相位 phase 上的实际位置（机械坐标，mm）。 */
static double pseudo_position(size_t index, double phase, double seed_phase)
{
    double travel = pseudo_travel(index);
    double u = phase + seed_phase + 0.11 * (double)index;

    return -travel / 2.0 + travel * pseudo_tri(u);
}

/** 轴字母 -> 在这台机床的轴表里的序号，没有就回 -1。 */
static int pseudo_axis_index(const pseudo_machine *m, char letter)
{
    const char *at;
    char upper = letter;

    if (upper >= 'a' && upper <= 'z') {
        upper = (char)(upper - 'a' + 'A');
    }
    at = strchr(m->axes, upper);
    return at == NULL ? -1 : (int)(at - m->axes);
}

/* ------------------------------------------------------------------ 取值 ---- */

/** 一次取值的快照：锁只在这个结构填出来之前拿着。 */
typedef struct {
    const char *status;
    long long   cycle;
    double      phase;
    long long   part_count;
    const char *program;
    long long   line_number;
    bool        alarm;
    long long   alarm_no;
    const char *alarm_text;
    long long   feed_override;
    long long   spindle_override;
    double      feed_speed;
    double      spindle_speed;
} pseudo_view;

/**
 * 把"现在这台机床什么样"算出来。相位时钟是唯一的驱动源：给定配置与 seed，
 * 同一时刻问两次得同一个答案（确定性），frozen 时连时间都不看。
 */
static void pseudo_view_fill(pseudo_machine *m, pseudo_view *v)
{
    bool running;
    unsigned cycle_ms;

    ncl_mutex_lock(m->lock);
    cycle_ms = m->cycle_ms != 0 ? m->cycle_ms : 1000u;
    if (m->frozen) {
        /* 钉在循环中段：位置在行程中间、状态是 running、计件走完 4 个。 */
        v->phase = 0.5;
        v->cycle = 4;
    } else {
        int64_t elapsed = ncl_time_monotonic_millis() - m->t0;
        int64_t span = (int64_t)cycle_ms;

        if (elapsed < 0) {
            elapsed = 0;
        }
        v->cycle = (long long)(elapsed / span);
        v->phase = (double)(elapsed % span) / (double)span;
    }
    if (m->forced_status[0] != '\0') {
        v->status = m->forced_status;
    } else {
        v->status = v->phase < 0.9 ? "running"
                                   : (v->phase < 0.95 ? "holding" : "free");
    }
    running = strcmp(v->status, "running") == 0;
    v->program = m->program_count > 0
                     ? m->programs[(size_t)(v->cycle % (long long)m->program_count)]
                     : "";
    v->part_count = m->part_base + v->cycle;
    /* 行号：循环进度映射到程序里的行（看着像在走程序，不需要真的解析 G 代码）。 */
    v->line_number = 10 + (long long)(v->phase * 400.0) * 10;
    if (m->inj_alarm) {
        v->alarm = true;
        v->alarm_no = m->inj_alarm_no;
        v->alarm_text = m->inj_alarm_text;
    } else if (m->alarm_every > 0 && m->alarm_for > 0 &&
               (v->cycle % m->alarm_every) >= (m->alarm_every - m->alarm_for)) {
        v->alarm = true;
        v->alarm_no = m->alarm_no;
        v->alarm_text = m->alarm_text;
    } else {
        v->alarm = false;
        v->alarm_no = 0;
        v->alarm_text = "";
    }
    v->feed_override = m->feed_override;
    v->spindle_override = m->spindle_override;
    v->feed_speed = running ? m->feed_rate * (double)m->feed_override / 100.0 : 0.0;
    v->spindle_speed =
        running ? m->spindle_rpm * (double)m->spindle_override / 100.0 : 0.0;
    ncl_mutex_unlock(m->lock);
}

/* -------------------------------------------------------------- 注入的开关 -- */

/**
 * 每一次取值都先过这道门（三件事，都在配置里）：
 *
 *   1. latencyMs：先睡一会儿 —— 喂"取值慢/丢拍/超时"那条路；
 *   2. unavailable：这条路照实回"还读不了"，和新代那边待抓包的点位一个待遇；
 *   3. fail：前 count 次取值失败，理由里带配置里那句话 —— 喂 NG 那条路。
 *
 * 睡在锁外面（睡着的时候别人还能取值），其余都在锁里。
 */
static ncl_err pseudo_gate(pseudo_machine *m, const char *path, char **reason)
{
    char text[64];
    long long sleep_ms = 0;
    long long fail_code = 0;
    bool muted = false;
    bool failing = false;
    size_t i;

    text[0] = '\0';
    ncl_mutex_lock(m->lock);
    m->reads++;
    sleep_ms = m->latency_ms;
    for (i = 0; i < m->muted_count; i++) {
        if (pseudo_path_is(m->muted[i], path)) {
            muted = true;
            break;
        }
    }
    if (!muted && m->fail_left != 0) {
        if (m->fail_left > 0) {
            m->fail_left--;
        }
        failing = true;
        fail_code = m->fail_code;
        snprintf(text, sizeof(text), "%s", m->fail_text);
    }
    ncl_mutex_unlock(m->lock);

    if (sleep_ms > 0) {
        ncl_sleep_millis((unsigned)sleep_ms);
    }
    if (muted) {
        ncl_mutex_lock(m->lock);
        snprintf(m->last_error, sizeof(m->last_error),
                 "%s 标了 unavailable：这台伪机床答「还读不了」（与待抓包同一个待遇）",
                 path);
        ncl_mutex_unlock(m->lock);
        /* 不当失败：宿主按"还读不了"答客户端，自检算待抓包。 */
        return NCL_ERR_UNAVAILABLE;
    }
    if (failing) {
        ncl_mutex_lock(m->lock);
        snprintf(m->last_error, sizeof(m->last_error), "%s 注入的失败：%s", path,
                 text[0] != '\0' ? text : "配置里说的");
        ncl_mutex_unlock(m->lock);
        return ncl_tool_fail(reason, (ncl_err)fail_code,
                             "伪机床注入的失败：%s",
                             text[0] != '\0' ? text : "配置里说的");
    }
    return NCL_OK;
}

/** 路径上的一个普通读：过门之后交给取值函数。 */
#define PSEUDO_READ(m, self, reason)                                           \
    do {                                                                       \
        ncl_err rc_ = pseudo_gate((m), (self)->path, (reason));                 \
        if (rc_ != NCL_OK) {                                                   \
            return rc_;                                                        \
        }                                                                      \
    } while (0)

/* 下面两个总入口要调的点位函数按"值怎么算"分节排在后面，先把它们点出来。 */
static ncl_err pseudo_status(void *ctx, char *out, size_t cap);
static ncl_err pseudo_part_count(void *ctx, long long *value);
static ncl_err pseudo_program(void *ctx, char *out, size_t cap);
static ncl_err pseudo_warning(void *ctx, ncl_json **value);
static ncl_err pseudo_line_number(void *ctx, char *out, size_t cap);
static ncl_err pseudo_feed_speed(void *ctx, double *value);
static ncl_err pseudo_spindle_speed(void *ctx, double *value);
static ncl_err pseudo_feed_override_get(void *ctx, long long *value);
static ncl_err pseudo_feed_override_set(void *ctx, long long value);
static ncl_err pseudo_spindle_override_get(void *ctx, long long *value);
static ncl_err pseudo_spindle_override_set(void *ctx, long long value);
static ncl_err pseudo_axis_machine(void *ctx, long long arg, double *value);
static ncl_err pseudo_axis_command(void *ctx, long long arg, double *value);
static ncl_err pseudo_axis_absolute(void *ctx, long long arg, double *value);
static ncl_err pseudo_axis_relative(void *ctx, long long arg, double *value);
static ncl_err pseudo_axis_distance(void *ctx, long long arg, double *value);

/**
 * 数据项那一族的总入口（采样四项 + 覆盖量）。用 dispatch 形状而不是绑定形状，
 * 是因为**每个点位要认得自己的路径**：三个注入开关（latencyMs / unavailable /
 * fail）都是按路径生效的，绑定形状的取值函数拿不到路径。
 *
 * 认路径按"尾部相同"（声明里是相对路径，运行时给的是绝对路径，见 pseudo_path_is）。
 */
static ncl_err pseudo_data_dispatch(void *ctx, const ncl_tool_point *self,
                                    ncl_operation op, const ncl_json *params,
                                    ncl_json **result, char **reason)
{
    pseudo_machine *m = (pseudo_machine *)ctx;
    const char *path = self->path;
    ncl_err rc;

    PSEUDO_READ(m, self, reason);

    if (pseudo_path_is("/STATUS", path)) {
        char text[24];

        rc = pseudo_status(m, text, sizeof(text));
        return rc == NCL_OK ? ncl_tool_reply_text(result, text)
                            : ncl_tool_fail(reason, rc, "%s 取值失败", path);
    }
    if (pseudo_path_is("/PART_COUNT", path)) {
        long long value = 0;

        rc = pseudo_part_count(m, &value);
        return rc == NCL_OK ? ncl_tool_reply_int(result, value)
                            : ncl_tool_fail(reason, rc, "%s 取值失败", path);
    }
    if (pseudo_path_is("/CONTROLLER/PROGRAM", path)) {
        char text[24];

        rc = pseudo_program(m, text, sizeof(text));
        return rc == NCL_OK ? ncl_tool_reply_text(result, text)
                            : ncl_tool_fail(reason, rc, "%s 取值失败", path);
    }
    if (pseudo_path_is("/CONTROLLER/LINE_NUMBER", path)) {
        char text[24];

        rc = pseudo_line_number(m, text, sizeof(text));
        return rc == NCL_OK ? ncl_tool_reply_text(result, text)
                            : ncl_tool_fail(reason, rc, "%s 取值失败", path);
    }
    if (pseudo_path_is("/CONTROLLER/WARNING", path)) {
        ncl_json *alarms = NULL;

        rc = pseudo_warning(m, &alarms);
        if (rc != NCL_OK) {
            return ncl_tool_fail(reason, rc, "%s 取值失败", path);
        }
        *result = alarms;
        return NCL_OK;
    }
    if (pseudo_path_is("/FEED_SPEED", path) || pseudo_path_is("/SPINDLE_SPEED", path)) {
        double value = 0.0;

        rc = pseudo_path_is("/FEED_SPEED", path) ? pseudo_feed_speed(m, &value)
                                                 : pseudo_spindle_speed(m, &value);
        return rc == NCL_OK ? ncl_tool_reply_double(result, value)
                            : ncl_tool_fail(reason, rc, "%s 取值失败", path);
    }
    if (pseudo_path_is("/FEED_OVERRIDE", path) ||
        pseudo_path_is("/SPINDLE_OVERRIDE", path)) {
        bool feed = pseudo_path_is("/FEED_OVERRIDE", path);

        if (op == NCL_OP_SET_VALUE) {
            const ncl_json *value = ncl_tool_param_value(params);
            long long wanted = 0;

            if (value == NULL || !ncl_json_as_int(value, &wanted)) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "%s 要一个 0..200 的整数", path);
            }
            rc = feed ? pseudo_feed_override_set(m, wanted)
                      : pseudo_spindle_override_set(m, wanted);
            if (rc != NCL_OK) {
                return ncl_tool_fail(reason, rc, "%s 只收 0..200", path);
            }
            return ncl_tool_reply_int(result, wanted);
        }
        {
            long long value = 0;

            rc = feed ? pseudo_feed_override_get(m, &value)
                      : pseudo_spindle_override_get(m, &value);
            return rc == NCL_OK ? ncl_tool_reply_int(result, value)
                                : ncl_tool_fail(reason, rc, "%s 取值失败", path);
        }
    }
    return ncl_tool_fail(reason, NCL_ERR_NOT_FOUND, "伪机床上没有 %s 这个量", path);
}

/**
 * 轴那一族的总入口。点位自己的数据（arg）把"哪个轴 + 哪一格"编码在一起：
 * 路径照新代写死，轴字母从 arg 里取（真机上是读控制器轴表现查的）。
 */
#define PSEUDO_CELL_SCREW 0     /**< SCREW/POSITION（实际位置）      */
#define PSEUDO_CELL_SERVO 1     /**< SERVO_DRIVER/POSITION（指令位置）*/
#define PSEUDO_CELL_MOTOR 2     /**< MOTOR/POSITION（机械坐标）      */
#define PSEUDO_CELL_ABSOLUTE 3  /**< MOTOR/VARIABLE@ABSOLUTE         */
#define PSEUDO_CELL_RELATIVE 4  /**< MOTOR/VARIABLE@RELATIVE         */
#define PSEUDO_CELL_DISTANCE 5  /**< MOTOR/VARIABLE@DISTANCE         */
#define PSEUDO_AXIS_ARG(letter_value, cell_value)                              \
    ((const void *)(intptr_t)((((long long)(letter_value)) << 8) |             \
                              (long long)(cell_value)))

static ncl_err pseudo_axis_dispatch(void *ctx, const ncl_tool_point *self,
                                    ncl_operation op, const ncl_json *params,
                                    ncl_json **result, char **reason)
{
    pseudo_machine *m = (pseudo_machine *)ctx;
    long long raw = (long long)(intptr_t)self->arg;
    char letter = (char)(raw >> 8);
    int cell = (int)(raw & 0xFF);
    double value = 0.0;
    ncl_err rc;

    (void)op;
    (void)params;
    PSEUDO_READ(m, self, reason);

    switch (cell) {
    case PSEUDO_CELL_SCREW:
    case PSEUDO_CELL_MOTOR:
        rc = pseudo_axis_machine(m, letter, &value);
        break;
    case PSEUDO_CELL_SERVO:
        rc = pseudo_axis_command(m, letter, &value);
        break;
    case PSEUDO_CELL_ABSOLUTE:
        rc = pseudo_axis_absolute(m, letter, &value);
        break;
    case PSEUDO_CELL_RELATIVE:
        rc = pseudo_axis_relative(m, letter, &value);
        break;
    case PSEUDO_CELL_DISTANCE:
        rc = pseudo_axis_distance(m, letter, &value);
        break;
    default:
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "%s 的格子不对", self->path);
    }
    if (rc == NCL_ERR_NOT_FOUND) {
        /* 与新代同一条规矩：控制器没配这个轴就不给数、也不去读别的轴。 */
        return ncl_tool_fail(reason, rc, "%s：这台伪机床没有 %c 轴（axes=%s）",
                             self->path, letter, m->axes);
    }
    if (rc != NCL_OK) {
        return ncl_tool_fail(reason, rc, "%s 取值失败", self->path);
    }
    return ncl_tool_reply_double(result, value);
}

/* ---------------------------------------------------------------- 采样四项 -- */

static ncl_err pseudo_status(void *ctx, char *out, size_t cap)
{
    pseudo_view v;

    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    pseudo_view_fill((pseudo_machine *)ctx, &v);
    snprintf(out, cap, "%s", v.status);
    return NCL_OK;
}

static ncl_err pseudo_part_count(void *ctx, long long *value)
{
    pseudo_view v;

    pseudo_view_fill((pseudo_machine *)ctx, &v);
    if (value != NULL) {
        *value = v.part_count;
    }
    return NCL_OK;
}

static ncl_err pseudo_program(void *ctx, char *out, size_t cap)
{
    pseudo_view v;

    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    pseudo_view_fill((pseudo_machine *)ctx, &v);
    snprintf(out, cap, "%s", v.program);
    return NCL_OK;
}

/**
 * /CONTROLLER/WARNING：报警表。没有报警是**空表**（不是 null）—— 与新代那边
 * "没有报警正文是空的"一致；条目形状是 [{"number","text"}, ...]，就是册 5 里
 * 报警那个形状（真机上非空条目的布局还没抓到，这里按标准形状给）。
 */
static ncl_err pseudo_warning(void *ctx, ncl_json **value)
{
    pseudo_view v;
    ncl_json *array;

    if (value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    pseudo_view_fill((pseudo_machine *)ctx, &v);
    array = ncl_json_new_array();
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (v.alarm) {
        ncl_json *entry = ncl_json_new_object();
        ncl_json *text = ncl_json_new_string(v.alarm_text);

        if (entry == NULL || text == NULL ||
            ncl_json_obj_set_int(entry, "number", v.alarm_no) != NCL_OK ||
            ncl_json_obj_set(entry, "text", text) != NCL_OK ||
            ncl_json_arr_push(array, entry) != NCL_OK) {
            ncl_json_free(text);
            ncl_json_free(entry);
            ncl_json_free(array);
            return NCL_ERR_NOMEM;
        }
    }
    *value = array;
    return NCL_OK;
}

/* ------------------------------------------------------------------ 覆盖量 -- */

static ncl_err pseudo_line_number(void *ctx, char *out, size_t cap)
{
    pseudo_view v;
    char text[24];

    if (out == NULL || cap == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    pseudo_view_fill((pseudo_machine *)ctx, &v);
    snprintf(text, sizeof(text), "N%lld", v.line_number);
    snprintf(out, cap, "%s", text);
    return NCL_OK;
}

static ncl_err pseudo_feed_override_get(void *ctx, long long *value)
{
    pseudo_machine *m = (pseudo_machine *)ctx;

    ncl_mutex_lock(m->lock);
    if (value != NULL) {
        *value = m->feed_override;
    }
    ncl_mutex_unlock(m->lock);
    return NCL_OK;
}

static ncl_err pseudo_feed_override_set(void *ctx, long long value)
{
    pseudo_machine *m = (pseudo_machine *)ctx;

    if (value < 0 || value > 200) {
        return NCL_ERR_INVALID_ARG; /* 倍率就问 0..200%，真机也是这个范围 */
    }
    ncl_mutex_lock(m->lock);
    m->feed_override = value;
    ncl_mutex_unlock(m->lock);
    return NCL_OK;
}

static ncl_err pseudo_spindle_override_get(void *ctx, long long *value)
{
    return pseudo_feed_override_get(ctx, value); /* 同一格，各自的名字 */
}

static ncl_err pseudo_spindle_override_set(void *ctx, long long value)
{
    pseudo_machine *m = (pseudo_machine *)ctx;

    if (value < 0 || value > 200) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_mutex_lock(m->lock);
    m->spindle_override = value;
    ncl_mutex_unlock(m->lock);
    return NCL_OK;
}

static ncl_err pseudo_feed_speed(void *ctx, double *value)
{
    pseudo_view v;

    pseudo_view_fill((pseudo_machine *)ctx, &v);
    if (value != NULL) {
        *value = v.feed_speed;
    }
    return NCL_OK;
}

static ncl_err pseudo_spindle_speed(void *ctx, double *value)
{
    pseudo_view v;

    pseudo_view_fill((pseudo_machine *)ctx, &v);
    if (value != NULL) {
        *value = v.spindle_speed;
    }
    return NCL_OK;
}

/* -------------------------------------------------------------------- 轴 ---- */

/**
 * 六格里共用的一件事：先看这台机床有没有这个轴。没有就回 NOT_FOUND ——
 * 与新代"控制器没配这个轴就不给数、也不去读别的轴"是同一条规矩。
 */
static ncl_err pseudo_axis_lock(void *ctx, long long arg, pseudo_machine **out,
                                int *index, double *phase, char **reason,
                                const char *which)
{
    pseudo_machine *m = (pseudo_machine *)ctx;
    pseudo_view v;
    char letter = (char)arg;
    int at;

    pseudo_view_fill(m, &v);
    at = pseudo_axis_index(m, letter);
    if (at < 0) {
        return ncl_tool_fail(reason, NCL_ERR_NOT_FOUND,
                             "%s：这台伪机床没有 %c 轴（axes=%s）", which,
                             letter, m->axes);
    }
    *out = m;
    *index = at;
    *phase = v.phase;
    return NCL_OK;
}

/** SCREW/POSITION 与 MOTOR/POSITION：实际位置（机械坐标）。 */
static ncl_err pseudo_axis_machine(void *ctx, long long arg, double *value)
{
    pseudo_machine *m = NULL;
    double phase = 0.0;
    int index = 0;

    if (pseudo_axis_lock(ctx, arg, &m, &index, &phase, NULL, "位置") != NCL_OK) {
        return NCL_ERR_NOT_FOUND;
    }
    if (value != NULL) {
        *value = pseudo_position((size_t)index, phase, m->seed_phase);
    }
    return NCL_OK;
}

/** SERVO_DRIVER/POSITION：指令位置（p 稍靠前的那一点）。 */
static ncl_err pseudo_axis_command(void *ctx, long long arg, double *value)
{
    pseudo_machine *m = NULL;
    double phase = 0.0;
    int index = 0;

    if (pseudo_axis_lock(ctx, arg, &m, &index, &phase, NULL, "位置") != NCL_OK) {
        return NCL_ERR_NOT_FOUND;
    }
    if (value != NULL) {
        *value = pseudo_position((size_t)index, phase + 0.02, m->seed_phase);
    }
    return NCL_OK;
}

/** MOTOR/VARIABLE@ABSOLUTE：工件坐标系里的值（机械坐标 + 零点偏置）。 */
static ncl_err pseudo_axis_absolute(void *ctx, long long arg, double *value)
{
    pseudo_machine *m = NULL;
    double phase = 0.0;
    int index = 0;

    if (pseudo_axis_lock(ctx, arg, &m, &index, &phase, NULL, "位置") != NCL_OK) {
        return NCL_ERR_NOT_FOUND;
    }
    if (value != NULL) {
        *value = pseudo_position((size_t)index, phase, m->seed_phase) +
                 pseudo_work_offset((size_t)index);
    }
    return NCL_OK;
}

/**
 * MOTOR/VARIABLE@RELATIVE / @DISTANCE：相对当前程序段。
 *
 * 一个循环分四段（就当四段程序块）：RELATIVE 是"从这一段起点走了多少"，
 * DISTANCE 是"离这一段终点还剩多少" —— 两格都是段边界上跳一下、段内连续。
 */
static double pseudo_segment_phase(double phase, double *start, double *end)
{
    double scaled = phase * 4.0;
    long long segment = (long long)scaled;

    if (segment > 3) {
        segment = 3;
    }
    *start = (double)segment / 4.0;
    *end = (double)(segment + 1) / 4.0;
    return phase;
}

static ncl_err pseudo_axis_relative(void *ctx, long long arg, double *value)
{
    pseudo_machine *m = NULL;
    double phase = 0.0;
    double start = 0.0;
    double end = 0.0;
    int index = 0;

    if (pseudo_axis_lock(ctx, arg, &m, &index, &phase, NULL, "位置") != NCL_OK) {
        return NCL_ERR_NOT_FOUND;
    }
    (void)pseudo_segment_phase(phase, &start, &end);
    if (value != NULL) {
        *value = pseudo_position((size_t)index, phase, m->seed_phase) -
                 pseudo_position((size_t)index, start, m->seed_phase);
    }
    return NCL_OK;
}

static ncl_err pseudo_axis_distance(void *ctx, long long arg, double *value)
{
    pseudo_machine *m = NULL;
    double phase = 0.0;
    double start = 0.0;
    double end = 0.0;
    int index = 0;

    if (pseudo_axis_lock(ctx, arg, &m, &index, &phase, NULL, "位置") != NCL_OK) {
        return NCL_ERR_NOT_FOUND;
    }
    (void)pseudo_segment_phase(phase, &start, &end);
    if (value != NULL) {
        *value = pseudo_position((size_t)index, end, m->seed_phase) -
                 pseudo_position((size_t)index, phase, m->seed_phase);
    }
    return NCL_OK;
}

/* ------------------------------------------------------------ 调试/遥控 ---- */

/**
 * `/SESSION`：新代那边这条是"会话现在什么样 + 上一次失败的原话"。伪机床没有会话
 * （没有连接），所以这条答的是**模拟器自己的状态**，顺带当遥控器用：
 *
 *   {}                          只看状态
 *   {"status":"running"}        把状态钉死；{"status":""} 交回给相位
 *   {"frozen":true|false}       钉住/放开时钟
 *   {"reset":true}              回到第 0 个循环，计件回起始值，清掉注入的报警
 *   {"seed":42}                 换一条轨迹
 *   {"partCount":100}           改起始计件
 *   {"alarm":{"number":1201,"text":"..."}} / {"alarm":null}   注入/清掉报警
 *   {"feedOverride":50} / {"spindleOverride":120}              拨倍率
 *   {"fail":{"code":-5,"count":3}} / {"fail":null}             注入/停掉失败
 *
 * 全部通过 NC-Link 协议本身拨 —— 上位机不用改配置、不用重启就能把这台假机床
 * 拨到 RUN / ALARM / 慢响应，自动化用例从客户端那一侧就能跑完全流程。
 */
static ncl_err pseudo_session(void *ctx, const ncl_json *params, ncl_json **result,
                              char **reason)
{
    pseudo_machine *m = (pseudo_machine *)ctx;
    const ncl_json *alarm = params != NULL ? ncl_json_obj_get(params, "alarm") : NULL;
    const ncl_json *fail = params != NULL ? ncl_json_obj_get(params, "fail") : NULL;
    const ncl_json *status = params != NULL ? ncl_json_obj_get(params, "status") : NULL;
    pseudo_view v;
    ncl_json *reply;
    long long number = 0;

    if (params != NULL && ncl_json_obj_has(params, "status")) {
        const char *wanted = status != NULL ? ncl_json_as_string(status) : "";

        if (wanted == NULL) {
            wanted = "";
        }
        if (wanted[0] != '\0' && strcmp(wanted, "running") != 0 &&
            strcmp(wanted, "holding") != 0 && strcmp(wanted, "free") != 0) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                 "status 只能是 running / holding / free（空 = 跟着相位走）");
        }
        ncl_mutex_lock(m->lock);
        snprintf(m->forced_status, sizeof(m->forced_status), "%s", wanted);
        ncl_mutex_unlock(m->lock);
    }
    if (params != NULL && ncl_json_obj_has(params, "frozen")) {
        ncl_mutex_lock(m->lock);
        m->frozen = ncl_json_obj_get_bool(params, "frozen", m->frozen);
        ncl_mutex_unlock(m->lock);
    }
    if (params != NULL && ncl_json_obj_has(params, "seed")) {
        ncl_mutex_lock(m->lock);
        m->seed_phase = (double)(ncl_json_obj_get_int(params, "seed", 0) % 1000) / 1000.0;
        ncl_mutex_unlock(m->lock);
    }
    if (params != NULL && ncl_json_obj_has(params, "partCount")) {
        ncl_mutex_lock(m->lock);
        m->part_base = ncl_json_obj_get_int(params, "partCount", m->part_base);
        ncl_mutex_unlock(m->lock);
    }
    if (params != NULL && ncl_json_obj_get_bool(params, "reset", false)) {
        ncl_mutex_lock(m->lock);
        m->t0 = ncl_time_monotonic_millis();
        m->part_base = 0;
        m->inj_alarm = false;
        m->inj_alarm_no = 0;
        m->inj_alarm_text[0] = '\0';
        m->reads = 0;
        ncl_mutex_unlock(m->lock);
    }
    if (params != NULL && ncl_json_obj_has(params, "alarm")) {
        ncl_mutex_lock(m->lock);
        if (alarm == NULL || ncl_json_is_null(alarm)) {
            m->inj_alarm = false;
            m->inj_alarm_no = 0;
            m->inj_alarm_text[0] = '\0';
        } else {
            const char *text = ncl_json_obj_get_string(alarm, "text");

            m->inj_alarm = true;
            m->inj_alarm_no = ncl_json_obj_get_int(alarm, "number", 0);
            snprintf(m->inj_alarm_text, sizeof(m->inj_alarm_text), "%s",
                     text != NULL ? text : "伪机床注入的报警");
        }
        ncl_mutex_unlock(m->lock);
    }
    if (params != NULL && ncl_json_obj_has(params, "feedOverride")) {
        (void)pseudo_feed_override_set(m, ncl_json_obj_get_int(params, "feedOverride", 100));
    }
    if (params != NULL && ncl_json_obj_has(params, "spindleOverride")) {
        (void)pseudo_spindle_override_set(m,
                                          ncl_json_obj_get_int(params, "spindleOverride", 100));
    }
    if (params != NULL && ncl_json_obj_has(params, "fail")) {
        ncl_mutex_lock(m->lock);
        if (fail == NULL || ncl_json_is_null(fail)) {
            m->fail_left = 0;
            m->fail_text[0] = '\0';
        } else {
            const char *text = ncl_json_obj_get_string(fail, "message");

            m->fail_code = ncl_json_obj_get_int(fail, "code", NCL_ERR_IO);
            m->fail_left = ncl_json_obj_get_int(fail, "count", 1);
            snprintf(m->fail_text, sizeof(m->fail_text), "%s",
                     text != NULL ? text : "");
        }
        ncl_mutex_unlock(m->lock);
    }

    pseudo_view_fill(m, &v);
    reply = ncl_json_new_object();
    if (reply == NULL) {
        return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
    }
    /* 前两项与新代的 /SESSION 同名同义（没有连接，所以 open 恒 true）。 */
    (void)ncl_json_obj_set_bool(reply, "open", true);
    (void)ncl_json_obj_set_string(reply, "lastError", m->last_error);
    (void)ncl_json_obj_set_string(reply, "kind", "pseudo");
    (void)ncl_json_obj_set_string(reply, "status", v.status);
    (void)ncl_json_obj_set_int(reply, "cycle", v.cycle);
    (void)ncl_json_obj_set_double(reply, "phase", v.phase);
    (void)ncl_json_obj_set_int(reply, "partCount", v.part_count);
    (void)ncl_json_obj_set_string(reply, "program", v.program);
    (void)ncl_json_obj_set_string(reply, "axes", m->axes);
    (void)ncl_json_obj_set_int(reply, "cycleMs", (long long)m->cycle_ms);
    (void)ncl_json_obj_set_bool(reply, "frozen", m->frozen);
    (void)ncl_json_obj_set_int(reply, "reads", m->reads);
    (void)ncl_json_obj_set_bool(reply, "alarm", v.alarm);
    if (v.alarm) {
        (void)ncl_json_obj_set_int(reply, "alarmNumber", v.alarm_no);
        (void)ncl_json_obj_set_string(reply, "alarmText", v.alarm_text);
    }
    number = (long long)v.feed_override;
    (void)ncl_json_obj_set_int(reply, "feedOverride", number);
    (void)ncl_json_obj_set_int(reply, "spindleOverride", v.spindle_override);
    *result = reply;
    return NCL_OK;
}

/* ------------------------------------------------------- 集合类的公共小件 -- */

/** params.keys：按标准是个数组，也认单个（"321" / 321 都行）。 */
static bool pseudo_key_at(const ncl_json *keys, size_t index, long long *out)
{
    const ncl_json *item = keys;
    size_t count;

    if (keys == NULL) {
        return false;
    }
    count = ncl_json_arr_len(keys);
    if (count > 0) {
        if (index >= count) {
            return false;
        }
        item = ncl_json_arr_get(keys, index);
    } else if (index > 0) {
        return false;
    }
    return item != NULL && ncl_json_as_int(item, out);
}

/** params.keys 里有几个号（单个算一个，没有算零）。 */
static size_t pseudo_key_count(const ncl_json *keys)
{
    size_t count;

    if (keys == NULL) {
        return 0;
    }
    count = ncl_json_arr_len(keys);
    return count > 0 ? count : 1u;
}

/** 元数据表：一组 [名字, 含义] 拼成 [{"name","meaning"}, ...]。 */
static ncl_json *pseudo_fields_json(const char *const fields[][2], size_t count)
{
    ncl_json *array = ncl_json_new_array();
    size_t i;

    if (array == NULL) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        ncl_json *entry = ncl_json_new_object();

        if (entry == NULL ||
            ncl_json_obj_set_string(entry, "name", fields[i][0]) != NCL_OK ||
            ncl_json_obj_set_string(entry, "meaning", fields[i][1]) != NCL_OK ||
            ncl_json_arr_push(array, entry) != NCL_OK) {
            ncl_json_free(entry);
            ncl_json_free(array);
            return NULL;
        }
    }
    return array;
}

/* -------------------------------------------------------------- 参数表 ---- */

/**
 * `/CONTROLLER/PARAMETER`：系统参数。形状与新代逐条一致：
 *
 *   get_keys        参数号清单（字符串）
 *   get_value       按号读值：keys 给号，答 {"1001":800,...}
 *   get_attributes  按号取元数据：[{"no","title","flags","fallback"}, ...]
 *   set_value       写：{"keys":1001,"value":111} 或直接 {"1001":111}
 *
 * 没给 keys 不报错，答空的（轮询与自检会对每个点位盲读一次，报错只会让现场
 * 每次自检都看见一条假的失败）。
 */
static ncl_err pseudo_parameter(void *ctx, const ncl_tool_point *self,
                                ncl_operation op, const ncl_json *params,
                                ncl_json **result, char **reason)
{
    pseudo_machine *m = (pseudo_machine *)ctx;
    const ncl_json *keys = ncl_params_get(params, "keys");
    size_t i;

    (void)self;
    switch (op) {
    case NCL_OP_GET_KEYS: {
        ncl_json *array = ncl_json_new_array();

        if (array == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        ncl_mutex_lock(m->lock);
        for (i = 0; i < m->param_count; i++) {
            char text[16];
            ncl_json *item;

            snprintf(text, sizeof(text), "%d", (int)m->params[i].no);
            item = ncl_json_new_string(text);
            if (item == NULL || ncl_json_arr_push(array, item) != NCL_OK) {
                ncl_json_free(item);
                ncl_json_free(array);
                ncl_mutex_unlock(m->lock);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
        }
        ncl_mutex_unlock(m->lock);
        *result = array;
        return NCL_OK;
    }
    case NCL_OP_GET_VALUE:
    case NCL_OP_GET_ATTRIBUTES: {
        size_t count = pseudo_key_count(keys);
        ncl_json *out;

        if (count == 0) {
            *result = op == NCL_OP_GET_VALUE ? ncl_json_new_object()
                                             : ncl_json_new_array();
            return *result != NULL ? NCL_OK : NCL_ERR_NOMEM;
        }
        if (count > PSEUDO_BATCH_MAX) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "一次最多 %u 个号",
                                 (unsigned)PSEUDO_BATCH_MAX);
        }
        out = op == NCL_OP_GET_VALUE ? ncl_json_new_object() : ncl_json_new_array();
        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        for (i = 0; i < count; i++) {
            long long no = 0;
            const pseudo_param *found = NULL;
            size_t k;

            if (!pseudo_key_at(keys, i, &no)) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "keys 第 %u 个不是参数号", (unsigned)(i + 1));
            }
            ncl_mutex_lock(m->lock);
            for (k = 0; k < m->param_count; k++) {
                if (m->params[k].no == no) {
                    found = &m->params[k];
                    break;
                }
            }
            if (found == NULL) {
                ncl_mutex_unlock(m->lock);
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_NOT_FOUND, "参数 %lld 不在表里",
                                     no);
            }
            if (op == NCL_OP_GET_VALUE) {
                char name[16];
                long long value = found->value;

                snprintf(name, sizeof(name), "%d", (int)no);
                ncl_mutex_unlock(m->lock);
                if (ncl_json_obj_set_int(out, name, value) != NCL_OK) {
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
                }
            } else {
                ncl_json *entry = ncl_json_new_object();
                long long flags = found->flags;
                long long fallback = found->fallback;
                char title[48];

                snprintf(title, sizeof(title), "%s", found->title);
                ncl_mutex_unlock(m->lock);
                if (entry == NULL ||
                    ncl_json_obj_set_int(entry, "no", no) != NCL_OK ||
                    ncl_json_obj_set_string(entry, "title", title) != NCL_OK ||
                    ncl_json_obj_set_int(entry, "flags", flags) != NCL_OK ||
                    ncl_json_obj_set_int(entry, "fallback", fallback) != NCL_OK ||
                    ncl_json_arr_push(out, entry) != NCL_OK) {
                    ncl_json_free(entry);
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
                }
            }
        }
        *result = out;
        return NCL_OK;
    }
    case NCL_OP_SET_VALUE: {
        const ncl_json *value = ncl_params_get(params, "value");
        ncl_json *out;

        if (value != NULL) { /* {"keys":1001,"value":111} */
            long long no = 0;
            long long wanted = 0;
            size_t k;
            bool found = false;

            if (!pseudo_key_at(keys, 0, &no) || !ncl_json_as_int(value, &wanted)) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "要 keys（一个号）+ value（新值）");
            }
            ncl_mutex_lock(m->lock);
            for (k = 0; k < m->param_count; k++) {
                if (m->params[k].no == no) {
                    m->params[k].value = wanted;
                    found = true;
                    break;
                }
            }
            ncl_mutex_unlock(m->lock);
            if (!found) {
                return ncl_tool_fail(reason, NCL_ERR_NOT_FOUND, "参数 %lld 不在表里",
                                     no);
            }
            out = ncl_json_new_object();
            if (out == NULL) {
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
            {
                char name[16];

                snprintf(name, sizeof(name), "%d", (int)no);
                (void)ncl_json_obj_set_int(out, name, wanted);
            }
            *result = out;
            return NCL_OK;
        }
        /* 也可以直接给字典：params 本身就是 {"1001":111,...} */
        out = ncl_json_new_object();
        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        for (i = 0; i < ncl_json_obj_len(params); i++) {
            const char *name = ncl_json_obj_key_at(params, i);
            long long no = 0;
            long long wanted = 0;
            bool found = false;
            size_t k;

            if (name == NULL || strcmp(name, "operation") == 0 ||
                strcmp(name, "keys") == 0 || strcmp(name, "check") == 0 ||
                strcmp(name, "token") == 0 || strcmp(name, "async") == 0) {
                continue; /* 这几个是框架自己的成员 */
            }
            if (i >= PSEUDO_BATCH_MAX) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "一次最多 %u 个号",
                                     (unsigned)PSEUDO_BATCH_MAX);
            }
            if (!ncl_json_as_int(ncl_json_obj_get(params, name), &wanted) ||
                sscanf(name, "%lld", &no) != 1) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "写法是 {参数号:新值} 或 {\"keys\":...,\"value\":...}");
            }
            ncl_mutex_lock(m->lock);
            for (k = 0; k < m->param_count; k++) {
                if (m->params[k].no == no) {
                    m->params[k].value = wanted;
                    found = true;
                    break;
                }
            }
            ncl_mutex_unlock(m->lock);
            if (!found) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_NOT_FOUND, "参数 %lld 不在表里",
                                     no);
            }
            (void)ncl_json_obj_set_int(out, name, wanted);
        }
        *result = out;
        return NCL_OK;
    }
    default:
        return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                             "参数只答 get_keys / get_value / get_attributes / set_value");
    }
}

/* -------------------------------------------------------------- 刀具表 ---- */

/** 一条刀补 -> 元素 JSON（字段名与新代逐条一致）。 */
static ncl_json *pseudo_tool_json(const pseudo_tool *tool, long long no)
{
    ncl_json *obj = ncl_json_new_object();
    ncl_json *geometry = ncl_json_new_array();
    ncl_json *wear = ncl_json_new_array();
    size_t i;

    if (obj == NULL || geometry == NULL || wear == NULL) {
        ncl_json_free(obj);
        ncl_json_free(geometry);
        ncl_json_free(wear);
        return NULL;
    }
    (void)ncl_json_obj_set_int(obj, "id", no);
    (void)ncl_json_obj_set_int(obj, "kind", tool->kind);
    (void)ncl_json_obj_set_double(obj, "radius", tool->radius);
    (void)ncl_json_obj_set_double(obj, "length", tool->length_geometry[0]);
    (void)ncl_json_obj_set_double(obj, "tool_angle", tool->tool_angle);
    (void)ncl_json_obj_set_double(obj, "radius_wear", tool->radius_wear);
    for (i = 0; i < PSEUDO_TOOL_LENGTHS; i++) {
        (void)ncl_json_arr_push(geometry, ncl_json_new_double(tool->length_geometry[i]));
        (void)ncl_json_arr_push(wear, ncl_json_new_double(tool->length_wear[i]));
    }
    (void)ncl_json_obj_set(obj, "length_geometry", geometry);
    (void)ncl_json_obj_set(obj, "length_wear", wear);
    return obj;
}

/**
 * 写刀补：把 value 里的字段盖到一条刀补上。只认 get_attributes 报出来的名字，
 * 多一个就报错（"radiuswear" 这种手滑应该当场被拒，而不是悄悄什么都没写）。
 * `id` 是只读的，给了也照收 —— 读回来的对象改两个字段再写回去是最常见的用法。
 */
static ncl_err pseudo_tool_patch(pseudo_tool *tool, const ncl_json *value,
                                 char **reason)
{
    size_t n = ncl_json_obj_len(value);
    size_t i;

    if (n == 0) {
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                             "value 要是刀补对象，至少给一个字段");
    }
    for (i = 0; i < n; i++) {
        const char *name = ncl_json_obj_key_at(value, i);
        const ncl_json *item = ncl_json_obj_val_at(value, i);
        size_t k;

        if (name == NULL || item == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "value 里有坏字段");
        }
        if (strcmp(name, "id") == 0) {
            continue; /* 刀号由 key 定 */
        }
        if (strcmp(name, "kind") == 0 || strcmp(name, "tool_nose") == 0) {
            long long nose = 0;

            if (!ncl_json_as_int(item, &nose) || nose < -32768 || nose > 32767) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "kind 要是 -32768..32767 的整数");
            }
            tool->kind = nose;
            continue;
        }
        if (strcmp(name, "radius") == 0) {
            if (!ncl_json_as_double(item, &tool->radius)) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "radius 要是数");
            }
            continue;
        }
        if (strcmp(name, "radius_wear") == 0) {
            if (!ncl_json_as_double(item, &tool->radius_wear)) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "radius_wear 要是数");
            }
            continue;
        }
        if (strcmp(name, "tool_angle") == 0) {
            if (!ncl_json_as_double(item, &tool->tool_angle)) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "tool_angle 要是数");
            }
            continue;
        }
        if (strcmp(name, "length") == 0) { /* length = 长度几何第 0 组 */
            if (!ncl_json_as_double(item, &tool->length_geometry[0])) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "length 要是数");
            }
            continue;
        }
        if (strcmp(name, "length_geometry") == 0 || strcmp(name, "length_wear") == 0) {
            double *dst = strcmp(name, "length_geometry") == 0 ? tool->length_geometry
                                                               : tool->length_wear;

            if (ncl_json_arr_len(item) != PSEUDO_TOOL_LENGTHS) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "%s 要正好 %u 个数",
                                     name, (unsigned)PSEUDO_TOOL_LENGTHS);
            }
            for (k = 0; k < PSEUDO_TOOL_LENGTHS; k++) {
                if (!ncl_json_as_double(ncl_json_arr_get(item, k), &dst[k])) {
                    return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                         "%s 第 %u 个不是数", name, (unsigned)(k + 1));
                }
            }
            continue;
        }
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "不认识的字段 %s", name);
    }
    return NCL_OK;
}

/**
 * `/CONTROLLER/TOOL`：刀具表（一条 config，元素就是那把刀的刀补）。
 * 取值形状是 list：答 get_length（有几把）、按号读（keys 给刀号，从 1 起）。
 */
static ncl_err pseudo_tool_table(void *ctx, const ncl_tool_point *self,
                                 ncl_operation op, const ncl_json *params,
                                 ncl_json **result, char **reason)
{
    pseudo_machine *m = (pseudo_machine *)ctx;
    const ncl_json *keys = ncl_params_get(params, "keys");
    size_t i;

    (void)self;
    switch (op) {
    case NCL_OP_GET_LENGTH:
        ncl_mutex_lock(m->lock);
        i = m->tool_count;
        ncl_mutex_unlock(m->lock);
        return ncl_tool_reply_int(result, (long long)i);

    case NCL_OP_GET_ATTRIBUTES: {
        static const char *const fields[][2] = {
            {"id", "刀具编号（就是 key）"},
            {"kind", "种类：这里取刀尖号 ToolNose（册 4 的 kind 待再确认）"},
            {"radius", "半径几何（RadiusGeometry）"},
            {"length", "长度几何第 0 组（LengthGeometry[0]）"},
            {"tool_angle", "刀尖角（ToolAngle）"},
            {"radius_wear", "半径磨损（RadiusWear）"},
            {"length_geometry", "长度几何 12 组（LengthGeometry[0..11]）"},
            {"length_wear", "长度磨损 12 组（LengthWear[0..11]）"},
            {"time_usage", "寿命：伪机床没有寿命模型，不给"},
        };
        ncl_json *array = pseudo_fields_json(fields, sizeof(fields) / sizeof(fields[0]));

        if (array == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        *result = array;
        return NCL_OK;
    }
    case NCL_OP_GET_VALUE: {
        size_t n = pseudo_key_count(keys);
        ncl_json *out = ncl_json_new_object();

        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        if (n == 0) { /* 盲读（轮询/自检）：答空的，不报错 */
            *result = out;
            return NCL_OK;
        }
        if (n > PSEUDO_BATCH_MAX) {
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "一次最多 %u 把刀",
                                 (unsigned)PSEUDO_BATCH_MAX);
        }
        for (i = 0; i < n; i++) {
            long long no = 0;
            pseudo_tool copy;
            ncl_json *entry;
            char name[16];

            if (!pseudo_key_at(keys, i, &no) || no < 1) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "keys 里第 %u 个不是刀号", (unsigned)(i + 1));
            }
            ncl_mutex_lock(m->lock);
            if ((size_t)no > m->tool_count) {
                ncl_mutex_unlock(m->lock);
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_NOT_FOUND, "刀号 %lld 不在表里",
                                     no);
            }
            copy = m->tools[(size_t)no - 1];
            ncl_mutex_unlock(m->lock);
            entry = pseudo_tool_json(&copy, no);
            snprintf(name, sizeof(name), "%d", (int)no);
            if (entry == NULL || ncl_json_obj_set(out, name, entry) != NCL_OK) {
                ncl_json_free(entry);
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
        }
        *result = out;
        return NCL_OK;
    }
    case NCL_OP_SET_VALUE: {
        const ncl_json *value = ncl_params_get(params, "value");
        long long no = 0;
        ncl_json *out;
        ncl_json *entry;
        char name[16];
        ncl_err rc;

        if (!pseudo_key_at(keys, 0, &no) || no < 1) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                 "要 keys=一个刀号（从 1 起）");
        }
        if (value == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "要 value=一条刀补对象");
        }
        /* 先读回当前值打底、再让给到的字段覆盖：只写一两个字段不会把别的抹掉。 */
        ncl_mutex_lock(m->lock);
        if ((size_t)no > m->tool_count) {
            ncl_mutex_unlock(m->lock);
            return ncl_tool_fail(reason, NCL_ERR_NOT_FOUND, "刀号 %lld 不在表里", no);
        }
        {
            pseudo_tool tool = m->tools[(size_t)no - 1];

            ncl_mutex_unlock(m->lock);
            rc = pseudo_tool_patch(&tool, value, reason);
            if (rc != NCL_OK) {
                return rc;
            }
            ncl_mutex_lock(m->lock);
            m->tools[(size_t)no - 1] = tool;
            ncl_mutex_unlock(m->lock);
            out = ncl_json_new_object();
            entry = pseudo_tool_json(&tool, no);
        }
        if (out == NULL || entry == NULL) {
            ncl_json_free(entry);
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        snprintf(name, sizeof(name), "%d", (int)no);
        if (ncl_json_obj_set(out, name, entry) != NCL_OK) {
            ncl_json_free(entry);
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        *result = out;
        return NCL_OK;
    }
    default:
        return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                             "刀具表答 get_length / get_value / set_value / get_attributes");
    }
}

/* ------------------------------------------------------------ PLC 寄存器 -- */

/** 位族字母 -> 数组下标；R 或不懂的字母回 -1。 */
static int pseudo_bit_family(char family)
{
    switch (family) {
    case 'I':
        return PSEUDO_BIT_I;
    case 'O':
        return PSEUDO_BIT_O;
    case 'C':
        return PSEUDO_BIT_C;
    case 'S':
        return PSEUDO_BIT_S;
    case 'A':
        return PSEUDO_BIT_A;
    default:
        return -1;
    }
}

/**
 * `/CONTROLLER/REGISTER@<族>`：PLC 寄存器与位，一族一条（族在 self->arg 上）。
 *
 *   取值形状是按号排的表（LIST）：答 get_length，不答 get_keys；get_value 给号
 *   答 {"<号>":值}（R 是 u32 整数、位是 true/false）；set_value 同样是
 *   {"keys":号,"value":新值}。
 *
 * 位里 I0 / I1 由这台假机床自己驱动（运行中 / 有报警），其余位和 R 寄存器认写。
 * 真机上 I 位也是现场梯形图驱动的，写了读回来不一定变 —— 这里只保留其中一条
 * 现实的味道，免得伪装得太乖。
 */
static ncl_err pseudo_register_table(void *ctx, const ncl_tool_point *self,
                                     ncl_operation op, const ncl_json *params,
                                     ncl_json **result, char **reason)
{
    pseudo_machine *m = (pseudo_machine *)ctx;
    const ncl_json *keys = ncl_params_get(params, "keys");
    const char *family_text = (const char *)self->arg;
    char family;
    int slot;
    size_t count;
    size_t i;

    if (family_text == NULL || family_text[0] == '\0') {
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "这条点位没带族（@R/@I…）");
    }
    family = family_text[0];
    if (family >= 'a' && family <= 'z') {
        family = (char)(family - 'a' + 'A');
    }
    slot = pseudo_bit_family(family);
    if (family != 'R' && slot < 0) {
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "不认识的族 %c（R/I/O/C/S/A）",
                             family);
    }
    ncl_mutex_lock(m->lock);
    count = family == 'R' ? m->reg_count : m->bit_count[slot];
    ncl_mutex_unlock(m->lock);

    switch (op) {
    case NCL_OP_GET_LENGTH:
        return ncl_tool_reply_int(result, (long long)count);

    case NCL_OP_GET_ATTRIBUTES: {
        const char *const fields[][2] = {
            {"number", "号（从 0 起，就是 keys）"},
            {family == 'R' ? "value" : "value",
             family == 'R' ? "寄存器值（u32）" : "位（true/false）"},
            {"count", "这一族有多少个（= get_length）"},
        };
        ncl_json *array = pseudo_fields_json(fields, sizeof(fields) / sizeof(fields[0]));

        if (array == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        *result = array;
        return NCL_OK;
    }
    case NCL_OP_GET_VALUE: {
        size_t n = pseudo_key_count(keys);
        ncl_json *out = ncl_json_new_object();

        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        if (n == 0) { /* 空 keys：不猜，答空的 */
            *result = out;
            return NCL_OK;
        }
        if (n > PSEUDO_BATCH_MAX) {
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "一次最多 %u 个号",
                                 (unsigned)PSEUDO_BATCH_MAX);
        }
        for (i = 0; i < n; i++) {
            long long no = 0;
            char name[24];

            if (!pseudo_key_at(keys, i, &no) || no < 0 ||
                (unsigned long long)no >= (unsigned long long)count) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "keys 第 %u 个不是 %c 族的号（0..%u）",
                                     (unsigned)(i + 1), family,
                                     (unsigned)(count - 1u));
            }
            snprintf(name, sizeof(name), "%d", (int)no);
            if (family == 'R') {
                uint32_t value = 0;

                ncl_mutex_lock(m->lock);
                value = m->regs[no];
                ncl_mutex_unlock(m->lock);
                if (ncl_json_obj_set_int(out, name, (long long)value) != NCL_OK) {
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
                }
            } else {
                bool bit = false;
                pseudo_view v;

                if (family == 'I' && (no == 0 || no == 1)) {
                    pseudo_view_fill(m, &v);
                    bit = no == 0 ? strcmp(v.status, "running") == 0 : v.alarm;
                } else {
                    ncl_mutex_lock(m->lock);
                    bit = m->bits[slot][no];
                    ncl_mutex_unlock(m->lock);
                }
                if (ncl_json_obj_set_bool(out, name, bit) != NCL_OK) {
                    ncl_json_free(out);
                    return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
                }
            }
        }
        *result = out;
        return NCL_OK;
    }
    case NCL_OP_SET_VALUE: {
        const ncl_json *value = ncl_params_get(params, "value");
        long long no = 0;
        ncl_json *out;
        char name[24];

        if (!pseudo_key_at(keys, 0, &no) || no < 0 ||
            (unsigned long long)no >= (unsigned long long)count) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                 "要 keys=%c 族的一个号（0..%u）", family,
                                 (unsigned)(count - 1u));
        }
        if (value == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "要 value=新值");
        }
        if (family == 'R') {
            long long raw = 0;

            if (!ncl_json_as_int(value, &raw) || raw < 0 || raw > 0xFFFFFFFFll) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "R 是 32 位无符号，value 给整数");
            }
            ncl_mutex_lock(m->lock);
            m->regs[no] = (uint32_t)raw;
            ncl_mutex_unlock(m->lock);
        } else {
            bool bit = false;

            if (!ncl_json_as_bool(value, &bit)) {
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "位只能写 true / false");
            }
            ncl_mutex_lock(m->lock);
            m->bits[slot][no] = bit;
            ncl_mutex_unlock(m->lock);
        }
        /* 答"我写下去的值"：认不认要自己读回来看（真机上的规矩也是这样）。 */
        out = ncl_json_new_object();
        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        snprintf(name, sizeof(name), "%d", (int)no);
        if (ncl_json_obj_set(out, name, ncl_json_clone(value)) != NCL_OK) {
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        *result = out;
        return NCL_OK;
    }
    default:
        return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                             "寄存器表答 get_length / get_value / set_value / get_attributes");
    }
}

/* -------------------------------------------------------------- 变量表 ---- */

/**
 * `/CONTROLLER/VARIABLE`：变量表（key 就是程序里的 #号）。值按类型给整数或浮点，
 * "空"的号答 `{"id":n,"type":0}` —— 不硬凑一个 0 出来（新代那边就是这个口径）。
 */
static ncl_err pseudo_variable_table(void *ctx, const ncl_tool_point *self,
                                     ncl_operation op, const ncl_json *params,
                                     ncl_json **result, char **reason)
{
    pseudo_machine *m = (pseudo_machine *)ctx;
    const ncl_json *keys = ncl_params_get(params, "keys");
    size_t i;

    (void)self;
    switch (op) {
    case NCL_OP_GET_LENGTH:
        ncl_mutex_lock(m->lock);
        i = m->var_count;
        ncl_mutex_unlock(m->lock);
        return ncl_tool_reply_int(result, (long long)i);

    case NCL_OP_GET_ATTRIBUTES: {
        static const char *const fields[][2] = {
            {"id", "变量号（就是程序里的 #号）"},
            {"value", "值：整数或浮点"},
            {"type", "控制器说的类型：0 空、1 整数、2 浮点"},
        };
        ncl_json *array = pseudo_fields_json(fields, sizeof(fields) / sizeof(fields[0]));

        if (array == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        *result = array;
        return NCL_OK;
    }
    case NCL_OP_GET_VALUE: {
        size_t n = pseudo_key_count(keys);
        ncl_json *out = ncl_json_new_object();

        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        if (n == 0) {
            *result = out;
            return NCL_OK;
        }
        if (n > PSEUDO_BATCH_MAX) {
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "一次最多 %u 个号",
                                 (unsigned)PSEUDO_BATCH_MAX);
        }
        for (i = 0; i < n; i++) {
            long long no = 0;
            pseudo_variable copy;
            ncl_json *entry;
            char name[24];

            if (!pseudo_key_at(keys, i, &no) || no < 0) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                     "keys 第 %u 个不是变量号", (unsigned)(i + 1));
            }
            ncl_mutex_lock(m->lock);
            if ((size_t)no >= m->var_count) {
                ncl_mutex_unlock(m->lock);
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_NOT_FOUND, "变量 #%lld 不在表里",
                                     no);
            }
            copy = m->vars[(size_t)no];
            ncl_mutex_unlock(m->lock);
            entry = ncl_json_new_object();
            if (entry == NULL) {
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
            (void)ncl_json_obj_set_int(entry, "id", no);
            (void)ncl_json_obj_set_int(entry, "type", copy.type);
            if (copy.type == 1) {
                (void)ncl_json_obj_set_int(entry, "value", copy.int_value);
            } else if (copy.type == 2) {
                (void)ncl_json_obj_set_double(entry, "value", copy.double_value);
            }
            snprintf(name, sizeof(name), "%d", (int)no);
            if (ncl_json_obj_set(out, name, entry) != NCL_OK) {
                ncl_json_free(entry);
                ncl_json_free(out);
                return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
            }
        }
        *result = out;
        return NCL_OK;
    }
    case NCL_OP_SET_VALUE: {
        const ncl_json *value = ncl_params_get(params, "value");
        long long no = 0;
        long long as_int = 0;
        double dbl = 0.0;
        bool is_double;
        ncl_json *out;
        char name[24];

        if (!pseudo_key_at(keys, 0, &no) || no < 0) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                 "要 keys=一个变量号（程序里的 #号）");
        }
        if (value == NULL || !ncl_json_as_double(value, &dbl)) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "要 value=新值（数）");
        }
        /* 整数还是浮点：先看字面量里有没有小数点/指数（"2.5" 与 "2" 分得开）。 */
        is_double = !(ncl_json_as_int(value, &as_int) && (double)as_int == dbl);
        {
            const char *raw = ncl_json_number_raw(value);

            if (raw != NULL) {
                is_double = strpbrk(raw, ".eE") != NULL;
            }
        }
        if (!is_double && (dbl > 2147483647.0 || dbl < -2147483648.0)) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                 "整数变体是 32 位，超了就用浮点写");
        }
        ncl_mutex_lock(m->lock);
        if ((size_t)no >= m->var_count) {
            ncl_mutex_unlock(m->lock);
            return ncl_tool_fail(reason, NCL_ERR_NOT_FOUND, "变量 #%lld 不在表里", no);
        }
        m->vars[(size_t)no].type = is_double ? 2 : 1;
        m->vars[(size_t)no].int_value = (long long)dbl;
        m->vars[(size_t)no].double_value = dbl;
        ncl_mutex_unlock(m->lock);
        out = ncl_json_new_object();
        if (out == NULL) {
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        snprintf(name, sizeof(name), "%d", (int)no);
        if (ncl_json_obj_set(out, name, ncl_json_clone(value)) != NCL_OK) {
            ncl_json_free(out);
            return ncl_tool_fail(reason, NCL_ERR_NOMEM, "内存不足");
        }
        *result = out;
        return NCL_OK;
    }
    default:
        return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED,
                             "变量表答 get_length / get_value / set_value / get_attributes");
    }
}

/* ------------------------------------------------------------ 开局与收场 -- */

/** 默认参数表：一台普通立式加工中心会有的那几项。 */
static void pseudo_default_params(pseudo_machine *m)
{
    static const struct {
        long long   no;
        const char *title;
        long long   value;
    } k_default[] = {
        {1001, "X 轴行程（0.001mm）", 800000},
        {1002, "Y 轴行程（0.001mm）", 500000},
        {1003, "Z 轴行程（0.001mm）", 400000},
        {1004, "主轴最高转速（rpm）", 8000},
        {1005, "快速移动速度（mm/min）", 24000},
        {1006, "切削进给上限（mm/min）", 5000},
        {1007, "刀库刀位数", 24},
        {1008, "换刀时间（ms）", 1500},
        {1009, "X 轴丝杠螺距（0.001mm）", 10000},
        {1010, "Y 轴丝杠螺距（0.001mm）", 10000},
        {1011, "Z 轴丝杠螺距（0.001mm）", 10000},
        {1012, "累计开机时间（h）", 0},
    };
    size_t i;

    for (i = 0; i < sizeof(k_default) / sizeof(k_default[0]); i++) {
        pseudo_param *p = &m->params[m->param_count++];

        p->no = k_default[i].no;
        snprintf(p->title, sizeof(p->title), "%s", k_default[i].title);
        p->value = k_default[i].value;
        p->flags = 0;
        p->fallback = 0;
    }
}

/** 配置里给的参数表盖到默认表上（没见过的号就追加）。 */
static void pseudo_apply_params(pseudo_machine *m, const ncl_json *list)
{
    size_t n = ncl_json_arr_len(list);
    size_t i;

    for (i = 0; i < n && m->param_count < PSEUDO_MAX_PARAMS; i++) {
        const ncl_json *item = ncl_json_arr_get(list, i);
        long long no = ncl_json_obj_get_int(item, "no", 0);
        const char *title = ncl_json_obj_get_string(item, "title");
        long long value = ncl_json_obj_get_int(item, "value", 0);
        pseudo_param *slot = NULL;
        size_t k;

        if (no <= 0) {
            continue;
        }
        for (k = 0; k < m->param_count; k++) {
            if (m->params[k].no == no) {
                slot = &m->params[k];
                break;
            }
        }
        if (slot == NULL) {
            slot = &m->params[m->param_count++];
            slot->no = no;
            slot->flags = 0;
            slot->fallback = 0;
            slot->title[0] = '\0';
        }
        if (title != NULL) {
            snprintf(slot->title, sizeof(slot->title), "%s", title);
        }
        slot->value = value;
    }
}

/** 开局：把配置读进来，把模拟器支起来。 */
static void *pseudo_open(const ncl_json *params, char **err)
{
    pseudo_machine *m = (pseudo_machine *)ncl_mem_calloc(1, sizeof(*m));
    const ncl_json *programs;
    const ncl_json *muted;
    const ncl_json *regs;
    const ncl_json *alarms;
    size_t i;

    if (m == NULL) {
        if (err != NULL) {
            (void)ncl_asprintf(err, "内存不足：伪机床起不来");
        }
        return NULL;
    }
    m->lock = ncl_mutex_create();
    if (m->lock == NULL) {
        ncl_free_safe(m);
        if (err != NULL) {
            (void)ncl_asprintf(err, "内存不足：伪机床的锁建不起来");
        }
        return NULL;
    }
    m->t0 = ncl_time_monotonic_millis();

    /* 机器的性子 */
    m->cycle_ms = (unsigned)ncl_tool_param_int(params, "cycleMs", 8000);
    if (m->cycle_ms == 0) {
        m->cycle_ms = 8000;
    }
    snprintf(m->axes, sizeof(m->axes), "%s",
             ncl_tool_param_str(params, "axes", "XYZC"));
    m->frozen = ncl_tool_param_bool(params, "frozen", false);
    m->seed_phase = (double)(ncl_tool_param_int(params, "seed", 0) % 1000) / 1000.0;
    if (m->seed_phase < 0.0) {
        m->seed_phase += 1.0;
    }
    m->part_base = ncl_tool_param_int(params, "partCount", 0);
    m->feed_rate = (double)ncl_tool_param_int(params, "feedRate", 800);
    m->spindle_rpm = (double)ncl_tool_param_int(params, "spindleRpm", 1200);
    m->feed_override = ncl_tool_param_int(params, "feedOverride", 100);
    m->spindle_override = ncl_tool_param_int(params, "spindleOverride", 100);
    snprintf(m->forced_status, sizeof(m->forced_status), "%s",
             ncl_tool_param_str(params, "status", ""));

    /* 程序名表 */
    programs = params != NULL ? ncl_json_obj_get(params, "programs") : NULL;
    if (programs != NULL && ncl_json_type_of(programs) == NCL_JSON_ARRAY) {
        size_t n = ncl_json_arr_len(programs);

        for (i = 0; i < n && m->program_count < PSEUDO_MAX_PROGRAMS; i++) {
            const char *text = ncl_json_as_string(ncl_json_arr_get(programs, i));

            if (text != NULL && text[0] != '\0') {
                snprintf(m->programs[m->program_count], 24, "%s", text);
                m->program_count++;
            }
        }
    }
    if (m->program_count == 0) {
        snprintf(m->programs[0], 24, "%s", "O0001");
        m->program_count = 1;
    }

    /* 报警节奏 */
    m->alarm_every = ncl_tool_param_int(params, "alarmEvery", 0);
    m->alarm_for = ncl_tool_param_int(params, "alarmFor", 1);
    m->alarm_no = 1201;
    snprintf(m->alarm_text, sizeof(m->alarm_text), "%s", "伺服轴过载（伪）");
    alarms = params != NULL ? ncl_json_obj_get(params, "alarms") : NULL;
    if (alarms != NULL && ncl_json_type_of(alarms) == NCL_JSON_ARRAY &&
        ncl_json_arr_len(alarms) > 0) {
        const ncl_json *first = ncl_json_arr_get(alarms, 0);
        const char *text = ncl_json_obj_get_string(first, "text");

        m->alarm_no = ncl_json_obj_get_int(first, "number", m->alarm_no);
        if (text != NULL) {
            snprintf(m->alarm_text, sizeof(m->alarm_text), "%s", text);
        }
    }

    /* 注入的开关 */
    m->latency_ms = ncl_tool_param_int(params, "latencyMs", 0);
    muted = params != NULL ? ncl_json_obj_get(params, "unavailable") : NULL;
    if (muted != NULL && ncl_json_type_of(muted) == NCL_JSON_ARRAY) {
        size_t n = ncl_json_arr_len(muted);

        for (i = 0; i < n && m->muted_count < PSEUDO_MAX_MUTED; i++) {
            const char *path = ncl_json_as_string(ncl_json_arr_get(muted, i));

            if (path != NULL && path[0] != '\0') {
                snprintf(m->muted[m->muted_count], sizeof(m->muted[0]), "%s", path);
                m->muted_count++;
            }
        }
    }
    if (params != NULL && ncl_json_obj_has(params, "fail")) {
        const ncl_json *fail = ncl_json_obj_get(params, "fail");
        const char *text = ncl_json_obj_get_string(fail, "message");

        m->fail_code = ncl_json_obj_get_int(fail, "code", NCL_ERR_IO);
        m->fail_left = ncl_json_obj_get_int(fail, "count", 1);
        snprintf(m->fail_text, sizeof(m->fail_text), "%s", text != NULL ? text : "");
    }

    /* "控制器内存"：默认一台普通机床该有的样子 */
    pseudo_default_params(m);
    if (params != NULL && ncl_json_obj_has(params, "params")) {
        const ncl_json *list = ncl_json_obj_get(params, "params");

        if (list != NULL && ncl_json_type_of(list) == NCL_JSON_ARRAY) {
            pseudo_apply_params(m, list);
        }
    }
    m->tool_count = (size_t)ncl_tool_param_int(params, "tools", 8);
    if (m->tool_count > PSEUDO_MAX_TOOLS) {
        m->tool_count = PSEUDO_MAX_TOOLS;
    }
    for (i = 0; i < m->tool_count; i++) {
        pseudo_tool *t = &m->tools[i];
        size_t k;

        t->kind = 3;                                    /* 刀尖半径补偿 */
        t->radius = 0.4 + 0.2 * (double)(i % 4u);
        t->tool_angle = 80.0;
        t->radius_wear = 0.0;
        for (k = 0; k < PSEUDO_TOOL_LENGTHS; k++) {
            t->length_geometry[k] = k == 0 ? 100.0 + 5.0 * (double)i : 0.0;
            t->length_wear[k] = 0.0;
        }
    }
    m->reg_count = (size_t)ncl_tool_param_int(params, "registers", 64);
    if (m->reg_count > PSEUDO_MAX_REGS) {
        m->reg_count = PSEUDO_MAX_REGS;
    }
    regs = params != NULL ? ncl_json_obj_get(params, "registers") : NULL;
    if (regs != NULL && ncl_json_type_of(regs) == NCL_JSON_OBJECT) {
        long long count = ncl_json_obj_get_int(regs, "R", (long long)m->reg_count);

        if (count > 0 && count <= PSEUDO_MAX_REGS) {
            m->reg_count = (size_t)count;
        }
    }
    {
        static const char k_families[] = "IOCSA";
        static const long long k_defaults[] = {32, 32, 32, 32, 16};

        for (i = 0; i < PSEUDO_BIT_FAMILIES; i++) {
            long long count = k_defaults[i];
            char key[2];

            key[0] = k_families[i];
            key[1] = '\0';
            if (regs != NULL && ncl_json_type_of(regs) == NCL_JSON_OBJECT) {
                count = ncl_json_obj_get_int(regs, key, count);
            }
            if (count < 0) {
                count = 0;
            }
            if (count > PSEUDO_MAX_BITS) {
                count = PSEUDO_MAX_BITS;
            }
            m->bit_count[i] = (size_t)count;
        }
    }
    for (i = 0; i < m->reg_count; i++) {
        m->regs[i] = i == 1 ? 1234u : 0u; /* R1 摆一个数，看着像有东西在动 */
    }
    m->var_count = (size_t)ncl_tool_param_int(params, "variables", 64);
    if (m->var_count > PSEUDO_MAX_VARS) {
        m->var_count = PSEUDO_MAX_VARS;
    }
    for (i = 0; i < m->var_count; i++) {
        if (i >= 1 && i <= 8) {
            m->vars[i].type = 2;
            m->vars[i].double_value = 1.5 * (double)i;
            m->vars[i].int_value = (long long)m->vars[i].double_value;
        } else {
            m->vars[i].type = 0; /* 空号：不硬凑一个 0 出来 */
        }
    }
    return m;
}

static void pseudo_close(void *ctx)
{
    pseudo_machine *m = (pseudo_machine *)ctx;

    if (m == NULL) {
        return;
    }
    ncl_mutex_destroy(m->lock);
    ncl_free_safe(m);
}

/* ------------------------------------------------------------------ 声明 ---- */

/* 轴：九个字母都占位，每轴六格。路径写死、轴号在读值的时候现查（与新代同一条规矩）。 */
#define PSEUDO_AXIS_POINTS(NAME, LETTER)                                       \
    NCL_DATAITEM("/AXIS@" NAME "/SCREW/POSITION", pseudo_axis_dispatch,         \
                 PSEUDO_AXIS_ARG(LETTER, PSEUDO_CELL_SCREW))                   \
    NCL_DATAITEM("/AXIS@" NAME "/SERVO_DRIVER/POSITION", pseudo_axis_dispatch,  \
                 PSEUDO_AXIS_ARG(LETTER, PSEUDO_CELL_SERVO))                   \
    NCL_DATAITEM("/AXIS@" NAME "/MOTOR/POSITION", pseudo_axis_dispatch,         \
                 PSEUDO_AXIS_ARG(LETTER, PSEUDO_CELL_MOTOR))                   \
    NCL_DATAITEM("/AXIS@" NAME "/MOTOR/VARIABLE@ABSOLUTE",                      \
                 pseudo_axis_dispatch, PSEUDO_AXIS_ARG(LETTER, PSEUDO_CELL_ABSOLUTE)) \
    NCL_DATAITEM("/AXIS@" NAME "/MOTOR/VARIABLE@RELATIVE",                      \
                 pseudo_axis_dispatch, PSEUDO_AXIS_ARG(LETTER, PSEUDO_CELL_RELATIVE)) \
    NCL_DATAITEM("/AXIS@" NAME "/MOTOR/VARIABLE@DISTANCE",                      \
                 pseudo_axis_dispatch, PSEUDO_AXIS_ARG(LETTER, PSEUDO_CELL_DISTANCE))

/* 寄存器六族：R/I/C/S 可写，O/A 只读（与新代的权限口径一致，权限本身在外面控）。 */
#define PSEUDO_REGISTER_OPS                                                    \
    (NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_GET_LENGTH) |            \
     NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES))
#define PSEUDO_REGISTER_RW_OPS (PSEUDO_REGISTER_OPS | NCL_OP_BIT(NCL_OP_SET_VALUE))
#define PSEUDO_REGISTER_POINTS(FAMILY, OPS)                                    \
    NCL_CONFIG_OPS("/CONTROLLER/REGISTER@" FAMILY, pseudo_register_table,      \
                   FAMILY, OPS)

NCL_TOOL_BEGIN("pseudo", "伪机床：内置模拟器，点位模型照新代摆（不接硬件）", "MACHINE",
               1000, 1000, pseudo_open, pseudo_close)

    /* 采样通道四样，与新代同一套口径：状态、计件、程序名、报警。 */
    NCL_DATAITEM_SAMPLED("/STATUS", pseudo_data_dispatch, NULL)
    NCL_DATAITEM_SAMPLED("/PART_COUNT", pseudo_data_dispatch, NULL)
    NCL_DATAITEM_SAMPLED("/CONTROLLER/PROGRAM", pseudo_data_dispatch, NULL)
    NCL_DATAITEM_SAMPLED("/CONTROLLER/WARNING", pseudo_data_dispatch, NULL)

    /* 覆盖量：倍率这两条伪机床开了写（新代那边只读），方便上位机把它拨到任意值。 */
    NCL_DATAITEM("/CONTROLLER/LINE_NUMBER", pseudo_data_dispatch, NULL)
    NCL_DATAITEM_RW("/FEED_OVERRIDE", pseudo_data_dispatch, NULL)
    NCL_DATAITEM_RW("/SPINDLE_OVERRIDE", pseudo_data_dispatch, NULL)
    NCL_DATAITEM("/FEED_SPEED", pseudo_data_dispatch, NULL)
    NCL_DATAITEM("/SPINDLE_SPEED", pseudo_data_dispatch, NULL)

    PSEUDO_AXIS_POINTS("X", 'X')
    PSEUDO_AXIS_POINTS("Y", 'Y')
    PSEUDO_AXIS_POINTS("Z", 'Z')
    PSEUDO_AXIS_POINTS("A", 'A')
    PSEUDO_AXIS_POINTS("B", 'B')
    PSEUDO_AXIS_POINTS("C", 'C')
    PSEUDO_AXIS_POINTS("U", 'U')
    PSEUDO_AXIS_POINTS("V", 'V')
    PSEUDO_AXIS_POINTS("W", 'W')

    NCL_METHOD_CALL("/SESSION", pseudo_session)

    NCL_CONFIG_OPS("/CONTROLLER/PARAMETER", pseudo_parameter, NULL,
                   NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_SET_VALUE) |
                       NCL_OP_BIT(NCL_OP_GET_KEYS) |
                       NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES))

    NCL_CONFIG_OPS("/CONTROLLER/TOOL", pseudo_tool_table, NULL,
                   NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_SET_VALUE) |
                       NCL_OP_BIT(NCL_OP_GET_LENGTH) |
                       NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES))

    PSEUDO_REGISTER_POINTS("R", PSEUDO_REGISTER_RW_OPS)
    PSEUDO_REGISTER_POINTS("I", PSEUDO_REGISTER_RW_OPS)
    PSEUDO_REGISTER_POINTS("O", PSEUDO_REGISTER_OPS)
    PSEUDO_REGISTER_POINTS("C", PSEUDO_REGISTER_RW_OPS)
    PSEUDO_REGISTER_POINTS("S", PSEUDO_REGISTER_RW_OPS)
    PSEUDO_REGISTER_POINTS("A", PSEUDO_REGISTER_OPS)

    NCL_CONFIG_OPS("/CONTROLLER/VARIABLE", pseudo_variable_table, NULL,
                   NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_SET_VALUE) |
                       NCL_OP_BIT(NCL_OP_GET_LENGTH) |
                       NCL_OP_BIT(NCL_OP_GET_ATTRIBUTES))

NCL_TOOL_END()

NCL_TOOL_MODULE("0.1.0", "伪机床（pseudo）：内置模拟器 + 新代形状的点位模型，零硬件")
