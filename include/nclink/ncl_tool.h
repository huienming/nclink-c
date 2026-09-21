/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - 一个文件就是一个适配器。
 *
 * This is the header an adapter author reads, and the only one: the point map
 * lives in code, so writing an adapter is writing one .c file.
 *
 *     #include "nclink/ncl_tool.h"
 *
 *     static void *open_box(const ncl_json *params, char **err) { ... }
 *     static void  close_box(void *ctx) { ... }
 *
 *     static ncl_err status(void *ctx, const ncl_tool_point *self,
 *                           ncl_operation op, const ncl_json *params,
 *                           ncl_json **result, char **reason) { ... }
 *
 *     NCL_TOOL_BEGIN("cnc", "FANUC 数控机床", 1000, 1000, open_box, close_box)
 *         NCL_DATAITEM_SAMPLED("/MACHINE/STATUS@RUN", status, NULL)
 *         NCL_DATAITEM_RW("/MACHINE/MODE", mode, NULL)
 *         NCL_CONFIG("/MACHINE/CONTROLLER/PARAMETER", parameter, NULL)
 *         NCL_METHOD("/MACHINE/RESET", reset, NULL)
 *     NCL_TOOL_END()
 *
 * The declaration is data - which model path exists, what may be done with it,
 * and which function serves it - and the host does the rest with it:
 *
 *   - ncl_tool_model() turns it into the model document (one data item per
 *     point, plus one sample channel when the declaration asks for sampling),
 *     which is what the device publishes and what the schema is derived from;
 *   - ncl_tool_register() opens the connection once and binds every declared
 *     operation ("<operation>#<path>" -> the point's function);
 *   - everything else - MQTT, REST, sampling, audit, the model file - stays the
 *     host's business and is not something an adapter touches.
 *
 * Where an adapter's arguments arrive, since every kind has them:
 *
 *   - the configuration's "parameters" object (host, port, timeout, unit, ...)
 *     goes to open() once, and what open() returns is handed to every point;
 *   - the *request*'s parameters (a Query's params, a Set's "value", a method
 *     call's arguments) arrive as the handler's @p params, untouched;
 *   - the point's own data - the register address, the FOCAS item name, the
 *     entry of a mapping table - is written next to the path (NCL_DATAITEM and
 *     friends) and comes back as @p self->arg, so one dispatch function can
 *     serve a whole table of points and still know which entry it is on.
 *
 * A point declares which **operations** it answers - the standard's Query
 * (get_value, get_length, get_keys, get_attributes), its Set (set_value,
 * add, delete) and a method call, plus the status / result / cancel that
 * follow one (册 5 §5.2.7/§5.2.8). One path may answer several of them -
 * "read the mode, and write it" is one point with both bits - and
 * `sampled` asks the host to put the path in the sample channel (which
 * means the point has to answer get_value).
 *
 * A point that changes something is readable as well: the validator refuses
 * a point whose editing operation (NCL_OP_WRITE_MASK) is not next to
 * get_value. The trail wants the old value of a write and a write nobody
 * can read back is a write nobody can confirm. What a device only takes as
 * a command - a password, a reset pulse, a clear - is not a data object at
 * all: declare it with NCL_METHOD and let the call's params carry the value.
 *
 * Two deliberate omissions:
 *
 *   - there is no point map in the configuration file. A site with different
 *     addresses writes its own adapter: the address sits in the same file as
 *     the code that uses it, where a compiler checks it.
 *   - there is no driver/ops table to fill in. A point names one function with
 *     one calling convention; an operation the point does not declare is
 *     answered by the host as NCL_ERR_NOT_SUPPORTED, without calling anything.
 */
#ifndef NCL_TOOL_H
#define NCL_TOOL_H

#include <stddef.h>

#include "nclink/ncl_common.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Author facing --------------------------------------------------------- */

/**
 * Open the connection. @p params is the module's "parameters" object (may be
 * NULL when the configuration carries none). On failure return NULL and, when
 * @p err is not NULL, hand back a heap message (ncl_free_safe()) the host logs
 * verbatim.
 */
typedef void *(*ncl_tool_open_fn)(const ncl_json *params, char **err);

/** Release what open() returned. Called once, NULL is accepted. */
typedef void (*ncl_tool_close_fn)(void *ctx);

typedef struct ncl_tool_point ncl_tool_point;

/**
 * A point's function.
 *
 * @param ctx    what open() returned, shared by every point of the tool
 * @param self   the point being served (path, arg, declared operations)
 * @param op     NCL_OP_GET_VALUE, NCL_OP_SET_VALUE or NCL_OP_FUNC_CALL - which
 *               of the point's operations this call is. The host never calls
 *               an operation the point did not declare.
 * @param params the request's parameters: a Query's params, a Set's "value",
 *               a method call's arguments. NULL when the request had none.
 * @param result on success, the value to answer with (NULL = no value)
 * @param reason on failure, optionally a heap message for the client
 */
typedef ncl_err (*ncl_point_fn)(void *ctx, const ncl_tool_point *self,
                               ncl_operation op, const ncl_json *params,
                               ncl_json **result, char **reason);

/**
 * The frames of the last exchange, for the audit trail (§6 of the spec asks for
 * the bytes of every request). Borrowed from the module, which keeps them until
 * its next exchange; either frame may be NULL.
 */
typedef struct {
    const void *request;
    size_t      request_len;
    const void *reply;
    size_t      reply_len;
} ncl_tool_frames;

/**
 * Optional: hand the host the frames of the last exchange. An adapter that sits
 * on a protocol client (see clients/) implements it in three lines with the
 * driver's ncl_driver_last_raw(); an adapter without frames of its own leaves it
 * out. This is the *only* audit related thing an author ever writes - the trail
 * itself is the host's business.
 */
typedef void (*ncl_tool_last_raw_fn)(void *ctx, ncl_tool_frames *out);

/** One declared point; the macros below fill it in. */
struct ncl_tool_point {
    /** Model path, e.g. "/MACHINE/STATUS@RUN". Also how the point is addressed. */
    const char *path;
    /**
     * The operations this point answers: one bit per ncl_operation, written
     * NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_SET_VALUE) | ... - the
     * standard's Query / Set operations and a method call (册 5 §5.2.7/§5.2.8).
     * The macros below fill it in; a point that names no value operation
     * (NCL_OP_VALUE_MASK) is a **method** (ncl_tool_point_is_method()).
     *
     * Editing an operation changes something, so it is only allowed next to
     * GET_VALUE (NCL_OP_WRITE_MASK implies GET_VALUE): the trail wants the old
     * value of a write and what cannot be read back cannot be confirmed. A
     * device that only takes a command gets a method instead.
     */
    unsigned    ops;
    /** Ask the host to sample this path (the point has to be readable). */
    bool        sampled;
    /**
     * The function serving this point. Required.
     *
     * A point whose protocol call has not been implemented yet is declared like
     * any other: this function is there, and it answers NCL_ERR_UNAVAILABLE
     * (nclink/ncl_common.h) until the frame is captured. The tool layer turns
     * that into the standard "还读不了" answer, keeps the point out of the
     * polling rounds and out of the trail, and the self check reports it as
     * "待抓包" instead of "failed". Implementing the call later changes this one
     * function in the client - 点位表一行都不用动。
     */
    ncl_point_fn fn;
    /**
     * True for the second kind of data object (册 3 §5.4/§5.5): a **config** -
     * a parameter, a coordinate system, a tool table, the object's own meta
     * data. Such a point goes into the model's "configs", can be read and
     * written on demand, and - 册 3 表 1 注 b - **must not be a sample source**,
     * which is why only the NCL_CONFIG_* family can declare one (it has no
     * SAMPLED form) and why the validator refuses a sampled config.
     *
     * False is the default and the first kind: a **data item** - a physical
     * quantity or something the device senses (NCL_DATAITEM_*), which the sample
     * channel may pick up.
     */
    bool config;
    /**
     * The author's own data for this point - a register address, a mapping
     * table entry, a protocol item name. Borrowed: the host never reads or
     * writes it, it only hands it back through @p self. NULL is fine. It is
     * const so a read only table needs no cast at the declaration; an adapter
     * that parks something it mutates here (a cache, a last value) casts it
     * back in its own handler.
     */
    const void *arg;
};

/**
 * True when the point answers @p op. The host asks this before it binds an
 * operation, and it is what makes a declaration's point table the whole
 * truth about a point: an operation nobody named is never called.
 */
static inline bool ncl_tool_point_handles(const ncl_tool_point *point,
                                          ncl_operation op)
{
    return point != NULL && (point->ops & NCL_OP_BIT(op)) != 0;
}

/**
 * True when the point is a **method**: it names no value operation, so a
 * call is the only way to reach it. A method is not a data object - it is
 * in no model and in no sample channel - which is why the model writer and
 * an adapter's point list both ask here.
 */
static inline bool ncl_tool_point_is_method(const ncl_tool_point *point)
{
    return point == NULL || (point->ops & NCL_OP_VALUE_MASK) == 0;
}

/**
 * One tool: what the host needs to build a model, a sample channel and the
 * bindings. NCL_TOOL_BEGIN/NCL_TOOL_END fill it in; its fields and its points
 * are borrowed and must outlive the server (a static declaration does).
 */
typedef struct {
    /** Tool name: the sample channel is named after it, and a method call is
     *  addressed as "<name>/<point name>". */
    const char           *name;
    /** One line description, for `--plugins` and the schema. */
    const char           *description;
    /**
     * 设备节点的 type（表 1 的设备对象类型：MACHINE / ROBOT / …），也是每条点位
     * 路径隐含的第一段。**只在这里定义一次**：点位路径相对设备节点写（"/STATUS"），
     * 模型里的绝对路径 "/MACHINE/STATUS" 由它拼出来（ncl_tool_model_path()）。
     */
    const char           *device_type;
    /**
     * Sample channel period in ms; 0 means "no sample channel from this
     * declaration" (a site can still add one to the model file, which is where
     * the running period ends up being tuned).
     */
    long long             sample_ms;
    /** Upload period of the sample channel in ms (0 = same as sample_ms). */
    long long             upload_ms;
    ncl_tool_open_fn      open;
    ncl_tool_close_fn     close;
    /** Optional frames for the audit trail; NULL when the protocol has none. */
    ncl_tool_last_raw_fn  last_raw;
    const ncl_tool_point *points;
    size_t                point_count;
} ncl_tool_decl;

/*
 * The declaration macros. One tool per file - that is the whole point - so the
 * generated names can be file scope and the author never sees them.
 *
 * **路径是相对设备节点的**：写 "/STATUS"、"/AXIS@X/POSITION@REAL"，不写设备段。
 * 设备段在 NCL_TOOL_BEGIN 的第三个参数里定义一次（这台设备在模型里是什么：表 1 的
 * MACHINE / ROBOT / …），换机型只改那一处，点位表不动；也不可能出现"路径说
 * MACHINE、配置说 ROBOT"这种两处打架。路径可以任意深：中间每一段都是模型里的一个
 * 组件（可以嵌套），最后一段是这个数据对象。
 * 运行时（模型里的路径、采样通道、REST 地址）仍是绝对路径 "/MACHINE/STATUS"，
 * 由设备段在生成模型时拼上（ncl_tool_model_path()）。
 *
 * 声明宏：一个点位一行，先写**哪一类数据对象**（册 3 §5.4/§5.5），再写它能被
 * 怎么访问 —— 访问就是标准第 5 部分那些操作：Query（get_value / get_length /
 * get_keys / get_attributes）、Set（set_value / add / delete）和方法调用（call）。
 *
 *   NCL_DATAITEM(路径, 函数, 数据)   dataItem：物理量、感知量 —— 能进采样通道
 *   NCL_CONFIG(路径, 函数, 数据)     config：参数、坐标系、刀具表、文件… —— 不进采样通道
 *   NCL_METHOD(路径, 函数, 数据)     方法：不是数据对象，只响应调用
 *
 * 基本形之外还有这些形状：
 *
 *   _SAMPLED        并进默认采样通道（**只有 dataItem 有** —— 册 3 表 1 注 b：
 *                   配置中的数据对象不得作为采样数据源）
 *   _RW             可读可写（get_value | set_value）
 *   _OPS            自报操作集：集合类数据对象（list / dict）要 get_length、
 *                   get_keys、get_attributes、add、delete（册 5 表 11 / 表 13），
 *                   就把要的操作按位写全，例如
 *                   NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_ADD)
 *
 * 两条校验兜底（手写这张表也拦得住）：**可写必然可读**（set_value / add / delete
 * 都要求 get_value —— 审计要记写之前的旧值，读不回来的写也没法确认，真只写的
 * 东西写成 NCL_METHOD，值走方法调用的参数）；**config 不许进采样通道**。
 *
 * 点位自己的数据（寄存器地址、协议项名、映射表条目）写在第三个参数上；点位没有自己的
 * 数据就写 NULL。一个 dispatch 服务整张表时，靠 self->arg 分辨自己落在哪一行。
 *
 *   NCL_DATAITEM_SAMPLED("/STATUS", dispatch, &k_status)
 *   NCL_DATAITEM_RW("/MODE", dispatch, &k_mode)
 *   NCL_CONFIG_RW("/CONTROLLER/PARAMETER", dispatch, &k_param)
 *   NCL_METHOD("/RESET", dispatch, NULL)
 *
 * **协议调用还没实现的点位照样这么写** —— 它和别的点位没有第二种形状。函数先绑
 * 上，client 那边（clients/ 里的语义函数）在帧抓到之前回 **NCL_ERR_UNAVAILABLE**
 * （见 ncl_common.h）：模型里有这条路径（现场看得见它要来）、问它答"还读不了"、
 * 轮询与 §6 审计都不碰它、自检把它算成"待抓包"而不是失败。等抓包补上，**只改
 * client 里那个函数**，这张表一行都不用动。
 *
 *   NCL_DATAITEM_STR_SAMPLED("/WARNING", ncl_focas_alarm)
 *   NCL_DATAITEM_F64("/AXIS@X/POSITION@CMD", ncl_focas_axis_position_cmd,
 *                    NCL_FOCAS_AXIS_X)
 *
 * **点位名字从路径自动推**：'@' 换 '_'、'/' 换 '.' ——
 * /AXIS@X/POSITION@REAL -> AXIS_X.POSITION_REAL；方法调用地址是
 * "<工具名>/<点位名>"（focas/AXIS_X.POSITION_REAL），绑定键是 "<操作>#<路径>"。
 * 路径唯一，推出来的名字就唯一，所以不用手写名字。
 */
#define NCL_DATAITEM(path_literal, fn, arg)                                    \
    { path_literal, NCL_OP_BIT(NCL_OP_GET_VALUE), false, fn, false, arg },

#define NCL_DATAITEM_SAMPLED(path_literal, fn, arg)                            \
    { path_literal, NCL_OP_BIT(NCL_OP_GET_VALUE), true, fn, false, arg },

#define NCL_DATAITEM_RW(path_literal, fn, arg)                                 \
    { path_literal, NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_SET_VALUE), \
      false, fn, false, arg },

