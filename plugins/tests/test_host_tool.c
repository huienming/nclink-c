/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * End to end test of the declaration seam: a one file host
 * (module_tool_basic.c) is built as a module, loaded by protocol name, and its
 * declaration is turned into the model, the bindings and - through real
 * Query/Set requests - into calls to its own functions.
 *
 * No configuration point map takes part in this: that is the whole point.
 */
#include "ncl_test.h"

#include <string.h>

#include "nclink/ncl_message.h"
#include "nclink/ncl_tool.h"
#include "nclink/ncl_audit.h"
#include "nclink/ncl_host.h"
#include "nclink/ncl_module.h"

#ifndef NCL_TEST_PLUGIN_DIR
#  error "NCL_TEST_PLUGIN_DIR must point at the directory holding the modules"
#endif
#ifndef NCL_TEST_MODULE_DIR
#  error "NCL_TEST_MODULE_DIR must point at the directory of the refused fixtures"
#endif
#ifndef NCL_TEST_PSEUDO_DIR
#  error "NCL_TEST_PSEUDO_DIR must point at the directory holding ncl_driver_pseudo"
#endif

/* ------------------------------------------------------- 伪机床用的几个助手 -- */

/** 浮点比较的容差：模拟器里的数都是同一条算式算出来的，差在舍入末尾。 */
#define PSEUDO_EPS 1e-6

/** 一次 Query 的结果：code 与理由拷出来、数据克隆一份（存活的只有这些）。 */
typedef struct {
    bool      ok;
    char      code[8];
    char      message[192];
    ncl_json *data; /**< 调用方释放，可能为 NULL */
} probe_result;

static probe_result probe_query(ncl_server *server, const char *path,
                                const char *operation, const ncl_json *keys)
{
    probe_result out;
    ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    ncl_query_request_item *item = ncl_query_request_item_new(path);
    ncl_message *response;

    memset(&out, 0, sizeof(out));
    (void)ncl_params_set_string(&item->params, "operation", operation);
    if (keys != NULL) {
        (void)ncl_params_set(&item->params, "keys", ncl_json_clone(keys));
    }
    (void)ncl_message_set_message_id(request, "q");
    (void)ncl_message_add_query_request_item(request, item);
    response = ncl_server_invoke_query(server, request);
    ncl_message_free(request);
    if (response != NULL) {
        const ncl_query_response_item *reply =
            (const ncl_query_response_item *)ncl_message_item_at(response, 0);

        if (reply != NULL) {
            out.ok = strcmp(reply->code, NCL_KW_CODE_OK) == 0;
            (void)snprintf(out.code, sizeof(out.code), "%s", reply->code);
            if (reply->reason != NULL) {
                (void)snprintf(out.message, sizeof(out.message), "%s", reply->reason);
            }
            if (ncl_query_response_item_data(reply) != NULL) {
                out.data = ncl_json_clone(ncl_query_response_item_data(reply));
            }
        }
        ncl_message_free(response);
    }
    return out;
}

/** 一次 Set（整数）。回 true 当且仅当 code=OK。 */
static bool probe_set_int(ncl_server *server, const char *path, long long value)
{
    ncl_message *request = ncl_message_new(NCL_MSG_SET_REQUEST);
    ncl_set_request_item *item = ncl_set_request_item_new(path);
    ncl_message *response;
    bool ok = false;

    (void)ncl_params_set_string(&item->params, "operation", "set_value");
    (void)ncl_params_set_int(&item->params, "value", value);
    (void)ncl_message_set_message_id(request, "s");
    (void)ncl_message_add_set_request_item(request, item);
    response = ncl_server_invoke_set(server, request);
    ncl_message_free(request);
    if (response != NULL) {
        const ncl_set_response_item *reply =
            (const ncl_set_response_item *)ncl_message_item_at(response, 0);

        ok = reply != NULL && strcmp(reply->code, NCL_KW_CODE_OK) == 0;
        ncl_message_free(response);
    }
    return ok;
}

/** 一次 Set（keys + JSON 值，值的所有权交出去）。 */
static bool probe_set_json(ncl_server *server, const char *path,
                           const ncl_json *keys, ncl_json *value)
{
    ncl_message *request = ncl_message_new(NCL_MSG_SET_REQUEST);
    ncl_set_request_item *item = ncl_set_request_item_new(path);
    ncl_message *response;
    bool ok = false;

    (void)ncl_params_set_string(&item->params, "operation", "set_value");
    if (keys != NULL) {
        (void)ncl_params_set(&item->params, "keys", ncl_json_clone(keys));
    }
    (void)ncl_params_set(&item->params, "value", value);
    (void)ncl_message_set_message_id(request, "s");
    (void)ncl_message_add_set_request_item(request, item);
    response = ncl_server_invoke_set(server, request);
    ncl_message_free(request);
    if (response != NULL) {
        const ncl_set_response_item *reply =
            (const ncl_set_response_item *)ncl_message_item_at(response, 0);

        ok = reply != NULL && strcmp(reply->code, NCL_KW_CODE_OK) == 0;
        ncl_message_free(response);
    }
    return ok;
}

/** 一次方法调用（params 的所有权交出去）。 */
static probe_result probe_call(ncl_server *server, const char *method,
                               ncl_json *params)
{
    probe_result out;
    ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
    ncl_message *response;

    memset(&out, 0, sizeof(out));
    (void)ncl_message_set_method(request, method);
    (void)ncl_message_set_params(request, params);
    response = ncl_server_invoke_method_call(server, request);
    ncl_message_free(request);
    if (response != NULL) {
        out.ok = response->as.method_call_response.code != NULL &&
                 strcmp(response->as.method_call_response.code, NCL_KW_CODE_OK) == 0;
        (void)snprintf(out.code, sizeof(out.code), "%s",
                       response->as.method_call_response.code != NULL
                           ? response->as.method_call_response.code
                           : "");
        if (response->as.method_call_response.reason != NULL) {
            (void)snprintf(out.message, sizeof(out.message), "%s",
                           response->as.method_call_response.reason);
        }
        if (response->as.method_call_response.data != NULL) {
            out.data = ncl_json_clone(response->as.method_call_response.data);
        }
        ncl_message_free(response);
    }
    return out;
}

