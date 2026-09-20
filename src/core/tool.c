/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - the declaration seam between an adapter author and the host.
 *
 * nclink/ncl_tool.h is what an author writes; this file is the other half: the
 * helpers a point function uses, plus the two steps the host runs on a
 * declaration - ncl_tool_model() (declaration -> model document) and
 * ncl_tool_register() (declaration -> open once + one binding per point).
 *
 * Both steps take a declaration and a server and touch nothing else, so they
 * are unit tested without a device, a broker or a module in sight.
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

ncl_operation ncl_tool_kind_operation(ncl_tool_kind kind)
{
    switch (kind) {
    case NCL_TOOL_SET:
        return NCL_OP_SET_VALUE;
    case NCL_TOOL_CALL:
        return NCL_OP_FUNC_CALL;
    case NCL_TOOL_GET:
    default:
        return NCL_OP_GET_VALUE;
    }
}

const char *ncl_tool_kind_method(ncl_tool_kind kind)
{
    switch (kind) {
    case NCL_TOOL_SET:
        return "write";
    case NCL_TOOL_CALL:
        return "call";
    case NCL_TOOL_GET:
    default:
        return "read";
    }
}

/**
 * The name a point answers to inside its tool: the tail of its path, so
 * "/CNC/RESET" is reached as a call to "<tool>/RESET" (and appears as
 * "<tool>/RESET" in the OpenAPI document).
 */
static const char *ncl_tool_point_name(const ncl_tool_point *point)
{
    const char *slash = strrchr(point->path, '/');

    return slash != NULL ? slash + 1 : point->path;
}

ncl_err ncl_tool_validate(const ncl_tool_decl *decl, ncl_strbuf *err)
{
    size_t i;
    bool sampled = false;

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
        size_t j;

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
        if (point->kind != NCL_TOOL_GET && point->kind != NCL_TOOL_SET &&
            point->kind != NCL_TOOL_CALL) {
            err_append1(err, "the point %s has an unknown kind", point->path);
            return NCL_ERR_INVALID_ARG;
        }
        if (point->sampled && point->kind != NCL_TOOL_GET) {
            err_append1(err, "the point %s is sampled but not readable",
                        point->path);
            return NCL_ERR_INVALID_ARG;
        }
        for (j = 0; j < i; j++) {
            if (decl->points[j].kind == point->kind &&
                strcmp(decl->points[j].path, point->path) == 0) {
                err_append1(err, "the point %s is declared twice",
                            point->path);
                return NCL_ERR_INVALID_ARG;
            }
        }
        /* A method call is addressed as "<tool>/<name>", and the name is the
         * tail of the point path, so the tails have to be unique. */
        if (ncl_tool_point_name(point)[0] == '\0') {
            err_append1(err, "the point path %s ends with '/'", point->path);
            return NCL_ERR_INVALID_ARG;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(ncl_tool_point_name(&decl->points[j]),
                       ncl_tool_point_name(point)) == 0) {
                err_appendf(err,
                            "the points %s and %s share the method name %s",
                            decl->points[j].path, point->path,
                            ncl_tool_point_name(point));
                return NCL_ERR_INVALID_ARG;
            }
        }
        if (point->sampled) {
            sampled = true;
        }
    }
    if (sampled && decl->sample_ms <= 0) {
        err_append1(err, "the tool %s samples but declares no period",
                    decl->name);
        return NCL_ERR_INVALID_ARG;
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

    if (ncl_json_arr_len(ids) > 0) {
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

ncl_err ncl_tool_register(ncl_server *server, const ncl_tool_decl *decl,
                          const ncl_json *params, void **ctx_out,
                          ncl_strbuf *err)
{
    void *ctx;
    char *open_error = NULL;
    ncl_tool_method *methods = NULL;
    ncl_tool_binding *bindings = NULL;
    char *names = NULL; /* one block, one method name per point */
    size_t names_size = 0;
    size_t at = 0;
    size_t i;
    ncl_err result;

    if (ctx_out == NULL) {
        err_append(err, "ncl_tool_register() needs somewhere to put the context");
        return NCL_ERR_INVALID_ARG;
    }
    *ctx_out = NULL;
    if (server == NULL) {
        err_append(err, "ncl_tool_register() needs a server");
        return NCL_ERR_INVALID_ARG;
    }
    if (ncl_tool_validate(decl, err) != NCL_OK) {
        return NCL_ERR_INVALID_ARG;
    }

    /* Once for the whole tool: every point shares this context. */
    ctx = decl->open(params, &open_error);
    if (ctx == NULL) {
        err_append1(err, "the tool %s cannot open its connection", decl->name);
        if (open_error != NULL) {
            err_append1(err, ": %s", open_error);
            ncl_free_safe(open_error);
        }
        return NCL_ERR_CONNECT;
    }

    /* One tool with one method per point: that is the shape a method call
     * addresses ("<tool>/<name>") and what the schema is built from. The
     * server copies every name it is given, so the tables are scratch. */
    for (i = 0; i < decl->point_count; i++) {
        names_size += strlen(ncl_tool_point_name(&decl->points[i])) + 1;
    }
    methods = (ncl_tool_method *)ncl_mem_calloc(decl->point_count,
                                                sizeof(*methods));
    bindings = (ncl_tool_binding *)ncl_mem_calloc(decl->point_count,
                                                  sizeof(*bindings));
    names = (char *)ncl_mem_alloc(names_size);
    if (methods == NULL || bindings == NULL || names == NULL) {
        err_append1(err, "cannot register the tool %s", decl->name);
        ncl_mem_free(methods);
        ncl_mem_free(bindings);
        ncl_mem_free(names);
        ncl_tool_close(decl, ctx);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < decl->point_count; i++) {
        const ncl_tool_point *point = &decl->points[i];
        const char *name = ncl_tool_point_name(point);
        size_t length = strlen(name) + 1;

        memcpy(names + at, name, length);
        methods[i].name = names + at;
        methods[i].fn = point->fn;
        methods[i].params_schema = NULL;
        bindings[i].path = point->path;
        bindings[i].operation = ncl_tool_kind_operation(point->kind);
        bindings[i].method = names + at;
        bindings[i].tool = decl->name;
        at += length;
    }
    result = ncl_server_register_tool(server, decl->name, ctx, methods,
                                      decl->point_count, bindings,
                                      decl->point_count);
    ncl_mem_free(methods);
    ncl_mem_free(bindings);
    ncl_mem_free(names);
    if (result != NCL_OK) {
        err_append1(err, "cannot register the tool %s", decl->name);
        ncl_tool_close(decl, ctx);
        return result;
    }
    *ctx_out = ctx;
    return NCL_OK;
}

void ncl_tool_close(const ncl_tool_decl *decl, void *ctx)
{
    if (decl != NULL && decl->close != NULL) {
        decl->close(ctx);
    }
}