/** 自报操作集：集合类数据对象用它写清 get_length / get_keys / add / delete 这些。 */
#define NCL_DATAITEM_OPS(path_literal, fn, arg, ops_value)                     \
    { path_literal, (ops_value), false, fn, false, arg },

#define NCL_CONFIG(path_literal, fn, arg)                                      \
    { path_literal, NCL_OP_BIT(NCL_OP_GET_VALUE), false, fn, true, arg },

#define NCL_CONFIG_RW(path_literal, fn, arg)                                   \
    { path_literal, NCL_OP_BIT(NCL_OP_GET_VALUE) |                             \
          NCL_OP_BIT(NCL_OP_SET_VALUE), false, fn, true, arg },

/** 同上，操作集自己写：文件（dict）、刀具表（list）这类集合就是用它。 */
#define NCL_CONFIG_OPS(path_literal, fn, arg, ops_value)                       \
    { path_literal, (ops_value), false, fn, true, arg },

#define NCL_METHOD(path_literal, fn, arg)                                      \
    { path_literal, NCL_OP_BIT(NCL_OP_FUNC_CALL), false, fn, false, arg },

/* ==================================== 同一族的另一种形状：绑取值/置值函数 ==== */

/*
 * 上面那些形状是"自己写 dispatch"。同一族的第二种形状是**绑 client 的语义函数**：
 * 现场不写函数，只写"这条路径绑哪个函数"。
 *
 *     NCL_DATAITEM_STR_SAMPLED("/MACHINE/STATUS",           ncl_focas_status)
 *     NCL_DATAITEM_I64_SAMPLED("/MACHINE/PART_COUNT",       ncl_focas_part_count)
 *     NCL_DATAITEM_F64("/MACHINE/AXIS@X/POSITION@REAL",     ncl_focas_axis_position,
 *                      NCL_FOCAS_AXIS_X)
 *     NCL_CONFIG_I64_RW("/MACHINE/CONTROLLER/PARAMETER@1",  limit_get, limit_set)
 *
 * 名字就是把两个轴拼起来，没有第三套词汇：
 *
 *   族      NCL_DATAITEM_*（进 dataItems，可采样）/ NCL_CONFIG_*（进 configs，不许采样）
 *   类型    I64 / F64 / BOOL / STR / JSON（JSON 给的不是标量：报警的
 *           {"number","text"}、刀具表这样的表，整个值就是 client 交出来的那个 JSON）
 *           —— 宏名里的类型就是取值函数的出参类型
 *   访问    裸（只读）/ _SAMPLED（并进默认采样通道）/ _RW（可读可写）
 *   现场参数 给了第三个参数就自动走带参数那支（轴号、子项…），不用记第二个名字
 *
 * 三条约定，都是为了"少想"：
 *
 *   1. 实例就是 open() 返回的那个指针，所以这些宏里不写实例名 —— 也就不可能绑到
 *      一个没有实例的函数上。"必须有一个 client 实例"是结构上的，不是纪律。
 *   2. 函数名即语义：ncl_focas_part_count() 读回来的就是加工件数，没有人需要解释
 *      "RDCOUNT" 是什么。
 *   3. 要自己的解释（状态推导、单位换算、把两个量凑成一个）就用上面那些形状自己写
 *      函数，里面照样能调 client 的底层函数；两种写法可以混在同一张表里。
 *
 * 取值/置值函数的签名固定（_SAMPLED 只存在于 dataItem 族：配置不许采样）：
 *
 *     ncl_err get(void *instance, long long *value);        NCL_DATAITEM_I64
 *     ncl_err get(void *instance, double    *value);        NCL_DATAITEM_F64
 *     ncl_err get(void *instance, bool      *value);        NCL_DATAITEM_BOOL
 *     ncl_err get(void *instance, char *out, size_t cap);   NCL_DATAITEM_STR
 *     ncl_err set(void *instance, 同类型的值);               *_RW 的第二个函数
 *     ncl_err get(void *instance, long long arg, 出参);      给了现场参数时的形状
 *     ncl_err get(void *instance, ncl_json **value);        *_JSON
 *     ncl_err fn(void *instance, const ncl_json *params,
 *                ncl_json **result, char **reason);        NCL_METHOD_CALL
 *
 * 读/写失败 → 应答 code=NG，理由里带路径与错误码；要给出协议自己的原因（例如
 * "EW_PROTOCOL -17"）就在 client 里留一个 xxx_last_error()，再自己写函数用
 * ncl_tool_fail() 把它带上。
 */
