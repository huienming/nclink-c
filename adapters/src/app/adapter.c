/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - configuration to live device.
 *
 * Bring up order: drivers from the configuration, then the NC-Link model
 * (loaded from a file or generated from the point map), then one operation per
 * point, then the sample channel that publishes them.
 *
 * One tool per point is what makes the framework's single-instance-per-tool
 * rule work for a generic driver: the instance carries the point's path, so a
 * method stays a plain function and no C code is generated per point.
 */

#include "nclink_adapter/ncl_adapter.h"

#include <stdio.h>
#include <string.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_platform.h"

#include "core/adapter_text.h"

typedef struct {
    ncl_driver_manager *manager; /**< borrowed */
    char               *path;    /**< owned, the point's model path */
    bool                writable;
    bool                sampled;
    ncl_node           *node;    /**< borrowed, resolved after the model is up */
} adapter_point;

typedef struct {
    ncl_driver_manager *manager;   /**< borrowed */
    char               *path;      /**< owned */
    char               *operation; /**< owned, the driver's operation name */
} adapter_method;

struct ncl_adapter {
    ncl_server         *server;
    ncl_driver_manager *manager;
    char               *sn;
    adapter_point      *points;
    size_t              point_count;
    adapter_method     *methods;
    size_t              method_count;
};

/* --------------------------------------------------------------- helpers -- */

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

/* ----------------------------------------------------------- tool methods -- */

/** Read the point through the driver and answer with its value. */
static ncl_err point_read(void *instance, const ncl_json *params,
                          ncl_json **result, char **reason)
{
    adapter_point *point = (adapter_point *)instance;
    ncl_err err;

    (void)params;
    err = ncl_driver_manager_read(point->manager, point->path, result);
    if (err != NCL_OK && reason != NULL) {
        (void)ncl_asprintf(reason, "设备读取失败（%s：%s）",
                           ncl_driver_error_tier_name(err), ncl_err_name(err));
    }
    return err;
}

/** Write the point; the value is the request's "value", or the request itself. */
static ncl_err point_write(void *instance, const ncl_json *params,
                           ncl_json **result, char **reason)
{
    adapter_point *point = (adapter_point *)instance;
    const ncl_json *value;
    ncl_err err;

    if (!point->writable) {
        if (reason != NULL) {
            (void)ncl_asprintf(reason, "点位 %s 未开放写入", point->path);
        }
        return NCL_ERR_INVALID_REQUEST;
    }
    value = ncl_json_obj_get(params, "value");
    if (value == NULL) {
        value = params;
    }
    if (value == NULL) {
        if (reason != NULL) {
            *reason = ncl_strdup("缺少写入值");
        }
        return NCL_ERR_INVALID_VALUE;
    }
    err = ncl_driver_manager_write(point->manager, point->path, value);
    if (err != NCL_OK) {
        if (reason != NULL) {
            (void)ncl_asprintf(reason, "设备写入失败（%s：%s）",
                               ncl_driver_error_tier_name(err),
                               ncl_err_name(err));
        }
        return err;
    }
    if (result != NULL) {
        *result = ncl_json_new_bool(true);
    }
    return NCL_OK;
}

/** Run a driver operation (start a program, jog an axis, ...). */
static ncl_err method_call(void *instance, const ncl_json *params,
                           ncl_json **result, char **reason)
{
    adapter_method *method = (adapter_method *)instance;
    ncl_json *value = NULL;
    ncl_err err;

    err = ncl_driver_manager_call(method->manager, method->path,
                                  method->operation, params, &value);
    if (err != NCL_OK) {
        ncl_json_free(value);
        if (reason != NULL) {
            (void)ncl_asprintf(reason, "设备方法失败（%s：%s）",
                               ncl_driver_error_tier_name(err),
                               ncl_err_name(err));
        }
        return err;
    }
    if (result != NULL) {
        /* A method with nothing to report is still a success. */
        *result = value != NULL ? value : ncl_json_new_bool(true);
    } else {
        ncl_json_free(value);
    }
    return NCL_OK;
}

/* ------------------------------------------------------------ model build -- */

/** "AXIS@0" -> type "AXIS", number "0" (number stays NULL when there is none). */
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

/**
 * Generate a model that mirrors the point map: one data item per point, with
 * the point path as its model path, plus one sample channel over the points
 * that did not opt out.
 */
