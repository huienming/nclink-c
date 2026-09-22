/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * REST layer tests: the Result envelope, the generated OpenAPI document and the
 * /api/schema + /swagger-ui endpoints, all driven over a real socket.
 */
#include "ncl_test.h"

#include "nclink/ncl_http.h"
#include "nclink/ncl_rest.h"
#include "nclink/ncl_server.h"

#define TEST_SN "V203243111F"

static const char *kModelJson =
    "{\"name\":\"nclink\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\","
    "\"devices\":[{\"id\":\"02\",\"type\":\"PLC\",\"dataItems\":[],"
    "\"version\":\"2.0\"}]}";

/* ------------------------------------------------------------- test tool */

static ncl_err tool_get(void *instance, const ncl_json *params,
                        ncl_json **result, char **reason)
{
    (void)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_int(1);
    return NCL_OK;
}

static ncl_err tool_put(void *instance, const ncl_json *params,
                        ncl_json **result, char **reason)
{
    (void)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_bool(true);
    return NCL_OK;
}

/* ------------------------------------------------------------ HTTP client */

static bool http_get(unsigned port, const char *path, int *status, char *body,
                     size_t body_size)
{
    ncl_socket *sock;
    char request[512];
    char buffer[8192];
    ncl_strbuf raw;
    int rc;

    sock = ncl_socket_connect("127.0.0.1", port, 3000, NULL, 0);
    if (sock == NULL) {
        return false;
    }
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
             path);
    if (ncl_socket_send(sock, request, strlen(request)) != NCL_OK) {
        ncl_socket_close(sock);
        return false;
    }
    ncl_strbuf_init(&raw);
    while ((rc = ncl_socket_recv(sock, buffer, sizeof(buffer), 3000)) > 0) {
        ncl_strbuf_append(&raw, buffer, (size_t)rc);
    }
    ncl_socket_close(sock);
    if (raw.len == 0) {
        ncl_strbuf_free(&raw);
        return false;
    }
    if (status != NULL) {
        *status = atoi(raw.data + 9); /* "HTTP/1.1 " is 9 bytes */
    }
    {
        const char *sep = strstr(raw.data, "\r\n\r\n");
        if (body != NULL && body_size > 0) {
            size_t copy = 0;
            if (sep != NULL) {
                size_t available = raw.len - (size_t)(sep - raw.data) - 4;
                copy = available < body_size - 1 ? available : body_size - 1;
                memcpy(body, sep + 4, copy);
            }
            body[copy] = '\0';
        }
    }
    ncl_strbuf_free(&raw);
    return true;
}

/* -------------------------------------------------------------------- tests */

/** 发一个 POST，正文按 JSON 提交。 */
static bool http_post(unsigned port, const char *path, const char *json,
                      int *status, char *body, size_t body_size)
{
    ncl_socket *sock;
    char head[512];
    char buffer[8192];
    ncl_strbuf raw;
    int rc;

    sock = ncl_socket_connect("127.0.0.1", port, 3000, NULL, 0);
    if (sock == NULL) {
        return false;
    }
    snprintf(head, sizeof(head),
             "POST %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
             "Content-Type: application/json\r\nContent-Length: %u\r\n\r\n",
             path, (unsigned)(json != NULL ? strlen(json) : 0));
    if (ncl_socket_send(sock, head, strlen(head)) != NCL_OK ||
        (json != NULL && ncl_socket_send(sock, json, strlen(json)) != NCL_OK)) {
        ncl_socket_close(sock);
        return false;
    }
    ncl_strbuf_init(&raw);
    while ((rc = ncl_socket_recv(sock, buffer, sizeof(buffer), 3000)) > 0) {
        ncl_strbuf_append(&raw, buffer, (size_t)rc);
    }
    ncl_socket_close(sock);
    if (raw.len == 0) {
        ncl_strbuf_free(&raw);
        return false;
    }
    if (status != NULL) {
        *status = atoi(raw.data + 9);
    }
    {
        const char *sep = strstr(raw.data, "\r\n\r\n");
        if (body != NULL && body_size > 0) {
            size_t copy = 0;
            if (sep != NULL) {
                size_t available = raw.len - (size_t)(sep - raw.data) - 4;
                copy = available < body_size - 1 ? available : body_size - 1;
                memcpy(body, sep + 4, copy);
            }
            body[copy] = '\0';
        }
    }
    ncl_strbuf_free(&raw);
    return true;
}