typedef ncl_err (*ncl_tool_get_i64_fn)(void *instance, long long *value);
typedef ncl_err (*ncl_tool_set_i64_fn)(void *instance, long long value);
typedef ncl_err (*ncl_tool_get_i64_arg_fn)(void *instance, long long arg,
                                           long long *value);
typedef ncl_err (*ncl_tool_get_f64_fn)(void *instance, double *value);
typedef ncl_err (*ncl_tool_set_f64_fn)(void *instance, double value);
typedef ncl_err (*ncl_tool_get_f64_arg_fn)(void *instance, long long arg,
                                           double *value);
typedef ncl_err (*ncl_tool_get_bool_fn)(void *instance, bool *value);
typedef ncl_err (*ncl_tool_get_bool_arg_fn)(void *instance, long long arg,
                                            bool *value);
/** 文本出参：@p out 至少 @p cap 字节；实现要自己截断并保证 NUL 结尾。 */
typedef ncl_err (*ncl_tool_get_str_fn)(void *instance, char *out, size_t cap);
typedef ncl_err (*ncl_tool_set_str_fn)(void *instance, const char *value);
typedef ncl_err (*ncl_tool_get_str_arg_fn)(void *instance, long long arg,
                                           char *out, size_t cap);
/**
 * 结构化出参：值本身就是一个 JSON（list / dict），实现把所有权交给宿主，失败时
 * 不留下东西。值不是标量的点位用它：报警（NCL_DATAITEM_JSON）与表型的配置
 * （刀具表、坐标系…，NCL_CONFIG_JSON）。
 */