static ncl_json *build_model(const ncl_adapter *adapter, const ncl_json *device,
                             long long sample_ms, long long upload_ms)
{
    const char *device_type = ncl_json_obj_get_string(device, "type");
    const char *device_id = ncl_json_obj_get_string(device, "id");
    const char *device_name = ncl_json_obj_get_string(device, "name");
    ncl_json *root = ncl_json_new_object();
    ncl_json *devices = ncl_json_new_array();
    ncl_json *node = ncl_json_new_object();
    ncl_json *items = ncl_json_new_array();
    ncl_json *configs = ncl_json_new_array();
    ncl_json *channel = ncl_json_new_object();
    ncl_json *ids = ncl_json_new_array();
    size_t i;

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
                                  ncl_str_is_blank(device_id) ? "01" : device_id);
    (void)ncl_json_obj_set_string(node, "name",
                                  ncl_str_is_blank(device_name) ? "适配器设备"
                                                                : device_name);
    (void)ncl_json_obj_set_string(node, "version", "1.0");

    for (i = 0; i < adapter->point_count; i++) {
        const adapter_point *point = &adapter->points[i];
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

            if (ref != NULL) {
                (void)ncl_json_obj_set_string(ref, "id", id);
                (void)ncl_json_arr_push(ids, ref);
            }
        }
    }

    if (ncl_json_arr_len(ids) > 0) {
        (void)ncl_json_obj_set_string(channel, "id", "adapter");
        (void)ncl_json_obj_set_string(channel, "type",
                                      NCL_NODE_TYPE_SAMPLE_CHANNEL);
        (void)ncl_json_obj_set_string(channel, "name", "适配器采样");
        (void)ncl_json_obj_set_int(channel, "sampleInterval", sample_ms);
        (void)ncl_json_obj_set_int(channel, "uploadInterval", upload_ms);
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
    ncl_json_free(root);
    ncl_json_free(devices);
    ncl_json_free(node);
    ncl_json_free(items);
    ncl_json_free(configs);
    ncl_json_free(channel);
    ncl_json_free(ids);
    return NULL;
}

/* ---------------------------------------------------------------- create -- */

/** Count the point entries of a driver configuration, for the sizing pass. */
static size_t count_points(const ncl_driver_manager *manager)
{
    /* The manager owns the point maps; this counts through the driver list. */
    size_t total = 0;
    size_t i;

    for (i = 0; i < ncl_driver_manager_count(manager); i++) {
        total += ncl_driver_manager_point_count(manager, i);
    }
    return total;
}

static ncl_err load_points(ncl_adapter *adapter)
{
    size_t i;
    size_t at = 0;

    for (i = 0; i < ncl_driver_manager_count(adapter->manager); i++) {
        size_t count = ncl_driver_manager_point_count(adapter->manager, i);
        size_t p;

        for (p = 0; p < count; p++) {
            const char *path = ncl_driver_manager_point_path(adapter->manager,
                                                            i, p);
            adapter_point *point = &adapter->points[at++];

            point->manager = adapter->manager;
            point->path = ncl_strdup(path);
            point->writable = ncl_driver_manager_point_writable(adapter->manager,
                                                               i, p);
            point->sampled = ncl_driver_manager_point_sampled(adapter->manager,
                                                             i, p);
            if (point->path == NULL) {
                return NCL_ERR_NOMEM;
            }
        }
    }
    return NCL_OK;
}

static ncl_err register_points(ncl_adapter *adapter, ncl_strbuf *err)
{
    size_t i;

    for (i = 0; i < adapter->point_count; i++) {
        adapter_point *point = &adapter->points[i];
        ncl_tool_method methods[2];
        ncl_tool_binding bindings[2];
        size_t method_count = 0;
        size_t binding_count = 0;
        ncl_err result;

        methods[method_count].name = "read";
        methods[method_count].fn = point_read;
        methods[method_count].params_schema = NULL;
        method_count++;
        bindings[binding_count].path = point->path;
        bindings[binding_count].operation = NCL_OP_GET_VALUE;
        bindings[binding_count].method = "read";
        bindings[binding_count].tool = NULL;
        binding_count++;

        if (point->writable) {
            methods[method_count].name = "write";
            methods[method_count].fn = point_write;
            methods[method_count].params_schema = NULL;
            method_count++;
            bindings[binding_count].path = point->path;
            bindings[binding_count].operation = NCL_OP_SET_VALUE;
            bindings[binding_count].method = "write";
            bindings[binding_count].tool = NULL;
            binding_count++;
        }
        result = ncl_server_register_tool(adapter->server, point->path, point,
                                          methods, method_count, bindings,
                                          binding_count);
        if (result != NCL_OK) {
            err_append1(err, "cannot bind the point %s", point->path);
            return result;
        }
    }
    for (i = 0; i < adapter->method_count; i++) {
        adapter_method *method = &adapter->methods[i];
        ncl_tool_method methods[1];
        ncl_tool_binding bindings[1];

        methods[0].name = method->operation;
        methods[0].fn = method_call;
        methods[0].params_schema = NULL;
        bindings[0].path = method->path;
        bindings[0].operation = NCL_OP_FUNC_CALL;
        bindings[0].method = method->operation;
        bindings[0].tool = NULL;
        if (ncl_server_register_tool(adapter->server, method->path, method,
                                     methods, 1, bindings, 1) != NCL_OK) {
            err_append1(err, "cannot bind the method %s", method->path);
            return NCL_ERR_INVALID_ARG;
        }
    }
    return NCL_OK;
}