static void test_result_envelope(void)
{
    ncl_json *envelope;
    char *text;

    NCL_TEST_CASE("a successful answer is {status,data}");
    envelope = ncl_result_success(ncl_json_new_string("V1"));
    text = ncl_json_write_string(envelope);
    NCL_CHECK_EQ_STR(text, "{\"status\":true,\"data\":\"V1\"}");
    ncl_free_safe(text);
    ncl_json_free(envelope);

    NCL_TEST_CASE("an answer without data omits the member");
    envelope = ncl_result_success(NULL);
    text = ncl_json_write_string(envelope);
    NCL_CHECK_EQ_STR(text, "{\"status\":true}");
    ncl_free_safe(text);
    ncl_json_free(envelope);

    NCL_TEST_CASE("a failed answer carries status=false");
    envelope = ncl_result_failed("bad request");
    text = ncl_json_write_string(envelope);
    NCL_CHECK_EQ_STR(text, "{\"status\":false,\"data\":\"bad request\"}");
    ncl_free_safe(text);
    ncl_json_free(envelope);

    NCL_TEST_CASE("typed helpers");
    envelope = ncl_result_success_bool(false);
    text = ncl_json_write_string(envelope);
    NCL_CHECK_EQ_STR(text, "{\"status\":true,\"data\":false}");
    ncl_free_safe(text);
    ncl_json_free(envelope);
}

static void test_openapi_document(void)
{
    ncl_server_options options;
    ncl_server *server;
    ncl_json *document;
    char *text;
    static const ncl_tool_method methods[] = {
    {"getValue", tool_get, NULL, NULL},
    {"setValue", tool_put,
     "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\"}},"
     "\"required\":[\"value\"]}",
     "{\"type\":\"boolean\",\"description\":\"写入是否成功\"}"},
    };

    memset(&options, 0, sizeof(options));
    options.sn = TEST_SN;
    options.model_json = kModelJson;
    server = ncl_server_create(&options);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    NCL_CHECK_EQ_INT(ncl_server_register_tool(server, "plcTool", server, methods,
                                              sizeof(methods) / sizeof(methods[0]),
                                              NULL, 0),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_server_register_builtin_tool(server), NCL_OK);

    NCL_TEST_CASE("distinct operations are enumerated");
    /* addSample, removeSample, getValue, setValue */
    NCL_CHECK_EQ_INT(ncl_server_operation_count(server), 4);

    NCL_TEST_CASE("the OpenAPI document has the expected shape");
    document = ncl_server_openapi_schema(server, NULL);
    NCL_CHECK(document != NULL);
    if (document != NULL) {
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(document, "openapi"), "3.0.0");
        NCL_CHECK(ncl_json_obj_get(document, "info") != NULL);
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_obj_get(document, "info"),
                                                 "title"),
                         "MCP Server API");
        {
            ncl_json *servers = ncl_json_obj_get(document, "servers");
            NCL_CHECK_EQ_INT(ncl_json_arr_len(servers), 1);
            NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_arr_get(servers, 0),
                                                     "url"),
                             "/api"); /* no base url supplied */
        }
        {
            ncl_json *paths = ncl_json_obj_get(document, "paths");
            NCL_CHECK_EQ_INT(ncl_json_obj_len(paths), 4);
            NCL_CHECK(ncl_json_obj_get(paths, "/plcTool/getValue") != NULL);
            NCL_CHECK(ncl_json_obj_get(paths, "/nclinkServer/addSample") != NULL);
            {
                ncl_json *post = ncl_json_obj_get(
                    ncl_json_obj_get(paths, "/plcTool/setValue"), "post");
                NCL_CHECK(post != NULL);
                NCL_CHECK(ncl_json_obj_get(post, "requestBody") != NULL);
                NCL_CHECK(ncl_json_obj_get(post, "responses") != NULL);
            }
        }
        {
            /* The declared schemas travel with the document, so a client can
             * build the HTTP call from it alone. */
            ncl_json *paths = ncl_json_obj_get(document, "paths");
            ncl_json *post = ncl_json_obj_get(
                ncl_json_obj_get(paths, "/plcTool/setValue"), "post");
            ncl_json *schema =
                ncl_json_obj_get(ncl_json_obj_get(ncl_json_obj_get(post,
                                                                    "requestBody"),
                                                  "content"),
                                 "application/json");
            ncl_json *params = ncl_json_obj_get(schema, "schema");
            NCL_CHECK_EQ_STR(ncl_json_obj_get_string(
                                 ncl_json_obj_get(ncl_json_obj_get(params, "properties"),
                                                  "value"),
                                 "type"),
                             "integer");
            /* getValue declared none: it falls back to a free form object. */
            post = ncl_json_obj_get(ncl_json_obj_get(paths, "/plcTool/getValue"),
                                    "post");
            NCL_CHECK_EQ_STR(
                ncl_json_obj_get_string(
                    ncl_json_obj_get(
                        ncl_json_obj_get(ncl_json_obj_get(ncl_json_obj_get(
                                             post, "requestBody"),
                                         "content"),
                                         "application/json"),
                        "schema"),
                    "type"),
                "object");
        }
        text = ncl_json_write_string(document);
        NCL_CHECK(text != NULL && strlen(text) > 100);
        NCL_CHECK(strstr(text, "写入是否成功") != NULL); /* the result schema */
        ncl_free_safe(text);
        ncl_json_free(document);
    }

    NCL_TEST_CASE("a base url is used when supplied");
    text = ncl_server_openapi_schema_json(server, "http://127.0.0.1:9008");
    NCL_CHECK(text != NULL);
    NCL_CHECK(strstr(text, "\"url\":\"http://127.0.0.1:9008/api\"") != NULL);
    NCL_CHECK(strstr(text, "/nclinkServer/removeSample") != NULL);
    ncl_free_safe(text);

    ncl_server_free(server);
}