typedef ncl_err (*ncl_tool_get_json_fn)(void *instance, ncl_json **value);
/** 方法：和 ncl_point_fn 同一个形状，少了 self（method 自己就是那条路径）。 */
typedef ncl_err (*ncl_tool_method_fn)(void *instance, const ncl_json *params,
                                      ncl_json **result, char **reason);

/**
 * 一个绑定带了什么：取值/置值函数 + 一个现场概念的参数（轴号、子项号…）。宏负责
 * 填对字段，作者不直接写这个结构；绑定函数按族的签名解释指针（和 ncl_library_*
 * 从 dlsym 拿符号是同一类转换）。
 */
typedef struct {
    const void *get; /**< 按族解释的取值函数，可为 NULL（点位答"不支持"）   */
    const void *set; /**< 按族解释的置值函数，可为 NULL（点位只读）        */
    long long   arg; /**< 传给 *_ARG 族取值函数的现场参数；其它族忽略     */
} ncl_tool_value_spec;

/* 语义函数的实现（src/tool/simple.c）：每个族一个，把出参转成 JSON 应答；带现场
 * 参数的那一族（*_ARG）用各自的包装，因为函数签名不同。 */
ncl_err ncl_tool_value_i64(void *ctx, const ncl_tool_point *self,
                          ncl_operation op, const ncl_json *params,
                          ncl_json **result, char **reason);
ncl_err ncl_tool_value_i64_arg(void *ctx, const ncl_tool_point *self,
                              ncl_operation op, const ncl_json *params,
                              ncl_json **result, char **reason);
ncl_err ncl_tool_value_f64(void *ctx, const ncl_tool_point *self,
                          ncl_operation op, const ncl_json *params,
                          ncl_json **result, char **reason);
ncl_err ncl_tool_value_f64_arg(void *ctx, const ncl_tool_point *self,
                              ncl_operation op, const ncl_json *params,
                              ncl_json **result, char **reason);
ncl_err ncl_tool_value_bool(void *ctx, const ncl_tool_point *self,
                           ncl_operation op, const ncl_json *params,
                           ncl_json **result, char **reason);
ncl_err ncl_tool_value_bool_arg(void *ctx, const ncl_tool_point *self,
                               ncl_operation op, const ncl_json *params,
                               ncl_json **result, char **reason);
ncl_err ncl_tool_value_str(void *ctx, const ncl_tool_point *self,
                          ncl_operation op, const ncl_json *params,
                          ncl_json **result, char **reason);
ncl_err ncl_tool_value_str_arg(void *ctx, const ncl_tool_point *self,
                              ncl_operation op, const ncl_json *params,
                              ncl_json **result, char **reason);
ncl_err ncl_tool_value_json(void *ctx, const ncl_tool_point *self,
                            ncl_operation op, const ncl_json *params,
                            ncl_json **result, char **reason);
ncl_err ncl_tool_value_method(void *ctx, const ncl_tool_point *self,
                             ncl_operation op, const ncl_json *params,
                             ncl_json **result, char **reason);

/** 一行 = 一个绑定点位（ops / sampled / 包装函数 / 绑定内容）。 */
#define NCL_POINT_ROW(path_literal, ops_value, sampled_value, bind_fn, spec_expr) \
    { path_literal, (ops_value), (sampled_value), (bind_fn), false,              \
      (spec_expr) },

/** 同上，但这一行是**配置型数据对象**（config，见下）。 */
#define NCL_POINT_CONFIG_ROW(path_literal, ops_value, bind_fn, spec_expr)         \
    { path_literal, (ops_value), false, (bind_fn), true, (spec_expr) },

#define NCL_POINT_READ(path_literal, sampled_value, bind_fn, getter, arg_value)   \
    NCL_POINT_ROW(path_literal, NCL_OP_BIT(NCL_OP_GET_VALUE), sampled_value,      \
                 bind_fn,                                                        \
                 (&(const ncl_tool_value_spec){(const void *)(getter), NULL,      \
                                              (long long)(arg_value)}))

