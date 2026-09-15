/* NC-Link core - Result envelope and the schema endpoints. */
#include "nclink/ncl_rest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"
#include "nclink/ncl_config.h"

ncl_json *ncl_result_success(ncl_json *data)
{
    ncl_json *result = ncl_json_new_object();
    if (result == NULL) {
        ncl_json_free(data);
        return NULL;
    }
    ncl_json_obj_set_bool(result, "status", true);
    if (data != NULL) {
        ncl_json_obj_set(result, "data", data);
    }
    return result;
}

ncl_json *ncl_result_failed(const char *message)
{
    ncl_json *result = ncl_json_new_object();
    if (result == NULL) {
        return NULL;
    }
    ncl_json_obj_set_bool(result, "status", false);
    if (message != NULL) {
        ncl_json_obj_set_string(result, "data", message);
    }
    return result;
}

ncl_json *ncl_result_success_bool(bool value)
{
    return ncl_result_success(ncl_json_new_bool(value));
}

ncl_json *ncl_result_success_string(const char *value)
{
    return ncl_result_success(value != NULL ? ncl_json_new_string(value)
                                            : ncl_json_new_null());
}

void ncl_rest_reply(ncl_http_response *response, int status, ncl_json *data)
{
    ncl_json *envelope = ncl_result_success(data);
    ncl_http_reply_json(response, status, envelope);
    ncl_json_free(envelope);
}

void ncl_rest_reply_error(ncl_http_response *response, int status,
                          const char *message)
{
    ncl_json *envelope = ncl_result_failed(message);
    ncl_http_reply_json(response, status, envelope);
    ncl_json_free(envelope);
}

/* ============================================================== endpoints = */

typedef struct {
    ncl_server *server;
} ncl_rest_context;

static void ncl_rest_handle_schema(ncl_http_request *request,
                                   ncl_http_response *response, void *user)
{
    ncl_rest_context *context = (ncl_rest_context *)user;
    char *document;

    (void)request;
    if (context == NULL || context->server == NULL) {
        ncl_http_reply_text(response, NCL_HTTP_INTERNAL_ERROR,
                            "server not attached");
        return;
    }
    /* The schema endpoint answers with the document itself, not the
     * {status,data} envelope. */
    document = ncl_server_openapi_schema_json(context->server, "/api");
    if (document == NULL) {
        ncl_http_reply_text(response, NCL_HTTP_INTERNAL_ERROR,
                            "Failed to generate API schema");
        return;
    }
    ncl_http_reply(response, NCL_HTTP_OK, "application/json; charset=utf-8",
                   document, strlen(document));
    free(document);
}