/*
 * 模型的 METHODS 数据项：probe 一次带回整个能力面（有哪些方法、地址怎么拼、
 * 入参/返回 schema、这个方法服务哪些路径），心跳因此不必再背着文档走。
 */
static void test_methods_item_in_the_model(void)
{
    static const char *kModel =
        "{\"name\":\"nclink\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\","
        "\"devices\":[{\"id\":\"02\",\"type\":\"PLC\","
        "\"dataItems\":[{\"id\":\"030001\",\"type\":\"STATUS\"}],"
        "\"version\":\"2.0\"}]}";
    static const ncl_tool_method methods[] = {
        {"getValue", tool_get, NULL, NULL},
        {"setValue", tool_put,
         "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\"}},"
         "\"required\":[\"value\"]}",
         "{\"type\":\"boolean\"}"},
    };
    static const ncl_tool_binding bindings[] = {
        {"/PLC/STATUS", NCL_OP_GET_VALUE, "getValue", "plcTool"},
        {"/PLC/STATUS", NCL_OP_SET_VALUE, "setValue", "plcTool"},
    };
    ncl_server_options options;
    ncl_server *server;
    ncl_node *node;
    const ncl_json *list;
    char *text;
    bool seen_set_value = false;
    size_t i;

    memset(&options, 0, sizeof(options));
    options.sn = TEST_SN;
    options.model_json = kModel;
    server = ncl_server_create(&options);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }

    NCL_TEST_CASE("the model has a METHODS item before any tool registers");
    node = ncl_node_find_by_id(ncl_server_model(server), NCL_METHODS_NODE_ID);
    NCL_CHECK(node != NULL);
    if (node != NULL) {
        NCL_CHECK_EQ_INT(node->type, NCL_NODE_CONFIG);
        NCL_CHECK_EQ_STR(node->node_type_name, NCL_METHODS_NODE_TYPE);
        NCL_CHECK_EQ_STR(ncl_node_path(node), NCL_METHODS_PATH);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(node->value), 0);
    }

    NCL_CHECK_EQ_INT(ncl_server_register_tool(server, "plcTool", server, methods,
                                              sizeof(methods) / sizeof(methods[0]),
                                              bindings,
                                              sizeof(bindings) / sizeof(bindings[0])),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_server_register_builtin_tool(server), NCL_OK);

    NCL_TEST_CASE("registering a tool refreshes the METHODS item");
    node = ncl_node_find_by_id(ncl_server_model(server), NCL_METHODS_NODE_ID);
    NCL_CHECK(node != NULL);
    if (node == NULL) {
        ncl_server_free(server);
        return;
    }
    list = node->value;
    /* getValue, setValue, addSample, removeSample */
    NCL_CHECK_EQ_INT(ncl_json_arr_len(list), 4);
    for (i = 0; i < ncl_json_arr_len(list); i++) {
        const ncl_json *entry = ncl_json_arr_get(list, i);
        const char *method = ncl_json_obj_get_string(entry, "method");
        const char *address = ncl_json_obj_get_string(entry, "address");

        NCL_CHECK(method != NULL);
        NCL_CHECK(address != NULL);
        if (method == NULL || address == NULL) {
            continue;
        }
        if (strcmp(method, "setValue") == 0) {
            const ncl_json *binds;
            const ncl_json *params_props;
            seen_set_value = true;
            NCL_CHECK_EQ_STR(address, "/plcTool/setValue");
            params_props = ncl_json_obj_get(ncl_json_obj_get(entry, "params"),
                                            "properties");
            NCL_CHECK(params_props != NULL);
            NCL_CHECK(ncl_json_obj_get(params_props, "value") != NULL);
            NCL_CHECK_EQ_STR(ncl_json_obj_get_string(
                                 ncl_json_obj_get(entry, "result"), "type"),
                             "boolean");
            /* 这个方法服务哪条路径，也在里面 */
            binds = ncl_json_obj_get(entry, "bindings");
            NCL_CHECK_EQ_INT(ncl_json_arr_len(binds), 1);
            NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_arr_get(binds, 0),
                                                     "operation"),
                             "set_value");
            NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_arr_get(binds, 0),
                                                     "path"),
                             "/PLC/STATUS");
        } else if (strcmp(method, "getValue") == 0) {
            NCL_CHECK_EQ_STR(address, "/plcTool/getValue");
            NCL_CHECK(ncl_json_obj_get(entry, "params") == NULL);
        }
    }
    NCL_CHECK(seen_set_value);

    NCL_TEST_CASE("the model written for a client carries the METHODS item");
    text = ncl_node_write_string(ncl_server_model(server));
    NCL_CHECK(text != NULL);
    if (text != NULL) {
        NCL_CHECK(strstr(text, "\"type\":\"METHODS\"") != NULL);
        NCL_CHECK(strstr(text, "\"/plcTool/setValue\"") != NULL);
        ncl_free_safe(text);
    }
    ncl_server_free(server);
}