#define NCL_POINT_WRITE(path_literal, sampled_value, bind_fn, getter, setter)     \
    NCL_POINT_ROW(path_literal,                                                   \
                 NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_SET_VALUE),    \
                 sampled_value, bind_fn,                                         \
                 (&(const ncl_tool_value_spec){(const void *)(getter),            \
                                              (const void *)(setter), 0}))

#define NCL_POINT_CONFIG_READ(path_literal, bind_fn, getter, arg_value)           \
    NCL_POINT_CONFIG_ROW(path_literal, NCL_OP_BIT(NCL_OP_GET_VALUE), bind_fn,     \
                        (&(const ncl_tool_value_spec){(const void *)(getter),     \
                                                      NULL,                      \
                                                      (long long)(arg_value)}))

#define NCL_POINT_CONFIG_WRITE(path_literal, bind_fn, getter, setter)             \
    NCL_POINT_CONFIG_ROW(                                                         \
        path_literal,                                                            \
        NCL_OP_BIT(NCL_OP_GET_VALUE) | NCL_OP_BIT(NCL_OP_SET_VALUE), bind_fn,    \
        (&(const ncl_tool_value_spec){(const void *)(getter),                     \
                                      (const void *)(setter), 0}))

/*
 * 一个概念一个名字：给了现场参数（轴号、子项）就用带参数那支实现，没给就用普通那支。
 * 现场只记 NCL_<族>_<类型>[_SAMPLED|_RW]，不需要记两套名字。
 */
#define NCL_POINT_SHAPE(_1, _2, _3, NAME, ...) NAME

/* 整数 --------------------------------------------------------------------- */
#define NCL_DATAITEM_I64(...)                                                      \
    NCL_POINT_SHAPE(__VA_ARGS__, NCL_DATAITEM_I64_AT_, NCL_DATAITEM_I64_PLAIN_)          \
    (__VA_ARGS__)
#define NCL_DATAITEM_I64_PLAIN_(path_literal, getter)                              \
    NCL_POINT_READ(path_literal, false, ncl_tool_value_i64, getter, 0)
#define NCL_DATAITEM_I64_AT_(path_literal, getter, arg_value)                      \
    NCL_POINT_READ(path_literal, false, ncl_tool_value_i64_arg, getter, arg_value)

#define NCL_DATAITEM_I64_SAMPLED(...)                                              \
    NCL_POINT_SHAPE(__VA_ARGS__, NCL_DATAITEM_I64_SAMPLED_AT_,                       \
                  NCL_DATAITEM_I64_SAMPLED_PLAIN_)(__VA_ARGS__)
#define NCL_DATAITEM_I64_SAMPLED_PLAIN_(path_literal, getter)                      \
    NCL_POINT_READ(path_literal, true, ncl_tool_value_i64, getter, 0)
#define NCL_DATAITEM_I64_SAMPLED_AT_(path_literal, getter, arg_value)              \
    NCL_POINT_READ(path_literal, true, ncl_tool_value_i64_arg, getter, arg_value)

#define NCL_DATAITEM_I64_RW(path_literal, getter, setter)                          \
    NCL_POINT_WRITE(path_literal, false, ncl_tool_value_i64, getter, setter)

/* 浮点 --------------------------------------------------------------------- */
#define NCL_DATAITEM_F64(...)                                                      \
    NCL_POINT_SHAPE(__VA_ARGS__, NCL_DATAITEM_F64_AT_, NCL_DATAITEM_F64_PLAIN_)          \
    (__VA_ARGS__)
#define NCL_DATAITEM_F64_PLAIN_(path_literal, getter)                              \
    NCL_POINT_READ(path_literal, false, ncl_tool_value_f64, getter, 0)
#define NCL_DATAITEM_F64_AT_(path_literal, getter, arg_value)                      \
    NCL_POINT_READ(path_literal, false, ncl_tool_value_f64_arg, getter, arg_value)

#define NCL_DATAITEM_F64_SAMPLED(...)                                              \
    NCL_POINT_SHAPE(__VA_ARGS__, NCL_DATAITEM_F64_SAMPLED_AT_,                       \
                  NCL_DATAITEM_F64_SAMPLED_PLAIN_)(__VA_ARGS__)
#define NCL_DATAITEM_F64_SAMPLED_PLAIN_(path_literal, getter)                      \
    NCL_POINT_READ(path_literal, true, ncl_tool_value_f64, getter, 0)
#define NCL_DATAITEM_F64_SAMPLED_AT_(path_literal, getter, arg_value)              \
    NCL_POINT_READ(path_literal, true, ncl_tool_value_f64_arg, getter, arg_value)

#define NCL_DATAITEM_F64_RW(path_literal, getter, setter)                          \
    NCL_POINT_WRITE(path_literal, false, ncl_tool_value_f64, getter, setter)

/* 布尔 --------------------------------------------------------------------- */
#define NCL_DATAITEM_BOOL(...)                                                     \
    NCL_POINT_SHAPE(__VA_ARGS__, NCL_DATAITEM_BOOL_AT_, NCL_DATAITEM_BOOL_PLAIN_)        \
    (__VA_ARGS__)
#define NCL_DATAITEM_BOOL_PLAIN_(path_literal, getter)                             \
    NCL_POINT_READ(path_literal, false, ncl_tool_value_bool, getter, 0)
#define NCL_DATAITEM_BOOL_AT_(path_literal, getter, arg_value)                     \
    NCL_POINT_READ(path_literal, false, ncl_tool_value_bool_arg, getter, arg_value)

#define NCL_DATAITEM_BOOL_SAMPLED(...)                                             \
    NCL_POINT_SHAPE(__VA_ARGS__, NCL_DATAITEM_BOOL_SAMPLED_AT_,                      \
                  NCL_DATAITEM_BOOL_SAMPLED_PLAIN_)(__VA_ARGS__)
#define NCL_DATAITEM_BOOL_SAMPLED_PLAIN_(path_literal, getter)                     \
    NCL_POINT_READ(path_literal, true, ncl_tool_value_bool, getter, 0)
