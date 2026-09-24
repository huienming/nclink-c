/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - server side. */
#include "nclink/ncl_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_schema.h"
#include "nclink/ncl_thread.h"
#include "nclink/ncl_topic.h"

#include "file/file_internal.h"

/* One binding: "<operation>#<path>" -> method. */
typedef struct {
    char         *key;
    void         *instance;
    ncl_tool_fn   fn;
    char         *method_name;
    char         *tool_name;
    ncl_schema   *params_schema; /**< borrowed from server->schemas */
    ncl_schema   *result_schema; /**< borrowed from server->schemas */
    const char   *params_text;   /**< its source text, borrowed from there too */
    const char   *result_text;   /**< likewise; NULL when the method has none */
} ncl_binding;

/** One compiled parameter schema, shared by every binding that declares it. */
typedef struct {
    char       *text;
    ncl_schema *schema;
} ncl_interned_schema;

/**
 * One sampling task.
 * The thread samples every sampleInterval, accumulates
 * uploadInterval / sampleInterval rounds, then publishes a Sample message on
 * "Sample/<sn>/<channel id>", one report per upload interval.
 */
typedef struct ncl_sample_task {
    ncl_server *server;
    char       *id;
    char       *topic;
    ncl_node   *config;      /**< deep copy of the config node */

    ncl_strvec  paths;       /**< absolute paths, one per sample item: the
                              *   lookup keys the bindings are registered under */
    ncl_strvec  header;      /**< the same paths with the device segment
                              *   dropped ("/STATUS"): what goes on the wire */
    ncl_thread *thread;

    ncl_mutex  *mutex;
    ncl_cond   *cond;
    bool        stop;
    bool        warned_slow;  /**< "跟不上模型"那条 warn 只打一次，别每秒刷屏 */
} ncl_sample_task;

struct ncl_server {
    char *sn;
    ncl_mqtt_client *mqtt;   /**< borrowed */

    ncl_node *root_node;
    ncl_mutex *mutex;

    ncl_binding *bindings;
    size_t       binding_count;
    size_t       binding_capacity;

    /* last registered tool, so a binding may omit the tool name */
    char  *last_tool_name;
    void  *last_tool_instance;

    /* sample channels */
    ncl_sample_task **samples;
    size_t           sample_count;
    size_t           sample_capacity;
    size_t           sample_uploads;

    /* Optional caller data (the file tool parks its FTP state here). */
    void                *user_data;
    ncl_server_cleanup_fn user_cleanup;

    /* Interned parameter schemas (compiled once per distinct declaration) and
     * the event counter. */
    ncl_interned_schema *schemas;
    size_t               schema_count;
    size_t               schema_capacity;
    size_t               events;

    /* The model's METHODS item is out of date: a tool registered since it was
     * last built. It is rebuilt when someone reads (model / probe / methods
     * JSON), so registering a tool - the tool layer does it once per point -
     * does not rebuild the item once per point. */
    bool methods_dirty;

    ncl_server_publish_fn publish_sink;
    void                 *publish_user;

    /* Asynchronous method calls: handler -> ncl_method_task, the state a
     * Method/Status or Method/Result query is answered from. */
    ncl_ptrvec            method_tasks;
    unsigned long long    method_seq;
};

/**
 * One running (or finished) asynchronous method call. The client got @p
 * handler in the Method/Call/Response and addresses this call with it in the
 * Method/Status and Method/Result pairs.
 */
typedef struct {
    char       *handler;
    bool        finished;   /**< the worker stored the outcome */
    bool        abandoned;  /**< the server is gone: the worker frees it */
    long long   process;    /**< progress, host reported (0 = unknown) */
    char       *status;     /**< host reported status, NULL = framework's */
    ncl_err     rc;         /**< outcome of the call */
    ncl_json   *value;      /**< its value, owned */
    char       *reason;     /**< its NG reason, owned */
    int64_t     finished_ms;
} ncl_method_task;

static void ncl_method_task_free(ncl_method_task *task);

typedef struct {
    ncl_server        *server;
    ncl_binding       *binding; /**< borrowed: the binding table outlives it */
    ncl_json          *params;  /**< cloned for the worker */
    ncl_method_task   *task;
} ncl_method_worker;

static void ncl_method_worker_run(void *arg);

/** Task of @p handler, or NULL. Caller holds server->mutex. */
static ncl_method_task *ncl_server_find_task_locked(ncl_server *server,
                                                    const char *handler);
/** Drop a task from the table (the caller keeps ownership of the memory). */
static void ncl_server_unlink_task_locked(ncl_server *server,
                                          ncl_method_task *task);

/* ============================================================ tool lookup = */

static ncl_binding *ncl_server_find_binding(ncl_server *server, const char *key)
{
    size_t i;
    for (i = 0; i < server->binding_count; i++) {
        if (strcmp(server->bindings[i].key, key) == 0) {
            return &server->bindings[i];
        }
    }
    return NULL;
}

/** Find a method by name in any registered binding of the given tool. */
static ncl_binding *ncl_server_find_method(ncl_server *server,
                                           const char *tool_name,
                                           const char *method_name)
{
    size_t i;
    for (i = 0; i < server->binding_count; i++) {
        ncl_binding *binding = &server->bindings[i];
        bool tool_matches = tool_name == NULL ||
                            (binding->tool_name != NULL &&
                             strcmp(binding->tool_name, tool_name) == 0);
        if (tool_matches && binding->method_name != NULL &&
            strcmp(binding->method_name, method_name) == 0) {
            return binding;
        }
    }
    return NULL;
}

/**
 * Compile @p text once and keep it on the server, so every binding that shares
 * the schema frees it exactly once.
 */
static ncl_schema *ncl_server_intern_schema(ncl_server *server,
                                            const char *text,
                                            const char *tool_name,
                                            const char *method_name,
                                            const char *what,
                                            const char **out_text)
{
    char *error = NULL;
    ncl_schema *schema;
    size_t i;

    for (i = 0; i < server->schema_count; i++) {
        if (strcmp(server->schemas[i].text, text) == 0) {
            if (out_text != NULL) {
                *out_text = server->schemas[i].text;
            }
            return server->schemas[i].schema;
        }
    }
    schema = ncl_schema_compile_text(text, strlen(text), &error);
    if (schema == NULL) {
        /* "params" / "result" 分开报：万一新旧结构体混用（字段错位），一眼看得出是哪个。 */
        ncl_log_error("工具 %s 的方法 %s 的 %s schema 无效: %s", tool_name,
                      method_name, what, error != NULL ? error : "?");
        ncl_mem_free(error);
        return NULL;
    }
    if (server->schema_count == server->schema_capacity) {
        size_t capacity =
            server->schema_capacity == 0 ? 8 : server->schema_capacity * 2;
        ncl_interned_schema *grown = (ncl_interned_schema *)ncl_mem_realloc(
            server->schemas, capacity * sizeof(ncl_interned_schema));
        if (grown == NULL) {
            ncl_schema_free(schema);
            return NULL;
        }
        server->schemas = grown;
        server->schema_capacity = capacity;
    }
    server->schemas[server->schema_count].text = ncl_strdup(text);
    server->schemas[server->schema_count].schema = schema;
    if (server->schemas[server->schema_count].text == NULL) {
        ncl_schema_free(schema);
        return NULL;
    }
    if (out_text != NULL) {
        *out_text = server->schemas[server->schema_count].text;
    }
    server->schema_count++;
    return schema;
}

/**
 * Register a method that is not tied to a model path. Used by the built in
 * "nclinkServer" tool and by method-call only tools.
 */
static ncl_err ncl_server_add_method(ncl_server *server, const char *tool_name,
                                     void *instance, const char *method_name,
                                     ncl_tool_fn fn, const char *key,
                                     ncl_schema *params_schema,
                                     ncl_schema *result_schema,
                                     const char *params_text,
                                     const char *result_text)
{
    ncl_binding *binding;

    if (server->binding_count == server->binding_capacity) {
        size_t capacity = server->binding_capacity == 0 ? 16
                                                        : server->binding_capacity * 2;
        ncl_binding *grown = (ncl_binding *)ncl_mem_realloc(
            server->bindings, capacity * sizeof(ncl_binding));
        if (grown == NULL) {
            return NCL_ERR_NOMEM;
        }
        server->bindings = grown;
        server->binding_capacity = capacity;
    }
    binding = &server->bindings[server->binding_count];
    memset(binding, 0, sizeof(*binding));
    binding->key = ncl_strdup(key);
    binding->method_name = ncl_strdup(method_name);
    binding->tool_name = tool_name != NULL ? ncl_strdup(tool_name) : NULL;
    binding->instance = instance;
    binding->fn = fn;
    binding->params_schema = params_schema;
    binding->result_schema = result_schema;
    binding->params_text = params_text;
    binding->result_text = result_text;
    if (binding->key == NULL || binding->method_name == NULL) {
        ncl_mem_free(binding->key);
        ncl_mem_free(binding->method_name);
        ncl_mem_free(binding->tool_name);
        return NCL_ERR_NOMEM;
    }
    server->binding_count++;
    return NCL_OK;
}

ncl_err ncl_server_register_tool(ncl_server *server, const char *tool_name,
                                 void *instance,
                                 const ncl_tool_method *methods,
                                 size_t method_count,
                                 const ncl_tool_binding *bindings,
                                 size_t binding_count)
{
    size_t i;

    if (server == NULL || tool_name == NULL || instance == NULL) {
        return NCL_ERR_INVALID_ARG;
    }

    /* Every method becomes addressable by name. The binding key is the method name
 * itself for now; path
     * bindings overwrite it below. */
    for (i = 0; i < method_count; i++) {
        char key[256];
        ncl_schema *schema = NULL;
        ncl_schema *result = NULL;
        const char *schema_text = NULL;
        const char *result_text = NULL;

        if (methods[i].params_schema != NULL) {
            schema = ncl_server_intern_schema(server, methods[i].params_schema,
                                              tool_name, methods[i].name, "params",
                                              &schema_text);
        }
        if (methods[i].result_schema != NULL) {
            result = ncl_server_intern_schema(server, methods[i].result_schema,
                                              tool_name, methods[i].name, "result",
                                              &result_text);
        }
        snprintf(key, sizeof(key), "%s::%s", tool_name, methods[i].name);
        if (ncl_server_add_method(server, tool_name, instance, methods[i].name,
                                  methods[i].fn, key, schema, result, schema_text,
                                  result_text) != NCL_OK) {
            return NCL_ERR_NOMEM;
        }
    }

    /* Model path bindings: "<operation>#<path>" -> method. */
    for (i = 0; i < binding_count; i++) {
        const ncl_tool_binding *spec = &bindings[i];
        ncl_tool_fn fn = NULL;
        ncl_schema *schema = NULL;
        ncl_schema *result = NULL;
        const char *schema_text = NULL;
        const char *result_text = NULL;
        size_t m;
        char key[1024];

        if (spec->path == NULL || spec->method == NULL) {
            continue;
        }
        for (m = 0; m < method_count; m++) {
            if (strcmp(methods[m].name, spec->method) == 0) {
                fn = methods[m].fn;
                if (methods[m].params_schema != NULL) {
                    schema = ncl_server_intern_schema(
                        server, methods[m].params_schema, tool_name,
                        methods[m].name, "params", &schema_text);
                }
                if (methods[m].result_schema != NULL) {
                    result = ncl_server_intern_schema(
                        server, methods[m].result_schema, tool_name,
                        methods[m].name, "result", &result_text);
                }
                break;
            }
        }
        if (fn == NULL) {
            ncl_log_error("工具 %s 中不存在方法 %s", tool_name, spec->method);
            continue;
        }
        snprintf(key, sizeof(key), "%s%s%s", ncl_operation_to_string(spec->operation),
                 NCL_OPERATION_SEPARATOR, spec->path);
        if (ncl_server_add_method(server, spec->tool != NULL ? spec->tool : tool_name,
                                  instance, spec->method, fn, key,
                                  schema, result, schema_text,
                                  result_text) != NCL_OK) {
            return NCL_ERR_NOMEM;
        }
    }

    ncl_mem_free(server->last_tool_name);
    server->last_tool_name = ncl_strdup(tool_name);
    server->last_tool_instance = instance;
    /* The model advertises what can be called, and a new tool changes that.
     * The item itself is rebuilt on the next read (ncl_server_model() /
     * probe / ncl_server_methods_json()): registering is cheap and cannot
     * fail on the metadata, and the tool layer registers once per point. */
    server->methods_dirty = true;
    return NCL_OK;
}