/** 声明里那条路径的点位，没有就回 NULL。 */
static const ncl_tool_point *probe_point(const ncl_tool_decl *decl, const char *path)
{
    size_t i;

    for (i = 0; i < decl->point_count; i++) {
        if (strcmp(decl->points[i].path, path) == 0) {
            return &decl->points[i];
        }
    }
    return NULL;
}

/** keys 数组：一个号。 */
static ncl_json *probe_keys(long long number)
{
    ncl_json *keys = ncl_json_new_array();

    (void)ncl_json_arr_push(keys, ncl_json_new_int(number));
    return keys;
}

static void probe_check_int(ncl_server *server, const char *path, long long expected)
{
    probe_result r = probe_query(server, path, "get_value", NULL);
    long long got = 0;

    NCL_CHECK(r.ok);
    NCL_CHECK(r.data != NULL && ncl_json_as_int(r.data, &got));
    NCL_CHECK_EQ_INT(got, expected);
    ncl_json_free(r.data);
}

static void probe_check_double(ncl_server *server, const char *path, double expected)
{
    probe_result r = probe_query(server, path, "get_value", NULL);
    double got = 0.0;
    double diff;

    NCL_CHECK(r.ok);
    NCL_CHECK(r.data != NULL && ncl_json_as_double(r.data, &got));
    diff = got - expected;
    NCL_CHECK(diff < PSEUDO_EPS && diff > -PSEUDO_EPS);
    ncl_json_free(r.data);
}

static void probe_check_str(ncl_server *server, const char *path, const char *expected)
{
    probe_result r = probe_query(server, path, "get_value", NULL);

    NCL_CHECK(r.ok);
    NCL_CHECK_EQ_STR(ncl_json_as_string(r.data), expected);
    ncl_json_free(r.data);
}

static void probe_check_length(ncl_server *server, const char *path, long long expected)
{
    probe_result r = probe_query(server, path, "get_length", NULL);
    long long got = 0;

    NCL_CHECK(r.ok);
    NCL_CHECK(r.data != NULL && ncl_json_as_int(r.data, &got));
    NCL_CHECK_EQ_INT(got, expected);
    ncl_json_free(r.data);
}

static ncl_message *query(const char *path)
{
    ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    ncl_query_request_item *item = ncl_query_request_item_new(path);

    (void)ncl_params_set_string(&item->params, "operation", "get_value");
    (void)ncl_message_set_message_id(request, "q1");
    (void)ncl_message_add_query_request_item(request, item);
    return request;
}

static ncl_message *set_value(const char *path, long long value)
{
    ncl_message *request = ncl_message_new(NCL_MSG_SET_REQUEST);
    ncl_set_request_item *item = ncl_set_request_item_new(path);

    (void)ncl_params_set_string(&item->params, "operation", "set_value");
    (void)ncl_params_set_int(&item->params, "value", value);
    (void)ncl_message_set_message_id(request, "s1");
    (void)ncl_message_add_set_request_item(request, item);
    return request;
}

static long long query_int(ncl_server *server, const char *path, bool *ok)
{
    ncl_message *request = query(path);
    ncl_message *response = ncl_server_invoke_query(server, request);
    long long value = 0;

    *ok = false;
    if (response != NULL) {
        ncl_query_response_item *item =
            (ncl_query_response_item *)ncl_message_item_at(response, 0);

        if (item != NULL && strcmp(item->code, NCL_KW_CODE_OK) == 0 &&
            ncl_json_as_int(ncl_query_response_item_data(item), &value)) {
            *ok = true;
        }
        ncl_message_free(response);
    }
    ncl_message_free(request);
    return value;
}

