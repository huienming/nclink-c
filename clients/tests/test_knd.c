/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * The KND driver: the item table of ncl_knd.h, and the driver talking to a
 * controller that answers the field's endpoints (`lua_mod/knd_mod.lua`).
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink_adapter/ncl_driver_manager.h"
#include "nclink/clients/knd.h"
#include "knd/ncl_knd_driver.h"

/* ------------------------------------------------------------- the table -- */

static void test_table(void)
{
    const ncl_knd_item *item;

    NCL_TEST_CASE("the item table is the delivered mapping layer's");
    item = ncl_knd_item_lookup("/STATUS");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        NCL_CHECK_EQ_STR(item->path, "/getValue");
        NCL_CHECK_EQ_STR(item->field, "run-status");
        NCL_CHECK_EQ_INT(item->shape, NCL_KND_RUN_STATUS);
    }
    /* The leading '/' and the case are optional, as the point map writes it. */
    NCL_CHECK(ncl_knd_item_lookup("STATUS") == item);
    NCL_CHECK(ncl_knd_item_lookup("status") == item);

    item = ncl_knd_item_lookup("/PART_COUNT");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        NCL_CHECK_EQ_STR(item->path, "/workcounts/total");
        NCL_CHECK_EQ_STR(item->field, "count");
        NCL_CHECK_EQ_INT(item->shape, NCL_KND_INTEGER);
    }
    item = ncl_knd_item_lookup("/FEED_OVERRIDE");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        NCL_CHECK_EQ_STR(item->path, "/overrides/feed");
        NCL_CHECK_EQ_INT(item->shape, NCL_KND_PERCENT);
    }
    item = ncl_knd_item_lookup("/VARIABLE@ESP_STATE");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        NCL_CHECK_EQ_STR(item->path, "/status");
        NCL_CHECK_EQ_STR(item->field, "not-ready-reason");
        NCL_CHECK_EQ_INT(item->shape, NCL_KND_BIT0);
    }
    item = ncl_knd_item_lookup("/TOOL_NUMBER");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        NCL_CHECK_EQ_STR(item->path, "/plc/vm/TL0");
        NCL_CHECK_EQ_INT(item->shape, NCL_KND_FIRST);
    }
    item = ncl_knd_item_lookup("/CONTROLLER/WARNING");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        NCL_CHECK_EQ_INT(item->shape, NCL_KND_ALARMS);
    }

    NCL_TEST_CASE("an axis reading names the axis, and both readings share one");
    item = ncl_knd_item_lookup("/AXIS@0/SCREW/POSITION");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        NCL_CHECK_EQ_STR(item->path, "/coors/machine");
        NCL_CHECK_EQ_INT(item->shape, NCL_KND_AXIS);
        NCL_CHECK_EQ_INT(item->axis, 0);
    }
    item = ncl_knd_item_lookup("/AXIS@8/MOTOR/POSITION");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        NCL_CHECK_EQ_INT(item->axis, 8); /* the ninth letter, "W" */
    }
    NCL_CHECK(ncl_knd_item_lookup("/AXIS@9/SCREW/POSITION") == NULL);
    NCL_CHECK(ncl_knd_item_lookup("/AXIS@0/NOPE") == NULL);

    NCL_CHECK(ncl_knd_item_lookup("KrnlAPI") == NULL);
    NCL_CHECK(ncl_knd_item_lookup("/NOT_AN_ITEM") == NULL);
    NCL_CHECK(ncl_knd_item_lookup("") == NULL);
    NCL_CHECK(ncl_knd_item_lookup(NULL) == NULL);

    NCL_TEST_CASE("the alarm classes are numbered the way the field numbers them");
    NCL_CHECK_EQ_INT(ncl_knd_alarm_class_count(), 14);
    NCL_CHECK_EQ_STR(ncl_knd_alarm_class(0), "prm-switch");
    NCL_CHECK_EQ_STR(ncl_knd_alarm_class(7), "servo");
    NCL_CHECK_EQ_STR(ncl_knd_alarm_class(13), "forbid-move");
    NCL_CHECK(ncl_knd_alarm_class(14) == NULL);
}

/* -------------------------------------------------------- the controller -- */