size_t ncl_server_binding_count(const ncl_server *server)
{
    return server != NULL ? server->binding_count : 0;
}

/** True when (tool, method) was already seen in an earlier binding. */
static bool ncl_server_operation_seen(const ncl_server *server, size_t upto,
                                      const char *tool, const char *method)
{
    size_t i;
    for (i = 0; i < upto; i++) {
        const ncl_binding *binding = &server->bindings[i];
        if (binding->method_name == NULL) {
            continue;
        }
        if (strcmp(binding->method_name, method) != 0) {
            continue;
        }
        if (tool == NULL) {
            if (binding->tool_name == NULL) {
                return true;
            }
        } else if (binding->tool_name != NULL &&
                   strcmp(binding->tool_name, tool) == 0) {
            return true;
        }
    }
    return false;
}

static const ncl_binding *ncl_server_operation_at(const ncl_server *server,
                                                  size_t index)
{
    size_t i;
    size_t seen = 0;

    for (i = 0; i < server->binding_count; i++) {
        const ncl_binding *binding = &server->bindings[i];
        if (binding->method_name == NULL) {
            continue;
        }
        if (ncl_server_operation_seen(server, i, binding->tool_name,
                                      binding->method_name)) {
            continue;
        }
        if (seen == index) {
            return binding;
        }
        seen++;
    }
    return NULL;
}

size_t ncl_server_operation_count(const ncl_server *server)
{
    size_t count = 0;
    size_t i;
    if (server == NULL) {
        return 0;
    }
    for (i = 0; i < server->binding_count; i++) {
        const ncl_binding *binding = &server->bindings[i];
        if (binding->method_name == NULL) {
            continue;
        }
        if (!ncl_server_operation_seen(server, i, binding->tool_name,
                                       binding->method_name)) {
            count++;
        }
    }
    return count;
}

const char *ncl_server_operation_tool(const ncl_server *server, size_t index)
{
    const ncl_binding *binding =
        server != NULL ? ncl_server_operation_at(server, index) : NULL;
    return binding != NULL ? binding->tool_name : NULL;
}

const char *ncl_server_operation_method(const ncl_server *server, size_t index)
{
    const ncl_binding *binding =
        server != NULL ? ncl_server_operation_at(server, index) : NULL;
    return binding != NULL ? binding->method_name : NULL;
}

/**
 * The path a path binding serves. A path binding's key is
 * "<operation>#<path>"; a method-only binding's key is "<tool>::<method>", so
 * the operation keyword in front of the separator decides. Returns NULL for a
 * method-only binding.
 */
static const char *ncl_binding_path(const ncl_binding *binding,
                                    const char **out_operation)
{
    const char *hash;
    unsigned op;

    if (binding == NULL || binding->key == NULL) {
        return NULL;
    }
    hash = strchr(binding->key, NCL_OPERATION_SEPARATOR[0]);
    if (hash == NULL || hash == binding->key || hash[1] == '\0') {
        return NULL;
    }
    for (op = 0; op < NCL_OP_COUNT; op++) {
        const char *name = ncl_operation_to_string((ncl_operation)op);
        size_t len;
        if (name == NULL) {
            continue;
        }
        len = strlen(name);
        if ((size_t)(hash - binding->key) == len &&
            strncmp(binding->key, name, len) == 0) {
            if (out_operation != NULL) {
                *out_operation = name;
            }
            return hash + 1;
        }
    }
    return NULL;
}

/** Parse a declared schema's source text; NULL when absent or malformed. */
static ncl_json *ncl_schema_text_json(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return NULL;
    }
    return ncl_json_parse_cstr(text, NULL);
}

/** Build the capabilities array from the current bindings. */
static ncl_json *ncl_server_build_methods_json(ncl_server *server)
{
    ncl_json *methods;
    size_t count;
    size_t i;

    if (server == NULL) {
        return NULL;
    }
    methods = ncl_json_new_array();
    if (methods == NULL) {
        return NULL;
    }
    count = ncl_server_operation_count(server);
    for (i = 0; i < count; i++) {
        const ncl_binding *binding = ncl_server_operation_at(server, i);
        ncl_json *entry;
        ncl_json *binds;
        ncl_json *schema;
        char address[512];
        size_t b;

        if (binding == NULL || binding->tool_name == NULL ||
            binding->method_name == NULL) {
            continue;
        }
        entry = ncl_json_new_object();
        if (entry == NULL) {
            break;
        }
        snprintf(address, sizeof(address), "/%s/%s", binding->tool_name,
                 binding->method_name);
        ncl_json_obj_set_string(entry, "tool", binding->tool_name);
        ncl_json_obj_set_string(entry, "method", binding->method_name);
        ncl_json_obj_set_string(entry, "address", address);

        schema = ncl_schema_text_json(binding->params_text);
        if (schema != NULL) {
            ncl_json_obj_set(entry, "params", schema);
        }
        schema = ncl_schema_text_json(binding->result_text);
        if (schema != NULL) {
            ncl_json_obj_set(entry, "result", schema);
        }

        /* The model paths this method serves, when it serves any (a point's
         * read / write is one of these; a method-only tool has none). */
        binds = ncl_json_new_array();
        for (b = 0; binds != NULL && b < server->binding_count; b++) {
            const ncl_binding *candidate = &server->bindings[b];
            const char *operation = NULL;
            const char *path;
            ncl_json *item;

            if (candidate->method_name == NULL || candidate->tool_name == NULL ||
                strcmp(candidate->method_name, binding->method_name) != 0 ||
                strcmp(candidate->tool_name, binding->tool_name) != 0) {
                continue;
            }
            path = ncl_binding_path(candidate, &operation);
            if (path == NULL) {
                continue;
            }
            item = ncl_json_new_object();
            if (item == NULL) {
                continue;
            }
            ncl_json_obj_set_string(item, "operation", operation);
            ncl_json_obj_set_string(item, "path", path);
            ncl_json_arr_push(binds, item);
        }
        if (binds != NULL && ncl_json_arr_len(binds) > 0) {
            ncl_json_obj_set(entry, "bindings", binds);
        } else {
            ncl_json_free(binds);
        }
        ncl_json_arr_push(methods, entry);
    }
    return methods;
}

/** The METHODS node of the model, or NULL when there is none. It is ours by
 *  type, never by id: a model that happens to use the id keeps its own point. */
static ncl_node *ncl_server_methods_node(const ncl_server *server)
{
    size_t i;

    if (server == NULL || server->root_node == NULL) {
        return NULL;
    }
    for (i = 0; i < ncl_ptrvec_len(&server->root_node->configs); i++) {
        ncl_node *config = ncl_node_config_at(server->root_node, i);
        if (config != NULL && config->node_type_name != NULL &&
            strcmp(config->node_type_name, NCL_METHODS_NODE_TYPE) == 0) {
            return config;
        }
    }
    return NULL;
}

