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
 *         NCL_DATAITEM_SAMPLED("/MACHINE/STATUS@RUN", status)
 *         NCL_DATAITEM_RW("/MACHINE/MODE", mode)
 *         NCL_CONFIG("/MACHINE/CONTROLLER/PARAMETER", parameter)
 *         NCL_METHOD("/MACHINE/RESET", reset)
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
 *     entry of a mapping table - is written next to the path (NCL_DATAITEM_ARG and
 *     friends) and comes back as @p self->arg, so one dispatch function can
 *     serve a whole table of points and still know which entry it is on.
 *
 * A point declares what may be done with it: readable (Query/get_value),
 * writable (Set/set_value) and/or callable (a Method call). The same path may
 * be several of those at once - "read the mode, and write it" is one point
 * with both flags - and `sampled` asks the host to put the path in the sample
 * channel (which means the point has to be readable).
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
    bool        readable; /**< Query / get_value may read it  */
    bool        writable; /**< Set / set_value may write it   */
    bool        callable; /**< a Method call may reach it     */
    /** Ask the host to sample this path (the point has to be readable). */
    bool        sampled;
    /**
     * Optional name this point answers to inside its tool, for the case where
     * the tail of its path is not usable: three axes declared under one tree
     * all end in "/POSITION", and a method name has to be unique. NULL means
     * "the tail of the path", which is what most points want.
     */
    const char *name;
    /** The function serving this point. Required. */
    ncl_point_fn fn;
    /**
     * False when the point is declared but cannot be read yet - the protocol
     * call it needs has not been captured, or the machine has not been seen.
     * Such a point is still a point: it is in the model (the site sees what is
     * coming) and asking for it answers a clear "not available yet" instead of
     * "no such point". What the host never does is call @p fn - it cannot, the
     * function is NULL - and a self check reports it as "not available"
     * instead of "failed". Use a *_PENDING() macro to declare one; every other
     * macro leaves it true.
     */
    bool available;
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
    /** Optional one line description, for the schema. */
    const char *summary;
};

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
 *   NCL_DATAITEM(path, fn)        a data item, readable
 *   NCL_CONFIG(path, fn)          a config, readable
 *   NCL_DATAITEM_SAMPLED(...)     ... and in the sample channel
 *   NCL_DATAITEM_WRITE(...)       writable
 *   NCL_CONFIG_RW(...)            readable and writable
 *   NCL_METHOD(path, fn)          callable
 *
 * The *_ARG forms take one more argument, the point's own data:
 *
 *   NCL_DATAITEM_RW_ARG("/MACHINE/MODE", mode, &kModeEntry)
 *   NCL_CONFIG_ARG("/MACHINE/CONTROLLER/PARAMETER", read_param, &kParam)
 *
 * And the two PENDING forms declare a point whose protocol call is not
 * captured yet - readable in the model, but no function behind it (see below):
 *
 *   NCL_DATAITEM_PENDING("/MACHINE/WARNING", "报警：帧待抓包（cnc_rdalmmsg2）")
 *   NCL_DATAITEM_PENDING_SAMPLED("/MACHINE/WARNING", "...")
 *   NCL_DATAITEM_PENDING_NAMED(path, "AXIS_X.POSITION_CMD", "...")
 *   NCL_CONFIG_PENDING("/MACHINE/CONTROLLER/PARAMETER", "参数：帧待抓包")
 *
 * NCL_TOOL_END closes the table and defines ncl_tool_declaration(), which hands
 * the host a filled in ncl_tool_decl by value: a module stores that value in
 * its descriptor, a test passes &decl around. It is a value rather than a file
 * static object on purpose - in C a static initialiser needs constant
 * expressions, and a const qualified variable holding a function pointer is not
 * one (MSVC: "error C2099: 初始值设定项不是常量").
 */