typedef struct {
    ncl_socket *listener;
    unsigned    port;
    ncl_thread *thread;
    bool        stop;
    int         requests;      /**< every request                            */
    int         coord_requests; /**< /coors/machine, to see a batch merge off */
    int         status_requests; /**< /status, the open() probe and the item  */
    char        last_target[128];
} knd_mock;

/** The canned answer of one endpoint, as the field's controller sends it. */
static const char *mock_body(const char *target, size_t *len)
{
    static const struct {
        const char *path;
        const char *body;
    } kReplies[] = {
        {"/status", "{\"not-ready-reason\": 1}"},
        {"/getValue", "{\"run-status\": 2}"},
        {"/progs/cur", "{\"number\": 42.0}"},
        {"/progs/exec-status", "{\"P\": 1234}"},
        {"/workcounts/total", "{\"count\": 77}"},
        {"/workcountgoals/total", "{\"count\": 100}"},
        {"/cycletime", "{\"total\": 3600, \"cur\": 120}"},
        {"/overrides/feed", "{\"ov\": 1.5}"},
        {"/overrides/rapid", "{\"ov\": 1.0}"},
        {"/overrides/jog", "{\"ov\": 0.5}"},
        {"/overrides/handle", "{\"ov\": 0.1}"},
        {"/sp/overrides/1", "{\"ov\": 1.2}"},
        {"/sp/speeds/1", "{\"speed\": 8000}"},
        {"/plc/vm/TL0", "[7, 8]"},
        {"/alarms/", "{\"servo\": \"SV0411 伺服过载\", \"over-travel\": \"+X 超程\"}"},
        {"/coors/machine", "{\"X\": 1.5, \"Y\": 2.5, \"Z\": -3.25, \"W\": 8}"},
    };
    size_t i;

    for (i = 0; i < sizeof(kReplies) / sizeof(kReplies[0]); i++) {
        if (strcmp(kReplies[i].path, target) == 0) {
            *len = strlen(kReplies[i].body);
            return kReplies[i].body;
        }
    }
    return NULL;
}

static void mock_reply(ncl_socket *peer, const char *target)
{
    ncl_strbuf response;
    const char *body;
    size_t len = 0;

    body = mock_body(target, &len);
    ncl_strbuf_init(&response);
    if (body == NULL) {
        (void)ncl_strbuf_printf(&response,
                                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                "Connection: close\r\n\r\n");
    } else {
        (void)ncl_strbuf_printf(&response,
                                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                "Content-Length: %u\r\nConnection: close\r\n\r\n",
                                (unsigned)len);
        (void)ncl_strbuf_append(&response, body, len);
    }
    (void)ncl_socket_send(peer, response.data, response.len);
    ncl_strbuf_free(&response);
}

static void mock_main(void *arg)
{
    knd_mock *mock = (knd_mock *)arg;

    while (!mock->stop) {
        ncl_socket *peer = ncl_socket_accept(mock->listener, 200);

        if (peer == NULL) {
            continue;
        }
        for (;;) {
            char request[1024];
            size_t used = 0;
            bool got_line = false;
            char target[128];
            size_t i = 0;

            while (used + 1 < sizeof(request)) {
                int got = ncl_socket_recv(peer, request + used,
                                          sizeof(request) - 1 - used, 2000);

                if (got <= 0) {
                    break;
                }
                used += (size_t)got;
                request[used] = '\0';
                if (strstr(request, "\r\n\r\n") != NULL) {
                    got_line = true;
                    break;
                }
            }
            if (!got_line) {
                break;
            }
            mock->requests++;
            target[0] = '\0';
            if (strncmp(request, "GET ", 4) == 0) {
                i = 4;
                while (request[i] != ' ' && request[i] != '\0' &&
                       i + 1 < sizeof(target)) {
                    target[i - 4] = request[i];
                    i++;
                }
                target[i - 4] = '\0';
            }
            snprintf(mock->last_target, sizeof(mock->last_target), "%s", target);
            if (strcmp(target, "/coors/machine") == 0) {
                mock->coord_requests++;
            } else if (strcmp(target, "/status") == 0) {
                mock->status_requests++;
            }
            mock_reply(peer, target);
            break; /* one request per connection, as the client asks */
        }
        ncl_socket_close(peer);
    }
}

