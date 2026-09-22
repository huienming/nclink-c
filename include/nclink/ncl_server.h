/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - server side.
 *
 * Architecture note. A device implements its model with plain C functions and
 * declares them explicitly, so the server needs no reflection:
 *
 *   ncl_tool_method  - the method table of one "tool": a name, a C function
 *                      pointer and an optional parameter schema.
 *   ncl_tool_binding - "<operation>#<path>" -> method name, computed from the
 *                      model and the tool registration.
 *   ncl_server_invoke_query/set/method_call - the three request kinds.
 *
 * A tool method receives the raw params object and answers with a JSON value;
 * NULL means "no value" and is reported as code NG.
 */
#ifndef NCL_SERVER_H
#define NCL_SERVER_H

#include <stdbool.h>

#include "nclink/ncl_client.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_model.h"
#include "nclink/ncl_mqtt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ncl_server ncl_server;

/**
 * Outbound transport hook (ncl_server_options.publish): receives the topic and
 * the serialised message body a publish would have carried.
 */
typedef ncl_err (*ncl_server_publish_fn)(void *user, const char *topic,
                                         const char *payload, size_t len);

/**
 * A tool method. @p params is the request params object (may be NULL).
 * On success set *result to a JSON value owned by the caller (NULL means the
 * operation produced no value, which is reported as code NG) and return NCL_OK.
 * On failure return an error and optionally set *reason to a heap message
 * string.
 */
typedef ncl_err (*ncl_tool_fn)(void *instance, const ncl_json *params,
                               ncl_json **result, char **reason);

typedef struct {
    const char *name; /**< method name used by the bindings */
    ncl_tool_fn fn;
    /**
     * Optional JSON Schema (draft-07 subset) for the method parameters. It is
     * compiled once at registration and consulted when a method call arrives
     * with "check" enabled; see ncl_server_check_method_call().
     */
    const char *params_schema;
    /**
     * Optional JSON Schema for what the method returns (the "return" member of
     * a Method/Call response). Not validated at runtime - it is published: it
     * goes into the OpenAPI document and into the model's METHODS item, so a
     * client knows what it is about to receive. NULL = not described.
     */
    const char *result_schema;
} ncl_tool_method;

/** Binds one operation on one model path to a tool method. */
typedef struct {
    const char  *path;      /**< full model path, e.g. "/NC_LINK_ROOT/PLC/STATUS" */
    ncl_operation operation;
    const char  *method;    /**< name from the tool's method table */
    const char  *tool;      /**< tool name; NULL applies to the most recently
                                 registered tool */
} ncl_tool_binding;

typedef struct {
    const char      *sn;          /**< device serial number (required) */
    ncl_mqtt_client *mqtt;        /**< borrowed; NULL for offline invocations */
    const char      *model_json;  /**< optional initial model document */
    /**
     * Optional outbound transport override. When set, every response and event
     * is handed to this callback (serialised, as published) instead of the MQTT
     * client. Useful for embedding the server in a host that owns its own
     * transport, and for tests that need a server without a broker.
     */
    ncl_server_publish_fn publish;
    void                 *publish_user;
} ncl_server_options;

/**
 * Fingerprint of the structs a caller fills in and the core reads
 * (ncl_server_options, ncl_tool_method, ncl_tool_binding): their sizes, which
 * change whenever a field is added.
 *
 * A caller that links a *prebuilt* core library - the language shims, the
 * adapters, a host program - compares this against ncl_server_abi_shape(),
 * which was compiled into that library. A mismatch means the two were built
 * from different headers: the core would read fields that are not there, and
 * the array element count would be wrong. That is silent memory corruption, so
 * refuse the call instead (nclshim_server_create() does exactly that).
 */
#define NCL_SERVER_ABI_SHAPE                                                   \
    ((unsigned)(sizeof(ncl_server_options) * 1000000u +                        \
                sizeof(ncl_tool_method) * 1000u + sizeof(ncl_tool_binding)))

/** NCL_SERVER_ABI_SHAPE as compiled into the core library. */
unsigned ncl_server_abi_shape(void);