static void resolve_nodes(ncl_adapter *adapter)
{
    ncl_node_map map;
    size_t i;

    ncl_node_map_init(&map);
    if (ncl_root_node_path_map(ncl_server_model(adapter->server), &map) !=
        NCL_OK) {
        ncl_node_map_free(&map);
        return;
    }
    for (i = 0; i < adapter->point_count; i++) {
        adapter->points[i].node = ncl_node_map_get(&map, adapter->points[i].path);
    }
    ncl_node_map_free(&map);
}

static ncl_err load_drivers(ncl_adapter *adapter, const ncl_json *config,
                            ncl_strbuf *err)
{
    const char *directory = ncl_json_obj_get_string(config, "driverDir");
    const char *file = ncl_json_obj_get_string(config, "driverFile");
    const ncl_json *inline_drivers = ncl_json_obj_get(config, "drivers");

    if (inline_drivers != NULL) {
        return ncl_driver_manager_add_json(adapter->manager, inline_drivers, err);
    }
    if (!ncl_str_is_blank(file)) {
        return ncl_driver_manager_load_file(adapter->manager, file, err);
    }
    if (ncl_str_is_blank(directory)) {
        directory = ncl_env_driver_path();
    }
    return ncl_driver_manager_load_dir(adapter->manager, directory, err);
}

static ncl_json *load_model_document(const ncl_json *config, ncl_strbuf *err)
{
    const char *file = ncl_json_obj_get_string(config, "model");

    if (ncl_str_is_blank(file)) {
        return NULL;
    }
    return ncl_adapter_json_from_file(file, err);
}

ncl_adapter *ncl_adapter_create(const ncl_json *config, ncl_strbuf *err)
{
    ncl_adapter *adapter;
    const ncl_json *device;
    const ncl_json *sample;
    const ncl_json *methods;
    ncl_server_options options;
    ncl_json *model = NULL;
    ncl_err result;

    if (ncl_json_type_of(config) != NCL_JSON_OBJECT) {
        err_append(err, "the adapter configuration must be an object");
        return NULL;
    }
    adapter = (ncl_adapter *)ncl_mem_calloc(1, sizeof(*adapter));
    if (adapter == NULL) {
        return NULL;
    }
    adapter->manager = ncl_driver_manager_create();
    if (adapter->manager == NULL) {
        ncl_adapter_free(adapter);
        return NULL;
    }
    result = load_drivers(adapter, config, err);
    if (result != NCL_OK || ncl_driver_manager_count(adapter->manager) == 0) {
        if (result == NCL_OK) {
            err_append(err, "the configuration describes no driver");
        }
        ncl_adapter_free(adapter);
        return NULL;
    }

    adapter->sn = ncl_json_obj_get_string(config, "sn") != NULL
                      ? ncl_strdup(ncl_json_obj_get_string(config, "sn"))
                      : ncl_sn_read();
    if (adapter->sn == NULL) {
        err_append(err, "no serial number (\"sn\" or bin/sn.txt)");
        ncl_adapter_free(adapter);
        return NULL;
    }

    device = ncl_json_obj_get(config, "device");
    sample = ncl_json_obj_get(config, "sample");
    methods = ncl_json_obj_get(config, "methods");

    adapter->point_count = count_points(adapter->manager);
    adapter->method_count = ncl_json_arr_len(methods);
    if (adapter->point_count > 0) {
        adapter->points = (adapter_point *)ncl_mem_calloc(adapter->point_count,
                                                          sizeof(adapter_point));
    }
    if (adapter->method_count > 0) {
        adapter->methods = (adapter_method *)ncl_mem_calloc(
            adapter->method_count, sizeof(adapter_method));
    }
    if ((adapter->point_count > 0 && adapter->points == NULL) ||
        (adapter->method_count > 0 && adapter->methods == NULL)) {
        ncl_adapter_free(adapter);
        return NULL;
    }
    if (load_points(adapter) != NCL_OK) {
        err_append(err, "out of memory while reading the point map");
        ncl_adapter_free(adapter);
        return NULL;
    }
    {
        size_t i;

        for (i = 0; i < adapter->method_count; i++) {
            const ncl_json *entry = ncl_json_arr_get(methods, i);
            const char *path = ncl_json_obj_get_string(entry, "path");
            const char *operation = ncl_json_obj_get_string(entry, "operation");

            if (ncl_str_is_blank(path) || ncl_str_is_blank(operation)) {
                err_append(err, "a method entry needs \"path\" and \"operation\"");
                ncl_adapter_free(adapter);
                return NULL;
            }
            adapter->methods[i].manager = adapter->manager;
            adapter->methods[i].path = ncl_strdup(path);
            adapter->methods[i].operation = ncl_strdup(operation);
            if (adapter->methods[i].path == NULL ||
                adapter->methods[i].operation == NULL) {
                ncl_adapter_free(adapter);
                return NULL;
            }
        }
    }

    model = load_model_document(config, err);
    if (model == NULL && ncl_json_obj_get_string(config, "model") != NULL) {
        ncl_adapter_free(adapter);
        return NULL;
    }
    if (model == NULL) {
        model = build_model(adapter, device,
                            ncl_json_obj_get_int(sample, "intervalMs", 1000),
                            ncl_json_obj_get_int(sample, "uploadMs", 1000));
        if (model == NULL) {
            err_append(err, "cannot build the model");
            ncl_adapter_free(adapter);
            return NULL;
        }
    }

    memset(&options, 0, sizeof(options));
    options.sn = adapter->sn;
    options.model_json = ncl_json_write_string(model);
    ncl_json_free(model);
    if (options.model_json == NULL) {
        ncl_adapter_free(adapter);
        return NULL;
    }
    adapter->server = ncl_server_create(&options);
    ncl_free_safe((void *)options.model_json);
    if (adapter->server == NULL) {
        err_append(err, "cannot bring up the NC-Link server");
        ncl_adapter_free(adapter);
        return NULL;
    }
    resolve_nodes(adapter);
    if (register_points(adapter, err) != NCL_OK) {
        ncl_adapter_free(adapter);
        return NULL;
    }
    return adapter;
}