/*
 * 先说清是**哪一类数据对象**（册 3 §5.4/§5.5），再说它能怎么被访问：
 *
 *   NCL_DATAITEM*(路径, 函数)   dataItem：物理量、从设备感知的实时量，**能进采样通道**
 *   NCL_CONFIG*(路径, 函数)     config：参数、坐标系、刀具表、元信息这类不常变的数据
 *   NCL_METHOD*(路径, 函数)     方法：不是数据对象，只响应调用
 *
 * 两族的形状一一对应，只差一件事：**config 没有 *_SAMPLED** —— 册 3 表 1 注 b
 * 说"配置中的数据对象不得作为采样数据源"，少一个宏比运行期再报一句错更早拦住它。
 *
 *   NCL_DATAITEM(path, fn)              readable
 *   NCL_CONFIG_ARG(path, fn, arg)       readable, 带点位自己的数据
 *   NCL_DATAITEM_SAMPLED_ARG(...)       readable + 进默认采样通道
 *   NCL_DATAITEM_WRITE_ARG(...)         writable
 *   NCL_CONFIG_RW_NAMED(路径, 函数, 参数, 名字)   readable + writable，显式名字
 *
 * 路径尾段不能当方法名时（同名点位多）用 *_NAMED 显式给名字；协议调用还没抓到帧时用
 * *_PENDING[(_SAMPLED)](路径[, 名字], 理由) —— 理由必填，见下面那段注释。
 */
#define NCL_DATAITEM(path_literal, fn)                                         \
    { path_literal, true, false, false, false, NULL, fn, true, false, NULL, NULL },

#define NCL_DATAITEM_ARG(path_literal, fn, arg)                                \
    { path_literal, true, false, false, false, NULL, fn, true, false, arg, NULL },

#define NCL_DATAITEM_SAMPLED(path_literal, fn)                                 \
    { path_literal, true, false, false, true, NULL, fn, true, false, NULL, NULL },

#define NCL_DATAITEM_SAMPLED_ARG(path_literal, fn, arg)                        \
    { path_literal, true, false, false, true, NULL, fn, true, false, arg, NULL },

#define NCL_DATAITEM_WRITE(path_literal, fn)                                   \
    { path_literal, false, true, false, false, NULL, fn, true, false, NULL, NULL },

#define NCL_DATAITEM_WRITE_ARG(path_literal, fn, arg)                          \
    { path_literal, false, true, false, false, NULL, fn, true, false, arg, NULL },

#define NCL_DATAITEM_RW(path_literal, fn)                                      \
    { path_literal, true, true, false, false, NULL, fn, true, false, NULL, NULL },

#define NCL_DATAITEM_RW_ARG(path_literal, fn, arg)                             \
    { path_literal, true, true, false, false, NULL, fn, true, false, arg, NULL },

#define NCL_DATAITEM_NAMED(path_literal, fn, arg, name_literal)                \
    { path_literal, true, false, false, false, name_literal, fn, true, false, arg, NULL },

#define NCL_DATAITEM_SAMPLED_NAMED(path_literal, fn, arg, name_literal)        \
    { path_literal, true, false, false, true, name_literal, fn, true, false, arg, NULL },

#define NCL_DATAITEM_WRITE_NAMED(path_literal, fn, arg, name_literal)          \
    { path_literal, false, true, false, false, name_literal, fn, true, false, arg, NULL },

#define NCL_DATAITEM_RW_NAMED(path_literal, fn, arg, name_literal)             \
    { path_literal, true, true, false, false, name_literal, fn, true, false, arg, NULL },

#define NCL_CONFIG(path_literal, fn)                                           \
    { path_literal, true, false, false, false, NULL, fn, true, true, NULL, NULL },

#define NCL_CONFIG_ARG(path_literal, fn, arg)                                  \
    { path_literal, true, false, false, false, NULL, fn, true, true, arg, NULL },

#define NCL_CONFIG_WRITE(path_literal, fn)                                     \
    { path_literal, false, true, false, false, NULL, fn, true, true, NULL, NULL },

#define NCL_CONFIG_WRITE_ARG(path_literal, fn, arg)                            \
    { path_literal, false, true, false, false, NULL, fn, true, true, arg, NULL },

#define NCL_CONFIG_RW(path_literal, fn)                                        \
    { path_literal, true, true, false, false, NULL, fn, true, true, NULL, NULL },

#define NCL_CONFIG_RW_ARG(path_literal, fn, arg)                               \
    { path_literal, true, true, false, false, NULL, fn, true, true, arg, NULL },