ncl_server *ncl_server_create(const ncl_server_options *options);
void        ncl_server_free(ncl_server *server);

/** Destructor run on the attached data when the server is released. */
typedef void (*ncl_server_cleanup_fn)(void *data);

/**
 * Attach caller-owned data to the server, released (through @p cleanup when it
 * is not NULL) by ncl_server_free(). The file tool uses this to park its FTP
 * client and endpoint next to the server they belong to.
 */
ncl_err ncl_server_set_user_data(ncl_server *server, void *data,
                                 ncl_server_cleanup_fn cleanup);
void   *ncl_server_user_data(const ncl_server *server);

/** Serial number this server answers for. */
const char *ncl_server_sn(const ncl_server *server);

/* Model ------------------------------------------------------------------ */

/** Parse a model document, run post-construction and take ownership. */
ncl_err ncl_server_load_model(ncl_server *server, const char *model_json);

/** Install a model constructed by the caller (ownership transfers). */
ncl_err ncl_server_set_model(ncl_server *server, ncl_node *root);

ncl_node *ncl_server_model(ncl_server *server);

/** Persist the model to the file ncl_env_model_file() names. */
ncl_err ncl_server_save_model(ncl_server *server);

/* Tools ------------------------------------------------------------------ */

ncl_err ncl_server_register_tool(ncl_server *server, const char *tool_name,
                                 void *instance,
                                 const ncl_tool_method *methods,
                                 size_t method_count,
                                 const ncl_tool_binding *bindings,
                                 size_t binding_count);

/** Number of "<operation>#<path>" bindings currently registered. */
size_t ncl_server_binding_count(const ncl_server *server);

/** Number of distinct (tool, method) pairs, i.e. callable operations. */
size_t ncl_server_operation_count(const ncl_server *server);

/** Name of the tool owning operation @p index. */
const char *ncl_server_operation_tool(const ncl_server *server, size_t index);
/** Method name of operation @p index. */
const char *ncl_server_operation_method(const ncl_server *server, size_t index);

/**
 * One object per callable method (see NCL_METHODS_NODE_ID): tool, method,
 * address, the declared params / result schemas and the model paths the method
 * serves. This is what the model's METHODS item carries and what the OpenAPI
 * document is built from. The caller frees the array; NULL on failure.
 */
ncl_json *ncl_server_methods_json(ncl_server *server);

/**
 * Rebuild the model's METHODS item (see NCL_METHODS_NODE_ID) from the
 * currently registered tools. ncl_server_register_tool() only marks the item
 * stale - one tool costs one flag, not one rebuild - and the readers
 * (ncl_server_model(), a Probe answer, ncl_server_methods_json()) rebuild it
 * on the way out. ncl_server_set_model() rebuilds it right away. A host that
 * changes bindings behind the server's back calls this itself.
 */
ncl_err ncl_server_refresh_methods(ncl_server *server);

/**
 * Build the OpenAPI 3.0 document describing the server's operations: one POST
 * path per "<tool>/<method>" pair.
 * @param base_url value for the "servers[0].url" entry; the conventional value
 *                 is the request URL plus "/api".
 */
ncl_json *ncl_server_openapi_schema(ncl_server *server, const char *base_url);

/** ncl_server_openapi_schema() serialised to a heap JSON string. */
char *ncl_server_openapi_schema_json(ncl_server *server, const char *base_url);

/* Invocation ------------------------------------------------------------- */

/** Takes ownership of nothing; returns a new message the caller frees. */
ncl_message *ncl_server_invoke_query(ncl_server *server,
                                     const ncl_message *request);
ncl_message *ncl_server_invoke_set(ncl_server *server,
                                   const ncl_message *request);
ncl_message *ncl_server_invoke_method_call(ncl_server *server,
                                           const ncl_message *request);

/**
 * A *dry run* of a method call: the parameters are validated against the
 * method's schema (when it declares one) without invoking anything, and the
 * response carries code OK, or NG with the collected validation messages. A
 * method without a schema accepts only empty parameters, otherwise it answers
 * NG "参数数量不匹配".
 *
 * ncl_server_invoke_method_call() calls this on its own whenever the request
 * sets "check": true.
 */