static void test_ping_is_a_liveness_answer(void)
{
    ncl_server_options options;
    ncl_server *server;
    ncl_message *ping;
    ncl_message *pong;
    char *text;

    memset(&options, 0, sizeof(options));
    options.sn = TEST_SN;
    server = ncl_server_create(&options);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    ncl_server_register_builtin_tool(server);

    NCL_TEST_CASE("Ping is answered with the status alone");
    ping = ncl_message_new(NCL_MSG_PING);
    ncl_message_set_message_id(ping, "p1");
    pong = ncl_server_dispatch(server, "Ping/" TEST_SN, ping);
    NCL_CHECK(pong != NULL);
    if (pong != NULL) {
        NCL_CHECK_EQ_INT(pong->type, NCL_MSG_PONG);
        NCL_CHECK_EQ_STR(pong->message_id, "p1");
        NCL_CHECK_EQ_STR(ncl_message_code(pong), NCL_KW_CODE_OK);
        text = ncl_message_write_string(pong);
        NCL_CHECK_EQ_STR(text, "{\"@id\":\"p1\",\"code\":\"OK\"}");
        ncl_free_safe(text);
        ncl_message_free(pong);
    }
    ncl_message_free(ping);
    ncl_server_free(server);
}

/*
 * POST /api/<工具>/<方法> 直接调用工具方法，
 * 失败时把 reason 放进失败应答回给调用方。
 */
