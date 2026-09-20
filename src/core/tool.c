/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - the declaration seam between an adapter author and the host.
 *
 * nclink/ncl_tool.h is what an author writes; this file is the other half: the
 * helpers a point function uses, plus the two steps the host runs on a
 * declaration - ncl_tool_model() (declaration -> model document) and
 * ncl_tool_register() (declaration -> open once + methods and bindings).
 *
 * The host's tool API hands every method one shared `instance` pointer and a
 * callback of its own shape (ncl_tool_fn: no place to say *which* point is
 * being served, no place for the point's own data). So the declaration layer
 * puts a small shim in front of each point - {context, point} - and three
 * trampolines that call the author's function with the operation that reached
 * it. The shims live in the registration the caller holds.
 */

#include "nclink/ncl_tool.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- diagnostics -- */

static void err_append(ncl_strbuf *err, const char *text)
{
    if (err == NULL) {
        return;
    }
    if (err->len > 0) {
        (void)ncl_strbuf_puts(err, "; ");
    }
    (void)ncl_strbuf_puts(err, text);
}

static void err_append1(ncl_strbuf *err, const char *fmt, const char *arg)
{
    if (err == NULL) {
        return;
    }
    if (err->len > 0) {
        (void)ncl_strbuf_puts(err, "; ");
    }
    (void)ncl_strbuf_printf(err, fmt, arg);
}

static void err_appendf(ncl_strbuf *err, const char *fmt, ...)
{
    char buffer[256];
    va_list args;

    if (err == NULL) {
        return;
    }
    va_start(args, fmt);
    (void)vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (err->len > 0) {
        (void)ncl_strbuf_puts(err, "; ");
    }
    (void)ncl_strbuf_puts(err, buffer);
}

/* ----------------------------------------------------------- point helpers -- */

bool ncl_tool_param_bool(const ncl_json *params, const char *key, bool fallback)
{
    if (params == NULL || key == NULL) {
        return fallback;
    }
    return ncl_json_obj_get_bool(params, key, fallback);
}

long long ncl_tool_param_int(const ncl_json *params, const char *key,
                             long long fallback)
{
    if (params == NULL || key == NULL) {
        return fallback;
    }
    return ncl_json_obj_get_int(params, key, fallback);
}

const char *ncl_tool_param_str(const ncl_json *params, const char *key,
                               const char *fallback)
{
    const char *value;

    if (params == NULL || key == NULL) {
        return fallback;
    }
    value = ncl_json_obj_get_string(params, key);
    return value != NULL ? value : fallback;
}

const ncl_json *ncl_tool_param_value(const ncl_json *params)
{
    const ncl_json *value;

    if (params == NULL) {
        return NULL;
    }
    value = ncl_json_obj_get(params, "value");
    return value != NULL ? value : params;
}

ncl_err ncl_tool_reply_int(ncl_json **result, long long value)
{
    ncl_json *json;

    if (result == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    json = ncl_json_new_int(value);
    if (json == NULL) {
        return NCL_ERR_NOMEM;
    }
    *result = json;
    return NCL_OK;
}

ncl_err ncl_tool_reply_double(ncl_json **result, double value)
{
    ncl_json *json;

    if (result == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    json = ncl_json_new_double(value);
    if (json == NULL) {
        return NCL_ERR_NOMEM;
    }
    *result = json;
    return NCL_OK;
}

ncl_err ncl_tool_reply_bool(ncl_json **result, bool value)
{
    ncl_json *json;

    if (result == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    json = ncl_json_new_bool(value);
    if (json == NULL) {
        return NCL_ERR_NOMEM;
    }
    *result = json;
    return NCL_OK;
}

ncl_err ncl_tool_reply_text(ncl_json **result, const char *text)
{
    ncl_json *json;

    if (result == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    json = ncl_json_new_string(text != NULL ? text : "");
    if (json == NULL) {
        return NCL_ERR_NOMEM;
    }
    *result = json;
    return NCL_OK;
}

ncl_err ncl_tool_fail(char **reason, ncl_err code, const char *fmt, ...)
{
    char buffer[512];
    va_list args;

    if (reason != NULL && fmt != NULL) {
        va_start(args, fmt);
        (void)vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        *reason = ncl_strdup(buffer);
    }
    return code;
}

/* ------------------------------------------------------------- declaration -- */

/** The three operations a point may declare, in a fixed order. */
static const ncl_operation k_operations[3] = {NCL_OP_GET_VALUE, NCL_OP_SET_VALUE,
                                              NCL_OP_FUNC_CALL};

const char *ncl_tool_point_name(const ncl_tool_point *point)
{
    const char *slash;

    if (point == NULL || point->path == NULL) {
        return "";
    }
    slash = strrchr(point->path, '/');
    return slash != NULL ? slash + 1 : point->path;
}

/** True when @p point declares @p op. */
static bool point_declares(const ncl_tool_point *point, ncl_operation op)
{
    switch (op) {
    case NCL_OP_GET_VALUE:
        return point->readable;
    case NCL_OP_SET_VALUE:
        return point->writable;
    case NCL_OP_FUNC_CALL:
        return point->callable;
    default:
        return false;
    }
}

/**
 * The name a point's operation answers to inside its tool. A call keeps the
 * bare name - "<tool>/RESET" is what a client writes - while the value
 * operations are suffixed, because they are addressed by model path rather
 * than by name and the suffix keeps every name unique.
 */
static void point_method_name(const ncl_tool_point *point, ncl_operation op,
                              char *buffer, size_t size)
{
    const char *name = ncl_tool_point_name(point);

    switch (op) {
    case NCL_OP_GET_VALUE:
        (void)snprintf(buffer, size, "%s.read", name);
        break;
    case NCL_OP_SET_VALUE:
        (void)snprintf(buffer, size, "%s.write", name);
        break;
    default:
        (void)snprintf(buffer, size, "%s", name);
        break;
    }
}

ncl_err ncl_tool_validate(const ncl_tool_decl *decl, ncl_strbuf *err)
{
    size_t i;

    if (decl == NULL) {
        err_append(err, "the tool declaration is missing");
        return NCL_ERR_INVALID_ARG;
    }
    if (ncl_str_is_blank(decl->name)) {
        err_append(err, "the tool has no name");
        return NCL_ERR_INVALID_ARG;
    }
    if (decl->open == NULL) {
        err_append1(err, "the tool %s has no open function", decl->name);
        return NCL_ERR_INVALID_ARG;
    }
    if (decl->points == NULL || decl->point_count == 0) {
        err_append1(err, "the tool %s declares no point", decl->name);
        return NCL_ERR_INVALID_ARG;
    }
    if (decl->sample_ms < 0 || decl->upload_ms < 0) {
        err_append1(err, "the tool %s has a negative period", decl->name);
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < decl->point_count; i++) {
        const ncl_tool_point *point = &decl->points[i];
        const char *name = ncl_tool_point_name(point);
        size_t j;
        size_t op;
        size_t declared = 0;

        if (ncl_str_is_blank(point->path)) {
            err_append1(err, "a point of the tool %s has no path", decl->name);
            return NCL_ERR_INVALID_ARG;
        }
        if (point->path[0] != '/') {
            err_append1(err, "the point path %s must start with '/'",
                        point->path);
            return NCL_ERR_INVALID_ARG;
        }
        if (point->fn == NULL) {
            err_append1(err, "the point %s has no function", point->path);
            return NCL_ERR_INVALID_ARG;
        }
        if (name[0] == '\0') {
            err_append1(err, "the point path %s ends with '/'", point->path);
            return NCL_ERR_INVALID_ARG;
        }
        if (point->sampled && !point->readable) {
            err_append1(err, "the point %s is sampled but not readable",
                        point->path);
            return NCL_ERR_INVALID_ARG;
        }
        for (op = 0; op < 3; op++) {
            if (point_declares(point, k_operations[op])) {
                declared++;
            }
        }
        if (declared == 0) {
            err_append1(err, "the point %s declares no operation", point->path);
            return NCL_ERR_INVALID_ARG;
        }
        /* The value operations get their own suffix, so a name ending in one
         * of them could collide with another point's name. */
        if (strstr(name, ".read") != NULL || strstr(name, ".write") != NULL) {
            err_append1(err, "the point name %s must not contain .read/.write",
                        name);
            return NCL_ERR_INVALID_ARG;
        }
        for (j = 0; j < i; j++) {
            const ncl_tool_point *other = &decl->points[j];
            size_t other_op;

            if (ncl_str_is_blank(other->path) || other->path[0] != '/') {
                continue;
            }
            if (strcmp(ncl_tool_point_name(other), name) != 0) {
                continue;
            }
            /* Same name: each (path, operation) pair may be declared once. */
            for (other_op = 0; other_op < 3; other_op++) {
                ncl_operation candidate = k_operations[other_op];

                if (point_declares(other, candidate) &&
                    point_declares(point, candidate)) {
                    err_appendf(err, "the points %s and %s both declare %s",
                                other->path, point->path,
                                ncl_operation_to_string(candidate));
                    return NCL_ERR_INVALID_ARG;
                }
            }
            if (strcmp(other->path, point->path) == 0) {
                err_append1(err, "the point %s is declared twice", point->path);
                return NCL_ERR_INVALID_ARG;
            }
            err_appendf(err, "the points %s and %s share the name %s",
                        other->path, point->path, name);
            return NCL_ERR_INVALID_ARG;
        }
    }
    return NCL_OK;
}

/* ----------------------------------------------------------------- model -- */

/**
 * "/CNC/STATUS@RUN" -> type "STATUS", number "RUN". Same rule the adapter host
 * applies to a configured point name, so both model writers agree.
 */
static void split_type_number(const char *text, char **type_out,
                              char **number_out)
{
    const char *at = strchr(text, '@');

    if (at == NULL) {
        *type_out = ncl_strdup(text);
        *number_out = NULL;
        return;
    }
    *type_out = ncl_strndup(text, (size_t)(at - text));
    *number_out = ncl_strdup(at + 1);
}

ncl_json *ncl_tool_model(const ncl_tool_decl *decl, const ncl_json *device,
                         ncl_strbuf *err)
{
    const char *device_type =
        device != NULL ? ncl_json_obj_get_string(device, "type") : NULL;
    const char *device_id =
        device != NULL ? ncl_json_obj_get_string(device, "id") : NULL;
    const char *device_name =
        device != NULL ? ncl_json_obj_get_string(device, "name") : NULL;
    ncl_json *root = NULL;
    ncl_json *devices = NULL;
    ncl_json *node = NULL;
    ncl_json *items = NULL;
    ncl_json *configs = NULL;
    ncl_json *channel = NULL;
    ncl_json *ids = NULL;
    size_t i;

    if (ncl_tool_validate(decl, err) != NCL_OK) {
        return NULL;
    }
    root = ncl_json_new_object();
    devices = ncl_json_new_array();
    node = ncl_json_new_object();
    items = ncl_json_new_array();
    configs = ncl_json_new_array();
    channel = ncl_json_new_object();
    ids = ncl_json_new_array();
    if (root == NULL || devices == NULL || node == NULL || items == NULL ||
        configs == NULL || channel == NULL || ids == NULL) {
        goto fail;
    }
    (void)ncl_json_obj_set_string(root, "id", "01");
    (void)ncl_json_obj_set_string(root, "type", NCL_NODE_TYPE_ROOT);
    (void)ncl_json_obj_set_string(root, "name", "适配器设备模型");
    (void)ncl_json_obj_set_string(root, "version", "1.1.0");

    (void)ncl_json_obj_set_string(node, "type",
                                  ncl_str_is_blank(device_type) ? "MACHINE"
                                                               : device_type);
    (void)ncl_json_obj_set_string(node, "id",
                                  ncl_str_is_blank(device_id) ? "01"
                                                              : device_id);
    (void)ncl_json_obj_set_string(node, "name",
                                  ncl_str_is_blank(device_name) ? "适配器设备"
                                                                : device_name);
    (void)ncl_json_obj_set_string(node, "version", "1.0");

    for (i = 0; i < decl->point_count; i++) {
        const ncl_tool_point *point = &decl->points[i];
        const char *slash = strrchr(point->path, '/');
        const char *tail = slash != NULL ? slash + 1 : point->path;
        char *type = NULL;
        char *number = NULL;
        char id[32];
        ncl_json *item = ncl_json_new_object();

        if (item == NULL) {
            goto fail;
        }
        split_type_number(tail, &type, &number);
        snprintf(id, sizeof(id), "p%u", (unsigned)i);
        (void)ncl_json_obj_set_string(item, "id", id);
        (void)ncl_json_obj_set_string(item, "name", point->path);
        (void)ncl_json_obj_set_string(item, "type",
                                      type != NULL ? type : point->path);
        if (number != NULL) {
            (void)ncl_json_obj_set_string(item, "number", number);
        }
        if (point->summary != NULL) {
            (void)ncl_json_obj_set_string(item, "description", point->summary);
        }
        if (point->writable) {
            (void)ncl_json_obj_set_bool(item, "settable", true);
        }
        /* "source" is what makes the model path equal to the point path. */
        if (slash != NULL && slash != point->path) {
            char *source = ncl_strndup(point->path + 1,
                                       (size_t)(slash - point->path - 1));

            if (source != NULL) {
                (void)ncl_json_obj_set_string(item, "source", source);
                ncl_free_safe(source);
            }
        }
        ncl_free_safe(type);
        ncl_free_safe(number);
        (void)ncl_json_arr_push(items, item);

        if (point->sampled) {
            ncl_json *ref = ncl_json_new_object();

            if (ref == NULL) {
                goto fail;
            }
            (void)ncl_json_obj_set_string(ref, "id", id);
            (void)ncl_json_arr_push(ids, ref);
        }
    }

    /* No period means "this declaration asks for no sample channel"; a site can
     * still add one to the model file, which is where the running period is
     * kept. */
    if (decl->sample_ms > 0 && ncl_json_arr_len(ids) > 0) {
        long long upload = decl->upload_ms > 0 ? decl->upload_ms
                                              : decl->sample_ms;

        (void)ncl_json_obj_set_string(channel, "id", decl->name);
        (void)ncl_json_obj_set_string(channel, "type",
                                      NCL_NODE_TYPE_SAMPLE_CHANNEL);
        (void)ncl_json_obj_set_string(
            channel, "name",
            ncl_str_is_blank(decl->description) ? decl->name
                                                : decl->description);
        (void)ncl_json_obj_set_int(channel, "sampleInterval", decl->sample_ms);
        (void)ncl_json_obj_set_int(channel, "uploadInterval", upload);
        (void)ncl_json_obj_set(channel, "ids", ids);
        (void)ncl_json_arr_push(configs, channel);
        ids = NULL;
        channel = NULL;
    }
    (void)ncl_json_obj_set(node, "dataItems", items);
    (void)ncl_json_obj_set(node, "configs", configs);
    (void)ncl_json_arr_push(devices, node);
    (void)ncl_json_obj_set(root, "devices", devices);
    return root;

fail:
    if (err != NULL && err->len == 0) {
        err_append(err, "cannot build the model of the declared tool");
    }
    ncl_json_free(root);
    ncl_json_free(devices);
    ncl_json_free(node);
    ncl_json_free(items);
    ncl_json_free(configs);
    ncl_json_free(channel);
    ncl_json_free(ids);
    return NULL;
}

/* --------------------------------------------------------------- register -- */

/** What the server hands back to the author's function. */
typedef struct {
    void                  *ctx;
    const ncl_tool_point  *point;
} ncl_tool_shim;

struct ncl_tool_registration {
    const ncl_tool_decl *decl;
    void                *ctx;
    ncl_tool_shim       *shims;
    size_t               shim_count;
};

static ncl_err shim_read(void *instance, const ncl_json *params,
                         ncl_json **result, char **reason)
{
    const ncl_tool_shim *shim = (const ncl_tool_shim *)instance;

    return shim->point->fn(shim->ctx, shim->point, NCL_OP_GET_VALUE, params,
                           result, reason);
}

static ncl_err shim_write(void *instance, const ncl_json *params,
                          ncl_json **result, char **reason)
{
    const ncl_tool_shim *shim = (const ncl_tool_shim *)instance;

    return shim->point->fn(shim->ctx, shim->point, NCL_OP_SET_VALUE, params,
                           result, reason);
}

static ncl_err shim_call(void *instance, const ncl_json *params,
                         ncl_json **result, char **reason)
{
    const ncl_tool_shim *shim = (const ncl_tool_shim *)instance;

    return shim->point->fn(shim->ctx, shim->point, NCL_OP_FUNC_CALL, params,
                           result, reason);
}

static ncl_tool_fn shim_for(ncl_operation op)
{
    switch (op) {
    case NCL_OP_GET_VALUE:
        return shim_read;
    case NCL_OP_SET_VALUE:
        return shim_write;
    default:
        return shim_call;
    }
}

/** How many operations @p point declares. */
static size_t point_operation_count(const ncl_tool_point *point)
{
    size_t i;
    size_t count = 0;

    for (i = 0; i < 3; i++) {
        if (point_declares(point, k_operations[i])) {
            count++;
        }
    }
    return count;
}

ncl_err ncl_tool_register(ncl_server *server, const ncl_tool_decl *decl,
                          const ncl_json *params, ncl_tool_registration **out,
                          ncl_strbuf *err)
{
    ncl_tool_registration *registration;
    size_t operations = 0;
    size_t at = 0;
    size_t i;

    if (out == NULL) {
        err_append(err, "ncl_tool_register() needs somewhere to put the result");
        return NCL_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (server == NULL) {
        err_append(err, "ncl_tool_register() needs a server");
        return NCL_ERR_INVALID_ARG;
    }
    if (ncl_tool_validate(decl, err) != NCL_OK) {
        return NCL_ERR_INVALID_ARG;
    }
    registration = (ncl_tool_registration *)ncl_mem_calloc(
        1, sizeof(*registration));
    if (registration == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < decl->point_count; i++) {
        operations += point_operation_count(&decl->points[i]);
    }
    registration->shims =
        (ncl_tool_shim *)ncl_mem_calloc(operations, sizeof(ncl_tool_shim));
    if (registration->shims == NULL) {
        ncl_mem_free(registration);
        return NCL_ERR_NOMEM;
    }
    registration->shim_count = operations;
    registration->decl = decl;

    /* Once for the whole tool: every point shares this connection. */
    registration->ctx = decl->open(params, NULL);
    if (registration->ctx == NULL) {
        err_append1(err, "the tool %s cannot open its connection", decl->name);
        ncl_mem_free(registration->shims);
        ncl_mem_free(registration);
        return NCL_ERR_CONNECT;
    }

    /* One registration per point: the host's instance pointer is per tool, and
     * a point's operations have to keep their own context and their own name.
     * The tool name is the declaration's, so a call is addressed
     * "<tool>/<point name>" whichever point it lands on. */
    for (i = 0; i < decl->point_count; i++) {
        const ncl_tool_point *point = &decl->points[i];
        ncl_tool_method methods[3];
        ncl_tool_binding bindings[3];
        char names[3][256];
        ncl_tool_shim *shim = &registration->shims[at];
        size_t count = 0;
        size_t op;

        shim->ctx = registration->ctx;
        shim->point = point;
        for (op = 0; op < 3; op++) {
            ncl_operation candidate = k_operations[op];

            if (!point_declares(point, candidate)) {
                continue;
            }
            point_method_name(point, candidate, names[count],
                              sizeof(names[count]));
            methods[count].name = names[count];
            methods[count].fn = shim_for(candidate);
            methods[count].params_schema = NULL;
            bindings[count].path = point->path;
            bindings[count].operation = candidate;
            bindings[count].method = names[count];
            bindings[count].tool = decl->name;
            count++;
        }
        if (ncl_server_register_tool(server, decl->name, shim, methods, count,
                                     bindings, count) != NCL_OK) {
            err_append1(err, "cannot bind the point %s", point->path);
            ncl_tool_unregister(decl, registration);
            return NCL_ERR_INVALID_ARG;
        }
        at++;
    }
    *out = registration;
    return NCL_OK;
}

void ncl_tool_unregister(const ncl_tool_decl *decl,
                         ncl_tool_registration *registration)
{
    if (registration == NULL) {
        return;
    }
    if (decl != NULL && decl->close != NULL) {
        decl->close(registration->ctx);
    }
    ncl_mem_free(registration->shims);
    ncl_mem_free(registration);
}
