/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - configuration to live device.
 *
 * Bring up order: the MQTT session (when the configuration asks for one), the
 * drivers, then the NC-Link model (loaded from a file or generated from the
 * point map), then one operation per point, then the sample channel that
 * publishes them.
 *
 * One tool per point is what makes the framework's single-instance-per-tool
 * rule work for a generic driver: the instance carries the point's path, so a
 * method stays a plain function and no C code is generated per point.
 *
 * The broker session lives here rather than in the host so that "a point map
 * in, a device on the bus out" holds for every host: ncl_adapter and the
 * vendor collectors both hand the configuration over and get a device that
 * answers on its MQTT topics. A broker that is not up yet is not fatal - the
 * host's loop calls ncl_adapter_broker_poll(), which retries with a backoff.
 */

#include "nclink_adapter/ncl_adapter.h"

#include <stdio.h>
#include <string.h>

#include "nclink/ncl_env.h"
#include "nclink_adapter/ncl_driver.h"
#include "nclink_adapter/ncl_audit.h"
#include "nclink/ncl_config.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_mqtt.h"
#include "nclink/ncl_platform.h"

#include "core/adapter_text.h"

typedef struct {
    char     *path;     /**< owned, the point's model path */
    bool      writable;
    bool      sampled;
    /** False for a point that is declared but not readable yet (a pending
     *  point, see ncl_link's NCL_POINT_PENDING): it stays in the list so the
     *  self check can name it, but no round ever reads it - it cannot succeed,
     *  and a round is not the place to learn that again every second. */
    bool      available;
    /** Borrowed from the declaration: why it is not readable yet. */
    const char *summary;
    ncl_node *node;     /**< borrowed, resolved after the model is up */
} adapter_point;

