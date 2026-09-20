/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * The assembled device program: one NC-Link server, whose vendor adapter is
 * loaded at run time and whose point map comes from the configuration.
 *
 *   plugins/ncl_driver_focas.dll  --load-->  driver registry
 *   conf/fanuc.json               --read-->  link + points + model + sampling
 *   ncl_server                    <-- one device, answering on MQTT/REST
 *
 * The module is the real one built by the adapter CMakeLists (NCL_TEST_PLUGIN
 * _DIR); the machine behind it is a mock, so the whole chain - module, factory,
 * configuration, point map, FOCAS blocks, model value - runs without a CNC.
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink_adapter/ncl_adapter.h"
#include "nclink/clients/focas.h"
#include "nclink_adapter/ncl_module.h"

/* ---------------------------------------------------------------- helpers -- */

static void put_u16be(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void put_u32be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint32_t get_u16be(const uint8_t *in)
{
    return (uint32_t)(((uint16_t)in[0] << 8) | in[1]);
}

static void put_float_be(uint8_t *out, float number)
{
    uint32_t bits = 0;

    memcpy(&bits, &number, sizeof(bits));
    put_u32be(out, bits);
}

/* ------------------------------------------------------- the mock machine -- */

#define MOCK_ALARM 1
#define MOCK_RUN 3
#define MOCK_COUNT 4242
#define MOCK_PROGRAM "O1234"

typedef struct {
    ncl_socket *listener;
    unsigned    port;
    ncl_thread *thread;
    bool        stop;
} fanuc_mock;

static size_t add_block(uint8_t *out, size_t used, const uint8_t *payload,
                        size_t payload_len)
{
    size_t size = 16u + payload_len;

    memset(out + used, 0, size);
    put_u16be(out + used, (uint16_t)size);
    put_u16be(out + used + 8, 0);
    put_u16be(out + used + 14, (uint16_t)payload_len);
    if (payload_len > 0) {
        memcpy(out + used + 16, payload, payload_len);
    }
    return used + size;
}

/** Reply body for one request: blocks per the request's first command code. */
static size_t mock_body(uint8_t *out, size_t cap, const uint8_t *request,
                        size_t request_len, size_t blocks)
{
    uint8_t payload[64];
    size_t payload_len = 4;
    size_t used;
    size_t i;

    memset(payload, 0, sizeof(payload));
    if (request_len >= 2u + 8u) {
        uint16_t code = (uint16_t)get_u16be(request + 2u + 6u);

        switch (code) {
        case 25: /* cnc_statinfo: block 0 is ODST's manual..oper */
            payload_len = 18;
            put_u16be(payload + 2, MOCK_RUN);
            put_u16be(payload + 12, MOCK_ALARM);
            break;
        case 36: /* cnc_actf */
            payload_len = 12;
            put_float_be(payload + 0, 1.5f);
            put_float_be(payload + 4, -2.25f);
            put_float_be(payload + 8, 3.75f);
            break;
        case 37: /* cnc_acts */
            payload_len = 12;
            put_float_be(payload + 0, 100.0f);
            put_float_be(payload + 4, 200.0f);
            put_float_be(payload + 8, 300.0f);
            break;
        case 139: /* cnc_rdcount */
            payload_len = 4;
            put_u32be(payload, MOCK_COUNT);
            break;
        case 252: /* cnc_exeprgname2 */
            payload_len = 44;
            memcpy(payload, MOCK_PROGRAM, sizeof(MOCK_PROGRAM) - 1u);
            break;
        default: /* the handshake probes */
            payload_len = 4;
            break;
        }
    }
    if (blocks == 0) {
        blocks = 1; /* §2.2 rule 6 */
    }
    used = 2u;
    put_u16be(out, (uint16_t)blocks);
    for (i = 0; i < blocks; i++) {
        size_t len = i == 0u ? payload_len : 2u;

        if (used + 16u + len > cap) {
            return 0;
        }
        used = add_block(out, used, payload, len);
    }
    return used;
}

static void mock_main(void *arg)
{
    fanuc_mock *mock = (fanuc_mock *)arg;

    while (!mock->stop) {
        ncl_socket *peer = ncl_socket_accept(mock->listener, 100);

        if (peer == NULL) {
            continue;
        }
        for (;;) {
            uint8_t header[NCL_FOCAS_HEADER];
            uint8_t frame[2048];
            uint8_t body[1024];
            uint8_t reply[2048];
            ncl_focas_pdu pdu;
            size_t total = 0;
            size_t body_len = 0;
            size_t frame_len;
            size_t blocks = 0;

            if (ncl_socket_recv_exact(peer, header, sizeof(header), 2000) !=
                NCL_OK) {
                break;
            }
            if (ncl_focas_split(header, sizeof(header), &pdu, &total) !=
                NCL_ERR_RANGE) {
                break;
            }
            if (total > sizeof(frame)) {
                break;
            }
            memcpy(frame, header, sizeof(header));
            if (pdu.length > 0 &&
                ncl_socket_recv_exact(peer, frame + sizeof(header), pdu.length,
                                      2000) != NCL_OK) {
                break;
            }
            if (pdu.func == NCL_FOCAS_FUNC_HELLO) {
                memset(body, 0, 16); /* field 2 = 0: the §2.3 else branch */
                body_len = 16;
            } else if (pdu.func == NCL_FOCAS_FUNC_CMD) {
                if (pdu.length >= 2u) {
                    blocks = get_u16be(frame + sizeof(header));
                }
                body_len = mock_body(body, sizeof(body), frame + sizeof(header),
                                     pdu.length, blocks);
            } else {
                break;
            }
            if (body_len == 0) {
                break;
            }
            frame_len = ncl_focas_build(reply, sizeof(reply), pdu.func,
                                        NCL_FOCAS_DIR_RESP, body, body_len);
            if (frame_len == 0 ||
                ncl_socket_send(peer, reply, frame_len) != NCL_OK) {
                break;
            }
        }
        ncl_socket_close(peer);
    }
}

static fanuc_mock *mock_start(void)
{
    fanuc_mock *mock = (fanuc_mock *)ncl_mem_calloc(1, sizeof(*mock));

    if (mock == NULL) {
        return NULL;
    }
    mock->listener = ncl_socket_listen(0, NULL, 0);
    if (mock->listener == NULL) {
        ncl_free_safe(mock);
        return NULL;
    }
    mock->port = ncl_socket_local_port(mock->listener);
    mock->thread = ncl_thread_start(mock_main, mock);
    if (mock->thread == NULL) {
        ncl_socket_close(mock->listener);
        ncl_free_safe(mock);
        return NULL;
    }
    return mock;
}

static void mock_stop(fanuc_mock *mock)
{
    if (mock == NULL) {
        return;
    }
    mock->stop = true;
    ncl_thread_join(mock->thread);
    ncl_socket_close(mock->listener);
    ncl_free_safe(mock);
}

/* ----------------------------------------------------------- the assembly -- */

/** Loaded once: the driver registry is global, so a second set would only
 *  report "already registered". */
static ncl_module_set *g_modules = NULL;

static void test_module_loading(void)
{
    ncl_strbuf err;
    ncl_driver *driver;

    ncl_strbuf_init(&err);

    NCL_TEST_CASE("the FOCAS module in the plugin directory loads");
    g_modules = ncl_modules_create();
    NCL_CHECK(g_modules != NULL);
    NCL_CHECK_EQ_INT(ncl_modules_add(g_modules, "focas", NCL_TEST_PLUGIN_DIR,
                                     &err),
                     NCL_OK);
    if (ncl_module_count(g_modules) == 0) {
        printf("      loader said: %s\n", ncl_strbuf_cstr(&err));
    }
    NCL_CHECK_EQ_INT(ncl_module_count(g_modules), 1u);
    NCL_CHECK_EQ_STR(ncl_module_name(g_modules, 0), "focas");
    NCL_CHECK(ncl_module_version(g_modules, 0) != NULL);
    NCL_CHECK(ncl_module_description(g_modules, 0) != NULL);
    NCL_CHECK(strstr(ncl_module_path(g_modules, 0), "ncl_driver_focas") != NULL);
    NCL_CHECK(!ncl_module_registered(g_modules, 0));

    NCL_TEST_CASE("loading the same protocol twice is not a duplicate");
    NCL_CHECK_EQ_INT(ncl_modules_add(g_modules, "focas", NCL_TEST_PLUGIN_DIR,
                                     &err),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_module_count(g_modules), 1u);

    NCL_TEST_CASE("registering it makes the protocol usable");
    ncl_strbuf_reset(&err);
    NCL_CHECK_EQ_INT(ncl_modules_register(g_modules, &err), NCL_OK);
    NCL_CHECK(ncl_module_registered(g_modules, 0));
    driver = ncl_driver_create("focas");
    NCL_CHECK(driver != NULL);
    if (driver != NULL) {
        NCL_CHECK_EQ_STR(driver->ops->protocol, "focas");
        driver->ops->destroy(driver);
    }

    NCL_TEST_CASE("its alias answers too");
    driver = ncl_driver_create("fanuc");
    NCL_CHECK(driver != NULL);
    if (driver != NULL) {
        driver->ops->destroy(driver);
    }

    NCL_TEST_CASE("a module with a foreign ABI is refused, and says so");
    ncl_strbuf_reset(&err);
    NCL_CHECK(ncl_modules_add(g_modules, "test_bad_abi",
                              NCL_TEST_MODULE_DIR, &err) != NCL_OK);
    if (strstr(ncl_strbuf_cstr(&err), "ABI") == NULL) {
        printf("      message: %s\n", ncl_strbuf_cstr(&err));
    }
    NCL_CHECK(strstr(ncl_strbuf_cstr(&err), "ABI") != NULL);
    NCL_CHECK_EQ_INT(ncl_module_count(g_modules), 1u);

    NCL_TEST_CASE("a file without the entry point is refused, and says so");
    ncl_strbuf_reset(&err);
    NCL_CHECK(ncl_modules_add(g_modules, "test_no_entry",
                              NCL_TEST_MODULE_DIR, &err) != NCL_OK);
    if (strstr(ncl_strbuf_cstr(&err), NCL_ADAPTER_MODULE_ENTRY) == NULL) {
        printf("      message: %s\n", ncl_strbuf_cstr(&err));
    }
    NCL_CHECK(strstr(ncl_strbuf_cstr(&err), NCL_ADAPTER_MODULE_ENTRY) != NULL);
    NCL_CHECK_EQ_INT(ncl_module_count(g_modules), 1u);

    NCL_TEST_CASE("a module file that is not there reports the platform");
    ncl_strbuf_reset(&err);
    NCL_CHECK(ncl_modules_add(g_modules, "nothing",
                              NCL_TEST_MODULE_DIR, &err) != NCL_OK);
    ncl_strbuf_free(&err);
}

static void test_config_section(void)
{
    ncl_strbuf err;
    char *dir;

    ncl_strbuf_init(&err);

    NCL_TEST_CASE("the configuration's plugins section names the directory");
    {
        ncl_json *config = ncl_json_parse_cstr(
            "{\"plugins\": {\"dir\": \"D:/some/site/plugins\","
            " \"load\": [\"focas\"]}}",
            NULL);

        NCL_CHECK(config != NULL);
        if (config != NULL) {
            dir = ncl_modules_dir_from_config(config, "fallback");
            NCL_CHECK_EQ_STR(dir, "D:/some/site/plugins");
            ncl_free_safe(dir);
            /* The named module is not there, so this one reports a failure. */
            NCL_CHECK(ncl_modules_add_config(g_modules, config, "fallback",
                                             &err) != NCL_OK);
            ncl_json_free(config);
        }
    }

    NCL_TEST_CASE("an array of names is accepted as well");
    {
        ncl_json *config = ncl_json_parse_cstr("{\"plugins\": [\"focas\"]}",
                                               NULL);

        NCL_CHECK(config != NULL);
        if (config != NULL) {
            dir = ncl_modules_dir_from_config(config, NCL_TEST_PLUGIN_DIR);
            NCL_CHECK_EQ_STR(dir, NCL_TEST_PLUGIN_DIR);
            ncl_free_safe(dir);
            NCL_CHECK_EQ_INT(ncl_modules_add_config(g_modules, config,
                                                    NCL_TEST_PLUGIN_DIR, &err),
                             NCL_OK);
            ncl_json_free(config);
        }
    }

    NCL_TEST_CASE("a configuration without the section loads the directory");
    {
        ncl_json *config = ncl_json_parse_cstr("{\"sn\": \"V2TEST\"}", NULL);
        size_t before = ncl_module_count(g_modules);

        NCL_CHECK(config != NULL);
        if (config != NULL) {
            NCL_CHECK_EQ_INT(ncl_modules_add_config(g_modules, config,
                                                    NCL_TEST_PLUGIN_DIR, &err),
                             NCL_OK);
            /* focas is already loaded, so "auto" finds nothing new. */
            NCL_CHECK_EQ_INT(ncl_module_count(g_modules), before);
            ncl_json_free(config);
        }
    }
    ncl_strbuf_free(&err);
}

/** The configuration a site would write: link + point map, agents the plugin
 *  for the protocol. Read here from a file, exactly like a deployment. */
static ncl_json *device_config(unsigned port)
{
    char text[2048];

    snprintf(text, sizeof(text),
             "{"
             "  \"sn\": \"V2FANUCPLUGIN\","
             "  \"plugins\": { \"load\": [\"focas\"] },"
             "  \"device\": { \"type\": \"MACHINE\", \"id\": \"01\","
             "                \"name\": \"FANUC 数控机床\" },"
             "  \"sample\": { \"intervalMs\": 1000, \"uploadMs\": 1000 },"
             "  \"drivers\": ["
             "    { \"id\": \"cnc\", \"path\": \"/CNC\", \"type\": \"focas\","
             "      \"parameters\": { \"host\": \"127.0.0.1\", \"port\": %u,"
             "                        \"timeoutMs\": 1000,"
             "                        \"connectTimeoutMs\": 1000,"
             "                        \"retries\": 0,"
             "                        \"negotiate\": true },"
             "      \"points\": ["
             "        {\"path\": \"STATUS@ALARM\","
             "         \"addr\": {\"area\": \"STATINFO@12\", \"dtype\": \"int16\"}},"
             "        {\"path\": \"STATUS@RUN\","
             "         \"addr\": {\"area\": \"STATINFO@2\", \"dtype\": \"int16\"}},"
             "        {\"path\": \"PART_COUNT\","
             "         \"addr\": {\"area\": \"RDCOUNT\", \"dtype\": \"int32\"}},"
             "        {\"path\": \"PROGRAM@NAME\","
             "         \"addr\": {\"area\": \"EXEPRGNAME2\", \"dtype\": \"string\","
             "                     \"length\": 36}},"
             "        {\"path\": \"AXIS@0/POSITION\","
             "         \"addr\": {\"area\": \"ACTF@0\", \"dtype\": \"float32\"}},"
             "        {\"path\": \"AXIS@1/POSITION\","
             "         \"addr\": {\"area\": \"ACTF@4\", \"dtype\": \"float32\"}},"
             "        {\"path\": \"AXIS@2/SPEED\","
             "         \"addr\": {\"area\": \"ACTS@8\", \"dtype\": \"float32\"}},"
             "        {\"path\": \"LIFE@0\", \"sample\": false,"
             "         \"addr\": {\"area\": \"RDLIFE\", \"dtype\": \"int32\"}}"
             "      ] }"
             "  ],"
             "  \"methods\": [ {\"path\": \"/CNC/ITEMS\", \"operation\": \"items\"} ]"
             "}",
             port);
    return ncl_json_parse_cstr(text, NULL);
}

static bool query_number(ncl_server *server, const char *path, double *out)
{
    ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    ncl_query_request_item *item = ncl_query_request_item_new(path);
    ncl_message *response;
    bool ok = false;

    *out = 0;
    if (request == NULL || item == NULL) {
        ncl_message_free(request);
        return false;
    }
    ncl_message_set_message_id(request, "q1");
    ncl_params_set_string(&item->params, "operation", "get_value");
    ncl_message_add_query_request_item(request, item);
    response = ncl_server_invoke_query(server, request);
    ncl_message_free(request);
    if (response != NULL) {
        const ncl_query_response_item *result =
            (const ncl_query_response_item *)ncl_message_item_at(response, 0);

        if (result != NULL && result->code != NULL &&
            strcmp(result->code, "OK") == 0) {
            ncl_json *value = ncl_json_arr_get(result->values, 0);

            ok = value != NULL && ncl_json_as_double(value, out);
        }
        ncl_message_free(response);
    }
    return ok;
}

static void test_assembled_device(void)
{
    fanuc_mock *mock = mock_start();
    ncl_json *config;
    ncl_strbuf err;
    ncl_adapter *adapter;
    ncl_server *server;
    double number = 0;

    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    ncl_strbuf_init(&err);
    config = device_config(mock->port);
    NCL_CHECK(config != NULL);
    if (config == NULL) {
        mock_stop(mock);
        ncl_strbuf_free(&err);
        return;
    }

    NCL_TEST_CASE("the point map of the configuration becomes the device model");
    adapter = ncl_adapter_create(config, &err);
    ncl_json_free(config);
    if (adapter == NULL) {
        printf("      adapter error: %s\n", ncl_strbuf_cstr(&err));
    }
    NCL_CHECK(adapter != NULL);
    if (adapter == NULL) {
        mock_stop(mock);
        ncl_strbuf_free(&err);
        return;
    }
    server = ncl_adapter_server(adapter);
    NCL_CHECK_EQ_INT(ncl_adapter_point_count(adapter), 8);
    NCL_CHECK_EQ_STR(ncl_adapter_point_path(adapter, 0), "/CNC/STATUS@ALARM");
    NCL_CHECK_EQ_INT(ncl_adapter_method_count(adapter), 1);

    NCL_TEST_CASE("a query reaches the machine through the loaded adapter");
    NCL_CHECK(query_number(server, "/CNC/STATUS@ALARM", &number));
    NCL_CHECK_EQ_INT((long long)number, MOCK_ALARM);
    NCL_CHECK(query_number(server, "/CNC/STATUS@RUN", &number));
    NCL_CHECK_EQ_INT((long long)number, MOCK_RUN);
    NCL_CHECK(query_number(server, "/CNC/PART_COUNT", &number));
    NCL_CHECK_EQ_INT((long long)number, MOCK_COUNT);
    NCL_CHECK(query_number(server, "/CNC/AXIS@1/POSITION", &number));
    NCL_CHECK(number > -2.26 && number < -2.24);
    NCL_CHECK(query_number(server, "/CNC/AXIS@2/SPEED", &number));
    NCL_CHECK(number > 299.9 && number < 300.1);

    NCL_TEST_CASE("a string point comes back as text");
    {
        ncl_json *value = NULL;

        NCL_CHECK_EQ_INT(ncl_driver_manager_read(ncl_adapter_drivers(adapter),
                                                 "/CNC/PROGRAM@NAME", &value),
                         NCL_OK);
        NCL_CHECK_EQ_STR(ncl_json_as_string(value), MOCK_PROGRAM);
        ncl_json_free(value);
    }

    NCL_TEST_CASE("polling refreshes the model, and sample:false stays in it");
    {
        ncl_node_map map;
        ncl_node *node = NULL;
        long long count = -1;

        NCL_CHECK_EQ_INT(ncl_adapter_poll_one(adapter, "/CNC/PART_COUNT", &err),
                         NCL_OK);
        ncl_node_map_init(&map);
        if (ncl_root_node_path_map(ncl_server_model(server), &map) == NCL_OK) {
            node = ncl_node_map_get(&map, "/CNC/PART_COUNT");
        }
        NCL_CHECK(node != NULL);
        if (node != NULL && node->value != NULL &&
            ncl_json_as_int(node->value, &count)) {
            NCL_CHECK_EQ_INT(count, MOCK_COUNT);
        } else {
            NCL_CHECK(false);
        }
        ncl_node_map_free(&map);
        NCL_CHECK(ncl_adapter_point_path(adapter, 7) != NULL);
    }

    ncl_adapter_free(adapter);
    ncl_strbuf_free(&err);
    mock_stop(mock);
}

NCL_TEST_MAIN_BEGIN()
    test_module_loading();
    test_config_section();
    test_assembled_device();
    ncl_modules_free(g_modules);
NCL_TEST_MAIN_END()