static void test_http_tool_invocation(void)
{
    ncl_http_server *http;
    ncl_server_options options;
    ncl_server *server;
    unsigned port;
    int status = 0;
    static char body[8192];
    static const char *kModel =
        "{\"name\":\"nclink\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\","
        "\"devices\":[{\"id\":\"02\",\"type\":\"PLC\",\"configs\":[],"
        "\"dataItems\":[{\"id\":\"030001\",\"type\":\"STATUS\"}],"
        "\"version\":\"2.0\"}]}";
    static const ncl_tool_method methods[] = {
        {"getValue", tool_get, NULL},
        {"setValue", tool_put, NULL},
    };

    memset(&options, 0, sizeof(options));
    options.sn = TEST_SN;
    options.model_json = kModel;
    server = ncl_server_create(&options);
    http = ncl_http_server_create(0);
    NCL_CHECK(server != NULL && http != NULL);
    if (server == NULL || http == NULL) {
        return;
    }
    NCL_CHECK_EQ_INT(ncl_server_register_tool(server, "plcTool", server, methods,
                                              sizeof(methods) / sizeof(methods[0]),
                                              NULL, 0),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_server_register_builtin_tool(server), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_rest_attach(http, server), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_http_server_start(http), NCL_OK);
    port = ncl_http_server_port(http);

    NCL_TEST_CASE("POST /api/<工具>/<方法> invokes the tool");
    NCL_CHECK(http_post(port, "/api/plcTool/setValue", "{\"value\":1}", &status,
                        body, sizeof(body)));
    NCL_CHECK_EQ_INT(status, 200);
    NCL_CHECK_EQ_STR(body, "{\"status\":true,\"data\":true}");

    NCL_TEST_CASE("采样通道经 HTTP 添加（形态 ②：模型里没定义，直接给表头）");
    NCL_CHECK(http_post(
        port, "/api/nclinkServer/addSample",
        "{\"request\":{\"id\":\"chHttp\",\"type\":\"SAMPLE_CHANNEL\","
        "\"sampleInterval\":1000,\"uploadInterval\":2000,"
        "\"ids\":[{\"id\":\"/STATUS\"}]}}",
        &status, body, sizeof(body)));
    NCL_CHECK_EQ_INT(status, 200);
    NCL_CHECK_EQ_STR(body, "{\"status\":true,\"data\":true}");
    NCL_CHECK_EQ_INT(ncl_server_sample_count(server), 1);

    NCL_TEST_CASE("添加失败时把原因映射进失败应答");
    NCL_CHECK(http_post(
        port, "/api/nclinkServer/addSample",
        "{\"request\":{\"id\":\"chBad\",\"type\":\"SAMPLE_CHANNEL\","
        "\"sampleInterval\":1000,\"uploadInterval\":2000,"
        "\"ids\":[{\"id\":\"030999\"}]}}",
        &status, body, sizeof(body)));
    NCL_CHECK_EQ_INT(status, 200);
    NCL_CHECK_EQ_STR(body, "{\"status\":false,\"data\":\"NotFoundException\"}");
    NCL_CHECK_EQ_INT(ncl_server_sample_count(server), 1); /* 没被启动 */

    NCL_TEST_CASE("POST /api/nclinkServer/removeSample stops the channel");
    NCL_CHECK(http_post(port, "/api/nclinkServer/removeSample", "{\"id\":\"chHttp\"}",
                        &status, body, sizeof(body)));
    NCL_CHECK_EQ_INT(status, 200);
    NCL_CHECK_EQ_STR(body, "{\"status\":true,\"data\":true}");
    NCL_CHECK_EQ_INT(ncl_server_sample_count(server), 0);

    NCL_TEST_CASE("未注册的工具方法回答 NG 与原因");
    NCL_CHECK(http_post(port, "/api/plcTool/nope", "{}", &status, body,
                        sizeof(body)));
    NCL_CHECK_EQ_INT(status, 200);
    NCL_CHECK(strstr(body, "\"status\":false") != NULL);
    /* invoke_method_call 的文案与 check() 不同，都按协议原文 */
    NCL_CHECK(strstr(body, "未找到方法") != NULL);

    NCL_TEST_CASE("路径段数不对或非 POST 一律 404");
    NCL_CHECK(http_post(port, "/api/plcTool", "{}", &status, body, sizeof(body)));
    NCL_CHECK_EQ_INT(status, 404);
    NCL_CHECK(http_get(port, "/api/plcTool/setValue", &status, body, sizeof(body)));
    NCL_CHECK_EQ_INT(status, 404);

    NCL_TEST_CASE("精确路由优先于 /api/* 前缀路由");
    NCL_CHECK(http_get(port, "/api/schema", &status, body, sizeof(body)));
    NCL_CHECK_EQ_INT(status, 200);
    NCL_CHECK(strstr(body, "\"openapi\":\"3.0.0\"") != NULL);
    NCL_CHECK(http_get(port, "/swagger-ui", &status, body, sizeof(body)));
    NCL_CHECK_EQ_INT(status, 200);

    ncl_http_server_stop(http);
    ncl_http_server_free(http);
    ncl_server_free(server);
}