/* Minimal stand-in for the bundled Swagger UI assets. */
static const char kSwaggerPage[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"zh-CN\"><head><meta charset=\"utf-8\">\n"
    "<title>NC-Link API</title>\n"
    "<style>body{font-family:-apple-system,Segoe UI,Arial,sans-serif;margin:2rem;"
    "color:#222}code{background:#f4f4f4;padding:.1rem .3rem;border-radius:3px}"
    "li{margin:.3rem 0}</style></head><body>\n"
    "<h1>NC-Link API</h1>\n"
    "<p>OpenAPI 文档：<a href=\"/api/schema\">/api/schema</a></p>\n"
    "<p>把该地址填入任意 OpenAPI 3.0 客户端（Swagger UI / Postman / Insomnia）"
    "即可浏览与调试全部操作。</p>\n"
    "<div id=\"ops\">正在加载…</div>\n"
    "<script>\n"
    "fetch('/api/schema').then(function (r) { return r.json(); }).then(function (d) {\n"
    "  var out = ['<h2>操作</h2><ul>'];\n"
    "  Object.keys(d.paths || {}).forEach(function (p) {\n"
    "    out.push('<li><code>POST ' + p + '</code></li>');\n"
    "  });\n"
    "  out.push('</ul>');\n"
    "  document.getElementById('ops').innerHTML = out.join('');\n"
    "}).catch(function (e) {\n"
    "  document.getElementById('ops').textContent = '加载失败: ' + e;\n"
    "});\n"
    "</script>\n"
    "</body></html>\n";

static void ncl_rest_handle_swagger(ncl_http_request *request,
                                    ncl_http_response *response, void *user)
{
    (void)request;
    (void)user;
    ncl_http_reply(response, NCL_HTTP_OK, "text/html; charset=utf-8",
                   kSwaggerPage, sizeof(kSwaggerPage) - 1);
}

/**
 * 把 HTTP 请求变成一个 MethodCallRequest。
 *
 *   POST /api/<工具>/<方法>
 *     Content-Type: application/json  -> 正文即 params
 *     Content-Type: multipart/form-data -> 不支持（文件传输请走文件通道）
 *   code=OK -> {"status":true,"data":...}   code=NG -> {"status":false,...}
 *   非 POST、或路径不是 /api/<工具>/<方法> -> 404
 *
 * 注意：工具返回的文件由 ncl_server_invoke_method_call() 换成 "/temp/<名字>"
 * 令牌并在 data.fileKeys 里列出键名，HTTP 调用方再按文件通道取字节（同 MQTT）。
 */
static void ncl_rest_handle_call(ncl_http_request *request,
                                 ncl_http_response *response, void *user)
{
    ncl_rest_context *context = (ncl_rest_context *)user;
    const char *path = ncl_http_path(request);
    const char *http_method = ncl_http_method(request);
    const char *content_type;
    const char *rest;
    const char *slash;
    char call_method[256];
    ncl_json *params = NULL;
    ncl_message *call;
    ncl_message *result;

    if (!ncl_streq_ignore_case(http_method, "POST")) {
        ncl_http_reply_text(response, NCL_HTTP_NOT_FOUND, "not found");
        return;
    }
    if (path == NULL || strncmp(path, "/api/", 5) != 0) {
        ncl_http_reply_text(response, NCL_HTTP_NOT_FOUND, "not found");
        return;
    }
    rest = path + 5;
    slash = strchr(rest, '/');
    if (slash == NULL || slash == rest || slash[1] == '\0' ||
        strchr(slash + 1, '/') != NULL) {
        /* 必须正好两段：<工具>/<方法> */
        ncl_http_reply_text(response, NCL_HTTP_NOT_FOUND, "not found");
        return;
    }
    snprintf(call_method, sizeof(call_method), "/%s", rest);

    content_type = ncl_http_header(request, "Content-Type");
    if (content_type != NULL &&
        ncl_strncasecmp(content_type, "multipart/form-data", 19) == 0) {
        ncl_rest_reply_error(response, NCL_HTTP_OK,
                             "multipart/form-data 暂不支持，请用 application/json");
        return;
    }
    if (ncl_http_body_len(request) > 0) {
        params = ncl_http_json_body(request);
        if (params == NULL) {
            ncl_rest_reply_error(response, NCL_HTTP_OK, "请求体不是合法 JSON");
            return;
        }
    }

    call = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
    if (call == NULL) {
        ncl_json_free(params);
        ncl_http_reply_text(response, NCL_HTTP_INTERNAL_ERROR, "out of memory");
        return;
    }
    ncl_message_set_method(call, call_method);
    if (params != NULL) {
        ncl_message_set_params(call, params); /* 所有权转移 */
    }
    if (ncl_message_finalise(call) != NCL_OK) {
        ncl_message_free(call);
        ncl_http_reply_text(response, NCL_HTTP_INTERNAL_ERROR, "invalid request");
        return;
    }

    result = ncl_server_invoke_method_call(context->server, call);
    ncl_message_free(call);
    if (result == NULL) {
        ncl_http_reply_text(response, NCL_HTTP_INTERNAL_ERROR, "no response");
        return;
    }
    if (ncl_check_is_code_ok(result->as.method_call_response.code)) {
        ncl_json *data = result->as.method_call_response.data;
        ncl_rest_reply(response, NCL_HTTP_OK,
                       data != NULL ? ncl_json_clone(data) : NULL);
    } else {
        const char *reason = result->as.method_call_response.reason;
        ncl_rest_reply_error(response, NCL_HTTP_OK,
                             reason != NULL ? reason : "调用失败");
    }
    ncl_message_free(result);
}

ncl_err ncl_rest_attach(ncl_http_server *http, ncl_server *server)
{
    ncl_rest_context *context;
    ncl_err rc;

    if (http == NULL || server == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    context = (ncl_rest_context *)calloc(1, sizeof(ncl_rest_context));
    if (context == NULL) {
        return NCL_ERR_NOMEM;
    }
    context->server = server;

    rc = ncl_http_server_route(http, "GET", "/api/schema",
                               ncl_rest_handle_schema, context);
    if (rc == NCL_OK) {
        rc = ncl_http_server_route(http, "GET", "/swagger-ui",
                                   ncl_rest_handle_swagger, context);
    }
    if (rc == NCL_OK) {
        rc = ncl_http_server_route(http, "GET", "/swagger-ui/",
                                   ncl_rest_handle_swagger, context);
    }
    /* POST /api/<tool>/<method> 直接调用已注册的工具方法。
     * 用前缀路由兜底，因此后注册的工具也能走 HTTP（精确路由优先匹配）。 */
    if (rc == NCL_OK) {
        rc = ncl_http_server_route(http, "*", "/api/*", ncl_rest_handle_call,
                                   context);
    }
    if (rc != NCL_OK) {
        free(context);
        return rc;
    }
    ncl_log_info("HTTP 接口已挂载: /api/schema, /swagger-ui, "
                 "POST /api/<工具>/<方法>");
    return NCL_OK;
}

/* ==================================================== configuration API === */

/* --- GET handlers: every one answers the success or failure envelope. --- */

static void ncl_rest_get_sn(ncl_http_request *request, ncl_http_response *response,
                            void *user)
{
    char *sn = ncl_config_get_sn();
    (void)request;
    (void)user;
    if (sn == NULL) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "SN号不存在");
        return;
    }
    ncl_rest_reply(response, NCL_HTTP_OK, ncl_json_new_string(sn));
    free(sn);
}