static knd_mock *mock_start(void)
{
    knd_mock *mock = (knd_mock *)ncl_mem_calloc(1, sizeof(*mock));

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

static void mock_stop(knd_mock *mock)
{
    if (mock == NULL) {
        return;
    }
    mock->stop = true;
    ncl_thread_join(mock->thread);
    ncl_socket_close(mock->listener);
    ncl_free_safe(mock);
}

/** A driver pointed at the mock. */
static ncl_driver *knd_driver(knd_mock *mock)
{
    ncl_driver *driver = ncl_driver_create("knd");
    ncl_strbuf json;
    ncl_json *parameters;

    if (driver == NULL) {
        return NULL;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"host\":\"127.0.0.1\",\"port\":%u,"
                            "\"timeoutMs\":800}",
                            mock->port);
    parameters = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    if (parameters == NULL ||
        driver->ops->create(driver, parameters) != NCL_OK) {
        ncl_json_free(parameters);
        driver->ops->destroy(driver);
        return NULL;
    }
    ncl_json_free(parameters);
    return driver;
}

static ncl_err read_item(ncl_driver *driver, const char *item, ncl_json **value)
{
    ncl_strbuf json;
    ncl_json *node;
    ncl_address address;
    ncl_err err;

    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json, "{\"area\":\"%s\",\"offset\":0}", item);
    node = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    if (node == NULL) {
        return NCL_ERR_PARSE;
    }
    err = ncl_address_from_json(node, &address);
    ncl_json_free(node);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_driver_read_one(driver, &address, value);
    ncl_address_clear(&address);
    return err;
}

/* -------------------------------------------------------------- the read -- */

static void test_read(void)
{
    knd_mock *mock = mock_start();
    ncl_driver *driver = mock != NULL ? knd_driver(mock) : NULL;
    ncl_json *value = NULL;

    NCL_TEST_CASE("the mapped items come back shaped like the field has them");
    NCL_CHECK(mock != NULL && driver != NULL);
    if (mock == NULL || driver == NULL) {
        mock_stop(mock);
        return;
    }
    /* The session probe is /status, so the first read pays for it. */
    NCL_CHECK_EQ_INT(read_item(driver, "/STATUS", &value), NCL_OK);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "running");
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(mock->status_requests, 1);

    NCL_CHECK_EQ_INT(read_item(driver, "/PART_COUNT", &value), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_json_obj_get_int(value, "x", -1), -1); /* a scalar */
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 77);
    }
    ncl_json_free(value);
    value = NULL;

    /* The overrides arrive as fractions and the field reports percent. */
    NCL_CHECK_EQ_INT(read_item(driver, "/FEED_OVERRIDE", &value), NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == 150.0);
    }
    ncl_json_free(value);
    value = NULL;

    /* The ready bit: bit 0 of the reason field. */
    NCL_CHECK_EQ_INT(read_item(driver, "/VARIABLE@ESP_STATE", &value), NCL_OK);
    {
        long long flag = 0;

        NCL_CHECK(ncl_json_as_int(value, &flag));
        NCL_CHECK_EQ_INT(flag, 1); /* JSON true */
    }
    ncl_json_free(value);
    value = NULL;

    /* A program number is a number on the wire and text in the model. */
    NCL_CHECK_EQ_INT(read_item(driver, "/CONTROLLER/PROGRAM", &value), NCL_OK);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "42");
    ncl_json_free(value);
    value = NULL;

    /* The tool number is element 0 of a one element array. */
    NCL_CHECK_EQ_INT(read_item(driver, "/TOOL_NUMBER", &value), NCL_OK);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 7);
    }
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("an axis reading is one field of the machine coordinates");
    NCL_CHECK_EQ_INT(read_item(driver, "/AXIS@2/SCREW/POSITION", &value), NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == -3.25);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(mock->coord_requests, 1);

    NCL_TEST_CASE("the warning item is the alarm map as a list");
    NCL_CHECK_EQ_INT(read_item(driver, "/CONTROLLER/WARNING", &value), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_json_arr_len(value), 2);
    if (ncl_json_arr_len(value) == 2) {
        /* In class order: "over-travel" is the fifth class, "servo" the eighth. */
        ncl_json *first = ncl_json_arr_get(value, 0);

        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(first, "number"), "10005");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(first, "text"), "+X 超程");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_arr_get(value, 1),
                                                 "number"),
                         "10008");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(ncl_json_arr_get(value, 1),
                                                 "text"),
                         "SV0411 伺服过载");
    }
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("an item the table does not know is a business error");
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(read_item(driver, "/NOPE", &value)), 3);

    NCL_TEST_CASE("writing is refused: the field registers no write endpoint");
    NCL_CHECK_EQ_INT(ncl_driver_write_one(driver, NULL, NULL),
                     NCL_ERR_NOT_SUPPORTED);

    NCL_TEST_CASE("a batch asks a shared endpoint only once");
    {
        ncl_address addresses[2];
        ncl_json *values = NULL;
        int before = mock->coord_requests;

        memset(addresses, 0, sizeof(addresses));
        addresses[0].area = ncl_strdup("/AXIS@0/SCREW/POSITION");
        addresses[1].area = ncl_strdup("/AXIS@1/MOTOR/POSITION");
        addresses[0].length = 1;
        addresses[1].length = 1;
        addresses[0].bit = -1;
        addresses[1].bit = -1;
        NCL_CHECK_EQ_INT(driver->ops->read_batch(driver, addresses, 2, &values),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(values), 2);
        if (ncl_json_arr_len(values) == 2) {
            double x = 0;
            double y = 0;

            NCL_CHECK(ncl_json_as_double(ncl_json_arr_get(values, 0), &x));
            NCL_CHECK(ncl_json_as_double(ncl_json_arr_get(values, 1), &y));
            NCL_CHECK(x == 1.5);
            NCL_CHECK(y == 2.5);
        }
        NCL_CHECK_EQ_INT(mock->coord_requests - before, 1);
        ncl_json_free(values);
        ncl_address_clear(&addresses[0]);
        ncl_address_clear(&addresses[1]);
    }

    driver->ops->destroy(driver);
    mock_stop(mock);
}

