/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * End to end test of the declaration seam: a one file adapter
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
#include "nclink_adapter/ncl_audit.h"
#include "nclink_adapter/ncl_adapter.h"
#include "nclink_adapter/ncl_module.h"

#ifndef NCL_TEST_PLUGIN_DIR
#  error "NCL_TEST_PLUGIN_DIR must point at the directory holding the modules"
#endif
#ifndef NCL_TEST_MODULE_DIR
#  error "NCL_TEST_MODULE_DIR must point at the directory of the refused fixtures"
#endif

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

    NCL_TEST_CASE("a one file adapter loads as a tool module");
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
    NCL_CHECK_EQ_INT(decl->point_count, 3);
    NCL_CHECK_EQ_STR(decl->points[0].path, "/MACHINE/RUN");
    NCL_CHECK(decl->points[0].sampled);
    NCL_CHECK(decl->points[1].writable);
    /* 第三个点位是"待抓包"：声明了、可查，但没有函数。 */
    NCL_CHECK(!decl->points[2].available);
    NCL_CHECK(decl->points[2].fn == NULL);
    NCL_CHECK_EQ_STR(decl->points[2].summary, "报警：待抓包（帧还没抓到）");
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
        /* 待抓包的点位也在模型里，理由是它的 description。 */
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(
                             ncl_json_arr_get(items, 2), "name"),
                         "ALARM");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(
                             ncl_json_arr_get(items, 2), "description"),
                         "报警：待抓包（帧还没抓到）");
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

    NCL_TEST_CASE("the loaded adapter serves requests through the server");
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

        /* Writing the read only point is refused by the adapter itself, with
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

    NCL_TEST_CASE("the adapter builds the device from the loaded module");
    {
        ncl_json *config = ncl_json_parse_cstr(
            "{ \"sn\": \"V000000001\","
            "  \"tools\": [ { \"name\": \"test_tool_basic\","
            "                 \"parameters\": { \"unit\": 3 } } ],"
            "  \"device\": { \"type\": \"MACHINE\", \"id\": \"01\","
            "                \"name\": \"夹具机床\" },"
            "  \"sample\": { \"intervalMs\": 250, \"uploadMs\": 250 } }",
            &err);
        ncl_adapter *adapter;

        NCL_CHECK(config != NULL);
        if (config != NULL) {
            adapter = ncl_adapter_create_with_modules(config, modules, &err);
            ncl_json_free(config);
            NCL_CHECK(adapter != NULL);
            if (adapter != NULL) {
                NCL_CHECK(ncl_adapter_tool(adapter) == decl);
                NCL_CHECK_EQ_INT(ncl_adapter_point_count(adapter), 3);
                NCL_CHECK_EQ_STR(ncl_adapter_point_path(adapter, 0),
                                 "/MACHINE/RUN");
                /* 待抓包的点位在列表里（自检要点名它），但读取直接说清楚，
                 * 不走服务器、也不进轮询失败数。 */
                NCL_CHECK(!ncl_adapter_point_available(adapter, 2));
                NCL_CHECK_EQ_STR(ncl_adapter_point_summary(adapter, 2),
                                 "报警：待抓包（帧还没抓到）");
                ncl_strbuf_reset(&err);
                NCL_CHECK_EQ_INT(ncl_adapter_poll_one(adapter, "/MACHINE/ALARM",
                                                      &err),
                                 NCL_ERR_NOT_SUPPORTED);
                NCL_CHECK(strstr(ncl_strbuf_cstr(&err), "待抓包") != NULL);
                {
                    size_t failed = 99;

                    ncl_strbuf_reset(&err);
                    NCL_CHECK_EQ_INT(ncl_adapter_poll_round(adapter, &failed,
                                                            &err),
                                     NCL_OK);
                    NCL_CHECK_EQ_INT(failed, 0);
                }

                /* The host's own read path (what --once and the poll loop use)
                 * goes through the module's binding. */
                NCL_CHECK_EQ_INT(ncl_adapter_poll_one(adapter, "/MACHINE/RUN",
                                                      &err),
                                 NCL_OK);
                {
                    const ncl_json *value =
                        ncl_adapter_point_value(adapter, 0);
                    long long got = 0;

                    NCL_CHECK(value != NULL);
                    NCL_CHECK(value != NULL && ncl_json_as_int(value, &got));
                    NCL_CHECK_EQ_INT(got, 7);
                }
                NCL_CHECK_EQ_INT(ncl_adapter_poll_one(adapter, "/MACHINE/NOPE",
                                                      &err),
                                 NCL_ERR_NOT_FOUND);

                /* §6: the host keeps the trail for the declared tool - an
                 * adapter author never writes audit code. */
                NCL_TEST_CASE("the host's trail records what the tool did");
                ncl_audit_reset_stats();
                {
                    ncl_message *request = query("/MACHINE/RUN");
                    ncl_message *response = ncl_server_invoke_query(
                        ncl_adapter_server(adapter), request);

                    ncl_message_free(response);
                    ncl_message_free(request);
                }
                {
                    ncl_message *request = set_value("/MACHINE/MODE", 9);
                    ncl_message *response = ncl_server_invoke_set(
                        ncl_adapter_server(adapter), request);

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
                ncl_adapter_free(adapter);
            }
        }
    }

    ncl_modules_free(modules);
    ncl_strbuf_free(&err);
NCL_TEST_MAIN_END()