#define NCL_CONFIG_NAMED(path_literal, fn, arg, name_literal)                  \
    { path_literal, true, false, false, false, name_literal, fn, true, true, arg, NULL },

#define NCL_CONFIG_WRITE_NAMED(path_literal, fn, arg, name_literal)            \
    { path_literal, false, true, false, false, name_literal, fn, true, true, arg, NULL },

#define NCL_CONFIG_RW_NAMED(path_literal, fn, arg, name_literal)               \
    { path_literal, true, true, false, false, name_literal, fn, true, true, arg, NULL },

#define NCL_METHOD(path_literal, fn)                                           \
    { path_literal, false, false, true, false, NULL, fn, true, false, NULL, NULL },

#define NCL_METHOD_ARG(path_literal, fn, arg)                                  \
    { path_literal, false, false, true, false, NULL, fn, true, false, arg, NULL },

#define NCL_METHOD_NAMED(path_literal, fn, arg, name_literal)                  \
    { path_literal, false, false, true, false, name_literal, fn, true, false, arg, NULL },


/*
 * 声明一个"已经定好、但现在还读不了"的点位：协议调用还没抓到帧，或者这台机床还没见到。
 *
 * 它仍然是个正常点位 —— 模型里有它（现场看得见后面有什么），客户端问它会拿到一句明确
 * 的"还读不了"（而不是"没有这个点位"），只是从没有人调用它的函数（本来是 NULL）。
 * dataItem 的 *_PENDING_SAMPLED 会占住默认采样通道：通道里现在就有这一列，抓包补上之前
 * 取值是 null，自检把它算成"待抓包"而不是"失败"，所以现场不会误以为机床坏了；
 * config 的待抓包用 *_CONFIG_PENDING（它本来就不进通道）。
 *
 * 抓包补上之后，把这一行换成同一族的普通宏（要进通道就用 *_SAMPLED_*）即可，别的什么都
 * 不用改。summary 是必填的 —— 它写着"为什么现在读不了"。
 */
#define NCL_DATAITEM_PENDING(path_literal, summary_literal)                    \
    { path_literal, true, false, false, false, NULL, NULL, false, false, NULL, \
      summary_literal },

/** 同上，但已经占住默认采样通道（现在取值是 null，抓包后换宏即可）。 */
#define NCL_DATAITEM_PENDING_SAMPLED(path_literal, summary_literal)            \
    { path_literal, true, false, false, true, NULL, NULL, false, false, NULL,  \
      summary_literal },

/** 同上，但路径尾段不能当方法名用（同名点位多，例如五个轴的目标位置）。 */
#define NCL_DATAITEM_PENDING_NAMED(path_literal, name_literal, summary_literal) \
    { path_literal, true, false, false, false, name_literal, NULL, false, false, \
      NULL, summary_literal },

#define NCL_DATAITEM_PENDING_SAMPLED_NAMED(path_literal, name_literal,         \
                                           summary_literal)                    \
    { path_literal, true, false, false, true, name_literal, NULL, false, false, \
      NULL, summary_literal },

#define NCL_CONFIG_PENDING(path_literal, summary_literal)                      \
    { path_literal, true, false, false, false, NULL, NULL, false, true, NULL,  \
      summary_literal },

#define NCL_CONFIG_PENDING_NAMED(path_literal, name_literal, summary_literal)  \
    { path_literal, true, false, false, false, name_literal, NULL, false, true, \
      NULL, summary_literal },

#define NCL_TOOL_BEGIN(name_literal, description_literal, sample_ms_value,     \
                       upload_ms_value, open_fn, close_fn)                     \
    static const char ncl_tool_name_[] = name_literal;                         \
    static const char ncl_tool_description_[] = description_literal;           \
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
 * does (nclink_adapter/ncl_audit.h), so a host without one passes NULL and the
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

/** The name a point answers to inside its tool: the tail of its path. */
const char *ncl_tool_point_name(const ncl_tool_point *point);

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

/* The module side ------------------------------------------------------- */

/**
 * ABI generation a tool module declares. Generation 1 (ncl_module.h) is the
 * older shape - a module that hands over a driver factory - so the two can be
 * told apart from the descriptor's first field.
 */
#define NCL_TOOL_MODULE_ABI 2u

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
