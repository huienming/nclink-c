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
 *         NCL_POINT_SAMPLED("/CNC/STATUS@RUN", status)
 *         NCL_POINT_RW("/CNC/MODE", mode)
 *         NCL_METHOD("/CNC/RESET", reset)
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
 *     entry of a mapping table - is written next to the path (NCL_POINT_ARG and
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

/** One declared point; the macros below fill it in. */
struct ncl_tool_point {
    /** Model path, e.g. "/CNC/STATUS@RUN". Also how the point is addressed. */
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
    const ncl_tool_point *points;
    size_t                point_count;
} ncl_tool_decl;

/*
 * The declaration macros. One tool per file - that is the whole point - so the
 * generated names can be file scope and the author never sees them.
 *
 *   NCL_POINT(path, fn)           readable
 *   NCL_POINT_SAMPLED(path, fn)   readable and sampled
 *   NCL_POINT_WRITE(path, fn)     writable
 *   NCL_POINT_RW(path, fn)        readable and writable
 *   NCL_METHOD(path, fn)          callable
 *
 * The *_ARG forms take one more argument, the point's own data:
 *
 *   NCL_POINT_RW_ARG("/CNC/MODE", mode, &kModeEntry)
 *
 * NCL_TOOL_END closes the table and defines ncl_tool_declaration(), which hands
 * the host a filled in ncl_tool_decl by value: a module stores that value in
 * its descriptor, a test passes &decl around. It is a value rather than a file
 * static object on purpose - in C a static initialiser needs constant
 * expressions, and a const qualified variable holding a function pointer is not
 * one (MSVC: "error C2099: 初始值设定项不是常量").
 */
#define NCL_POINT(path_literal, fn)                                            \
    { path_literal, true, false, false, false, NULL, fn, NULL, NULL },

#define NCL_POINT_ARG(path_literal, fn, arg)                                   \
    { path_literal, true, false, false, false, NULL, fn, arg, NULL },

#define NCL_POINT_SAMPLED(path_literal, fn)                                    \
    { path_literal, true, false, false, true, NULL, fn, NULL, NULL },

#define NCL_POINT_SAMPLED_ARG(path_literal, fn, arg)                           \
    { path_literal, true, false, false, true, NULL, fn, arg, NULL },

#define NCL_POINT_WRITE(path_literal, fn)                                      \
    { path_literal, false, true, false, false, NULL, fn, NULL, NULL },

#define NCL_POINT_WRITE_ARG(path_literal, fn, arg)                             \
    { path_literal, false, true, false, false, NULL, fn, arg, NULL },

#define NCL_POINT_RW(path_literal, fn)                                         \
    { path_literal, true, true, false, false, NULL, fn, NULL, NULL },

#define NCL_POINT_RW_ARG(path_literal, fn, arg)                                \
    { path_literal, true, true, false, false, NULL, fn, arg, NULL },

#define NCL_METHOD(path_literal, fn)                                           \
    { path_literal, false, false, true, false, NULL, fn, NULL, NULL },

#define NCL_METHOD_ARG(path_literal, fn, arg)                                  \
    { path_literal, false, false, true, false, NULL, fn, arg, NULL },

/*
 * The same five shapes with an explicit name, for a point whose path tail is
 * not usable as a method name:
 *
 *   NCL_POINT_SAMPLED_NAMED("/CNC/AXIS@0/POSITION", read_axis, &k_axis0,
 *                           "AXIS0.POSITION")
 */
#define NCL_POINT_NAMED(path_literal, fn, arg, name_literal)                   \
    { path_literal, true, false, false, false, name_literal, fn, arg, NULL },

#define NCL_POINT_SAMPLED_NAMED(path_literal, fn, arg, name_literal)           \
    { path_literal, true, false, false, true, name_literal, fn, arg, NULL },

#define NCL_POINT_WRITE_NAMED(path_literal, fn, arg, name_literal)             \
    { path_literal, false, true, false, false, name_literal, fn, arg, NULL },

#define NCL_POINT_RW_NAMED(path_literal, fn, arg, name_literal)                \
    { path_literal, true, true, false, false, name_literal, fn, arg, NULL },

#define NCL_METHOD_NAMED(path_literal, fn, arg, name_literal)                  \
    { path_literal, false, false, true, false, name_literal, fn, arg, NULL },

#define NCL_TOOL_BEGIN(name_literal, description_literal, sample_ms_value,     \
                       upload_ms_value, open_fn, close_fn)                     \
    static const char ncl_tool_name_[] = name_literal;                         \
    static const char ncl_tool_description_[] = description_literal;           \
    static const long long ncl_tool_sample_ms_ = (sample_ms_value);            \
    static const long long ncl_tool_upload_ms_ = (upload_ms_value);            \
    static ncl_tool_open_fn const ncl_tool_open_ = (open_fn);                  \
    static ncl_tool_close_fn const ncl_tool_close_ = (close_fn);               \
    static const ncl_tool_point ncl_tool_points_[] = {

#define NCL_TOOL_END()                                                         \
    }                                                                          \
    ;                                                                          \
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
        decl.points = ncl_tool_points_;                                        \
        decl.point_count =                                                     \
            sizeof(ncl_tool_points_) / sizeof(ncl_tool_points_[0]);            \
        return decl;                                                           \
    }

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
                          const ncl_json *params, ncl_tool_registration **out,
                          ncl_strbuf *err);

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