static void test_schema_endpoints(void)
{
    ncl_http_server *http;
    ncl_server_options options;
    ncl_server *server;
    unsigned port;
    int status = 0;
    static char body[8192];

    memset(&options, 0, sizeof(options));
    options.sn = TEST_SN;
    server = ncl_server_create(&options);
    http = ncl_http_server_create(0);
    NCL_CHECK(server != NULL && http != NULL);
    if (server == NULL || http == NULL) {
        return;
    }
    ncl_server_register_builtin_tool(server);
    NCL_CHECK_EQ_INT(ncl_rest_attach(http, server), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_http_server_start(http), NCL_OK);
    port = ncl_http_server_port(http);

    NCL_TEST_CASE("GET /api/schema returns the OpenAPI document");
    NCL_CHECK(http_get(port, "/api/schema", &status, body, sizeof(body)));
    NCL_CHECK_EQ_INT(status, 200);
    NCL_CHECK(strstr(body, "\"openapi\":\"3.0.0\"") != NULL);
    NCL_CHECK(strstr(body, "\"/nclinkServer/addSample\"") != NULL);

    NCL_TEST_CASE("GET /swagger-ui serves the viewer page");
    NCL_CHECK(http_get(port, "/swagger-ui", &status, body, sizeof(body)));
    NCL_CHECK_EQ_INT(status, 200);
    NCL_CHECK(strstr(body, "<title>NC-Link API</title>") != NULL);
    NCL_CHECK(strstr(body, "/api/schema") != NULL);

    NCL_TEST_CASE("GET /swagger-ui/ (trailing slash) works too");
    NCL_CHECK(http_get(port, "/swagger-ui/", &status, body, sizeof(body)));
    NCL_CHECK_EQ_INT(status, 200);

    NCL_TEST_CASE("unknown API paths still answer 404");
    NCL_CHECK(http_get(port, "/api/does-not-exist", &status, body, sizeof(body)));
    NCL_CHECK_EQ_INT(status, 404);

    ncl_http_server_stop(http);
    ncl_http_server_free(http);
    ncl_server_free(server);
}

NCL_TEST_MAIN_BEGIN()
    test_result_envelope();
    test_openapi_document();
    test_methods_item_in_the_model();
    test_ping_is_a_liveness_answer();
    test_schema_endpoints();
    test_http_tool_invocation();
NCL_TEST_MAIN_END()