#define NCL_DATAITEM_BOOL_SAMPLED_AT_(path_literal, getter, arg_value)             \
    NCL_POINT_READ(path_literal, true, ncl_tool_value_bool_arg, getter, arg_value)

/* 文本 --------------------------------------------------------------------- */
#define NCL_DATAITEM_STR(...)                                                      \
    NCL_POINT_SHAPE(__VA_ARGS__, NCL_DATAITEM_STR_AT_, NCL_DATAITEM_STR_PLAIN_)          \
    (__VA_ARGS__)
#define NCL_DATAITEM_STR_PLAIN_(path_literal, getter)                              \
    NCL_POINT_READ(path_literal, false, ncl_tool_value_str, getter, 0)
#define NCL_DATAITEM_STR_AT_(path_literal, getter, arg_value)                      \
    NCL_POINT_READ(path_literal, false, ncl_tool_value_str_arg, getter, arg_value)

#define NCL_DATAITEM_STR_SAMPLED(...)                                              \
    NCL_POINT_SHAPE(__VA_ARGS__, NCL_DATAITEM_STR_SAMPLED_AT_,                       \
                  NCL_DATAITEM_STR_SAMPLED_PLAIN_)(__VA_ARGS__)
#define NCL_DATAITEM_STR_SAMPLED_PLAIN_(path_literal, getter)                      \
    NCL_POINT_READ(path_literal, true, ncl_tool_value_str, getter, 0)
#define NCL_DATAITEM_STR_SAMPLED_AT_(path_literal, getter, arg_value)              \
    NCL_POINT_READ(path_literal, true, ncl_tool_value_str_arg, getter, arg_value)

#define NCL_DATAITEM_STR_RW(path_literal, getter, setter)                          \
    NCL_POINT_WRITE(path_literal, false, ncl_tool_value_str, getter, setter)

/* 结构化 --------------------------------------------------------------- */

/* 值不是标量，是 client 交出来的一个 JSON（报警的 {"number","text"}、表…）。
 * 没有 _RW 形状：client 侧还没有 JSON 的置值函数。 */
#define NCL_DATAITEM_JSON(path_literal, getter)                                    \
    NCL_POINT_READ(path_literal, false, ncl_tool_value_json, getter, 0)
#define NCL_DATAITEM_JSON_SAMPLED(path_literal, getter)                            \
    NCL_POINT_READ(path_literal, true, ncl_tool_value_json, getter, 0)

/* 方法 --------------------------------------------------------------------- */
#define NCL_METHOD_CALL(path_literal, fn)                                      \
    NCL_POINT_ROW(path_literal, NCL_OP_BIT(NCL_OP_FUNC_CALL), false,             \
                 ncl_tool_value_method,                                          \
                 (&(const ncl_tool_value_spec){(const void *)(fn), NULL, 0}))

/*
 * 配置型数据对象（config：参数、坐标系、刀具表、对象自己的元信息）的绑定。
 *
 * 和上面那几个是同一件事，只差"这一条进模型的 configs 而不是 dataItems"：
 * 册 3 §5.4/§5.5 把数据对象分成两类，config 按表 1 注 b **不得作为采样数据源**，
 * 所以这一族故意没有 _SAMPLED 形状（声明档的校验器也会拒收采样配置）。
 * 形状与上面一致：给了现场参数就走带参数那支。
 */
#define NCL_CONFIG_I64(...)                                               \
    NCL_POINT_SHAPE(__VA_ARGS__, NCL_CONFIG_I64_AT_,                        \
                  NCL_CONFIG_I64_PLAIN_)(__VA_ARGS__)
#define NCL_CONFIG_I64_PLAIN_(path_literal, getter)                       \
    NCL_POINT_CONFIG_READ(path_literal, ncl_tool_value_i64, getter, 0)
#define NCL_CONFIG_I64_AT_(path_literal, getter, arg_value)               \
    NCL_POINT_CONFIG_READ(path_literal, ncl_tool_value_i64_arg, getter, arg_value)

#define NCL_CONFIG_F64(...)                                               \
    NCL_POINT_SHAPE(__VA_ARGS__, NCL_CONFIG_F64_AT_,                        \
                  NCL_CONFIG_F64_PLAIN_)(__VA_ARGS__)
#define NCL_CONFIG_F64_PLAIN_(path_literal, getter)                       \
    NCL_POINT_CONFIG_READ(path_literal, ncl_tool_value_f64, getter, 0)
#define NCL_CONFIG_F64_AT_(path_literal, getter, arg_value)               \
    NCL_POINT_CONFIG_READ(path_literal, ncl_tool_value_f64_arg, getter, arg_value)

#define NCL_CONFIG_BOOL(...)                                              \
    NCL_POINT_SHAPE(__VA_ARGS__, NCL_CONFIG_BOOL_AT_,                       \
                  NCL_CONFIG_BOOL_PLAIN_)(__VA_ARGS__)
#define NCL_CONFIG_BOOL_PLAIN_(path_literal, getter)                      \
    NCL_POINT_CONFIG_READ(path_literal, ncl_tool_value_bool, getter, 0)
#define NCL_CONFIG_BOOL_AT_(path_literal, getter, arg_value)              \
    NCL_POINT_CONFIG_READ(path_literal, ncl_tool_value_bool_arg, getter, arg_value)

#define NCL_CONFIG_STR(...)                                               \
    NCL_POINT_SHAPE(__VA_ARGS__, NCL_CONFIG_STR_AT_,                        \
                  NCL_CONFIG_STR_PLAIN_)(__VA_ARGS__)
#define NCL_CONFIG_STR_PLAIN_(path_literal, getter)                       \
    NCL_POINT_CONFIG_READ(path_literal, ncl_tool_value_str, getter, 0)
#define NCL_CONFIG_STR_AT_(path_literal, getter, arg_value)               \
    NCL_POINT_CONFIG_READ(path_literal, ncl_tool_value_str_arg, getter, arg_value)

/* 可读可写的配置（参数、坐标系…）：读回来能改回去，审计里才有旧值。 */
#define NCL_CONFIG_I64_RW(path_literal, getter, setter)                   \
    NCL_POINT_CONFIG_WRITE(path_literal, ncl_tool_value_i64, getter, setter)
#define NCL_CONFIG_F64_RW(path_literal, getter, setter)                   \
    NCL_POINT_CONFIG_WRITE(path_literal, ncl_tool_value_f64, getter, setter)