NCL_TEST_MAIN_BEGIN()
    ncl_module_set *modules = ncl_modules_create();
    ncl_strbuf err;
    const ncl_tool_decl *decl;
    ncl_json *model;
    char *model_json;
    ncl_server_options options;
    ncl_server *server;
    ncl_tool_registration *registration = NULL;

    ncl_audit_options audit_options;

    ncl_audit_options_default(&audit_options); /* §6: on, without the raw bytes */
    ncl_audit_init(&audit_options);
    ncl_strbuf_init(&err);
    NCL_CHECK(modules != NULL);

    NCL_TEST_CASE("a one file host loads as a tool module");
    NCL_CHECK_EQ_INT(ncl_modules_add(modules, "test_tool_basic",
                                     NCL_TEST_PLUGIN_DIR, &err),
                     NCL_OK);
    NCL_CHECK_EQ_INT((int)err.len, 0);
    NCL_CHECK_EQ_INT(ncl_module_count(modules), 1);
    NCL_CHECK_EQ_INT(ncl_module_abi(modules, 0), NCL_TOOL_MODULE_ABI);
    NCL_CHECK_EQ_STR(ncl_module_name(modules, 0), "test_tool_basic");
    NCL_CHECK_EQ_STR(ncl_module_version(modules, 0), "0.1.0");
    NCL_CHECK(ncl_module_description(modules, 0) != NULL);

    NCL_TEST_CASE("its declaration arrives validated, with the points in code");
    decl = ncl_module_tool(modules, 0);
    NCL_CHECK(decl != NULL);
    if (decl == NULL) {
        ncl_modules_free(modules);
        ncl_strbuf_free(&err);
        return ncl_test_failures == 0 ? 0 : 1;
    }
    NCL_CHECK_EQ_STR(decl->name, "test_tool_basic");
    NCL_CHECK_EQ_INT(decl->sample_ms, 500);
    NCL_CHECK_EQ_INT(decl->point_count, 4);
    NCL_CHECK_EQ_STR(decl->points[0].path, "/RUN");
    NCL_CHECK(decl->points[0].sampled);
    NCL_CHECK(ncl_tool_point_handles(&decl->points[1], NCL_OP_SET_VALUE));
    /* 第三个点位是"待抓包"：声明得和别的点位一样（有函数），差别在函数回什么 ——
     * 它回 NCL_ERR_UNAVAILABLE（见 ncl_common.h），所以"还没实现"这件事只写在
     * client 里，点位表上没有任何"待抓包"的形状。 */
    NCL_CHECK(decl->points[2].fn != NULL);
    NCL_CHECK(ncl_tool_point_handles(&decl->points[2], NCL_OP_GET_VALUE));
    NCL_CHECK_EQ_INT(ncl_tool_validate(decl, &err), NCL_OK);


    NCL_TEST_CASE("loading it twice is not an error and does not load two");
    NCL_CHECK_EQ_INT(ncl_modules_add(modules, "test_tool_basic",
                                     NCL_TEST_PLUGIN_DIR, &err),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_module_count(modules), 1);

    NCL_TEST_CASE("a module with a foreign ABI is refused, and says so");
    ncl_strbuf_reset(&err);
    NCL_CHECK(ncl_modules_add(modules, "test_bad_abi", NCL_TEST_MODULE_DIR,
                              &err) != NCL_OK);
    NCL_CHECK(strstr(ncl_strbuf_cstr(&err), "ABI") != NULL);
    NCL_CHECK_EQ_INT(ncl_module_count(modules), 1);

    NCL_TEST_CASE("a file without the entry point is refused");
    ncl_strbuf_reset(&err);
    NCL_CHECK(ncl_modules_add(modules, "test_no_entry", NCL_TEST_MODULE_DIR,
                              &err) != NCL_OK);
    NCL_CHECK_EQ_INT(ncl_module_count(modules), 1);

    NCL_TEST_CASE("the declaration builds the model the device publishes");
    model = ncl_tool_model(decl, NULL, &err);
    NCL_CHECK(model != NULL);
    if (model == NULL) {
        ncl_modules_free(modules);
        ncl_strbuf_free(&err);
        return ncl_test_failures == 0 ? 0 : 1;
    }
    {
        ncl_json *node = ncl_json_arr_get(ncl_json_obj_get(model, "devices"), 0);
        ncl_json *items = ncl_json_obj_get(node, "dataItems");
        ncl_json *channel =
            ncl_json_arr_get(ncl_json_obj_get(node, "configs"), 0);

        NCL_CHECK_EQ_INT(ncl_json_arr_len(items), 3);
        /* name 给人看：字典里查得到的 type 用中文含义，查不到的照原名（RUN 不是
         * 第 4 部分的名字，所以名字就是 RUN）。 */
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(
                             ncl_json_arr_get(items, 0), "name"),
                         "RUN");
        /* 待抓包的点位也在模型里：现场看得见它要来（模型不止是"现在读得到的"）。 */
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(
                             ncl_json_arr_get(items, 2), "name"),
                         "ALARM");
        /* 配置型数据（PARAMETER）不在这里：它在 CONTROLLER 组件的 configs 里。 */
        {
            ncl_json *components = ncl_json_obj_get(node, "components");
            ncl_json *controller = ncl_json_arr_get(components, 0);
            ncl_json *cfgs;
            ncl_json *param;

            NCL_CHECK_EQ_INT(ncl_json_arr_len(components), 1);
            NCL_CHECK_EQ_STR(ncl_json_obj_get_string(controller, "name"),
                             "控制器");
            NCL_CHECK_EQ_STR(ncl_json_obj_get_string(controller, "type"),
                             "CONTROLLER");
            cfgs = ncl_json_obj_get(controller, "configs");
            NCL_CHECK_EQ_INT(ncl_json_arr_len(cfgs), 1);
            param = ncl_json_arr_get(cfgs, 0);
            NCL_CHECK_EQ_STR(ncl_json_obj_get_string(param, "type"), "PARAMETER");
            NCL_CHECK_EQ_STR(ncl_json_obj_get_string(param, "name"), "参数");
        }
        NCL_CHECK(channel != NULL);
        if (channel != NULL) {
            NCL_CHECK_EQ_STR(ncl_json_obj_get_string(channel, "id"),
                             "test_tool_basic");
            NCL_CHECK_EQ_INT(ncl_json_obj_get_int(channel, "sampleInterval", 0),
                             500);
            /* upload was declared 0: the sample period stands in for it. */
            NCL_CHECK_EQ_INT(ncl_json_obj_get_int(channel, "uploadInterval", 0),
                             500);
        }
    }
    model_json = ncl_json_write_string(model);
    ncl_json_free(model);
    NCL_CHECK(model_json != NULL);

    NCL_TEST_CASE("the loaded host serves requests through the server");
    memset(&options, 0, sizeof(options));
    options.sn = "V000000001";
    options.model_json = model_json;
    server = ncl_server_create(&options);
    ncl_free_safe(model_json);
    NCL_CHECK(server != NULL);
    if (server != NULL) {
        bool ok = false;

        NCL_CHECK_EQ_INT(ncl_tool_register(server, decl, NULL, NULL,
                                           &registration, &err),
                         NCL_OK);
        NCL_CHECK(registration != NULL);
        NCL_CHECK_EQ_INT(query_int(server, "/MACHINE/RUN", &ok), 7);
        NCL_CHECK(ok);

        /* Read, write, read back: one point, two operations. */
        {
            ncl_message *request = set_value("/MACHINE/MODE", 5);
            ncl_message *response = ncl_server_invoke_set(server, request);

            NCL_CHECK(response != NULL);
            if (response != NULL) {
                ncl_set_response_item *item =
                    (ncl_set_response_item *)ncl_message_item_at(response, 0);

                NCL_CHECK(item != NULL);
                if (item != NULL) {
                    NCL_CHECK_EQ_STR(item->code, NCL_KW_CODE_OK);
                }
                ncl_message_free(response);
            }
            ncl_message_free(request);
        }
        NCL_CHECK_EQ_INT(query_int(server, "/MACHINE/MODE", &ok), 5);
        NCL_CHECK(ok);

        /* Writing the read only point is refused by the host itself, with
         * its own protocol error. */
        {
            ncl_message *request = set_value("/MACHINE/RUN", 3);
            ncl_message *response = ncl_server_invoke_set(server, request);

            NCL_CHECK(response != NULL);
            if (response != NULL) {
                ncl_set_response_item *item =
                    (ncl_set_response_item *)ncl_message_item_at(response, 0);

                NCL_CHECK(item != NULL);
                if (item != NULL) {
                    NCL_CHECK_EQ_STR(item->code, NCL_KW_CODE_NG);
                }
                ncl_message_free(response);
            }
            ncl_message_free(request);
        }
        ncl_tool_unregister(decl, registration);
        ncl_server_free(server);
    }

    NCL_TEST_CASE("the host builds the device from the loaded module");
    {
        ncl_json *config = ncl_json_parse_cstr(
            "{ \"sn\": \"V000000001\","
            "  \"tools\": [ { \"name\": \"test_tool_basic\","
            "                 \"parameters\": { \"unit\": 3 } } ],"
            "  \"device\": { \"type\": \"MACHINE\", \"id\": \"01\","
            "                \"name\": \"夹具机床\" },"
            "  \"sample\": { \"intervalMs\": 250, \"uploadMs\": 250 } }",
            &err);
        ncl_host *host;

        NCL_CHECK(config != NULL);
        if (config != NULL) {
            host = ncl_host_create_with_modules(config, modules, &err);
            ncl_json_free(config);
            NCL_CHECK(host != NULL);
            if (host != NULL) {
                NCL_CHECK(ncl_host_tool(host) == decl);
                NCL_CHECK_EQ_INT(ncl_host_point_count(host), 4);
                NCL_CHECK_EQ_STR(ncl_host_point_path(host, 0),
                                 "/MACHINE/RUN");
                /* 点位名字从路径推：方法调用地址就是 <工具>/<名字>。 */
                {
                    char name[256];
                    const ncl_tool_decl *d = ncl_host_tool(host);

                    NCL_CHECK_EQ_STR(ncl_tool_point_name(&d->points[0], name,
                                                         sizeof(name)),
                                     "RUN");
                    NCL_CHECK_EQ_STR(ncl_tool_point_name(&d->points[3], name,
                                                         sizeof(name)),
                                     "CONTROLLER.PARAMETER");
                }
                /* 配置型数据（PARAMETER）也在点位表里、也能按路径读，但它在模型的
                 * configs 里（不是 dataItems），因此永远不进采样通道。 */
                NCL_CHECK_EQ_STR(ncl_host_point_path(host, 3),
                                 "/MACHINE/CONTROLLER/PARAMETER");
                {
                    ncl_strbuf note;
                    long long got = 0;
                    const ncl_json *value;

                    ncl_strbuf_init(&note);
                    NCL_CHECK_EQ_INT(
                        ncl_host_poll_one(host,
                                             "/MACHINE/CONTROLLER/PARAMETER",
                                             &note),
                        NCL_OK);
                    ncl_strbuf_free(&note);
                    value = ncl_host_point_value(host, 3);
                    NCL_CHECK(value != NULL && ncl_json_as_int(value, &got));
                    NCL_CHECK_EQ_INT(got, 1234);
                }
                /* 待抓包的点位在列表里（自检要点名它）。读一次：回来的不是失败，
                 * 而是"还读不了"（NCL_ERR_UNAVAILABLE），理由也照实说。 */
                ncl_strbuf_reset(&err);
                NCL_CHECK(!ncl_host_point_unavailable(host, 2));
                NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/ALARM",
                                                      &err),
                                 NCL_ERR_UNAVAILABLE);
                NCL_CHECK(strstr(ncl_strbuf_cstr(&err), "还读不了") != NULL);
                /* 学过一次就记住了：宿主不再把轮询浪费在它身上。 */
                NCL_CHECK(ncl_host_point_unavailable(host, 2));
                {
                    size_t failed = 99;

                    ncl_strbuf_reset(&err);
                    NCL_CHECK_EQ_INT(ncl_host_poll_round(host, &failed,
                                                            &err),
                                     NCL_OK);
                    NCL_CHECK_EQ_INT(failed, 0);
                }
                /* 再来一次，照样是"还读不了"，而且没再问过机床（没走服务器）。 */
                ncl_strbuf_reset(&err);
                NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/ALARM",
                                                      &err),
                                 NCL_ERR_UNAVAILABLE);
                NCL_CHECK(strstr(ncl_strbuf_cstr(&err), "还读不了") != NULL);

                /* The host's own read path (what --once and the poll loop use)
                 * goes through the module's binding. */
                NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/RUN",
                                                      &err),
                                 NCL_OK);
                {
                    const ncl_json *value =
                        ncl_host_point_value(host, 0);
                    long long got = 0;

                    NCL_CHECK(value != NULL);
                    NCL_CHECK(value != NULL && ncl_json_as_int(value, &got));
                    NCL_CHECK_EQ_INT(got, 7);
                }
                NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/NOPE",
                                                      &err),
                                 NCL_ERR_NOT_FOUND);

                /* §6: the host keeps the trail for the declared tool - an
                 * host author never writes audit code. */
                NCL_TEST_CASE("the host's trail records what the tool did");
                ncl_audit_reset_stats();
                {
                    ncl_message *request = query("/MACHINE/RUN");
                    ncl_message *response = ncl_server_invoke_query(
                        ncl_host_server(host), request);

                    ncl_message_free(response);
                    ncl_message_free(request);
                }
                {
                    ncl_message *request = set_value("/MACHINE/MODE", 9);
                    ncl_message *response = ncl_server_invoke_set(
                        ncl_host_server(host), request);

                    ncl_message_free(response);
                    ncl_message_free(request);
                }
                {
                    ncl_json *stats = ncl_audit_stats();

                    NCL_CHECK(stats != NULL);
                    if (stats != NULL) {
                        /* One Query and one Set (the Set's own old value read
                         * is preparation for the trail, not a request of its own -
                         * the same accounting the point map used). */
                        NCL_CHECK_EQ_INT(
                            ncl_json_obj_get_int(stats, "requests", -1), 2);
                        NCL_CHECK_EQ_INT(
                            ncl_json_obj_get_int(stats, "writes", -1), 1);
                        ncl_json_free(stats);
                    }
                }
                ncl_host_free(host);
            }
        }
    }

    /* ==================================================================== *
     * 伪机床（pseudo）：一台不接硬件的机床。
     *
     * 这一节驱动的是 plugins/pseudo.c 编出来的**真适配器**（不是夹具）。它的用处
     * 是"手上没有机床也能把整条链路走通"，所以用例按现场那套来：装载 → 建模 →
     * 取值 → 写回 → 四张表 → 遥控。点位模型照 plugins/syntec.c 摆，下面的断言
     * 就是那份"新代形状"的合同。
     * ==================================================================== */
    {
        ncl_module_set *pseudo_modules = ncl_modules_create();
        const ncl_tool_decl *pseudo_decl;
        ncl_tool_registration *pseudo_registration = NULL;
        ncl_server *pseudo_server = NULL;
        ncl_json *pseudo_params;
        char *pseudo_model_json = NULL;

        NCL_TEST_CASE("the pseudo machine is an adapter module like any other");
        NCL_CHECK(pseudo_modules != NULL);
        ncl_strbuf_reset(&err);
        NCL_CHECK_EQ_INT(
            ncl_modules_add(pseudo_modules, "pseudo", NCL_TEST_PSEUDO_DIR, &err),
            NCL_OK);
        NCL_CHECK_EQ_INT(ncl_module_count(pseudo_modules), 1);
        NCL_CHECK_EQ_INT(ncl_module_abi(pseudo_modules, 0), NCL_TOOL_MODULE_ABI);
        NCL_CHECK_EQ_STR(ncl_module_name(pseudo_modules, 0), "pseudo");
        pseudo_decl = ncl_module_tool(pseudo_modules, 0);
        NCL_CHECK(pseudo_decl != NULL);
        if (pseudo_decl == NULL) {
            ncl_modules_free(pseudo_modules);
            ncl_modules_free(modules);
            ncl_strbuf_free(&err);
            return ncl_test_failures == 0 ? 0 : 1;
        }
        /* 形状照新代摆：设备类型、两个周期、点位条数都对得上。
         * 73 = 9 数据项 + 54 轴点 + 9 配置 + /SESSION 那一个方法（方法也是一行，
         * 只是不进模型、不进采样通道 —— 装载器报的是"72 个点位 + 1 个方法"）。 */
        NCL_CHECK_EQ_STR(pseudo_decl->device_type, "MACHINE");
        NCL_CHECK_EQ_INT(pseudo_decl->sample_ms, 1000);
        NCL_CHECK_EQ_INT(pseudo_decl->upload_ms, 1000);
        NCL_CHECK_EQ_INT(pseudo_decl->point_count, 73);
        NCL_CHECK_EQ_INT(ncl_tool_validate(pseudo_decl, &err), NCL_OK);
        /* 采样通道四样：状态、计件、程序名、报警 —— 与新代同一个口径。 */
        NCL_CHECK(pseudo_decl->points[0].sampled);
        NCL_CHECK_EQ_STR(pseudo_decl->points[0].path, "/STATUS");
        NCL_CHECK(pseudo_decl->points[1].sampled);
        NCL_CHECK_EQ_STR(pseudo_decl->points[1].path, "/PART_COUNT");
        NCL_CHECK_EQ_STR(pseudo_decl->points[2].path, "/CONTROLLER/PROGRAM");
        NCL_CHECK_EQ_STR(pseudo_decl->points[3].path, "/CONTROLLER/WARNING");
        /* 轴：九个字母 × 六格，条条都在；数据项、只读、不进采样通道。 */
        {
            const ncl_tool_point *screw =
                probe_point(pseudo_decl, "/AXIS@X/SCREW/POSITION");

            NCL_CHECK(screw != NULL);
            if (screw != NULL) {
                NCL_CHECK(ncl_tool_point_handles(screw, NCL_OP_GET_VALUE));
                NCL_CHECK(!ncl_tool_point_handles(screw, NCL_OP_SET_VALUE));
                NCL_CHECK(!screw->sampled);
                NCL_CHECK(!screw->config);
            }
            NCL_CHECK(probe_point(pseudo_decl, "/AXIS@Z/SERVO_DRIVER/POSITION") != NULL);
            NCL_CHECK(probe_point(pseudo_decl, "/AXIS@W/MOTOR/VARIABLE@DISTANCE") != NULL);
        }
        /* 寄存器六族：R/I/C/S 可写、O/A 只读（新代的权限口径，权限本身在外面控）。 */
        {
            const ncl_tool_point *reg_r = probe_point(pseudo_decl, "/CONTROLLER/REGISTER@R");
            const ncl_tool_point *reg_o = probe_point(pseudo_decl, "/CONTROLLER/REGISTER@O");

            NCL_CHECK(reg_r != NULL && reg_o != NULL);
            if (reg_r != NULL && reg_o != NULL) {
                NCL_CHECK(reg_r->config);
                NCL_CHECK(ncl_tool_point_handles(reg_r, NCL_OP_GET_LENGTH));
                NCL_CHECK(ncl_tool_point_handles(reg_r, NCL_OP_GET_ATTRIBUTES));
                NCL_CHECK(ncl_tool_point_handles(reg_r, NCL_OP_SET_VALUE));
                NCL_CHECK(!ncl_tool_point_handles(reg_o, NCL_OP_SET_VALUE));
            }
        }
        /* /SESSION 是方法：不进模型、不进采样通道，调用地址是 <工具>/<点位名>。 */
        {
            const ncl_tool_point *session = probe_point(pseudo_decl, "/SESSION");
            char name[64];

            NCL_CHECK(session != NULL);
            if (session != NULL) {
                NCL_CHECK(ncl_tool_point_is_method(session));
                NCL_CHECK_EQ_STR(ncl_tool_point_name(session, name, sizeof(name)),
                                 "SESSION");
            }
        }

        NCL_TEST_CASE("the simulator answers from its own clock (frozen for the test)");
        pseudo_params = ncl_json_parse_cstr(
            "{ \"frozen\": true, \"axes\": \"XYZ\", \"seed\": 0, \"partCount\": 7,"
            "  \"programs\": [\"O1000\"], \"feedRate\": 800, \"spindleRpm\": 1200 }",
            &err);
        NCL_CHECK(pseudo_params != NULL);
        {
            ncl_json *pseudo_model = ncl_tool_model(pseudo_decl, NULL, &err);

            NCL_CHECK(pseudo_model != NULL);
            pseudo_model_json =
                pseudo_model != NULL ? ncl_json_write_string(pseudo_model) : NULL;
            ncl_json_free(pseudo_model);
        }
        NCL_CHECK(pseudo_model_json != NULL);
        memset(&options, 0, sizeof(options));
        options.sn = "V000000001";
        options.model_json = pseudo_model_json;
        pseudo_server = ncl_server_create(&options);
        NCL_CHECK(pseudo_server != NULL);
        if (pseudo_server != NULL && pseudo_params != NULL) {
            NCL_CHECK_EQ_INT(ncl_tool_register(pseudo_server, pseudo_decl, pseudo_params,
                                               NULL, &pseudo_registration, &err),
                             NCL_OK);
            NCL_CHECK(pseudo_registration != NULL);
            /* 状态：frozen 把时钟钉在循环中段 —— 跑着的机床（新代的字面量是 running）。 */
            probe_check_str(pseudo_server, "/MACHINE/STATUS", "running");
            /* 计件：起始 7 + 走完 4 个循环（frozen 的口径，事后可复算）。 */
            probe_check_int(pseudo_server, "/MACHINE/PART_COUNT", 11);
            probe_check_str(pseudo_server, "/MACHINE/CONTROLLER/PROGRAM", "O1000");
            probe_check_str(pseudo_server, "/MACHINE/CONTROLLER/LINE_NUMBER", "N2010");
            probe_check_int(pseudo_server, "/MACHINE/FEED_OVERRIDE", 100);
            probe_check_int(pseudo_server, "/MACHINE/SPINDLE_OVERRIDE", 100);
            probe_check_double(pseudo_server, "/MACHINE/FEED_SPEED", 800.0);
            probe_check_double(pseudo_server, "/MACHINE/SPINDLE_SPEED", 1200.0);
            /* 轴：X 在行程顶点、Y 差一截；指令位置是稍靠后的那一点。 */
            probe_check_double(pseudo_server, "/MACHINE/AXIS@X/SCREW/POSITION", 50.0);
            probe_check_double(pseudo_server, "/MACHINE/AXIS@X/MOTOR/POSITION", 50.0);
            probe_check_double(pseudo_server, "/MACHINE/AXIS@Y/SCREW/POSITION", 39.2);
            probe_check_double(pseudo_server, "/MACHINE/AXIS@X/SERVO_DRIVER/POSITION", 46.0);
            /* 绝对 = 机械坐标 + 工件零点偏置（X 是 -100）；相对/剩余按程序段算。 */
            probe_check_double(pseudo_server, "/MACHINE/AXIS@X/MOTOR/VARIABLE@ABSOLUTE",
                               -50.0);
            probe_check_double(pseudo_server, "/MACHINE/AXIS@X/MOTOR/VARIABLE@RELATIVE",
                               0.0);
            probe_check_double(pseudo_server, "/MACHINE/AXIS@X/MOTOR/VARIABLE@DISTANCE",
                               -50.0);
            /* 没配的轴照新代的规矩回 NotFound：不给数、也不去读别的轴。 */
            {
                probe_result r = probe_query(pseudo_server,
                                             "/MACHINE/AXIS@B/SCREW/POSITION",
                                             "get_value", NULL);

                NCL_CHECK(!r.ok);
                NCL_CHECK_EQ_STR(r.code, NCL_KW_CODE_NG);
                NCL_CHECK(strstr(r.message, "没有 B 轴") != NULL);
                ncl_json_free(r.data);
            }
            /* 没有报警就是**空表**（不是 null）—— 与新代"没有报警正文是空的"一致。 */
            {
                probe_result r = probe_query(pseudo_server,
                                             "/MACHINE/CONTROLLER/WARNING",
                                             "get_value", NULL);

                NCL_CHECK(r.ok);
                NCL_CHECK(r.data != NULL && ncl_json_arr_len(r.data) == 0);
                ncl_json_free(r.data);
            }

            NCL_TEST_CASE("what a client writes lands in the simulator and reads back");
            NCL_CHECK(probe_set_int(pseudo_server, "/MACHINE/FEED_OVERRIDE", 50));
            probe_check_int(pseudo_server, "/MACHINE/FEED_OVERRIDE", 50);
            /* 倍率拨到 50%：进给速度跟着变（800 × 50%），不用重启、不用改配置。 */
            probe_check_double(pseudo_server, "/MACHINE/FEED_SPEED", 400.0);
            /* 只读的点位写不动：主机自己就挡了（点位没声明 set_value）。 */
            NCL_CHECK(!probe_set_int(pseudo_server, "/MACHINE/STATUS", 1));
            /* 超范围的值被适配器拒掉（倍率就是 0..200）。 */
            NCL_CHECK(!probe_set_int(pseudo_server, "/MACHINE/FEED_OVERRIDE", 500));

            NCL_TEST_CASE("the tables answer the shapes the Syntec adapter answers");
            { /* 系统参数：get_keys / get_value / get_attributes / set_value */
                ncl_json *keys = probe_keys(1001);
                probe_result r = probe_query(pseudo_server,
                                             "/MACHINE/CONTROLLER/PARAMETER",
                                             "get_value", keys);

                NCL_CHECK(r.ok);
                NCL_CHECK(r.data != NULL &&
                          ncl_json_obj_get_int(r.data, "1001", -1) == 800000);
                ncl_json_free(r.data);
                /* 写：{"keys":1001,"value":1234} —— 读回来就是新值。 */
                NCL_CHECK(probe_set_json(pseudo_server, "/MACHINE/CONTROLLER/PARAMETER",
                                         keys, ncl_json_new_int(1234)));
                r = probe_query(pseudo_server, "/MACHINE/CONTROLLER/PARAMETER",
                                "get_value", keys);
                NCL_CHECK(r.ok);
                NCL_CHECK(r.data != NULL &&
                          ncl_json_obj_get_int(r.data, "1001", -1) == 1234);
                ncl_json_free(r.data);
                ncl_json_free(keys);
                /* 不在表里的号：NotFound，不猜一个值出来。 */
                keys = probe_keys(9999);
                r = probe_query(pseudo_server, "/MACHINE/CONTROLLER/PARAMETER",
                                "get_value", keys);
                NCL_CHECK(!r.ok);
                ncl_json_free(keys);
                ncl_json_free(r.data);
            }
            { /* 刀具表：get_length / 按刀号读 / 只盖给到的字段 */
                ncl_json *keys = probe_keys(1);
                probe_result r;

                probe_check_length(pseudo_server, "/MACHINE/CONTROLLER/TOOL", 8);
                r = probe_query(pseudo_server, "/MACHINE/CONTROLLER/TOOL", "get_value",
                                keys);
                NCL_CHECK(r.ok);
                {
                    const ncl_json *tool = ncl_json_obj_get(r.data, "1");

                    NCL_CHECK(tool != NULL);
                    NCL_CHECK(ncl_json_obj_get_int(tool, "kind", -1) == 3);
                    NCL_CHECK(ncl_json_obj_get_double(tool, "length", 0.0) > 99.0);
                    NCL_CHECK_EQ_INT(
                        (int)ncl_json_arr_len(ncl_json_obj_get(tool, "length_geometry")),
                        12);
                }
                ncl_json_free(r.data);
                {
                    ncl_json *patch = ncl_json_new_object();

                    (void)ncl_json_obj_set_double(patch, "radius", 1.25);
                    NCL_CHECK(probe_set_json(pseudo_server, "/MACHINE/CONTROLLER/TOOL",
                                             keys, patch));
                }
                r = probe_query(pseudo_server, "/MACHINE/CONTROLLER/TOOL", "get_value",
                                keys);
                NCL_CHECK(r.ok);
                {
                    const ncl_json *tool = ncl_json_obj_get(r.data, "1");

                    NCL_CHECK(tool != NULL);
                    NCL_CHECK(ncl_json_obj_get_double(tool, "radius", 0.0) > 1.24);
                    /* 只写了半径：长度不许被抹掉（写整条、读回来还是整条）。 */
                    NCL_CHECK(ncl_json_obj_get_double(tool, "length", 0.0) > 99.0);
                }
                ncl_json_free(r.data);
                /* 不认识的字段当场被拒（写成 radiuswear 不该悄悄什么都没写）。 */
                {
                    ncl_json *bad = ncl_json_new_object();

                    (void)ncl_json_obj_set_double(bad, "radiuswear", 1.0);
                    NCL_CHECK(!probe_set_json(pseudo_server, "/MACHINE/CONTROLLER/TOOL",
                                              keys, bad));
                }
                ncl_json_free(keys);
            }
            { /* PLC 寄存器：一族一条，R 是 u32、位是 true/false */
                ncl_json *keys = probe_keys(1);
                probe_result r;

                probe_check_length(pseudo_server, "/MACHINE/CONTROLLER/REGISTER@R", 64);
                r = probe_query(pseudo_server, "/MACHINE/CONTROLLER/REGISTER@R",
                                "get_value", keys);
                NCL_CHECK(r.ok);
                NCL_CHECK(r.data != NULL && ncl_json_obj_get_int(r.data, "1", -1) == 1234);
                ncl_json_free(r.data);
                NCL_CHECK(probe_set_json(pseudo_server, "/MACHINE/CONTROLLER/REGISTER@R",
                                         keys, ncl_json_new_int(7)));
                r = probe_query(pseudo_server, "/MACHINE/CONTROLLER/REGISTER@R",
                                "get_value", keys);
                NCL_CHECK(r.ok);
                NCL_CHECK(r.data != NULL && ncl_json_obj_get_int(r.data, "1", -1) == 7);
                ncl_json_free(r.data);
                ncl_json_free(keys);
                /* O 位只读：声明里就没有 set_value，写进来是 NG。 */
                keys = probe_keys(0);
                NCL_CHECK(!probe_set_json(pseudo_server, "/MACHINE/CONTROLLER/REGISTER@O",
                                          keys, ncl_json_new_bool(true)));
                ncl_json_free(keys);
            }
            { /* 变量表：#号索引，空号照实说"空"（type 0） */
                ncl_json *keys = probe_keys(2);
                probe_result r = probe_query(pseudo_server, "/MACHINE/CONTROLLER/VARIABLE",
                                             "get_value", keys);

                NCL_CHECK(r.ok);
                {
                    const ncl_json *var = ncl_json_obj_get(r.data, "2");

                    NCL_CHECK(var != NULL);
                    NCL_CHECK(ncl_json_obj_get_int(var, "id", -1) == 2);
                    NCL_CHECK(ncl_json_obj_get_int(var, "type", -1) == 2);
                    NCL_CHECK(ncl_json_obj_get_double(var, "value", 0.0) > 2.9);
                }
                ncl_json_free(r.data);
                /* 写浮点 → type 2；写整数 → type 1（程序里 #1=2.5 与 #1=2 分得开）。 */
                NCL_CHECK(probe_set_json(pseudo_server, "/MACHINE/CONTROLLER/VARIABLE",
                                         keys, ncl_json_new_double(2.5)));
                r = probe_query(pseudo_server, "/MACHINE/CONTROLLER/VARIABLE", "get_value",
                                keys);
                NCL_CHECK(r.ok);
                NCL_CHECK(ncl_json_obj_get_int(ncl_json_obj_get(r.data, "2"), "type",
                                               -1) == 2);
                ncl_json_free(r.data);
                NCL_CHECK(probe_set_json(pseudo_server, "/MACHINE/CONTROLLER/VARIABLE",
                                         keys, ncl_json_new_int(4)));
                r = probe_query(pseudo_server, "/MACHINE/CONTROLLER/VARIABLE", "get_value",
                                keys);
                NCL_CHECK(r.ok);
                NCL_CHECK(ncl_json_obj_get_int(ncl_json_obj_get(r.data, "2"), "type",
                                               -1) == 1);
                NCL_CHECK(ncl_json_obj_get_int(ncl_json_obj_get(r.data, "2"), "value",
                                               -1) == 4);
                ncl_json_free(r.data);
                ncl_json_free(keys);
                keys = probe_keys(20);
                r = probe_query(pseudo_server, "/MACHINE/CONTROLLER/VARIABLE", "get_value",
                                keys);
                NCL_CHECK(r.ok);
                NCL_CHECK(ncl_json_obj_get_int(ncl_json_obj_get(r.data, "20"), "type",
                                               -1) == 0);
                NCL_CHECK(!ncl_json_obj_has(ncl_json_obj_get(r.data, "20"), "value"));
                ncl_json_free(keys);
                ncl_json_free(r.data);
            }

            NCL_TEST_CASE("the session method is the remote control for the fake machine");
            {
                probe_result r = probe_call(pseudo_server, "pseudo/SESSION", NULL);

                NCL_CHECK(r.ok);
                NCL_CHECK_EQ_STR(ncl_json_obj_get_string(r.data, "status"), "running");
                NCL_CHECK(ncl_json_obj_get_int(r.data, "reads", -1) > 0);
                ncl_json_free(r.data);
            }
            /* 拨到 holding：/STATUS 立刻跟着变（不用重启、不用改配置）。 */
            {
                probe_result r = probe_call(
                    pseudo_server, "pseudo/SESSION",
                    ncl_json_parse_cstr("{\"status\":\"holding\"}", &err));

                NCL_CHECK(r.ok);
                ncl_json_free(r.data);
            }
            probe_check_str(pseudo_server, "/MACHINE/STATUS", "holding");
            /* 注入报警：WARNING 变成一条 {"number","text"}。 */
            {
                probe_result r = probe_call(
                    pseudo_server, "pseudo/SESSION",
                    ncl_json_parse_cstr(
                        "{\"alarm\":{\"number\":1201,\"text\":\"注入的报警\"}}", &err));

                NCL_CHECK(r.ok);
                ncl_json_free(r.data);
            }
            {
                probe_result r = probe_query(pseudo_server,
                                             "/MACHINE/CONTROLLER/WARNING",
                                             "get_value", NULL);

                NCL_CHECK(r.ok);
                NCL_CHECK(r.data != NULL && ncl_json_arr_len(r.data) == 1);
                NCL_CHECK(ncl_json_obj_get_int(ncl_json_arr_get(r.data, 0), "number",
                                               -1) == 1201);
                ncl_json_free(r.data);
            }
            /* 清掉报警：又回到空表。 */
            {
                probe_result r = probe_call(pseudo_server, "pseudo/SESSION",
                                            ncl_json_parse_cstr("{\"alarm\":null}", &err));

                NCL_CHECK(r.ok);
                ncl_json_free(r.data);
            }
            {
                probe_result r = probe_query(pseudo_server,
                                             "/MACHINE/CONTROLLER/WARNING",
                                             "get_value", NULL);

                NCL_CHECK(r.ok);
                NCL_CHECK(r.data != NULL && ncl_json_arr_len(r.data) == 0);
                ncl_json_free(r.data);
            }
            /* 注入一次失败：下一次取值 NG 且理由带配置那句话，再下一次就好了。 */
            {
                probe_result r = probe_call(
                    pseudo_server, "pseudo/SESSION",
                    ncl_json_parse_cstr(
                        "{\"fail\":{\"code\":-5,\"count\":1,\"message\":\"注入的超时（伪）\"}}",
                        &err));

                NCL_CHECK(r.ok);
                ncl_json_free(r.data);
            }
            {
                probe_result r = probe_query(pseudo_server, "/MACHINE/STATUS", "get_value",
                                             NULL);

                NCL_CHECK(!r.ok);
                NCL_CHECK_EQ_STR(r.code, NCL_KW_CODE_NG);
                NCL_CHECK(strstr(r.message, "注入的超时") != NULL);
                ncl_json_free(r.data);
            }
            probe_check_str(pseudo_server, "/MACHINE/STATUS", "holding");
        }

        NCL_TEST_CASE("a muted path answers 还读不了, like an uncaptured frame");
        {
            /* 第二台（同一个声明、另一份参数）：把行号那条标成"还读不了"。 */
            ncl_json *muted_params = ncl_json_parse_cstr(
                "{ \"frozen\": true, \"axes\": \"XYZ\","
                "  \"unavailable\": [\"/CONTROLLER/LINE_NUMBER\"] }",
                &err);
            ncl_tool_registration *muted_registration = NULL;
            ncl_server *muted_server;

            NCL_CHECK(muted_params != NULL);
            muted_server = ncl_server_create(&options);
            NCL_CHECK(muted_server != NULL);
            if (muted_server != NULL && muted_params != NULL) {
                NCL_CHECK_EQ_INT(ncl_tool_register(muted_server, pseudo_decl,
                                                   muted_params, NULL,
                                                   &muted_registration, &err),
                                 NCL_OK);
                {
                    probe_result r = probe_query(muted_server,
                                                 "/MACHINE/CONTROLLER/LINE_NUMBER",
                                                 "get_value", NULL);

                    NCL_CHECK(!r.ok);
                    NCL_CHECK(strstr(r.message, "还读不了") != NULL);
                    ncl_json_free(r.data);
                }
                /* 只掐这一条：同一台机器的别的点位照常好使。 */
                probe_check_str(muted_server, "/MACHINE/STATUS", "running");
                ncl_tool_unregister(pseudo_decl, muted_registration);
                ncl_server_free(muted_server);
            }
            ncl_json_free(muted_params);
        }

        ncl_tool_unregister(pseudo_decl, pseudo_registration);
        if (pseudo_server != NULL) {
            ncl_server_free(pseudo_server);
        }
        ncl_json_free(pseudo_params);
        ncl_free_safe(pseudo_model_json);
        ncl_modules_free(pseudo_modules);
    }

    ncl_modules_free(modules);
    ncl_strbuf_free(&err);
NCL_TEST_MAIN_END()