static void ncl_rest_get_model(ncl_http_request *request,
                               ncl_http_response *response, void *user)
{
    ncl_json *model = ncl_config_get_model();
    (void)request;
    (void)user;
    if (model == NULL) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "文件不存在或不是有效文件");
        return;
    }
    ncl_rest_reply(response, NCL_HTTP_OK, model);
}

static void ncl_rest_get_driver(ncl_http_request *request,
                                ncl_http_response *response, void *user)
{
    ncl_json *driver = ncl_config_get_driver();
    (void)request;
    (void)user;
    if (driver == NULL) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "文件不存在");
        return;
    }
    ncl_rest_reply(response, NCL_HTTP_OK, driver);
}

static void ncl_rest_get_mqtt(ncl_http_request *request,
                              ncl_http_response *response, void *user)
{
    ncl_json *config = ncl_config_get_mqtt();
    (void)request;
    (void)user;
    if (config == NULL) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "mqtt.cfg文件不存在");
        return;
    }
    ncl_rest_reply(response, NCL_HTTP_OK, config);
}

static void ncl_rest_get_servers(ncl_http_request *request,
                                 ncl_http_response *response, void *user)
{
    ncl_json *servers = ncl_config_get_server_list();
    (void)request;
    (void)user;
    if (servers == NULL) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "读取失败");
        return;
    }
    ncl_rest_reply(response, NCL_HTTP_OK, servers);
}

static void ncl_rest_get_ip_conf(ncl_http_request *request,
                                 ncl_http_response *response, void *user)
{
    ncl_json *config = ncl_config_get_ip_conf();
    (void)request;
    (void)user;
    if (config == NULL) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "文件不存在或不是有效文件");
        return;
    }
    ncl_rest_reply(response, NCL_HTTP_OK, config);
}

/* --- POST handlers --- */

/** Body text, or the value of the "value" field, or a query parameter. */
static const char *ncl_rest_argument(ncl_http_request *request,
                                     ncl_json **parsed_body)
{
    const char *body = ncl_http_body(request);

    if (parsed_body != NULL) {
        *parsed_body = NULL;
    }
    if (body != NULL && ncl_http_body_len(request) > 0) {
        /* A JSON document may carry the argument under "value"; plain text is
         * the argument itself. */
        ncl_json *json = ncl_http_json_body(request);
        if (json != NULL) {
            const char *value = ncl_json_obj_get_string(json, "value");
            if (value != NULL) {
                if (parsed_body != NULL) {
                    *parsed_body = json;
                }
                return value;
            }
            ncl_json_free(json);
        }
        return body;
    }
    return ncl_http_query(request, "value");
}

static void ncl_rest_post_init(ncl_http_request *request,
                               ncl_http_response *response, void *user)
{
    char *sn = NULL;
    const char *argument = ncl_rest_argument(request, NULL);
    ncl_err rc;
    (void)user;

    rc = ncl_config_init(argument, &sn);
    if (rc != NCL_OK) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "初始化失败");
        return;
    }
    free(sn);
    ncl_rest_reply(response, NCL_HTTP_OK, NULL); /* success with no data */
}