ncl_adapter *ncl_adapter_create_from_file(const char *path, ncl_strbuf *err)
{
    ncl_json *config;
    ncl_adapter *adapter;

    config = ncl_adapter_json_from_file(path, err);
    if (config == NULL) {
        return NULL;
    }
    adapter = ncl_adapter_create(config, err);
    ncl_json_free(config);
    return adapter;
}

void ncl_adapter_free(ncl_adapter *adapter)
{
    size_t i;

    if (adapter == NULL) {
        return;
    }
    ncl_server_free(adapter->server);
    for (i = 0; i < adapter->point_count; i++) {
        ncl_free_safe(adapter->points[i].path);
    }
    for (i = 0; i < adapter->method_count; i++) {
        ncl_free_safe(adapter->methods[i].path);
        ncl_free_safe(adapter->methods[i].operation);
    }
    ncl_free_safe(adapter->points);
    ncl_free_safe(adapter->methods);
    ncl_driver_manager_free(adapter->manager);
    ncl_free_safe(adapter->sn);
    ncl_free_safe(adapter);
}

/* -------------------------------------------------------------- accessors -- */

ncl_server *ncl_adapter_server(ncl_adapter *adapter)
{
    return adapter != NULL ? adapter->server : NULL;
}

ncl_driver_manager *ncl_adapter_drivers(ncl_adapter *adapter)
{
    return adapter != NULL ? adapter->manager : NULL;
}

const char *ncl_adapter_sn(const ncl_adapter *adapter)
{
    return adapter != NULL ? adapter->sn : NULL;
}

size_t ncl_adapter_point_count(const ncl_adapter *adapter)
{
    return adapter != NULL ? adapter->point_count : 0;
}

const char *ncl_adapter_point_path(const ncl_adapter *adapter, size_t index)
{
    if (adapter == NULL || index >= adapter->point_count) {
        return NULL;
    }
    return adapter->points[index].path;
}

size_t ncl_adapter_method_count(const ncl_adapter *adapter)
{
    return adapter != NULL ? adapter->method_count : 0;
}

/* ------------------------------------------------------------------ poll -- */

ncl_err ncl_adapter_poll(ncl_adapter *adapter, ncl_strbuf *err)
{
    ncl_err first = NCL_OK;
    size_t i;

    if (adapter == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < adapter->point_count; i++) {
        adapter_point *point = &adapter->points[i];
        ncl_json *value = NULL;
        ncl_err result = ncl_driver_manager_read(adapter->manager, point->path,
                                                 &value);

        if (result != NCL_OK) {
            err_append1(err, "cannot read %s", point->path);
            if (first == NCL_OK) {
                first = result;
            }
            continue;
        }
        if (point->node != NULL) {
            ncl_json_free(point->node->value);
            point->node->value = value; /* the model takes it over */
        } else {
            ncl_json_free(value);
        }
    }
    return first;
}
