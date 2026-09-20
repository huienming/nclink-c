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
 *     static ncl_err read_run(void *ctx, const ncl_json *params,
 *                             ncl_json **result, char **reason) { ... }
 *     static ncl_err set_mode(void *ctx, const ncl_json *params,
 *                             ncl_json **result, char **reason) { ... }
 *
 *     NCL_TOOL_BEGIN("cnc", "FANUC 数控机床", 1000, 1000, open_box, close_box)
 *         NCL_POINT_SAMPLED("/CNC/STATUS@RUN", read_run)
 *         NCL_POINT_WRITE("/CNC/MODE", set_mode)
 *     NCL_TOOL_END()
 *
 * That declaration is data - "which model path answers to which function, and
 * which paths belong to the sample channel" - and the host does the rest with
 * it:
 *
 *   - ncl_tool_model() turns it into the model document (one data item per
 *     point, plus one sample channel over the sampled ones), which is what the
 *     device publishes and what the OpenAPI document is derived from;
 *   - ncl_tool_register() opens the connection once and binds every point
 *     ("<operation>#<path>" -> that point's function) into the server;
 *   - everything else - MQTT, REST, sampling, audit, the model file - stays the
 *     host's business and is not something an adapter touches.
 *
 * Two deliberate omissions:
 *
 *   - there is no point map in the configuration file. A site with different
 *     addresses writes its own adapter: the address strings sit next to the
 *     protocol calls in the same file (NCL_POINT), so a compiler checks them.
 *   - there is no driver/ops table to fill in. A point names one function with
 *     one calling convention (ncl_tool_fn); read-only adapters leave the write
 *     points out and the host answers NCL_ERR_NOT_SUPPORTED for them.
 *
 * The one thing to keep in mind while writing one: `open` runs once (the
 * returned context is handed to every point), and a point function returns
 * NCL_OK with *result set, or an error code with *reason set for the client.
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

/** What a point does; each kind maps onto one operation of the spec. */
typedef enum {
    NCL_TOOL_GET = 0, /**< readable value            (op: get_value)  */
    NCL_TOOL_SET,     /**< writable value            (op: set_value)  */
    NCL_TOOL_CALL     /**< method-like operation     (op: func_call)  */
} ncl_tool_kind;

/** One declared point: a model path and the function behind it. */
typedef struct {
    /** Model path, e.g. "/CNC/STATUS@RUN". Also the tool name it binds under. */
    const char   *path;
    ncl_tool_kind kind;
    /** True when the point belongs to the tool's sample channel. */
    bool          sampled;
    /** The function this path answers to (ncl_tool_fn, see ncl_server.h). */
    ncl_tool_fn   fn;
    /** Optional one line description, for the schema and for `--points`. */
    const char   *summary;
} ncl_tool_point;

/**
 * One tool: what the host needs to build a model, a sample channel and the
 * bindings. NCL_TOOL_BEGIN/NCL_TOOL_END fill it in; both fields and points are
 * borrowed, they must outlive the server (a static declaration does).
 */
typedef struct {
    /** Tool name; the sample channel is named after it. */
    const char           *name;
    /** One line description, for `--plugins` and the schema. */
    const char           *description;
    /** Sample channel period in ms (both 0 = do not sample). */
    long long             sample_ms;
    /** Upload period of the sample channel, in ms. */
    long long             upload_ms;
    ncl_tool_open_fn      open;
    ncl_tool_close_fn     close;
    const ncl_tool_point *points;
    size_t                point_count;
} ncl_tool_decl;

/*
 * The declaration macros. One tool per file - that is the whole point - so the
 * generated names can be file scope and the author never sees them.
 *
 * NCL_TOOL_BEGIN opens the point table; the point macros append to it:
 *
 *   NCL_POINT_SAMPLED(path, fn)   readable and part of the sample channel
 *   NCL_POINT(path, fn)           readable, not sampled
 *   NCL_POINT_WRITE(path, fn)     writable
 *   NCL_METHOD(path, fn)          callable
 *
 * NCL_TOOL_END closes it and defines ncl_tool_declaration(), which hands the
 * host a filled in ncl_tool_decl by value: a module stores that value in its
 * descriptor, a test passes &decl around. It is a value rather than a file
 * static object on purpose - in C a static initialiser needs constant
 * expressions, and a const qualified variable holding a function pointer is not
 * one (MSVC: "error C2099: 初始值设定项不是常量").
 */
#define NCL_TOOL_BEGIN(name_literal, description_literal, sample_ms_value,     \
                       upload_ms_value, open_fn, close_fn)                     \
    static const char ncl_tool_name_[] = name_literal;                         \
    static const char ncl_tool_description_[] = description_literal;           \
    static const long long ncl_tool_sample_ms_ = (sample_ms_value);            \
    static const long long ncl_tool_upload_ms_ = (upload_ms_value);            \
    static ncl_tool_open_fn const ncl_tool_open_ = (open_fn);                  \
    static ncl_tool_close_fn const ncl_tool_close_ = (close_fn);               \
    static const ncl_tool_point ncl_tool_points_[] = {

#define NCL_POINT_SAMPLED(path_literal, fn)                                    \
    { path_literal, NCL_TOOL_GET, true, fn, NULL },

#define NCL_POINT(path_literal, fn)                                            \
    { path_literal, NCL_TOOL_GET, false, fn, NULL },

#define NCL_POINT_WRITE(path_literal, fn)                                      \
    { path_literal, NCL_TOOL_SET, false, fn, NULL },

#define NCL_METHOD(path_literal, fn)                                           \
    { path_literal, NCL_TOOL_CALL, false, fn, NULL },

#define NCL_TOOL_END()                                                         \
    }                                                                          \
    ;                                                                          \
    static inline ncl_tool_decl ncl_tool_declaration(void)\
    {\
        ncl_tool_decl decl;\
\
        decl.name = ncl_tool_name_;\
        decl.description = ncl_tool_description_;\
        decl.sample_ms = ncl_tool_sample_ms_;\
        decl.upload_ms = ncl_tool_upload_ms_;\
        decl.open = ncl_tool_open_;\
        decl.close = ncl_tool_close_;\
        decl.points = ncl_tool_points_;\
        decl.point_count =\
            sizeof(ncl_tool_points_) / sizeof(ncl_tool_points_[0]);\
        return decl;\
    }