/* ------------------------------------------------------ through the map --- */

static void test_through_the_manager(void)
{
    knd_mock *mock = mock_start();
    ncl_driver_manager *manager = ncl_driver_manager_create();
    ncl_strbuf json;
    ncl_strbuf err;
    ncl_json *config;
    ncl_json *value = NULL;
    long long number = 0;

    NCL_TEST_CASE("a configured KND link reads through the point map");
    NCL_CHECK(mock != NULL && manager != NULL);
    if (mock == NULL || manager == NULL) {
        mock_stop(mock);
        ncl_driver_manager_free(manager);
        return;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"id\":\"cnc\",\"path\":\"/CNC\","
                            "\"type\":\"knd\","
                            "\"parameters\":{\"host\":\"127.0.0.1\","
                            "\"port\":%u,\"timeoutMs\":800},"
                            "\"points\":["
                            "{\"path\":\"/CNC/COUNT\",\"addr\":\"/PART_COUNT\"},"
                            "{\"path\":\"/CNC/X\","
                            "\"addr\":\"/AXIS@0/SCREW/POSITION\"},"
                            "{\"path\":\"/CNC/FEED\","
                            "\"addr\":{\"area\":\"FEED_OVERRIDE\"}}]}",
                            mock->port);
    config = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    NCL_CHECK(config != NULL);
    ncl_strbuf_init(&err);
    if (config != NULL) {
        NCL_CHECK_EQ_INT(ncl_driver_manager_add_json(manager, config, &err),
                         NCL_OK);
    }
    if (err.len > 0) {
        printf("    %s\n", ncl_strbuf_cstr(&err));
    }
    ncl_strbuf_free(&err);
    ncl_json_free(config);

    NCL_CHECK_EQ_INT(ncl_driver_manager_read(manager, "/CNC/COUNT", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 77);
    ncl_json_free(value);
    value = NULL;

    NCL_CHECK_EQ_INT(ncl_driver_manager_read(manager, "/CNC/X", &value), NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == 1.5);
    }
    ncl_json_free(value);
    value = NULL;

    NCL_CHECK_EQ_INT(ncl_driver_manager_read(manager, "/CNC/FEED", &value),
                     NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == 150.0);
    }
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(ncl_driver_manager_read(manager, "/CNC/NOPE", &value),
                     NCL_ERR_NOT_FOUND);
    ncl_driver_manager_free(manager);
    mock_stop(mock);
}

NCL_TEST_MAIN_BEGIN()
    test_table();
    test_read();
    test_through_the_manager();
NCL_TEST_MAIN_END()