static void ncl_rest_post_set_model(ncl_http_request *request,
                                    ncl_http_response *response, void *user)
{
    const char *argument = ncl_rest_argument(request, NULL);
    (void)user;
    if (argument == NULL) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "缺少模型内容");
        return;
    }
    if (ncl_config_set_model(argument) != NCL_OK) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "写入模型文件失败");
        return;
    }
    ncl_rest_reply(response, NCL_HTTP_OK, NULL);
}

static void ncl_rest_post_set_driver(ncl_http_request *request,
                                     ncl_http_response *response, void *user)
{
    const char *argument = ncl_rest_argument(request, NULL);
    (void)user;
    if (argument == NULL) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "缺少驱动内容");
        return;
    }
    if (ncl_config_set_driver(argument) != NCL_OK) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "写入驱动文件失败");
        return;
    }
    ncl_rest_reply(response, NCL_HTTP_OK, NULL);
}

static void ncl_rest_post_set_servers(ncl_http_request *request,
                                      ncl_http_response *response, void *user)
{
    const char *argument = ncl_rest_argument(request, NULL);
    (void)user;
    if (argument == NULL) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "缺少Server内容");
        return;
    }
    if (ncl_config_set_server_list(argument) != NCL_OK) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "写入Server文件失败");
        return;
    }
    ncl_rest_reply(response, NCL_HTTP_OK, NULL);
}

static void ncl_rest_post_set_mqtt(ncl_http_request *request,
                                   ncl_http_response *response, void *user)
{
    ncl_json *config = ncl_http_json_body(request);
    (void)user;

    /* setMqttUrl(url) parsed its argument as {"url","username","password"}. */
    if (config == NULL) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "缺少连接配置");
        return;
    }
    if (ncl_config_set_mqtt(config) != NCL_OK) {
        ncl_json_free(config);
        ncl_rest_reply_error(response, NCL_HTTP_OK, "写入mqtt.cfg文件失败");
        return;
    }
    ncl_json_free(config);
    ncl_rest_reply(response, NCL_HTTP_OK, NULL);
}

static void ncl_rest_post_set_ip_conf(ncl_http_request *request,
                                      ncl_http_response *response, void *user)
{
    const char *argument = ncl_rest_argument(request, NULL);
    (void)user;
    if (argument == NULL) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "缺少IpConf内容");
        return;
    }
    if (ncl_config_set_ip_conf(argument) != NCL_OK) {
        ncl_rest_reply_error(response, NCL_HTTP_OK, "写入ipConf文件失败");
        return;
    }
    ncl_rest_reply(response, NCL_HTTP_OK, NULL);
}

typedef struct {
    const char      *method;
    const char      *path;
    ncl_http_handler handler;
} ncl_rest_route_spec;

ncl_err ncl_rest_attach_config(ncl_http_server *http)
{
    static const ncl_rest_route_spec routes[] = {
        {"GET", "/api/cfg/getSn", ncl_rest_get_sn},
        {"GET", "/api/cfg/getModel", ncl_rest_get_model},
        {"GET", "/api/cfg/getDriver", ncl_rest_get_driver},
        {"GET", "/api/getMqttUrl", ncl_rest_get_mqtt},
        {"GET", "/api/method/getServerList", ncl_rest_get_servers},
        {"GET", "/api/cfg/getIpConf", ncl_rest_get_ip_conf},
        {"POST", "/api/cfg/init", ncl_rest_post_init},
        {"POST", "/api/cfg/setModel", ncl_rest_post_set_model},
        {"POST", "/api/cfg/setDriver", ncl_rest_post_set_driver},
        {"POST", "/api/setMqttUrl", ncl_rest_post_set_mqtt},
        {"POST", "/api/method/setServer", ncl_rest_post_set_servers},
        {"POST", "/api/cfg/setIpConf", ncl_rest_post_set_ip_conf},
    };
    size_t i;

    if (http == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ncl_err rc = ncl_http_server_route(http, routes[i].method, routes[i].path,
                                           routes[i].handler, NULL);
        if (rc != NCL_OK) {
            return rc;
        }
    }
    ncl_log_info("HTTP 配置接口已挂载 (%zu 个)", sizeof(routes) / sizeof(routes[0]));
    return NCL_OK;
}