#define NCL_CONFIG_STR_RW(path_literal, getter, setter)                   \
    NCL_POINT_CONFIG_WRITE(path_literal, ncl_tool_value_str, getter, setter)
/* 表型的配置（刀具表、坐标系…）：整个值是 client 交出来的一个 JSON（list/dict）。 */
#define NCL_CONFIG_JSON(path_literal, getter)                            \
    NCL_POINT_CONFIG_READ(path_literal, ncl_tool_value_json, getter, 0)

#define NCL_TOOL_BEGIN(name_literal, description_literal, device_type_literal, \
                       sample_ms_value, upload_ms_value, open_fn, close_fn)    \
    static const char ncl_tool_name_[] = name_literal;                         \
    static const char ncl_tool_description_[] = description_literal;           \
    static const char ncl_tool_device_type_[] = device_type_literal;           \
    static const long long ncl_tool_sample_ms_ = (sample_ms_value);            \
    static const long long ncl_tool_upload_ms_ = (upload_ms_value);            \
    static ncl_tool_open_fn const ncl_tool_open_ = (open_fn);                  \
    static ncl_tool_close_fn const ncl_tool_close_ = (close_fn);               \
    static const ncl_tool_point ncl_tool_points_[] = {

/*
 * NCL_TOOL_END closes the block. NCL_TOOL_END_WITH_RAW does the same and adds
 * the optional frames callback, so the audit trail can show the bytes:
 *
 *     NCL_TOOL_BEGIN(...)
 *         NCL_DATAITEM(...)
 *     NCL_TOOL_END_WITH_RAW(my_last_raw)
 */
#define NCL_TOOL_END_IMPL(last_raw_expr)                                       \
    }                                                                          \
    ;                                                                          \
    static ncl_tool_last_raw_fn const ncl_tool_last_raw_used_ = (last_raw_expr); \
    static inline ncl_tool_decl ncl_tool_declaration(void)                     \
    {                                                                          \
        ncl_tool_decl decl;                                                    \
                                                                               \
        decl.name = ncl_tool_name_;                                            \
        decl.description = ncl_tool_description_;                              \
        decl.device_type = ncl_tool_device_type_;                              \
        decl.sample_ms = ncl_tool_sample_ms_;                                  \
        decl.upload_ms = ncl_tool_upload_ms_;                                  \
        decl.open = ncl_tool_open_;                                            \
        decl.close = ncl_tool_close_;                                          \
        decl.last_raw = ncl_tool_last_raw_used_;                               \
        decl.points = ncl_tool_points_;                                        \
        decl.point_count =                                                     \
            sizeof(ncl_tool_points_) / sizeof(ncl_tool_points_[0]);            \
        return decl;                                                           \
    }

#define NCL_TOOL_END() NCL_TOOL_END_IMPL(NULL)
#define NCL_TOOL_END_WITH_RAW(fn) NCL_TOOL_END_IMPL(fn)

/* Small helpers, so a point function stays a few lines. */

/** Value of @p key in @p params, or @p fallback (params may be NULL). */
bool        ncl_tool_param_bool(const ncl_json *params, const char *key,
                                bool fallback);
long long   ncl_tool_param_int(const ncl_json *params, const char *key,
                               long long fallback);
const char *ncl_tool_param_str(const ncl_json *params, const char *key,
                               const char *fallback);

/**
 * The value a Set or a method call carries, or NULL. A Set normally sends
 * {"value": ...}; when the request has no "value" key the params object itself
 * is the value, which is what a plain scalar write looks like on the wire.
 */
const ncl_json *ncl_tool_param_value(const ncl_json *params);

/* Answer with a value: NCL_OK when it fit, NCL_ERR_NOMEM otherwise. */
ncl_err ncl_tool_reply_int(ncl_json **result, long long value);
ncl_err ncl_tool_reply_double(ncl_json **result, double value);
ncl_err ncl_tool_reply_bool(ncl_json **result, bool value);
ncl_err ncl_tool_reply_text(ncl_json **result, const char *text);

/**
 * Set *reason from a printf style format and return @p code - the shape every
 * failure path ends in: `return ncl_tool_fail(reason, NCL_ERR_IO, "no answer
 * from %s", host);`. The message goes out to the client as the response
 * message, so keep site secrets out of it.
 */
ncl_err ncl_tool_fail(char **reason, ncl_err code, const char *fmt, ...);

/* Host facing ----------------------------------------------------------- */

/**
 * Where the host records what the points did, for the audit trail (§6: every
 * request with its bytes, every write with its old and new value).
 *
 * The core only knows this shape - the implementation lives where the audit
 * does (nclink/ncl_audit.h), so a host without one passes NULL and the
 * shim records nothing. An adapter author never sees this: the accounting is
 * done for them.
 */
typedef struct {
    void *user;
    /** True when the trail wants the frames too (they cost a call to ask). */
    bool (*wants_raw)(void *user);
    /** One point call: what was asked, what it answered, how long it took. */
    void (*request)(void *user, const char *tool, const ncl_tool_point *point,
                    ncl_operation op, int code, int64_t micros,
                    const ncl_tool_frames *frames);
    /**
     * One write, in full: the value that was there (NULL when it could not be
     * read) and the one being written, plus the outcome.
     */
    void (*write)(void *user, const char *tool, const ncl_tool_point *point,
                  const ncl_json *old_value, const ncl_json *new_value,
                  int code);
} ncl_tool_audit;

/**
 * The name a point answers to inside its tool, written into @p buf: the path
 * with its device segment dropped and '@' / '/' turned into '_' / '.'
 *
 *   "/MACHINE/STATUS@RUN"            -> "STATUS_RUN"
 *   "/MACHINE/AXIS@X/POSITION@REAL"  -> "AXIS_X.POSITION_REAL"
 *
 * A method call is addressed "<tool>/<name>"; the value operations add
 * ".read"/".write" to it. Paths are unique, so the names are too.
 *
 * @return @p buf ("" when there is nothing to name).
 */
char *ncl_tool_point_name(const ncl_tool_point *point, char *buf, size_t cap);