/* Small helpers, so a point function stays a few lines. */

/** Value of @p key in @p params, or @p fallback (params may be NULL). */
bool        ncl_tool_param_bool(const ncl_json *params, const char *key,
                                bool fallback);
long long   ncl_tool_param_int(const ncl_json *params, const char *key,
                               long long fallback);
const char *ncl_tool_param_str(const ncl_json *params, const char *key,
                               const char *fallback);

/* Answer with a value: NCL_OK when it fit, NCL_ERR_NOMEM otherwise. */
ncl_err ncl_tool_reply_int(ncl_json **result, long long value);
ncl_err ncl_tool_reply_double(ncl_json **result, double value);
ncl_err ncl_tool_reply_bool(ncl_json **result, bool value);
ncl_err ncl_tool_reply_text(ncl_json **result, const char *text);

/**
 * Set *reason from a printf style format and return @p code - the shape every
 * failure path ends in: `return ncl_tool_fail(reason, NCL_ERR_IO, "no answer
 * from %s", host);`. The message must stay free of addresses the site may not
 * want in a log; it goes out to the client as the response message.
 */
ncl_err ncl_tool_fail(char **reason, ncl_err code, const char *fmt, ...);

/* Host facing ----------------------------------------------------------- */

/** Operation of @p kind, for the binding table (ncl_tool_binding). */
ncl_operation ncl_tool_kind_operation(ncl_tool_kind kind);
/** Method name a point of @p kind registers under ("read"/"write"/"call"). */
const char *ncl_tool_kind_method(ncl_tool_kind kind);

/**
 * Check a declaration before anything is built from it: a name, at least one
 * point, no duplicate path+kind, every point has a function, "sampled" only on
 * readable points, samples not asking for a channel without a period. A
 * diagnostic naming the offending path is appended to @p err.
 */
ncl_err ncl_tool_validate(const ncl_tool_decl *decl, ncl_strbuf *err);

/**
 * The model document for @p decl: one data item per point (its path is the
 * model path) plus one sample channel named after the tool, over the sampled
 * points with the declaration's periods. @p device is the configuration's
 * "device" object (type/id/name; may be NULL). The caller owns the result
 * (ncl_json_free()).
 */
ncl_json *ncl_tool_model(const ncl_tool_decl *decl, const ncl_json *device,
                         ncl_strbuf *err);

/**
 * Bring one declared tool up on @p server: validate, open() the connection
 * with @p params (the module's "parameters" object, may be NULL) and register
 * every point as a tool of its own, so "<operation>#<path>" resolves to the
 * point's function.
 *
 * The opened context comes back through @p ctx_out - the caller owns it and
 * releases it with ncl_tool_close() when the server goes away (the server does
 * not free it: an adapter may hand the same context to more than one tool).
 */
ncl_err ncl_tool_register(ncl_server *server, const ncl_tool_decl *decl,
                          const ncl_json *params, void **ctx_out,
                          ncl_strbuf *err);

/** close() the context ncl_tool_register() opened. NULL is accepted. */
void ncl_tool_close(const ncl_tool_decl *decl, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NCL_TOOL_H */