ncl_message *ncl_server_check_method_call(ncl_server *server,
                                          const ncl_message *request);

/** Dispatch a parsed request to the matching invoke_* function. */
ncl_message *ncl_server_dispatch(ncl_server *server, const char *topic,
                                 const ncl_message *request);

/* MQTT wiring ------------------------------------------------------------ */

/** Subscribe to the six request topics of this serial number. */
ncl_err ncl_server_subscribe(ncl_server *server);

/** Handle one inbound message: process it and publish the response.
 *  Takes ownership of @p request. */
void ncl_server_on_message(ncl_server *server, const char *topic,
                           ncl_message *request);

/*
 * Asynchronous method calls. A request that carries "async": true is answered
 * immediately with code=OK and a handler; the method itself runs on the shared
 * thread pool. The client then asks for progress (Method/Status) and for the
 * outcome (Method/Result) with that handler - both are answered by the server
 * out of its own bookkeeping, so a tool implementation stays a plain function.
 */

/**
 * Report progress of the running call @p handler (process 0..100, status one of
 * NCL_KW_STATUS_*). Optional: without it the framework answers process=0 and
 * executing -> stopped.
 */
ncl_err ncl_server_report_method_progress(ncl_server *server,
                                          const char *handler, long long process,
                                          const char *status);

/** Calls with a handler that have not been collected through the result pair. */
size_t ncl_server_pending_method_count(ncl_server *server);

/** Publish a response message on @p topic. */
ncl_err ncl_server_publish(ncl_server *server, const char *topic,
                           const ncl_message *response);

/** Install (or clear) the outbound transport hook after creation. */
void ncl_server_set_publish_sink(ncl_server *server, ncl_server_publish_fn fn,
                                 void *user);

/* Sampling --------------------------------------------------------------- */

/**
 * Register a sample channel and start its sampling/upload task. The channel is
 * deep copied, so @p config stays owned by the caller. Restarting an existing
 * id replaces the task.
 */
ncl_err ncl_server_add_sample(ncl_server *server, const ncl_node *config);

/** Stop and remove a sample channel by id. */
ncl_err ncl_server_remove_sample(ncl_server *server, const char *id);

/** Stop every sample channel. */
void ncl_server_stop_all_samples(ncl_server *server);

/**
 * Start a sampling task without checking the configuration first.
 */
ncl_err ncl_server_start_sample(ncl_server *server, const ncl_node *config);

/**
 * Start a task for every SAMPLE_CHANNEL config of the first device in the
 * model.
 */
ncl_err ncl_server_init_samples(ncl_server *server);

size_t ncl_server_sample_count(ncl_server *server);

/** Number of uploads published so far on the sample topics (diagnostics). */
size_t ncl_server_sample_upload_count(ncl_server *server);

/** Register the built in "nclinkServer" tool (addSample / removeSample). */
ncl_err ncl_server_register_builtin_tool(ncl_server *server);

/* Events ------------------------------------------------------------------ */

/**
 * Publish an Event message on "Event/<sn>".
 *
 * @p event_id becomes the message's "id" field (an item id, for example) and
 * @p event the "event" object describing what happened:
 * {"key": ..., "value": ..., "oldValue": ...}. The message "time" defaults to
 * now and "@id" to a fresh UUID; both may be overridden through @p options.
 *
 * @p event is borrowed, never owned.
 */
ncl_err ncl_server_push_event(ncl_server *server, const char *event_id,
                              const ncl_json *event);

/** Push an event, overriding the message time and/or "@id". */
ncl_err ncl_server_push_event_ex(ncl_server *server, const char *event_id,
                                 const ncl_json *event, int64_t time_ms,
                                 const char *message_id);

/** Number of events published so far (diagnostics). */
size_t ncl_server_event_count(ncl_server *server);

#ifdef __cplusplus
}
#endif

#endif /* NCL_SERVER_H */