/**
 * 声明里的相对路径 → 模型里的绝对路径："/STATUS" → "/MACHINE/STATUS"。
 *
 * 设备段取自 @p decl（NCL_TOOL_BEGIN 里定义的那个）。返回值是
 * @p buf；@p relative_path 为空时 buf 是空串。宿主就是用它把点位路径拼成客户端
 * 能寻址的模型路径（轮询、自检、REST 都走这一条）。@p buf 留够相对路径 + 64 字节。
 */
const char *ncl_tool_model_path(const ncl_tool_decl *decl,
                                const char *relative_path, char *buf,
                                size_t cap);

/**
 * Check a declaration before anything is built from it: a name, at least one
 * point, every point with a path, a function and at least one operation, point
 * names that do not collide, and "sampled" only where it can be read. A
 * missing sample period is *not* an error: 0 simply means the declaration asks
 * for no sample channel. A diagnostic naming the offending path is appended to
 * @p err.
 */
ncl_err ncl_tool_validate(const ncl_tool_decl *decl, ncl_strbuf *err);

/**
 * The model document for @p decl: one data item per point (its path is the
 * model path) plus, when sample_ms is not 0, one sample channel named after the
 * tool over the sampled points. @p device is the configuration's "device"
 * object (type/id/name; may be NULL). The caller owns the result
 * (ncl_json_free()).
 */
ncl_json *ncl_tool_model(const ncl_tool_decl *decl, const ncl_json *device,
                         ncl_strbuf *err);

/**
 * The same, for a host that publishes more than one tool: @p document is what
 * the first call returned (NULL for the first), and the declaration's points
 * are merged into the very same device - a component both tools use (the
 * CONTROLLER, say) stays one component, and ids are renumbered so they stay
 * unique across the document.
 *
 * Returns the document, or NULL with @p err set - @p document is the caller's
 * and is left as it was.
 */
ncl_json *ncl_tool_model_add(ncl_json *document, const ncl_tool_decl *decl,
                             const ncl_json *device, ncl_strbuf *err);

/** What one registered declaration keeps alive (the per point shims). */
typedef struct ncl_tool_registration ncl_tool_registration;

/**
 * Bring one declared tool up on @p server: validate, open() the connection with
 * @p params (the module's "parameters" object, may be NULL), register the
 * tool's methods and one binding per declared operation.
 *
 * The registration comes back through @p out; the caller keeps it as long as
 * the server lives and releases it with ncl_tool_unregister(). It owns the
 * connection context, which is closed there - the host does not have to know
 * what open() returned.
 */
ncl_err ncl_tool_register(ncl_server *server, const ncl_tool_decl *decl,
                          const ncl_json *params,
                          const ncl_tool_audit *audit,
                          ncl_tool_registration **out, ncl_strbuf *err);

/** close() the connection and release the registration. NULL is accepted. */
void ncl_tool_unregister(const ncl_tool_decl *decl,
                         ncl_tool_registration *registration);

/**
 * True when the point at @p index turned out to be unreadable in this build: its
 * function answered NCL_ERR_UNAVAILABLE (nclink/ncl_common.h), which means the
 * protocol call behind it has not been implemented yet - 帧还没抓到。
 *
 * Learned from the point's own answer and remembered for the life of the
 * process, so it is false until the point has been called once. It is not a
 * declaration flag: what a point can be read with lives in the client, and this
 * is only the host's memory of what the client said. A host that polls asks here
 * before it spends a round on a point; the point stays in the model either way,
 * because the site wants to see what is coming.
 */
bool ncl_tool_point_unavailable(const ncl_tool_registration *registration,
                                size_t index);

/* The module side ------------------------------------------------------- */

/**
 * ABI generation a tool module declares. Generation 1 (ncl_module.h) is the
 * older shape - a module that hands over a driver factory - so the two can be
 * told apart from the descriptor's first field.
 */
/*
 * Generation 2 added the "points live in the module" shape. Generation 3 drops
 * the pending declaration (ncl_tool_point lost `available` / `summary`; a point
 * that cannot be read yet says so from its function, with NCL_ERR_UNAVAILABLE),
 * so a module built against generation 2 has a point struct this host would
 * read at the wrong offsets - hence the bump rather than a silent mismatch.
 */
#define NCL_TOOL_MODULE_ABI 3u

/** Entry point symbol a module exports (same name for both generations). */
#define NCL_TOOL_MODULE_ENTRY "ncl_adapter_module"

#if defined(_WIN32) || defined(_WIN64)
#  define NCL_TOOL_MODULE_EXPORT __declspec(dllexport)
#else
#  define NCL_TOOL_MODULE_EXPORT __attribute__((visibility("default")))
#endif

/**
 * What a tool module hands the host: who it is, plus the declaration from
 * NCL_TOOL_BEGIN/NCL_TOOL_END. The tool name is the one the author wrote in the
 * declaration, so the tool name, the module name and (by the file naming
 * convention) the file name cannot drift apart.
 */
typedef struct {
    unsigned       abi;         /**< NCL_TOOL_MODULE_ABI                        */
    const char    *name;        /**< tool name, from NCL_TOOL_BEGIN             */
    const char    *version;     /**< module version, for `--plugins`            */
    const char    *description; /**< one line, for `--plugins`                  */
    ncl_tool_decl  decl;        /**< the points, the periods, open()/close()    */
} ncl_tool_module_desc;

/** What a module's entry point looks like. */
typedef const ncl_tool_module_desc *(*ncl_tool_module_fn)(void);

/**
 * The last line of an adapter file: exports the entry point above. Everything
 * the host needs is already in the declaration, so there is nothing to fill in
 * here beyond the two version strings.
 */
#define NCL_TOOL_MODULE(version_literal, description_literal)                  \
    static ncl_tool_module_desc ncl_tool_module_;                              \
                                                                               \
    NCL_TOOL_MODULE_EXPORT const ncl_tool_module_desc *ncl_adapter_module(void)   \
    {                                                                          \
        ncl_tool_module_.abi = NCL_TOOL_MODULE_ABI;                            \
        ncl_tool_module_.name = ncl_tool_name_;                                \
        ncl_tool_module_.version = version_literal;                            \
        ncl_tool_module_.description = description_literal;                    \
        ncl_tool_module_.decl = ncl_tool_declaration();                        \
        return &ncl_tool_module_;                                              \
    }

#ifdef __cplusplus
}
#endif

#endif /* NCL_TOOL_H */