struct ncl_adapter {
    ncl_server         *server;
    /** The host's loaded modules (borrowed), and the declaration one of them
     *  brought: with @p decl set, the points, the model and the bindings come
     *  from the module and the configuration only carries parameters. */
    const ncl_module_set  *modules;
    const ncl_tool_decl   *decl;
    /** Owned: what ncl_tool_register() opened, released in ncl_adapter_free(). */
    ncl_tool_registration *registration;
    char               *sn;
    /** Owned: the model document this device publishes - the one built from the
     *  declaration, or the file the configuration named. Kept after the server
     *  takes its own copy so a host can show or save it (`--model`). */
    ncl_json           *model;
    adapter_point      *points;
    size_t              point_count;
    /* MQTT session, owned: NULL when the configuration says offline. */
    ncl_mqtt_client    *mqtt;
    char               *broker_url;
    bool                broker_ever_connected;
    bool                subscribed;
    int64_t             broker_retry_at;  /**< monotonic ms, 0 = now */
    unsigned            broker_delay_ms;  /**< current backoff */
    unsigned            broker_delay_max_ms;
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

/** Same, for a diagnostic that needs more than one value. */
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

/* ---------------------------------------------------------- declared tool -- */

/* §6: the trail for a declared tool is the host's business, so an adapter
 * author never writes audit code. The core's shim reports every point call
 * here; what only the module knows - the frames - arrives through the optional
 * last_raw callback, and the address column carries the point's name because
 * the protocol address itself lives inside the module. */

static bool audit_wants_raw(void *user)
{
    (void)user;
    return ncl_audit_wants_raw();
}

static void audit_request(void *user, const char *tool,
                          const ncl_tool_point *point, ncl_operation op, int code,
                          int64_t micros, const ncl_tool_frames *frames)
{
    ncl_driver_raw raw;
    bool has_frames = frames != NULL &&
                      (frames->request != NULL || frames->reply != NULL);

    (void)user;
    (void)op; /* the path is what the request named */
    memset(&raw, 0, sizeof(raw));
    if (has_frames) {
        raw.request = (const uint8_t *)frames->request;
        raw.request_len = frames->request_len;
        raw.reply = (const uint8_t *)frames->reply;
        raw.reply_len = frames->reply_len;
    }
    ncl_audit_request(tool, point->path, ncl_tool_point_name(point), code, micros,
                      has_frames ? &raw : NULL);
}

static void audit_write(void *user, const char *tool,
                        const ncl_tool_point *point, const ncl_json *old_value,
                        const ncl_json *new_value, int code)
{
    (void)user;
    ncl_audit_write(tool, point->path, ncl_tool_point_name(point), old_value,
                    new_value, code);
}

static const ncl_tool_audit k_tool_audit = {
    NULL, audit_wants_raw, audit_request, audit_write,
};

/**
 * The declaration a loaded module brought, if any. One device serves one
 * declared tool: two such modules would fight over the same device node and the
 * same sample channel, so that is refused with both names.
 */
static const ncl_tool_decl *pick_tool(const ncl_module_set *modules,
                                      bool *conflict, ncl_strbuf *err)
{
    const ncl_tool_decl *tool = NULL;
    const char *first = NULL;
    size_t i;

    *conflict = false;
    for (i = 0; modules != NULL && i < ncl_module_count(modules); i++) {
        const ncl_tool_decl *candidate = ncl_module_tool(modules, i);

        if (candidate == NULL) {
            continue;
        }
        if (tool != NULL) {
            err_appendf(err, "一个设备只能有一个声明式适配器模块（%s 和 %s）",
                        first, ncl_module_name(modules, i));
            *conflict = true;
            return NULL;
        }
        tool = candidate;
        first = ncl_module_name(modules, i);
    }
    return tool;
}

/**
 * The "parameters" object a tool module opens with. Two shapes are accepted:
 *
 *   "tools":   [ { "name": "focas",   "parameters": { ... } } ]
 *   "drivers": [ { "type": "focas",   "parameters": { ... } } ]
 *
 * The second one is what a configuration already holds when the module used to
 * be a driver, so a site does not have to rewrite its connection settings while
 * an adapter moves over to a declaration.
 */
static const ncl_json *tool_parameters(const ncl_json *config, const char *name)
{
    const ncl_json *tools = ncl_json_obj_get(config, "tools");
    const ncl_json *drivers;
    size_t i;

    if (ncl_json_type_of(tools) == NCL_JSON_ARRAY) {
        for (i = 0; i < ncl_json_arr_len(tools); i++) {
            const ncl_json *entry = ncl_json_arr_get(tools, i);
            const char *entry_name = ncl_json_obj_get_string(entry, "name");

            if (entry_name != NULL && strcmp(entry_name, name) == 0) {
                return ncl_json_obj_get(entry, "parameters");
            }
        }
    }
    drivers = ncl_json_obj_get(config, "drivers");
    if (ncl_json_type_of(drivers) == NCL_JSON_ARRAY) {
        for (i = 0; i < ncl_json_arr_len(drivers); i++) {
            const ncl_json *entry = ncl_json_arr_get(drivers, i);
            const char *type = ncl_json_obj_get_string(entry, "type");
            const char *id = ncl_json_obj_get_string(entry, "id");

            if ((type != NULL && strcmp(type, name) == 0) ||
                (id != NULL && strcmp(id, name) == 0)) {
                return ncl_json_obj_get(entry, "parameters");
            }
        }
    }
    return NULL;
}

/** Every point of the declaration is a model path of this device. */
static ncl_err load_declared_points(ncl_adapter *adapter)
{
    size_t declared_count = adapter->decl->point_count;
    size_t at = 0;
    size_t i;

    /* A point that only answers calls is a method: it has no value to poll, so
     * it stays out of the point list (and out of the model) - the host reaches
     * it through its binding, exactly like a configuration's "methods". */
    for (i = 0; i < declared_count; i++) {
        const ncl_tool_point *declared = &adapter->decl->points[i];

        if (declared->readable || declared->writable) {
            adapter->point_count++;
        }
    }
    if (adapter->point_count == 0) {
        return NCL_OK;
    }
    adapter->points = (adapter_point *)ncl_mem_calloc(adapter->point_count,
                                                      sizeof(adapter_point));
    if (adapter->points == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < declared_count; i++) {
        const ncl_tool_point *declared = &adapter->decl->points[i];
        adapter_point *point;

        if (!declared->readable && !declared->writable) {
            continue;
        }
        point = &adapter->points[at++];
        point->path = ncl_strdup(declared->path);
        point->writable = declared->writable;
        point->sampled = declared->sampled;
        point->available = declared->available;
        point->summary = declared->summary;
        if (point->path == NULL) {
            return NCL_ERR_NOMEM;
        }
    }
    return NCL_OK;
}

/**
 * Read one declared point the way a client would - through the binding the
 * module registered - so the point's own function runs and a protocol failure
 * comes back as that module describes it.
 *
 * The numeric code of the failure does not survive the response (it carries OK
 * or NG plus the message), so a failed read is reported as NCL_ERR_IO: that is
 * tier 1, which makes ncl_adapter_poll_round() stop instead of paying one
 * connect timeout per point when the machine is down.
 */
static ncl_err read_declared_point(ncl_adapter *adapter, const char *path,
                                   ncl_json **value, ncl_strbuf *err)
{
    ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    ncl_query_request_item *item;
    ncl_message *response;
    ncl_query_response_item *answer;
    ncl_err result = NCL_OK;

    if (request == NULL) {
        return NCL_ERR_NOMEM;
    }
    item = ncl_query_request_item_new(path);
    if (item == NULL) {
        ncl_message_free(request);
        return NCL_ERR_NOMEM;
    }
    (void)ncl_params_set_string(&item->params, "operation", "get_value");
    (void)ncl_message_set_message_id(request, "poll");
    (void)ncl_message_add_query_request_item(request, item);
    response = ncl_server_invoke_query(adapter->server, request);
    ncl_message_free(request);
    if (response == NULL) {
        err_append1(err, "cannot read %s", path);
        return NCL_ERR_STATE;
    }
    answer = (ncl_query_response_item *)ncl_message_item_at(response, 0);
    if (answer == NULL || answer->code == NULL ||
        strcmp(answer->code, NCL_KW_CODE_OK) != 0) {
        err_append1(err, "cannot read %s", path);
        if (answer != NULL && answer->reason != NULL) {
            err_append(err, answer->reason);
        }
        result = NCL_ERR_IO;
    } else {
        *value = ncl_json_clone(ncl_query_response_item_data(answer));
        if (*value == NULL) {
            result = NCL_ERR_NOMEM;
        }
    }
    ncl_message_free(response);
    return result;
}

/* --------------------------------------------------------------- broker -- */

/** Every inbound request: parse it, then hand it to the server (which runs the
 *  tool on the shared pool, so the reader thread does not block). */
static void broker_on_message(void *user, const ncl_mqtt_publish *publish)
{
    ncl_adapter *adapter = (ncl_adapter *)user;
    ncl_message *request;

    if (publish->topic == NULL || adapter->server == NULL) {
        return;
    }
    request = ncl_message_parse(publish->topic,
                                (const char *)publish->payload,
                                publish->payload_len);
    if (request == NULL) {
        ncl_log_warn("无法解析来自 %s 的报文", publish->topic);
        return;
    }
    ncl_server_on_message(adapter->server, publish->topic, request);
}

static void broker_on_connect(void *user, bool reconnect,
                              const ncl_mqtt_connack *connack)
{
    ncl_adapter *adapter = (ncl_adapter *)user;

    (void)connack;
    if (reconnect) {
        ncl_log_info("MQTT 已重连: %s", ncl_adapter_broker_url(adapter));
    }
}

static void broker_on_disconnect(void *user, uint8_t reason_code,
                                 bool will_reconnect)
{
    ncl_adapter *adapter = (ncl_adapter *)user;

    ncl_log_warn("MQTT 断开（%s，原因码 %u%s）",
                 ncl_adapter_broker_url(adapter) != NULL
                     ? ncl_adapter_broker_url(adapter)
                     : "?",
                 (unsigned)reason_code,
                 will_reconnect ? "，库会在后台重连" : "");
}

/**
 * Bring the session up: create the client, connect, subscribe. A client that
 * cannot reach the broker yet is kept (with a backoff) so the poll can retry -
 * a collector must come up on a machine whose broker is still booting.
 */
static ncl_err broker_open(ncl_adapter *adapter, const ncl_json *config,
                           ncl_strbuf *err)
{
    ncl_mqtt_client_options options;
    const char *url;
    const char *text;
    ncl_err result;

    if (config == NULL || ncl_json_type_of(config) != NCL_JSON_OBJECT) {
        return NCL_OK; /* no "mqtt" object: offline */
    }
    if (ncl_json_obj_get_bool(config, "offline", false)) {
        ncl_log_info("适配器离线运行（配置里 mqtt.offline = true）");
        return NCL_OK;
    }
    url = ncl_json_obj_get_string(config, "url");
    if (ncl_str_is_blank(url)) {
        /* Say it out loud: an empty url is a configuration mistake, not a wish
         * to stay offline (that is what "offline": true is for). */
        err_append(err, "mqtt.url is empty (\"offline\": true means offline)");
        return NCL_ERR_INVALID_ARG;
    }
    adapter->broker_url = ncl_strdup(url);
    if (adapter->broker_url == NULL) {
        return NCL_ERR_NOMEM;
    }
    adapter->broker_delay_ms =
        (unsigned)ncl_json_obj_get_int(config, "reconnectDelayMs", 1000);
    if (adapter->broker_delay_ms < 200u) {
        adapter->broker_delay_ms = 200u;
    }
    adapter->broker_delay_max_ms =
        (unsigned)ncl_json_obj_get_int(config, "reconnectMaxDelayMs", 30000);
    if (adapter->broker_delay_max_ms < adapter->broker_delay_ms) {
        adapter->broker_delay_max_ms = adapter->broker_delay_ms;
    }

    ncl_mqtt_client_options_default(&options);
    options.url = adapter->broker_url;
    text = ncl_json_obj_get_string(config, "clientId");
    options.client_id = !ncl_str_is_blank(text) ? text : adapter->sn;
    text = ncl_json_obj_get_string(config, "username");
    options.username = !ncl_str_is_blank(text) ? text : NULL;
    text = ncl_json_obj_get_string(config, "password");
    options.password = !ncl_str_is_blank(text) ? text : NULL;
    options.keep_alive_seconds =
        (unsigned)ncl_json_obj_get_int(config, "keepAliveSeconds", 60);
    options.connect_timeout_ms =
        (unsigned)ncl_json_obj_get_int(config, "connectTimeoutMs", 10000);
    options.automatic_reconnect =
        ncl_json_obj_get_bool(config, "automaticReconnect", true);
    options.reconnect_delay_ms = adapter->broker_delay_ms;
    options.reconnect_max_delay_ms = adapter->broker_delay_max_ms;
    options.on_connect = broker_on_connect;
    options.on_disconnect = broker_on_disconnect;
    options.on_message = broker_on_message;
    options.user = adapter;

    adapter->mqtt = ncl_mqtt_client_create(&options);
    if (adapter->mqtt == NULL) {
        err_append(err, "MQTT 客户端创建失败");
        return NCL_ERR_NOMEM;
    }
    result = ncl_mqtt_client_connect(adapter->mqtt);
    if (result != NCL_OK) {
        /* Not fatal: the host loop retries (see ncl_adapter_broker_poll). */
        ncl_log_warn("MQTT 暂未连上 %s：%s（稍后自动重试）", adapter->broker_url,
                     ncl_mqtt_client_last_error(adapter->mqtt));
        adapter->broker_retry_at =
            ncl_time_monotonic_millis() + adapter->broker_delay_ms;
    } else {
        adapter->broker_ever_connected = true;
        ncl_log_info("MQTT 已连接: %s（clientId %s）", adapter->broker_url,
                     options.client_id != NULL ? options.client_id : "");
    }
    return NCL_OK;
}

/** Subscribe the request topics once the session is up. */
static void broker_subscribe(ncl_adapter *adapter)
{
    if (adapter->subscribed || adapter->server == NULL) {
        return;
    }
    if (ncl_server_subscribe(adapter->server) == NCL_OK) {
        adapter->subscribed = true;
        ncl_log_info("已订阅 %s 的请求主题", adapter->sn);
    } else {
        ncl_log_warn("MQTT 订阅失败，稍后重试");
    }
}

ncl_err ncl_adapter_config_set_broker(ncl_json *config, const char *broker,
                                      bool offline, ncl_strbuf *err)
{
    ncl_json *mqtt;
    ncl_err result = NCL_OK;

    if (config == NULL || ncl_json_type_of(config) != NCL_JSON_OBJECT) {
        err_append(err, "the adapter configuration must be an object");
        return NCL_ERR_INVALID_ARG;
    }
    if (broker == NULL && !offline &&
        ncl_json_type_of(ncl_json_obj_get(config, "mqtt")) == NCL_JSON_OBJECT) {
        /* The configuration names its own broker: neither the command line nor
         * <conf>/mqtt.cfg may override it. */
        return NCL_OK;
    }
    mqtt = ncl_json_new_object();
    if (mqtt == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (offline || (broker != NULL && strcmp(broker, "-") == 0)) {
        (void)ncl_json_obj_set_bool(mqtt, "offline", true);
    } else if (broker != NULL && broker[0] != '\0') {
        (void)ncl_json_obj_set_string(mqtt, "url", broker);
    } else {
        ncl_json *file = ncl_config_get_mqtt();
        const char *value;

        if (file == NULL) {
            err_append1(err, "cannot read the broker configuration %s",
                        ncl_env_mqtt_cfg_file());
            ncl_json_free(mqtt);
            return NCL_ERR_IO;
        }
        value = ncl_json_obj_get_string(file, "url");
        if (ncl_str_is_blank(value)) {
            /* No broker on this box: run offline rather than refuse to start. */
            (void)ncl_json_obj_set_bool(mqtt, "offline", true);
        } else {
            (void)ncl_json_obj_set_string(mqtt, "url", value);
            value = ncl_json_obj_get_string(file, "username");
            if (!ncl_str_is_blank(value)) {
                (void)ncl_json_obj_set_string(mqtt, "username", value);
            }
            value = ncl_json_obj_get_string(file, "password");
            if (!ncl_str_is_blank(value)) {
                (void)ncl_json_obj_set_string(mqtt, "password", value);
            }
        }
        ncl_json_free(file);
    }
    if (ncl_json_obj_set(config, "mqtt", mqtt) != NCL_OK) {
        result = NCL_ERR_NOMEM;
        ncl_json_free(mqtt);
        return result;
    }
    return NCL_OK; /* the configuration owns the object now */
}

/* ---------------------------------------------------------------- create -- */

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
    return ncl_adapter_create_with_modules(config, NULL, err);
}

ncl_adapter *ncl_adapter_create_with_modules(const ncl_json *config,
                                             const ncl_module_set *modules,
                                             ncl_strbuf *err)
{
    ncl_adapter *adapter;
    const ncl_json *device;
    ncl_server_options options;
    ncl_json *model = NULL;
    bool tool_conflict = false;

    if (ncl_json_type_of(config) != NCL_JSON_OBJECT) {
        err_append(err, "the adapter configuration must be an object");
        return NULL;
    }
    adapter = (ncl_adapter *)ncl_mem_calloc(1, sizeof(*adapter));
    if (adapter == NULL) {
        return NULL;
    }
    adapter->modules = modules;
    adapter->decl = pick_tool(modules, &tool_conflict, err);
    if (tool_conflict) {
        ncl_adapter_free(adapter);
        return NULL;
    }
    if (adapter->decl == NULL) {
        /* The points live in the module now. A configuration that still carries
         * the old point map is told so by name, because "no driver" would send a
         * site looking in the wrong place. */
        bool legacy = ncl_json_type_of(ncl_json_obj_get(config, "drivers")) ==
                          NCL_JSON_ARRAY ||
                      ncl_json_type_of(ncl_json_obj_get(config, "methods")) ==
                          NCL_JSON_ARRAY;

        if (legacy) {
            err_append(err,
                       "配置里的 drivers[]/methods[] 是老写法：点位和方法已经声明在"
                       "适配器模块里，配置只写 tools[].parameters"
                       "（见 adapters/FANUC-ADAPTER.md 第 5 节）");
        } else {
            err_append(err, "没有声明式适配器模块：把模块放进 plugins/，"
                            "并在配置里用 tools[].name 指名");
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

    if (load_declared_points(adapter) != NCL_OK) {
        err_append(err, "out of memory while reading the declaration");
        ncl_adapter_free(adapter);
        return NULL;
    }

    model = load_model_document(config, err);
    if (model == NULL && ncl_json_obj_get_string(config, "model") != NULL) {
        ncl_adapter_free(adapter);
        return NULL;
    }
    if (model == NULL) {
        /* The declaration is the point map: one data item per declared value,
         * one sample channel over the sampled ones, periods from the
         * declaration. A configuration that names a "model" file wins above -
         * that is where a site tunes sampling. */
        model = ncl_tool_model(adapter->decl, device, err);
        if (model == NULL) {
            err_append(err, "cannot build the model");
            ncl_adapter_free(adapter);
            return NULL;
        }
    }

    /* The broker first: the server takes the client at creation and needs the
     * session up before it can subscribe. */
    if (broker_open(adapter, ncl_json_obj_get(config, "mqtt"), err) != NCL_OK) {
        ncl_adapter_free(adapter);
        return NULL;
    }

    /* A copy stays here: the server owns its own, and a host that wants to look
     * at (or hand out) the model it is running should not have to guess it. */
    adapter->model = ncl_json_clone(model);
    if (adapter->model == NULL) {
        ncl_json_free(model);
        ncl_adapter_free(adapter);
        return NULL;
    }

    memset(&options, 0, sizeof(options));
    options.sn = adapter->sn;
    options.mqtt = adapter->mqtt;
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
    /* The callback is unregistered until the server exists; from here on the
     * request topics can be answered. */
    if (adapter->mqtt != NULL && ncl_mqtt_client_is_connected(adapter->mqtt)) {
        broker_subscribe(adapter);
    }
    resolve_nodes(adapter);
    if (adapter->decl != NULL) {
        /* The module opens its own connection and binds its own points; the
         * configuration only supplies its "parameters". */
        const ncl_json *parameters =
            tool_parameters(config, adapter->decl->name);

        if (ncl_tool_register(adapter->server, adapter->decl, parameters,
                              &k_tool_audit, &adapter->registration,
                              err) != NCL_OK) {
            ncl_adapter_free(adapter);
            return NULL;
        }
        ncl_audit_session(adapter->decl->name, "open", NULL);
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
    /* The server first: it stops the sample threads that publish through the
     * MQTT client this adapter owns. */
    ncl_server_free(adapter->server);
    /* Then the declared tool: the server is gone, so nothing can call into it
     * any more and close() runs exactly once. */
    if (adapter->decl != NULL) {
        ncl_audit_session(adapter->decl->name, "close", NULL);
    }
    ncl_tool_unregister(adapter->decl, adapter->registration);
    if (adapter->mqtt != NULL) {
        ncl_mqtt_client_disconnect(adapter->mqtt);
        ncl_mqtt_client_destroy(adapter->mqtt);
    }
    ncl_free_safe(adapter->broker_url);
    for (i = 0; i < adapter->point_count; i++) {
        ncl_free_safe(adapter->points[i].path);
    }
    ncl_free_safe(adapter->points);
    ncl_json_free(adapter->model);
    ncl_free_safe(adapter->sn);
    ncl_free_safe(adapter);
}

/* -------------------------------------------------------------- accessors -- */

ncl_server *ncl_adapter_server(ncl_adapter *adapter)
{
    return adapter != NULL ? adapter->server : NULL;
}


const ncl_tool_decl *ncl_adapter_tool(const ncl_adapter *adapter)
{
    return adapter != NULL ? adapter->decl : NULL;
}

const ncl_json *ncl_adapter_model(const ncl_adapter *adapter)
{
    return adapter != NULL ? adapter->model : NULL;
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

const ncl_json *ncl_adapter_point_value(const ncl_adapter *adapter,
                                        size_t index)
{
    if (adapter == NULL || index >= adapter->point_count) {
        return NULL;
    }
    return adapter->points[index].node != NULL
               ? adapter->points[index].node->value
               : NULL;
}

bool ncl_adapter_point_available(const ncl_adapter *adapter, size_t index)
{
    if (adapter == NULL || index >= adapter->point_count) {
        return false;
    }
    return adapter->points[index].available;
}

const char *ncl_adapter_point_summary(const ncl_adapter *adapter, size_t index)
{
    if (adapter == NULL || index >= adapter->point_count) {
        return NULL;
    }
    return adapter->points[index].summary;
}


const char *ncl_adapter_broker_url(const ncl_adapter *adapter)
{
    return adapter != NULL ? adapter->broker_url : NULL;
}

bool ncl_adapter_online(const ncl_adapter *adapter)
{
    return adapter != NULL && adapter->mqtt != NULL &&
           ncl_mqtt_client_is_connected((ncl_mqtt_client *)adapter->mqtt);
}

ncl_err ncl_adapter_broker_poll(ncl_adapter *adapter, ncl_strbuf *err)
{
    ncl_err result;

    if (adapter == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (adapter->mqtt == NULL) {
        return NCL_OK; /* offline by configuration */
    }
    if (ncl_mqtt_client_is_connected(adapter->mqtt)) {
        if (!adapter->subscribed) {
            broker_subscribe(adapter);
        }
        return NCL_OK;
    }
    if (adapter->broker_ever_connected) {
        /* The library owns this one: automatic reconnect restores the session
         * and the subscriptions, so a second connect here would only race. */
        if (adapter->subscribed) {
            adapter->subscribed = false; /* a later poll re-subscribes */
        }
        if (err != NULL) {
            (void)ncl_strbuf_printf(err, "MQTT 会话断开中（%s）",
                                    adapter->broker_url);
        }
        return NCL_ERR_CLOSED;
    }
    if (ncl_time_monotonic_millis() < adapter->broker_retry_at) {
        if (err != NULL) {
            (void)ncl_strbuf_printf(err, "MQTT 未连上（%s）", adapter->broker_url);
        }
        return NCL_ERR_CLOSED;
    }
    result = ncl_mqtt_client_connect(adapter->mqtt);
    if (result != NCL_OK) {
        adapter->broker_delay_ms = adapter->broker_delay_ms <
                                           adapter->broker_delay_max_ms / 2u
                                       ? adapter->broker_delay_ms * 2u
                                       : adapter->broker_delay_max_ms;
        adapter->broker_retry_at =
            ncl_time_monotonic_millis() + adapter->broker_delay_ms;
        if (err != NULL) {
            (void)ncl_strbuf_printf(err, "MQTT 重连失败（%s）：%s",
                                    adapter->broker_url,
                                    ncl_mqtt_client_last_error(adapter->mqtt));
        }
        return result;
    }
    adapter->broker_ever_connected = true;
    ncl_log_info("MQTT 已连接: %s", adapter->broker_url);
    broker_subscribe(adapter);
    return NCL_OK;
}

/* ------------------------------------------------------------------ poll -- */

/** The point with this model path, or NULL. */
static adapter_point *find_point(ncl_adapter *adapter, const char *path)
{
    size_t i;

    for (i = 0; i < adapter->point_count; i++) {
        if (strcmp(adapter->points[i].path, path) == 0) {
            return &adapter->points[i];
        }
    }
    return NULL;
}

ncl_err ncl_adapter_poll_one(ncl_adapter *adapter, const char *path,
                             ncl_strbuf *err)
{
    adapter_point *point;
    ncl_json *value = NULL;
    ncl_err result;

    if (adapter == NULL || path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    point = find_point(adapter, path);
    if (point == NULL) {
        err_append1(err, "no such point", path);
        return NCL_ERR_NOT_FOUND;
    }
    if (!point->available) {
        /* Declared, in the model, but the frame it needs is not captured yet:
         * there is nothing to read, and saying so is the whole answer. */
        err_appendf(err, "%s 还读不了（%s）", point->path,
                    point->summary != NULL ? point->summary : "待抓包");
        return NCL_ERR_NOT_SUPPORTED;
    }
    result = read_declared_point(adapter, point->path, &value, err);
    if (result != NCL_OK) {
        err_append1(err, "cannot read %s", point->path);
        ncl_json_free(value);
        return result;
    }
    if (point->node != NULL) {
        ncl_json_free(point->node->value);
        point->node->value = value; /* the model takes it over */
    } else {
        ncl_json_free(value);
    }
    return NCL_OK;
}

ncl_err ncl_adapter_poll(ncl_adapter *adapter, ncl_strbuf *err)
{
    ncl_err first = NCL_OK;
    size_t i;

    if (adapter == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < adapter->point_count; i++) {
        ncl_err result;

        if (!adapter->points[i].available) {
            continue; /* nothing to read yet: not a failure, just not there */
        }
        result = ncl_adapter_poll_one(adapter, adapter->points[i].path, err);

        if (result != NCL_OK && first == NCL_OK) {
            first = result;
        }
    }
    return first;
}

ncl_err ncl_adapter_poll_round(ncl_adapter *adapter, size_t *failed,
                               ncl_strbuf *err)
{
    ncl_err first = NCL_OK;
    size_t failures = 0;
    size_t i;

    if (adapter == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < adapter->point_count; i++) {
        ncl_err result;

        if (!adapter->points[i].available) {
            continue; /* 待抓包的点位不进轮询，也不计失败（自检会单独列出来） */
        }
        result = ncl_adapter_poll_one(adapter, adapter->points[i].path, err);
        if (result == NCL_OK) {
            continue;
        }
        failures++;
        if (first == NCL_OK) {
            first = result;
        }
        if (ncl_driver_error_tier(result) == 1) {
            /* The link is down: every remaining point would only pay the same
             * connect timeout again, so the round ends here. */
            failures += adapter->point_count - i - 1u;
            break;
        }
    }
    if (failed != NULL) {
        *failed = failures;
    }
    return first;
}