ncl_err ncl_server_refresh_methods(ncl_server *server)
{
    ncl_node *node;
    ncl_json *methods;
    char id[64];
    size_t i;

    if (server == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (server->root_node == NULL) {
        return NCL_OK;
    }
    methods = ncl_server_build_methods_json(server);
    if (methods == NULL) {
        return NCL_ERR_NOMEM;
    }

    node = ncl_server_methods_node(server);
    if (node == NULL) {
        node = ncl_node_new(NCL_NODE_CONFIG);
        if (node == NULL) {
            ncl_json_free(methods);
            return NCL_ERR_NOMEM;
        }
        /* A unique id, so path <-> id lookups stay unambiguous. */
        for (i = 0; i < 100; i++) {
            snprintf(id, sizeof(id), i == 0 ? NCL_METHODS_NODE_ID
                                            : NCL_METHODS_NODE_ID "%u",
                     (unsigned)i);
            if (ncl_node_find_by_id(server->root_node, id) == NULL) {
                break;
            }
        }
        if (ncl_node_set_id(node, id) != NCL_OK ||
            ncl_node_set_type_name(node, NCL_METHODS_NODE_TYPE) != NCL_OK ||
            ncl_node_set_data_type(node, NCL_DATA_TYPE_LIST) != NCL_OK ||
            ncl_node_set_description(
                node,
                "本设备可调用的方法清单：tool / method / address / params(schema)"
                " / result(schema) / bindings") != NCL_OK ||
            ncl_node_set_settable(node, false) != NCL_OK ||
            ncl_node_add_config(server->root_node, node) != NCL_OK) {
            ncl_node_free(node);
            ncl_json_free(methods);
            return NCL_ERR_NOMEM;
        }
        if (ncl_node_set_path(node, ncl_node_path(server->root_node)) !=
            NCL_OK) {
            ncl_json_free(methods);
            return NCL_ERR_NOMEM;
        }
    }
    /* Takes ownership of the array. */
    if (ncl_node_set_value(node, methods) != NCL_OK) {
        ncl_json_free(methods);
        return NCL_ERR_NOMEM;
    }
    server->methods_dirty = false;
    return NCL_OK;
}

/** Rebuild the METHODS item when a tool registered since it was last built.
 *  Best effort: a reader still gets the model, just with a stale item. */
static void ncl_server_flush_methods(ncl_server *server)
{
    if (server != NULL && server->methods_dirty) {
        (void)ncl_server_refresh_methods(server);
    }
}

ncl_json *ncl_server_methods_json(ncl_server *server)
{
    ncl_node *node;

    if (server == NULL) {
        return NULL;
    }
    ncl_server_flush_methods(server);
    node = ncl_server_methods_node(server);
    if (node == NULL || node->value == NULL) {
        /* No model to hang the item on; the bindings are still the truth. */
        return ncl_server_build_methods_json(server);
    }
    return ncl_json_clone(node->value);
}

ncl_json *ncl_server_openapi_schema(ncl_server *server, const char *base_url)
{
    ncl_json *document;
    ncl_json *info;
    ncl_json *servers;
    ncl_json *paths;
    size_t count;
    size_t i;

    if (server == NULL) {
        return NULL;
    }
    document = ncl_json_new_object();
    if (document == NULL) {
        return NULL;
    }

    /* The {status,data} envelope used by the REST layer. */
    ncl_json_obj_set_string(document, "openapi", "3.0.0");
    info = ncl_json_new_object();
    ncl_json_obj_set_string(info, "title", "MCP Server API");
    ncl_json_obj_set_string(info, "description", "Model Context Protocol Server");
    ncl_json_obj_set_string(info, "version", "1.0.0");
    ncl_json_obj_set(document, "info", info);

    servers = ncl_json_new_array();
    {
        ncl_json *entry = ncl_json_new_object();
        char *url = NULL;
        if (base_url != NULL) {
            ncl_asprintf(&url, "%s/api", base_url);
        }
        ncl_json_obj_set_string(entry, "url", url != NULL ? url : "/api");
        ncl_json_obj_set_string(entry, "description", "MCP API Server");
        ncl_mem_free(url);
        ncl_json_arr_push(servers, entry);
    }
    ncl_json_obj_set(document, "servers", servers);

    /* One POST path per "<tool>/<method>". */
    paths = ncl_json_new_object();
    count = ncl_server_operation_count(server);
    for (i = 0; i < count; i++) {
        const ncl_binding *binding = ncl_server_operation_at(server, i);
        const char *tool = ncl_server_operation_tool(server, i);
        const char *method = ncl_server_operation_method(server, i);
        ncl_json *path_item;
        ncl_json *post;
        ncl_json *responses;
        ncl_json *ok;
        ncl_json *ok_content;
        ncl_json *ok_media;
        ncl_json *envelope;
        ncl_json *props;
        ncl_json *value_prop;
        ncl_json *schema_prop;
        ncl_json *content;
        ncl_json *media;
        ncl_json *request_body;
        char path[512];

        if (tool == NULL || method == NULL) {
            continue;
        }
        snprintf(path, sizeof(path), "/%s/%s", tool, method);

        path_item = ncl_json_new_object();
        post = ncl_json_new_object();
        ncl_json_obj_set_string(post, "summary", path);
        ncl_json_obj_set_string(post, "operationId", path);

        /* Request body: the method's declared params schema when it has one,
         * so a client can build the call from this document alone; a free form
         * JSON object otherwise. */
        request_body = ncl_json_new_object();
        ncl_json_obj_set_bool(request_body, "required", false);
        content = ncl_json_new_object();
        media = ncl_json_new_object();
        schema_prop = binding != NULL ? ncl_schema_text_json(binding->params_text)
                                      : NULL;
        if (schema_prop == NULL) {
            schema_prop = ncl_json_new_object();
            ncl_json_obj_set_string(schema_prop, "type", "object");
        }
        ncl_json_obj_set(media, "schema", schema_prop);
        ncl_json_obj_set(content, "application/json", media);
        ncl_json_obj_set(request_body, "content", content);
        ncl_json_obj_set(post, "requestBody", request_body);

        /* 200: the envelope every method answer uses - "code" plus, when the
         * call has one, the value under "return" (the method's result schema
         * when it declared one) and the outcome under "result". */
        responses = ncl_json_new_object();
        ok = ncl_json_new_object();
        ncl_json_obj_set_string(ok, "description", "successful operation");
        envelope = ncl_json_new_object();
        ncl_json_obj_set_string(envelope, "type", "object");
        props = ncl_json_new_object();
        schema_prop = ncl_json_new_object();
        ncl_json_obj_set_string(schema_prop, "type", "string");
        ncl_json_obj_set(props, "code", schema_prop);
        value_prop = binding != NULL ? ncl_schema_text_json(binding->result_text)
                                     : NULL;
        ncl_json_obj_set(props, "return",
                         value_prop != NULL ? value_prop : ncl_json_new_object());
        schema_prop = ncl_json_new_object();
        ncl_json_obj_set_string(schema_prop, "type", "string");
        ncl_json_obj_set(props, "result", schema_prop);
        ncl_json_obj_set(envelope, "properties", props);
        ok_media = ncl_json_new_object();
        ncl_json_obj_set(ok_media, "schema", envelope);
        ok_content = ncl_json_new_object();
        ncl_json_obj_set(ok_content, "application/json", ok_media);
        ncl_json_obj_set(ok, "content", ok_content);
        ncl_json_obj_set(responses, "200", ok);
        ncl_json_obj_set(post, "responses", responses);

        ncl_json_obj_set(path_item, "post", post);
        ncl_json_obj_set(paths, path, path_item);
    }
    ncl_json_obj_set(document, "paths", paths);
    return document;
}

char *ncl_server_openapi_schema_json(ncl_server *server, const char *base_url)
{
    ncl_json *document = ncl_server_openapi_schema(server, base_url);
    char *text;
    if (document == NULL) {
        return NULL;
    }
    text = ncl_json_write_string(document);
    ncl_json_free(document);
    return text;
}

/* ========================================================== life cycle ==== */

unsigned ncl_server_abi_shape(void)
{
    return NCL_SERVER_ABI_SHAPE;
}

ncl_server *ncl_server_create(const ncl_server_options *options)
{
    ncl_server *server;

    if (options == NULL || options->sn == NULL) {
        return NULL;
    }
    server = (ncl_server *)ncl_mem_calloc(1, sizeof(ncl_server));
    if (server == NULL) {
        return NULL;
    }
    server->sn = ncl_strdup(options->sn);
    server->mqtt = options->mqtt;
    server->publish_sink = options->publish;
    server->publish_user = options->publish_user;
    server->mutex = ncl_mutex_create();
    ncl_ptrvec_init(&server->method_tasks, NULL);
    if (server->sn == NULL || server->mutex == NULL) {
        ncl_server_free(server);
        return NULL;
    }
    if (options->model_json != NULL &&
        ncl_server_load_model(server, options->model_json) != NCL_OK) {
        ncl_log_warn("模型加载失败，服务器以空模型启动");
    }
    return server;
}

void ncl_server_free(ncl_server *server)
{
    size_t i;
    if (server == NULL) {
        return;
    }
    /* Sampling tasks must be stopped (and joined) first: their collect cycle
     * queries the tool bindings and the model, so releasing either while a task
     * is still running is a use-after-ncl_mem_free(seen as a rare SIGSEGV in
     * ncl_server_lookup on a sampler thread). */
    ncl_server_stop_all_samples(server);
    if (server->user_cleanup != NULL) {
        server->user_cleanup(server->user_data);
        server->user_data = NULL;
        server->user_cleanup = NULL;
    }
    for (i = 0; i < server->binding_count; i++) {
        ncl_mem_free(server->bindings[i].key);
        ncl_mem_free(server->bindings[i].method_name);
        ncl_mem_free(server->bindings[i].tool_name);
    }
    ncl_mem_free(server->bindings);
    for (i = 0; i < server->schema_count; i++) {
        ncl_mem_free(server->schemas[i].text);
        ncl_schema_free(server->schemas[i].schema);
    }
    ncl_mem_free(server->schemas);
    ncl_mem_free(server->last_tool_name);
    ncl_node_free(server->root_node);
    /* Asynchronous method calls: a finished entry is ours to release, a running
     * one is handed to its worker (which frees it when the method returns). */
    for (i = 0; i < server->method_tasks.len; i++) {
        ncl_method_task *task =
            (ncl_method_task *)server->method_tasks.items[i];
        if (task->finished) {
            ncl_method_task_free(task);
        } else {
            task->abandoned = true;
        }
    }
    ncl_ptrvec_free(&server->method_tasks);
    ncl_mem_free(server->sn);
    ncl_mutex_destroy(server->mutex);
    ncl_mem_free(server);
}

ncl_err ncl_server_set_user_data(ncl_server *server, void *data,
                                 ncl_server_cleanup_fn cleanup)
{
    if (server == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (server->user_cleanup != NULL && server->user_data != data) {
        server->user_cleanup(server->user_data);
    }
    server->user_data = data;
    server->user_cleanup = cleanup;
    return NCL_OK;
}

void *ncl_server_user_data(const ncl_server *server)
{
    return server != NULL ? server->user_data : NULL;
}

const char *ncl_server_sn(const ncl_server *server)
{
    return server != NULL ? server->sn : NULL;
}

ncl_err ncl_server_load_model(ncl_server *server, const char *model_json)
{
    ncl_node *root;

    if (server == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    root = ncl_root_node_parse(model_json);
    if (root == NULL) {
        return NCL_ERR_INVALID_MODEL;
    }
    return ncl_server_set_model(server, root);
}

ncl_err ncl_server_set_model(ncl_server *server, ncl_node *root)
{
    if (server == NULL) {
        ncl_node_free(root);
        return NCL_ERR_INVALID_ARG;
    }
    ncl_node_free(server->root_node);
    server->root_node = root;
    /* The model the clients get advertises what can be called right now. */
    return ncl_server_refresh_methods(server);
}

ncl_node *ncl_server_model(ncl_server *server)
{
    /* A caller that reads the model wants what can be called *now*. */
    ncl_server_flush_methods(server);
    return server != NULL ? server->root_node : NULL;
}

ncl_err ncl_server_save_model(ncl_server *server)
{
    char *text;
    ncl_err rc;
    const char *path;
    ncl_node *methods;
    size_t index;
    bool detached = false;

    if (server == NULL || server->root_node == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    path = ncl_env_model_file();
    if (path == NULL) {
        return NCL_ERR_IO;
    }
    /* The model file is only written when it is missing. */
    if (ncl_path_exists(path)) {
        return NCL_OK;
    }
    /* The METHODS item is runtime capability, not part of the device model:
     * the file stays what the site handed us, the node comes back on the next
     * registration (see ncl_server_refresh_methods()). */
    methods = ncl_server_methods_node(server);
    if (methods != NULL) {
        for (index = 0; index < ncl_ptrvec_len(&server->root_node->configs);
             index++) {
            if (ncl_node_config_at(server->root_node, index) == methods) {
                (void)ncl_ptrvec_take(&server->root_node->configs, index);
                detached = true;
                break;
            }
        }
    }
    text = ncl_node_write_string(server->root_node);
    if (text == NULL) {
        if (detached) {
            /* The vector kept its capacity when the node was taken out, so
             * putting it back cannot fail (and cannot leak). */
            (void)ncl_ptrvec_push(&server->root_node->configs, methods);
        }
        return NCL_ERR_NOMEM;
    }
    rc = ncl_file_write_all(path, text, strlen(text));
    ncl_mem_free(text);
    if (detached) {
        (void)ncl_ptrvec_push(&server->root_node->configs, methods);
    }
    return rc;
}

/* ==================================================== invocation helpers == */

/** Resolve a request item id to a model path. */
static char *ncl_server_resolve_path(ncl_server *server, const char *id)
{
    ncl_node *node;

    if (id == NULL) {
        return NULL;
    }
    if (id[0] == NCL_PATH_SEPARATOR[0]) {
        return ncl_strdup(id);
    }
    /* Not starting with '/': interpreted as a node id. */
    node = server->root_node != NULL ? ncl_node_find_by_id(server->root_node, id)
                                     : NULL;
    if (node == NULL || ncl_node_path(node) == NULL) {
        return NULL;
    }
    return ncl_strdup(ncl_node_path(node));
}

static ncl_err ncl_server_call_binding(ncl_binding *binding, const ncl_json *params,
                                       ncl_json **result, char **reason)
{
    *result = NULL;
    if (reason != NULL) {
        *reason = NULL;
    }
    if (binding == NULL || binding->fn == NULL) {
        return NCL_ERR_NOT_FOUND;
    }
    return binding->fn(binding->instance, params, result, reason);
}

/** Build "<operation>#<path>" and look the binding up. */
static ncl_binding *ncl_server_lookup(ncl_server *server, const char *operation,
                                      const char *path)
{
    char key[1024];
    snprintf(key, sizeof(key), "%s%s%s", operation, NCL_OPERATION_SEPARATOR, path);
    return ncl_server_find_binding(server, key);
}

ncl_message *ncl_server_invoke_query(ncl_server *server,
                                     const ncl_message *request)
{
    ncl_message *response;
    size_t i;

    if (server == NULL || request == NULL) {
        return NULL;
    }
    response = ncl_message_new(NCL_MSG_QUERY_RESPONSE);
    if (response == NULL) {
        return NULL;
    }
    ncl_message_set_message_id(response, request->message_id);

    for (i = 0; i < ncl_message_item_count(request); i++) {
        const ncl_query_request_item *item =
            (const ncl_query_request_item *)ncl_message_item_at(request, i);
        ncl_query_response_item *result_item;
        char *path;
        ncl_binding *binding;
        ncl_json *value = NULL;
        char *reason = NULL;
        ncl_err rc;

        if (item == NULL) {
            continue;
        }
        result_item = ncl_query_response_item_new(item->id);
        if (result_item == NULL) {
            break;
        }
        result_item->params = item->params != NULL ? ncl_json_clone(item->params)
                                                   : NULL;

        path = ncl_server_resolve_path(server, item->id);
        binding = path != NULL
                      ? ncl_server_lookup(server, ncl_query_request_item_operation(item),
                                          path)
                      : NULL;
        if (binding == NULL) {
            result_item->code = ncl_strdup(NCL_KW_CODE_NG);
            result_item->reason = ncl_strdup("未找到");
        } else {
            rc = ncl_server_call_binding(binding, item->params, &value, &reason);
            if (rc == NCL_OK && value != NULL) {
                result_item->code = ncl_strdup(NCL_KW_CODE_OK);
                ncl_query_response_item_add_value(result_item, value);
            } else {
                result_item->code = ncl_strdup(NCL_KW_CODE_NG);
                if (reason != NULL) {
                    result_item->reason = reason;
                } else if (rc != NCL_OK) {
                    result_item->reason = ncl_strdup(ncl_err_name(rc));
                }
                ncl_json_free(value);
            }
        }
        ncl_mem_free(path);
        if (ncl_message_add_query_response_item(response, result_item) != NCL_OK) {
            break;
        }
    }
    return response;
}

ncl_message *ncl_server_invoke_set(ncl_server *server,
                                   const ncl_message *request)
{
    ncl_message *response;
    size_t i;

    if (server == NULL || request == NULL) {
        return NULL;
    }
    response = ncl_message_new(NCL_MSG_SET_RESPONSE);
    if (response == NULL) {
        return NULL;
    }
    ncl_message_set_message_id(response, request->message_id);

    for (i = 0; i < ncl_message_item_count(request); i++) {
        const ncl_set_request_item *item =
            (const ncl_set_request_item *)ncl_message_item_at(request, i);
        ncl_set_response_item *result_item;
        char *path;
        ncl_binding *binding;
        ncl_json *value = NULL;
        char *reason = NULL;
        ncl_err rc;

        if (item == NULL) {
            continue;
        }
        result_item = ncl_set_response_item_new(item->id);
        if (result_item == NULL) {
            break;
        }
        result_item->params = item->params != NULL ? ncl_json_clone(item->params)
                                                   : NULL;

        path = ncl_server_resolve_path(server, item->id);
        binding = path != NULL
                      ? ncl_server_lookup(server, ncl_set_request_item_operation(item),
                                          path)
                      : NULL;
        if (binding == NULL) {
            result_item->code = ncl_strdup(NCL_KW_CODE_NG);
            result_item->reason = ncl_strdup("未找到");
        } else {
            /* The set path always passes the value/index/offset/key params. */
            rc = ncl_server_call_binding(binding, item->params, &value, &reason);
            if (rc == NCL_OK && value != NULL) {
                result_item->code = ncl_strdup(NCL_KW_CODE_OK);
                result_item->result = value;
            } else {
                result_item->code = ncl_strdup(NCL_KW_CODE_NG);
                if (reason != NULL) {
                    result_item->reason = reason;
                } else if (rc != NCL_OK) {
                    result_item->reason = ncl_strdup(ncl_err_name(rc));
                }
                ncl_json_free(value);
            }
        }
        ncl_mem_free(path);
        if (ncl_message_add_set_response_item(response, result_item) != NCL_OK) {
            break;
        }
    }
    return response;
}

/**
 * Split an NC-Link method name into its tool and method halves. Both
 * "/tool/method" and a bare "method" are accepted.
 */
static char *ncl_server_split_method(const char *method, char **tool_out,
                                     char **name_out)
{
    char *copy = method != NULL ? ncl_strdup(method) : ncl_strdup("");
    char *slash;

    *tool_out = NULL;
    *name_out = copy;
    if (copy == NULL) {
        return NULL;
    }
    if (copy[0] == '/') {
        slash = strchr(copy + 1, '/');
        if (slash != NULL) {
            *slash = '\0';
            *tool_out = copy + 1;
            *name_out = slash + 1;
            return copy;
        }
        *name_out = copy + 1;
    }
    return copy;
}

ncl_message *ncl_server_check_method_call(ncl_server *server,
                                          const ncl_message *request)
{
    ncl_message *response;
    ncl_binding *binding;
    ncl_strvec errors;
    char *parsed;
    char *tool = NULL;
    char *name = NULL;
    const char *method;

    if (server == NULL || request == NULL) {
        return NULL;
    }
    response = ncl_message_new(NCL_MSG_METHOD_CALL_RESPONSE);
    if (response == NULL) {
        return NULL;
    }
    ncl_message_set_message_id(response, request->message_id);
    ncl_message_set_method(response, request->as.method_call_request.method);
    if (request->as.method_call_request.token != NULL) {
        ncl_message_set_token(response, request->as.method_call_request.token);
    }
    if (request->as.method_call_request.has_check) {
        ncl_message_set_check(response, request->as.method_call_request.check);
    }

    method = request->as.method_call_request.method;
    if (method == NULL) {
        ncl_message_set_code(response, NCL_KW_CODE_NG);
        ncl_message_set_reason(response, "method 不能为空");
        return response;
    }
    parsed = ncl_server_split_method(method, &tool, &name);
    binding = ncl_server_find_method(server, tool, name);
    if (binding == NULL) {
        /* An unknown path answers "没有找到方法". */
        ncl_message_set_code(response, NCL_KW_CODE_NG);
        ncl_message_set_reason(response, "没有找到方法");
        ncl_mem_free(parsed);
        return response;
    }
    if (binding->params_schema != NULL) {
        ncl_strvec_init(&errors);
        ncl_schema_validate(binding->params_schema,
                            request->as.method_call_request.params, &errors);
        if (errors.len > 0) {
            char *reason = ncl_schema_join_errors(&errors);
            ncl_message_set_code(response, NCL_KW_CODE_NG);
            ncl_message_set_reason(response, reason);
            ncl_mem_free(reason);
        } else {
            ncl_message_set_code(response, NCL_KW_CODE_OK);
        }
        ncl_strvec_free(&errors);
    } else {
        size_t count = request->as.method_call_request.params != NULL
                           ? ncl_json_obj_len(
                                 request->as.method_call_request.params)
                           : 0;
        if (count == 0) {
            ncl_message_set_code(response, NCL_KW_CODE_OK);
        } else {
            ncl_message_set_code(response, NCL_KW_CODE_NG);
            ncl_message_set_reason(response, "参数数量不匹配");
        }
    }
    ncl_mem_free(parsed);
    return response;
}

ncl_message *ncl_server_invoke_method_call(ncl_server *server,
                                           const ncl_message *request)
{
    ncl_message *response;
    ncl_binding *binding;
    ncl_json *value = NULL;
    char *reason = NULL;
    ncl_err rc;
    const char *method;
    const char *tool = NULL;
    char *tool_copy = NULL;
    char *method_copy = NULL;

    if (server == NULL || request == NULL) {
        return NULL;
    }
    response = ncl_message_new(NCL_MSG_METHOD_CALL_RESPONSE);
    if (response == NULL) {
        return NULL;
    }
    ncl_message_set_message_id(response, request->message_id);
    ncl_message_set_method(response, request->as.method_call_request.method);
    if (request->as.method_call_request.token != NULL) {
        ncl_message_set_token(response, request->as.method_call_request.token);
    }
    if (request->as.method_call_request.has_check) {
        ncl_message_set_check(response, request->as.method_call_request.check);
    }

    method = request->as.method_call_request.method;
    if (method == NULL) {
        ncl_message_set_code(response, NCL_KW_CODE_NG);
        ncl_message_set_reason(response, "method 不能为空");
        return response;
    }

    /* A request asking for "check" is never executed,
     * it only runs the parameter validation below. */
    if (request->as.method_call_request.has_check &&
        request->as.method_call_request.check) {
        ncl_message_free(response);
        return ncl_server_check_method_call(server, request);
    }

    /* "/tool/method", "tool/method" and a bare "method" are all accepted. */
    method_copy = ncl_strdup(method);
    if (method_copy == NULL) {
        ncl_message_set_code(response, NCL_KW_CODE_NG);
        return response;
    }
    {
        /* 少一个前导斜杠的 "tool/method" 也认：文件通道里的 "/temp/<名字>" 这类
         * 方法路径用得很随意，文档也一直是这么承诺的。 */
        char *scan = method_copy[0] == '/' ? method_copy + 1 : method_copy;
        char *slash = strchr(scan, '/');

        if (slash != NULL) {
            *slash = '\0';
            tool_copy = ncl_strdup(scan);
            memmove(method_copy, slash + 1, strlen(slash + 1) + 1);
            tool = tool_copy;
        }
    }

    binding = ncl_server_find_method(server, tool, method_copy);
    if (binding == NULL) {
        ncl_message_set_code(response, NCL_KW_CODE_NG);
        ncl_message_set_reason(response, "未找到方法");
        ncl_mem_free(method_copy);
        ncl_mem_free(tool_copy);
        return response;
    }

    /*
     * Asynchronous call: run the method on the shared thread pool and answer
     * right away with a handle. The client asks for progress through the
     * Method/Status pair and for the outcome through the Method/Result pair,
     * both carrying that handle.
     */
    if (ncl_message_async(request)) {
        ncl_method_task *task = NULL;
        ncl_method_worker *work = NULL;
        char handler[64];
        const ncl_json *params = request->as.method_call_request.params;

        if (ncl_uuid4(handler, sizeof(handler)) != NCL_OK) {
            snprintf(handler, sizeof(handler), "%s-%llu",
                     server->sn != NULL ? server->sn : "nclink",
                     (unsigned long long)++server->method_seq);
        }
        task = (ncl_method_task *)ncl_mem_calloc(1, sizeof(*task));
        work = (ncl_method_worker *)ncl_mem_calloc(1, sizeof(*work));
        if (task != NULL && work != NULL) {
            task->handler = ncl_strdup(handler);
            work->server = server;
            work->binding = binding;
            work->task = task;
            work->params = params != NULL ? ncl_json_clone(params) : NULL;
        }
        if (task == NULL || work == NULL || task->handler == NULL ||
            (params != NULL && work->params == NULL)) {
            ncl_method_task_free(task);
            if (work != NULL) {
                ncl_json_free(work->params);
                ncl_mem_free(work);
            }
            ncl_mem_free(method_copy);
            ncl_mem_free(tool_copy);
            ncl_message_set_code(response, NCL_KW_CODE_NG);
            ncl_message_set_reason(response, ncl_err_name(NCL_ERR_NOMEM));
            return response;
        }
        ncl_mutex_lock(server->mutex);
        server->method_seq++;
        if (ncl_ptrvec_push(&server->method_tasks, task) != NCL_OK) {
            ncl_mutex_unlock(server->mutex);
            ncl_method_task_free(task);
            ncl_json_free(work->params);
            ncl_mem_free(work);
            ncl_mem_free(method_copy);
            ncl_mem_free(tool_copy);
            ncl_message_set_code(response, NCL_KW_CODE_NG);
            ncl_message_set_reason(response, ncl_err_name(NCL_ERR_NOMEM));
            return response;
        }
        ncl_mutex_unlock(server->mutex);
        if (ncl_thread_pool_submit(ncl_thread_service(),
                                   ncl_method_worker_run, work) != NCL_OK) {
            /* Pool unavailable or full: run it here (the handle was already
             * handed out, the client just gets its answer a little later). */
            ncl_method_worker_run(work);
        }
        ncl_message_set_code(response, NCL_KW_CODE_OK);
        ncl_message_set_handler(response, handler);
        ncl_mem_free(method_copy);
        ncl_mem_free(tool_copy);
        return response;
    }

    rc = ncl_server_call_binding(binding, request->as.method_call_request.params,
                                 &value, &reason);
    if (rc == NCL_OK) {
        ncl_message_set_code(response, NCL_KW_CODE_OK);
        if (value != NULL) {
            /* A file valued result becomes the
             * "/temp/<name>" token plus a "fileKeys" list. */
            ncl_message_set_data(response,
                                 ncl_file_extract_file_values(value));
        }
    } else {
        ncl_message_set_code(response, NCL_KW_CODE_NG);
        ncl_message_set_reason(response, reason != NULL ? reason : ncl_err_name(rc));
        ncl_mem_free(reason);
        ncl_json_free(value);
    }
    ncl_mem_free(method_copy);
    ncl_mem_free(tool_copy);
    return response;
}

/** Method/Status/Request: how is the call with this handle doing? */
static ncl_message *ncl_server_invoke_method_status(ncl_server *server,
                                                    const ncl_message *request)
{
    ncl_message *response = ncl_message_new(NCL_MSG_METHOD_STATUS_RESPONSE);
    const char *handler = request->as.method_status_request.handler;
    ncl_method_task *task;

    if (response == NULL) {
        return NULL;
    }
    ncl_message_set_message_id(response, request->message_id);
    ncl_message_set_request_id(response, request->as.method_status_request.id);
    ncl_message_set_handler(response, handler);

    ncl_mutex_lock(server->mutex);
    task = ncl_server_find_task_locked(server, handler);
    if (task == NULL) {
        ncl_mutex_unlock(server->mutex);
        ncl_message_set_code(response, NCL_KW_CODE_NG);
        ncl_message_set_status(response, NCL_KW_STATUS_STOPPED);
        ncl_message_set_reason(response, "没有找到方法");
        return response;
    }
    ncl_message_set_code(response, NCL_KW_CODE_OK);
    ncl_message_set_process(response, task->process);
    ncl_message_set_status(response,
                           task->status != NULL
                               ? task->status
                               : (task->finished ? NCL_KW_STATUS_STOPPED
                                                 : NCL_KW_STATUS_EXECUTING));
    ncl_mutex_unlock(server->mutex);
    return response;
}

/**
 * Method/Result/Request: the outcome of the call with this handle. While it is
 * still running the answer is code=PENDING and carries no "result"; the first
 * query after it finished delivers the outcome and releases the handle.
 */
static ncl_message *ncl_server_invoke_method_result(ncl_server *server,
                                                    const ncl_message *request)
{
    ncl_message *response = ncl_message_new(NCL_MSG_METHOD_RESULT_RESPONSE);
    const char *handler = request->as.method_result_request.handler;
    ncl_method_task *task;

    if (response == NULL) {
        return NULL;
    }
    ncl_message_set_message_id(response, request->message_id);
    ncl_message_set_request_id(response, request->as.method_result_request.id);
    ncl_message_set_handler(response, handler);

    ncl_mutex_lock(server->mutex);
    task = ncl_server_find_task_locked(server, handler);
    if (task == NULL || !task->finished) {
        ncl_mutex_unlock(server->mutex);
        if (task == NULL) {
            ncl_message_set_code(response, NCL_KW_CODE_NG);
            ncl_message_set_result(response, NCL_KW_RESULT_ERROR);
        } else {
            ncl_message_set_code(response, NCL_KW_CODE_PENDING);
        }
        return response;
    }
    ncl_server_unlink_task_locked(server, task);
    ncl_mutex_unlock(server->mutex);

    if (task->rc == NCL_OK) {
        ncl_message_set_code(response, NCL_KW_CODE_OK);
        ncl_message_set_result(response, NCL_KW_RESULT_FINISHED);
        if (task->value != NULL) {
            ncl_message_set_return(response, ncl_json_clone(task->value));
        }
    } else {
        ncl_message_set_code(response, NCL_KW_CODE_NG);
        ncl_message_set_result(response, NCL_KW_RESULT_ERROR);
    }
    ncl_method_task_free(task);
    return response;
}

ncl_message *ncl_server_dispatch(ncl_server *server, const char *topic,
                                 const ncl_message *request)
{
    if (server == NULL || request == NULL) {
        return NULL;
    }
    switch (request->type) {
    case NCL_MSG_QUERY_REQUEST:
        return ncl_server_invoke_query(server, request);
    case NCL_MSG_SET_REQUEST:
        return ncl_server_invoke_set(server, request);
    case NCL_MSG_METHOD_CALL_REQUEST:
        return ncl_server_invoke_method_call(server, request);
    case NCL_MSG_METHOD_STATUS_REQUEST:
        return ncl_server_invoke_method_status(server, request);
    case NCL_MSG_METHOD_RESULT_REQUEST:
        return ncl_server_invoke_method_result(server, request);
    case NCL_MSG_PROBE_QUERY_REQUEST: {
        ncl_message *response = ncl_message_new(NCL_MSG_PROBE_QUERY_RESPONSE);
        if (response == NULL) {
            return NULL;
        }
        ncl_message_set_message_id(response, request->message_id);
        ncl_message_set_code(response, NCL_KW_CODE_OK);
        if (server->root_node != NULL) {
            /* The answer carries the capability surface too (METHODS). */
            ncl_server_flush_methods(server);
            ncl_message_set_model(response, ncl_node_clone(server->root_node, false));
        }
        return response;
    }
    case NCL_MSG_PROBE_SET_REQUEST: {
        ncl_message *response = ncl_message_new(NCL_MSG_PROBE_SET_RESPONSE);
        if (response == NULL) {
            return NULL;
        }
        ncl_message_set_message_id(response, request->message_id);
        if (request->as.probe_set_request.model != NULL) {
            ncl_node *model =
                ncl_node_clone(request->as.probe_set_request.model, false);
            if (model != NULL && ncl_server_set_model(server, model) == NCL_OK) {
                ncl_server_save_model(server);
                ncl_message_set_code(response, NCL_KW_CODE_OK);
            } else {
                ncl_node_free(model);
                ncl_message_set_code(response, NCL_KW_CODE_NG);
                ncl_message_set_reason(response, "模型无效");
            }
        } else {
            ncl_message_set_code(response, NCL_KW_CODE_NG);
            ncl_message_set_reason(response, "缺少模型");
        }
        return response;
    }
    case NCL_MSG_PING: {
        ncl_message *response = ncl_message_new(NCL_MSG_PONG);
        if (response == NULL) {
            return NULL;
        }
        ncl_message_set_message_id(response, request->message_id);
        /* A Ping is a liveness probe: the answer is the status alone, so a
         * heartbeat stays one small publish. The capability surface travels
         * elsewhere - the model's METHODS item (see
         * ncl_server_refresh_methods()) and, for HTTP clients, the OpenAPI
         * document on GET /api/schema. */
        ncl_message_set_code(response, NCL_KW_CODE_OK);
        return response;
    }
    default:
        (void)topic;
        return NULL;
    }
}

/* ============================================================ MQTT wiring = */

ncl_err ncl_server_publish(ncl_server *server, const char *topic,
                           const ncl_message *response)
{
    char *payload;
    ncl_mqtt_properties props;
    ncl_err rc;

    if (server == NULL || topic == NULL || response == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (server->publish_sink != NULL) {
        payload = ncl_message_write_string(response);
        if (payload == NULL) {
            return NCL_ERR_NOMEM;
        }
        rc = server->publish_sink(server->publish_user, topic, payload,
                                  strlen(payload));
        ncl_mem_free(payload);
        return rc;
    }
    if (server->mqtt == NULL) {
        return NCL_ERR_CLOSED;
    }
    payload = ncl_message_write_string(response);
    if (payload == NULL) {
        return NCL_ERR_NOMEM;
    }
    ncl_mqtt_properties_init(&props);
    ncl_mqtt_properties_add_user(&props, "version", "2.0");
    rc = ncl_mqtt_client_publish(server->mqtt, topic, payload, strlen(payload), 2,
                                 &props, NCL_CLIENT_OPERATION_TIMEOUT * 4);
    ncl_mqtt_properties_free(&props);
    if (rc != NCL_OK) {
        ncl_log_error("应答发布失败: %s", topic);
    }
    ncl_mem_free(payload);
    return rc;
}

void ncl_server_set_publish_sink(ncl_server *server, ncl_server_publish_fn fn,
                                 void *user)
{
    if (server != NULL) {
        server->publish_sink = fn;
        server->publish_user = user;
    }
}

/**
 * Response topic for a request: "Request" becomes "Response", except for
 * Ping, which is answered on the Pong topic.
 */
static void ncl_server_response_topic(const ncl_server *server,
                                      const char *request_topic,
                                      ncl_msg_type request_type, char *out,
                                      size_t out_len)
{
    const char *needle = strstr(request_topic, "Request");
    size_t prefix = needle != NULL ? (size_t)(needle - request_topic)
                                   : strlen(request_topic);

    if (request_type == NCL_MSG_PING) {
        snprintf(out, out_len, "%s%s", NCL_TOPIC_PONG_PREFIX,
                 server->sn != NULL ? server->sn : "");
        return;
    }
    if (prefix + strlen("Response") + 1 > out_len) {
        snprintf(out, out_len, "%s", request_topic);
        return;
    }
    memcpy(out, request_topic, prefix);
    snprintf(out + prefix, out_len - prefix, "Response%s", needle != NULL
                                                              ? needle + 7
                                                              : "");
}

ncl_err ncl_server_subscribe(ncl_server *server)
{
    char *topics[8];
    size_t i;
    ncl_err rc = NCL_OK;
    int granted = -1;

    if (server == NULL || server->mqtt == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    topics[0] = ncl_topic_query_request(server->sn, NULL);
    topics[1] = ncl_topic_probe_query_request(server->sn, NULL);
    topics[2] = ncl_topic_set_request(server->sn, NULL);
    topics[3] = ncl_topic_probe_set_request(server->sn, NULL);
    topics[4] = ncl_topic_method_call_request(server->sn, NULL);
    topics[5] = ncl_topic_ping(server->sn);
    topics[6] = ncl_topic_method_status_request(server->sn, NULL);
    topics[7] = ncl_topic_method_result_request(server->sn, NULL);

    for (i = 0; i < 8; i++) {
        if (topics[i] == NULL) {
            rc = NCL_ERR_NOMEM;
            break;
        }
        rc = ncl_mqtt_client_subscribe(server->mqtt, topics[i], 2, 10000, &granted);
        if (rc != NCL_OK) {
            ncl_log_error("服务端订阅失败: %s", topics[i]);
            break;
        }
    }
    for (i = 0; i < 8; i++) {
        ncl_mem_free(topics[i]);
    }
    return rc;
}

/* ================================================ asynchronous method call = */

static void ncl_method_task_free(ncl_method_task *task)
{
    if (task == NULL) {
        return;
    }
    ncl_mem_free(task->handler);
    ncl_mem_free(task->status);
    ncl_json_free(task->value);
    ncl_mem_free(task->reason);
    ncl_mem_free(task);
}

/** Task of @p handler, or NULL. Caller holds server->mutex. */
static ncl_method_task *ncl_server_find_task_locked(ncl_server *server,
                                                    const char *handler)
{
    size_t i;

    for (i = 0; i < server->method_tasks.len; i++) {
        ncl_method_task *task =
            (ncl_method_task *)server->method_tasks.items[i];
        if (task->handler != NULL && handler != NULL &&
            strcmp(task->handler, handler) == 0) {
            return task;
        }
    }
    return NULL;
}

/** Drop a task from the table (the caller keeps ownership of the memory). */
static void ncl_server_unlink_task_locked(ncl_server *server,
                                          ncl_method_task *task)
{
    size_t i;

    for (i = 0; i < server->method_tasks.len; i++) {
        if (server->method_tasks.items[i] == task) {
            for (; i + 1 < server->method_tasks.len; i++) {
                server->method_tasks.items[i] = server->method_tasks.items[i + 1];
            }
            server->method_tasks.len--;
            return;
        }
    }
}

/** The method of one asynchronous call: runs on the shared thread pool. */
static void ncl_method_worker_run(void *arg)
{
    ncl_method_worker *work = (ncl_method_worker *)arg;
    ncl_server *server = work->server;
    ncl_json *value = NULL;
    char *reason = NULL;
    ncl_err rc = ncl_server_call_binding(work->binding, work->params, &value,
                                         &reason);
    bool kept = false;

    ncl_mutex_lock(server->mutex);
    if (!work->task->abandoned) {
        work->task->rc = rc;
        work->task->value = value;
        work->task->reason = reason;
        work->task->finished = true;
        work->task->finished_ms = ncl_time_monotonic_millis();
        kept = true;
    }
    ncl_mutex_unlock(server->mutex);
    if (!kept) {
        ncl_json_free(value);
        ncl_mem_free(reason);
        ncl_method_task_free(work->task);
    }
    ncl_json_free(work->params);
    ncl_mem_free(work);
}

/* Asynchronous request handling: work is submitted to the shared thread pool. */
typedef struct {
    ncl_server  *server;
    char        *topic;
    ncl_message *request;
} ncl_server_task;

static void ncl_server_task_run(void *arg)
{
    ncl_server_task *task = (ncl_server_task *)arg;
    ncl_message *response;
    char response_topic[512];

    ncl_log_info("服务端处理请求: %s", task->topic);
    response = ncl_server_dispatch(task->server, task->topic, task->request);
    {
        ncl_msg_type request_type = task->request->type;
        ncl_message_free(task->request);
        if (response != NULL) {
            ncl_server_response_topic(task->server, task->topic, request_type,
                                      response_topic, sizeof(response_topic));
            ncl_server_publish(task->server, response_topic, response);
            ncl_message_free(response);
        }
    }
    ncl_mem_free(task->topic);
    ncl_mem_free(task);
}

void ncl_server_on_message(ncl_server *server, const char *topic,
                           ncl_message *request)
{
    ncl_server_task *task;

    if (server == NULL || request == NULL) {
        ncl_message_free(request);
        return;
    }
    if (topic == NULL) {
        ncl_message_free(request);
        return;
    }

    /* Handling must
     * not run on the MQTT reader thread, because publishing the response from
     * there would deadlock waiting for acknowledgements the reader itself
     * still has to process. */
    task = (ncl_server_task *)ncl_mem_calloc(1, sizeof(ncl_server_task));
    if (task == NULL) {
        ncl_message_free(request);
        return;
    }
    task->server = server;
    task->topic = ncl_strdup(topic);
    task->request = request;
    if (task->topic == NULL) {
        ncl_mem_free(task);
        ncl_message_free(request);
        return;
    }

    if (ncl_thread_pool_submit(ncl_thread_service(), ncl_server_task_run, task) !=
        NCL_OK) {
        /* Pool unavailable (shutting down): fall back to this thread. */
        ncl_server_task_run(task);
    }
}

ncl_err ncl_server_report_method_progress(ncl_server *server,
                                          const char *handler, long long process,
                                          const char *status)
{
    ncl_method_task *task;
    char *copy = NULL;

    if (server == NULL || ncl_str_is_blank(handler)) {
        return NCL_ERR_INVALID_ARG;
    }
    if (status != NULL && (copy = ncl_strdup(status)) == NULL) {
        return NCL_ERR_NOMEM;
    }
    ncl_mutex_lock(server->mutex);
    task = ncl_server_find_task_locked(server, handler);
    if (task == NULL) {
        ncl_mutex_unlock(server->mutex);
        ncl_mem_free(copy);
        return NCL_ERR_NOT_FOUND;
    }
    task->process = process;
    if (copy != NULL) {
        ncl_mem_free(task->status);
        task->status = copy;
    }
    ncl_mutex_unlock(server->mutex);
    return NCL_OK;
}

size_t ncl_server_pending_method_count(ncl_server *server)
{
    size_t count;

    if (server == NULL) {
        return 0;
    }
    ncl_mutex_lock(server->mutex);
    count = server->method_tasks.len;
    ncl_mutex_unlock(server->mutex);
    return count;
}

/* ============================================================== sampling == */

ncl_err ncl_server_push_event_ex(ncl_server *server, const char *event_id,
                                 const ncl_json *event, int64_t time_ms,
                                 const char *message_id)
{
    ncl_message *message;
    char *topic;
    ncl_err rc;

    if (server == NULL || event_id == NULL || event == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    message = ncl_message_new(NCL_MSG_EVENT);
    if (message == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (message_id != NULL) {
        ncl_message_set_message_id(message, message_id);
    }
    ncl_message_set_sample_id(message, event_id);
    ncl_message_set_event_time_ms(message, time_ms);
    ncl_message_set_event(message, ncl_json_clone(event));
    rc = ncl_message_finalise(message);
    if (rc != NCL_OK) {
        ncl_message_free(message);
        return rc;
    }
    topic = ncl_topic_event(server->sn, NULL);
    if (topic == NULL) {
        ncl_message_free(message);
        return NCL_ERR_NOMEM;
    }
    rc = ncl_server_publish(server, topic, message);
    if (rc == NCL_OK) {
        ncl_mutex_lock(server->mutex);
        server->events++;
        ncl_mutex_unlock(server->mutex);
    }
    ncl_mem_free(topic);
    ncl_message_free(message);
    return rc;
}

ncl_err ncl_server_push_event(ncl_server *server, const char *event_id,
                              const ncl_json *event)
{
    return ncl_server_push_event_ex(server, event_id, event, 0, NULL);
}

size_t ncl_server_event_count(ncl_server *server)
{
    size_t count;

    if (server == NULL) {
        return 0;
    }
    ncl_mutex_lock(server->mutex);
    count = server->events;
    ncl_mutex_unlock(server->mutex);
    return count;
}

static void ncl_sample_task_free(ncl_sample_task *task)
{
    if (task == NULL) {
        return;
    }
    ncl_mem_free(task->id);
    ncl_mem_free(task->topic);
    ncl_node_free(task->config);
    ncl_strvec_free(&task->paths);
    ncl_strvec_free(&task->header);
    ncl_mutex_destroy(task->mutex);
    ncl_cond_destroy(task->cond);
    ncl_mem_free(task);
}

/** Sleep that wakes early when the task is asked to stop. */
static void ncl_sample_sleep(ncl_sample_task *task, long long millis)
{
    if (millis <= 0) {
        return;
    }
    ncl_mutex_lock(task->mutex);
    if (!task->stop) {
        ncl_cond_wait_timeout(task->cond, task->mutex, (unsigned)millis);
    }
    ncl_mutex_unlock(task->mutex);
}

static bool ncl_sample_stopping(ncl_sample_task *task)
{
    bool stop;
    ncl_mutex_lock(task->mutex);
    stop = task->stop;
    ncl_mutex_unlock(task->mutex);
    return stop;
}

/**
 * Accumulate one value per sample item for the current upload window.
 * One query per sample item,
 * invoke() against the model, on failure a null value is appended.
 */
static void ncl_sample_collect(ncl_sample_task *task, ncl_message *sample)
{
    ncl_server *server = task->server;
    size_t count = ncl_strvec_len(&task->paths);
    size_t item;
    long long sample_interval = task->config->sample_interval;
    long long upload_interval = task->config->upload_interval;
    long long start;
    long long step_us;
    int sampled = 0;
    int rounds;
    int round;
    int missed = 0;

    if (sample_interval <= 0) {
        sample_interval = 1000;
    }
    if (upload_interval <= 0) {
        upload_interval = sample_interval;
    }
    rounds = (int)(upload_interval / sample_interval);
    if (rounds <= 0) {
        rounds = 1;
    }

    for (item = 0; item < count; item++) {
        ncl_sample_item *sample_item = ncl_sample_item_new();

        if (sample_item != NULL) {
            ncl_message_add_sample_item(sample, sample_item);
        }
    }

    /*
     * 排拍用**微秒钟**（`ncl_time_monotonic_us`）：毫秒钟在 Windows 上是
     * GetTickCount64，粒度 ~15.6 ms，拿它判"1 ms 这一拍到了没"纯属瞎猜，
     * 会把大量拍误判成"错过"（修这个 bug 时先踩过这一脚）。
     */
    start = ncl_time_monotonic_us();
    step_us = sample_interval * 1000;

    /*
     * 一包 = `rounds` 拍（uploadInterval / sampleInterval），**每拍每列正好一格**：
     *
     *   - 这一拍还没到        → 睡到那一刻；
     *   - 已经落后一整拍以上  → 这一拍**不补采**，给每列补一格空批 `[]`
     *                           （"这一拍没数据"，客户端 normalise 时会摊成 null）。
     *
     * 为什么非得按墙钟排拍：一轮（把该通道的采样项挨个取一遍）**是有成本的**。
     * 2026-09-23 在 Windows 上量的：示例模型 EdgeSersors 一轮 20 项（含各轴振动）
     * 约 4 ms；去掉那些项之后 8 项约 1 ms。模型写
     * `sampleInterval = 1 ms` 时一轮塞不进一拍；老实现只在"还有富余"时才睡，塞不进
     * 就背靠背连着跑，于是**整包时间 = rounds × 一轮时间**，上报周期从 100 ms 被悄悄
     * 拉成 420 ms —— 现场看到的就是"设置了 uploadInterval = 100，每秒却只有两包"。
     * 现在上报周期始终等于 uploadInterval，没采到的拍如实留空，并且**说一句**。
     */
    for (round = 0; round < rounds && !ncl_sample_stopping(task); round++) {
        long long tick = start + (long long)round * step_us;
        long long now = ncl_time_monotonic_us();

        if (tick - now >= 2000) {
            /*
             * 还有富余就睡到这一拍 —— **只差 1 ms 不睡**：Windows 上 Sleep(1)
             * 实际要 1.5~2 ms，睡下去反而把自己睡过一拍（下一拍被判成"错过"），
             * 于是拍几乎全被留空。宁可早采这一拍（±1 ms 的抖动），也别睡出个空拍。
             */
            ncl_sample_sleep(task, (unsigned)((tick - now) / 1000));
        } else if (now - tick >= step_us) {
            /* 这一拍已经错过：留空批，**不补采**（补采的值时间戳是假的，还会接着拖后面） */
            for (item = 0; item < count; item++) {
                ncl_sample_item *target =
                    (ncl_sample_item *)ncl_message_item_at(sample, item);

                if (target != NULL) {
                    ncl_sample_item_add_value(target, ncl_json_new_array());
                }
            }
            missed++;
            continue;
        }

        {
            for (item = 0; item < count; item++) {
                ncl_sample_item *target =
                    (ncl_sample_item *)ncl_message_item_at(sample, item);
                const char *path = ncl_strvec_at(&task->paths, item);
                ncl_binding *binding;
                ncl_json *value = NULL;
                char *reason = NULL;
                ncl_err rc;

                if (target == NULL) {
                    continue;
                }
                /*
                 * **直接叫绑定**，不走"请求报文 → 应答报文 → 克隆"那一套：采样这一路
                 * 每秒要跑几万次，报文开销比取值本身还大（老实现每项约 200 µs）。
                 * 值拿到手直接交给样本项（`ncl_sample_item_add_value` 接管所有权，
                 * 所以不用克隆）；取不到就记 null，与走查询那条路的行为一致。
                 */
                binding = ncl_server_lookup(server, "get_value", path);
                if (binding == NULL) {
                    ncl_sample_item_add_value(target, NULL);
                    continue;
                }
                rc = ncl_server_call_binding(binding, NULL, &value, &reason);
                ncl_free_safe(reason);
                if (rc == NCL_OK && value != NULL) {
                    ncl_sample_item_add_value(target, value);
                } else {
                    ncl_json_free(value);
                    ncl_sample_item_add_value(target, NULL);
                }
            }
            sampled++;
        }
    }

    if (missed > 0 && !task->warned_slow) {
        task->warned_slow = true;
        ncl_log_warn("采样通道 %s 跟不上模型：sampleInterval = %lld ms，一轮（%u 项）打不进一拍 —— "
                     "这一包只有 %d/%d 拍采到，没采到的留了空批 []。上报周期仍按 "
                     "uploadInterval = %lld ms 走（老实现会把这一包整个拖长，现在不会了）。"
                     "要有密数据：把 sampleInterval 调大，或者让取值一次多给几点。",
                     task->id, sample_interval, (unsigned)count, sampled, rounds,
                     upload_interval);
    }

    ncl_message_set_sample_id(sample, task->id);
    {
        /* 壁钟时间戳，对端拿来跟自己的钟对时。 */
        char begin[32];

        snprintf(begin, sizeof(begin), "%lld", (long long)ncl_time_millis());
        ncl_message_set_begin_time(sample, begin);
    }
    ncl_message_set_sample_interval(sample, sample_interval);
    ncl_message_set_upload_interval(sample, upload_interval);
    /* 表头用设备内路径（header），取值用绝对路径（paths）。两者在构造阶段
     * 就已经等长且同序，这里只按第一者的长度走一遍。 */
    for (item = 0; item < count; item++) {
        ncl_message_add_sample_path(sample, ncl_strvec_at(&task->header, item));
    }
}

static void ncl_sample_thread(void *arg)
{
    ncl_sample_task *task = (ncl_sample_task *)arg;
    ncl_server *server = task->server;
    long long upload_interval = task->config->upload_interval;

    if (upload_interval <= 0) {
        upload_interval = task->config->sample_interval > 0
                              ? task->config->sample_interval
                              : 1000;
    }

    while (!ncl_sample_stopping(task)) {
        long long window_start = ncl_time_monotonic_millis();
        ncl_message *sample = ncl_message_new(NCL_MSG_SAMPLE);

        if (sample == NULL) {
            break;
        }
        ncl_sample_collect(task, sample);

        /*
         * 发布前两步，顺序不能反：
         *
         *   1. **占位列换成 null**（`ncl_message_sample_fill_empty_columns`）。
         *      整列 `[]` 是"本周期该项没有数据"的占位，但一个**空表值**（比如
         *      报警列表为空）编码出来跟它一模一样 —— 拿"有一列是 `[]`"当"这一包
         *      没采到"会把整包错杀掉：通道里只要有一项的值可能是空表，这个通道就
         *      永远发不出去（伪机床的报警列就是这么把它自己钉死的）。换成 null
         *      之后两种情形都成立：真没采到 → 一条完整的 null 报文（"设备活着、
         *      这一刻没有数据"），空表值 → 语义等价的 null。
         *   2. 外层结构（表头与列数一致、各列槽位对齐）对不上才是真的残缺：
         *      丢弃并记错，绝不让消费端拿到半截数据。
         */
        if (!ncl_sample_stopping(task)) {
            (void)ncl_message_sample_fill_empty_columns(sample);
            if (ncl_message_sample_is_complete(sample)) {
                ncl_server_publish(server, task->topic, sample);
                ncl_mutex_lock(server->mutex);
                server->sample_uploads++;
                ncl_mutex_unlock(server->mutex);
            } else {
                ncl_log_error("采样报文不完整，已丢弃: 通道 %s", task->id);
            }
        }
        ncl_message_free(sample);

        {
            long long elapsed = ncl_time_monotonic_millis() - window_start;
            ncl_sample_sleep(task, upload_interval - elapsed);
        }
    }
    ncl_log_info("采样任务已停止: %s", task->id);
}

/**
 * Build the sampling task for @p config: validate the channel, resolve the
 * sample paths (the lookup keys) and the report header, then remember the
 * channel.
 *
 * Two shapes are supported and nothing else:
 *
 *   1. 采样通道在模型文件里有定义 - every sample item names a node (id), which is
 *      looked up in the loaded model to obtain its path.
 *   2. 采样通道在模型里没有定义，但给了表头 - every sample item is already a
 *      path ("/..."), queried through the tool bindings of that path.
 *
 * Either way the two forms differ by the device segment: bindings answer on the
 * absolute path ("/MACHINE/STATUS"), the header on the wire drops the segment
 * ("/STATUS") - every column of one channel sits on the same device, so the
 * prefix would just be repeated on every entry (see ncl_path_without_device).
 *
 * Anything else (empty id, no sample items, missing/non-positive intervals, a
 * relative id that the model cannot resolve) fails with a specific error
 * instead of quietly reporting a report full of nulls.
 */
static ncl_err ncl_sample_task_create(ncl_server *server,
                                      const ncl_node *config,
                                      ncl_sample_task **out)
{
    ncl_sample_task *task;
    const char *device_path;
    size_t i;

    *out = NULL;
    if (config->id == NULL || ncl_str_is_blank(config->id)) {
        ncl_log_error("采样通道缺少 id");
        return NCL_ERR_INVALID_ARG;
    }
    if (ncl_node_sample_count(config) == 0) {
        ncl_log_error("采样通道 %s 没有采样项（ids 为空）", config->id);
        return NCL_ERR_INVALID_MODEL;
    }
    if (!config->has_sample_interval || config->sample_interval <= 0) {
        ncl_log_error("采样通道 %s 缺少合法的 sampleInterval", config->id);
        return NCL_ERR_INVALID_ARG;
    }
    if (!config->has_upload_interval || config->upload_interval <= 0) {
        ncl_log_error("采样通道 %s 缺少合法的 uploadInterval", config->id);
        return NCL_ERR_INVALID_ARG;
    }

    task = (ncl_sample_task *)ncl_mem_calloc(1, sizeof(ncl_sample_task));
    if (task == NULL) {
        return NCL_ERR_NOMEM;
    }
    task->server = server;
    task->id = ncl_strdup(config->id);
    task->config = ncl_node_clone(config, false);
    task->mutex = ncl_mutex_create();
    task->cond = ncl_cond_create();
    ncl_strvec_init(&task->paths);
    ncl_strvec_init(&task->header);

    if (task->id == NULL || task->config == NULL || task->mutex == NULL ||
        task->cond == NULL) {
        ncl_sample_task_free(task);
        return NCL_ERR_NOMEM;
    }

    task->topic = ncl_topic_sample(server->sn, task->id);
    if (task->topic == NULL) {
        ncl_sample_task_free(task);
        return NCL_ERR_NOMEM;
    }

    /* The device the header is relative to: the channel's own device when it
     * came from the model, otherwise the one device the server serves. */
    device_path = ncl_node_device_path(config);
    if (device_path == NULL && server->root_node != NULL) {
        device_path = ncl_node_device_path(ncl_node_device_at(server->root_node, 0));
    }

    for (i = 0; i < ncl_node_sample_count(config); i++) {
        const ncl_sample_ref *ref = ncl_node_sample_at(config, i);
        char *path = NULL;
        char *header = NULL;

        if (ref == NULL || ref->id == NULL || ncl_str_is_blank(ref->id)) {
            ncl_log_error("采样通道 %s 的第 %u 个采样项缺少 id", config->id,
                          (unsigned)(i + 1));
            ncl_sample_task_free(task);
            return NCL_ERR_INVALID_ARG;
        }
        if (ref->id[0] == NCL_PATH_SEPARATOR[0]) {
            /* 情况 2：直接就是表头里的路径，不需要模型里有定义。 */
            path = ncl_strdup(ref->id);
        } else {
            /* 情况 1：节点 id，必须能在模型里解析出路径。 */
            ncl_node *node = server->root_node != NULL
                                 ? ncl_node_find_by_id(server->root_node, ref->id)
                                 : NULL;
            if (node != NULL && ncl_node_path(node) != NULL) {
                path = ncl_strdup(ncl_node_path(node));
            }
        }
        if (path == NULL) {
            ncl_log_error("采样通道 %s 的采样项 %s 既不是路径，也无法在模型里找到",
                          config->id, ref->id);
            ncl_sample_task_free(task);
            return NCL_ERR_NOT_FOUND;
        }
        if (ncl_strvec_push(&task->paths, path) != NCL_OK) {
            ncl_mem_free(path);
            ncl_sample_task_free(task);
            return NCL_ERR_NOMEM;
        }
        ncl_mem_free(path);

        header = ncl_path_without_device(ncl_strvec_at(&task->paths, i),
                                         device_path);
        if (header == NULL || ncl_strvec_push(&task->header, header) != NCL_OK) {
            ncl_mem_free(header);
            ncl_sample_task_free(task);
            return NCL_ERR_NOMEM;
        }
        ncl_mem_free(header);
    }

    /* 表头在构造阶段就已完整：header 的项数 == 采样项个数。 */
    *out = task;
    return NCL_OK;
}
/** Stop and release one task. Caller must not hold server->mutex. */
static void ncl_sample_task_stop(ncl_sample_task *task)
{
    if (task == NULL) {
        return;
    }
    ncl_mutex_lock(task->mutex);
    task->stop = true;
    ncl_cond_broadcast(task->cond);
    ncl_mutex_unlock(task->mutex);

    if (task->thread != NULL) {
        ncl_thread_join(task->thread);
        task->thread = NULL;
    }
    ncl_sample_task_free(task);
}

static int ncl_server_find_sample(ncl_server *server, const char *id)
{
    size_t i;
    for (i = 0; i < server->sample_count; i++) {
        if (strcmp(server->samples[i]->id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

ncl_err ncl_server_start_sample(ncl_server *server, const ncl_node *config)
{
    ncl_sample_task *task;
    ncl_err rc;
    int existing;

    if (server == NULL || config == NULL || config->id == NULL) {
        return NCL_ERR_INVALID_ARG;
    }

    /* An existing channel is stopped before it is restarted. */
    ncl_server_remove_sample(server, config->id);

    /* Missing id / items / intervals, or an id the model cannot resolve:
       情况 1、2 之外的一律当异常报出去，不启动任务。 */
    rc = ncl_sample_task_create(server, config, &task);
    if (rc != NCL_OK) {
        return rc;
    }
    task->thread = ncl_thread_start(ncl_sample_thread, task);
    if (task->thread == NULL) {
        ncl_sample_task_free(task);
        return NCL_ERR_NOMEM;
    }

    ncl_mutex_lock(server->mutex);
    existing = ncl_server_find_sample(server, task->id);
    if (existing >= 0) {
        ncl_mutex_unlock(server->mutex);
        ncl_sample_task_stop(task);
        return NCL_ERR_EXISTS;
    }
    if (server->sample_count == server->sample_capacity) {
        size_t capacity = server->sample_capacity == 0 ? 4
                                                       : server->sample_capacity * 2;
        ncl_sample_task **grown = (ncl_sample_task **)ncl_mem_realloc(
            server->samples, capacity * sizeof(ncl_sample_task *));
        if (grown == NULL) {
            ncl_mutex_unlock(server->mutex);
            ncl_sample_task_stop(task);
            return NCL_ERR_NOMEM;
        }
        server->samples = grown;
        server->sample_capacity = capacity;
    }
    server->samples[server->sample_count++] = task;
    ncl_mutex_unlock(server->mutex);
    ncl_log_info("采样任务已启动: %s -> %s", task->id, task->topic);
    return NCL_OK;
}

ncl_err ncl_server_add_sample(ncl_server *server, const ncl_node *config)
{
    if (config == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return ncl_server_start_sample(server, config);
}

ncl_err ncl_server_remove_sample(ncl_server *server, const char *id)
{
    ncl_sample_task *task = NULL;
    int index;

    if (server == NULL || id == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_mutex_lock(server->mutex);
    index = ncl_server_find_sample(server, id);
    if (index >= 0) {
        task = server->samples[index];
        memmove(&server->samples[index], &server->samples[index + 1],
                (server->sample_count - (size_t)index - 1) * sizeof(ncl_sample_task *));
        server->sample_count--;
    }
    ncl_mutex_unlock(server->mutex);

    if (task == NULL) {
        return NCL_ERR_NOT_FOUND;
    }
    ncl_sample_task_stop(task);
    return NCL_OK;
}

void ncl_server_stop_all_samples(ncl_server *server)
{
    ncl_sample_task **tasks;
    size_t count;
    size_t i;

    if (server == NULL) {
        return;
    }
    ncl_mutex_lock(server->mutex);
    tasks = server->samples;
    count = server->sample_count;
    server->samples = NULL;
    server->sample_count = 0;
    server->sample_capacity = 0;
    ncl_mutex_unlock(server->mutex);

    for (i = 0; i < count; i++) {
        ncl_sample_task_stop(tasks[i]);
    }
    ncl_mem_free(tasks);
}

ncl_err ncl_server_init_samples(ncl_server *server)
{
    ncl_node *device;
    size_t i;

    if (server == NULL || server->root_node == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    /* Only the first device of the model is scanned. */
    device = ncl_node_device_at(server->root_node, 0);
    if (device == NULL) {
        return NCL_ERR_INVALID_MODEL;
    }
    for (i = 0; i < ncl_ptrvec_len(&device->configs); i++) {
        ncl_node *config = ncl_node_config_at(device, i);
        if (ncl_node_is_sample_node(config)) {
            ncl_server_start_sample(server, config);
        }
    }
    return NCL_OK;
}

size_t ncl_server_sample_count(ncl_server *server)
{
    size_t count;
    if (server == NULL) {
        return 0;
    }
    ncl_mutex_lock(server->mutex);
    count = server->sample_count;
    ncl_mutex_unlock(server->mutex);
    return count;
}

size_t ncl_server_sample_upload_count(ncl_server *server)
{
    size_t count;
    if (server == NULL) {
        return 0;
    }
    ncl_mutex_lock(server->mutex);
    count = server->sample_uploads;
    ncl_mutex_unlock(server->mutex);
    return count;
}

/* ======================================================= built in tool ==== */

static ncl_err ncl_builtin_add_sample(void *instance, const ncl_json *params,
                                      ncl_json **result, char **reason)
{
    ncl_server *server = (ncl_server *)instance;
    const ncl_json *request;
    ncl_node *config;
    ncl_err rc;

    *result = NULL;
    request = ncl_json_obj_get(params, "request");
    if (request == NULL) {
        if (reason != NULL) {
            *reason = ncl_strdup("缺少 request 参数");
        }
        return NCL_ERR_INVALID_ARG;
    }
    config = ncl_node_from_json(request, NCL_NODE_CONFIG);
    if (config == NULL) {
        if (reason != NULL) {
            *reason = ncl_strdup("采样通道配置不是合法的配置节点");
        }
        return NCL_ERR_INVALID_MODEL;
    }
    rc = ncl_server_add_sample(server, config);
    ncl_node_free(config);
    if (rc != NCL_OK) {
        /* 把失败原因回给调用方（客户端看到 code=NG + reason）。 */
        if (reason != NULL) {
            *reason = ncl_strdup(ncl_err_name(rc));
        }
        return rc;
    }
    *result = ncl_json_new_bool(true);
    return NCL_OK;
}

static ncl_err ncl_builtin_remove_sample(void *instance, const ncl_json *params,
                                         ncl_json **result, char **reason)
{
    ncl_server *server = (ncl_server *)instance;
    const char *id = ncl_json_obj_get_string(params, "id");
    ncl_err rc;

    (void)reason;
    *result = NULL;
    if (id == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    rc = ncl_server_remove_sample(server, id);
    if (rc != NCL_OK) {
        return rc;
    }
    *result = ncl_json_new_bool(true);
    return NCL_OK;
}

ncl_err ncl_server_register_builtin_tool(ncl_server *server)
{
    static const ncl_tool_method methods[] = {
        /* addSample 的参数是一个采样通道配置对象；removeSample 按 id 撤。 */
        {"addSample", ncl_builtin_add_sample,
         "{\"type\":\"object\",\"properties\":{\"request\":{\"type\":\"object\"}},"
         "\"required\":[\"request\"]}",
         "{\"type\":\"boolean\"}"},
        {"removeSample", ncl_builtin_remove_sample,
         "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\","
         "\"minLength\":1}},\"required\":[\"id\"]}",
         "{\"type\":\"boolean\"}"},
    };
    return ncl_server_register_tool(server, "nclinkServer", server, methods,
                                    sizeof(methods) / sizeof(methods[0]), NULL, 0);
}
