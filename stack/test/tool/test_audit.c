/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * The audit trail of 00-通用-实现约定 §6: counters that tell a cable problem
 * from a protocol one, and a write record that carries the old value, the new
 * one and who asked for it.
 */
#include <stdio.h>

#include "ncl_test.h"

#include "nclink/ncl_logger.h"
#include "nclink/ncl_audit.h"
#include "mock/ncl_mock_driver.h"
#include "test_point_map.h"

#define RAW_LOG_DIR "ncl_audit_log_test"

static const char kConfig[] =
    "{"
    "  \"drivers\": ["
    "    { \"id\": \"plc1\", \"path\": \"/PLC1\", \"type\": \"mock\","
    "      \"parameters\": { \"points\": ["
    "        {\"area\": \"D\", \"offset\": 100, \"dtype\": \"int16\","
    "         \"value\": 7},"
    "        {\"area\": \"D\", \"offset\": 200, \"dtype\": \"int16\","
    "         \"value\": 0}]},"
    "      \"points\": ["
    "        {\"path\": \"/PLC1/STATUS\", \"addr\": \"D100\"},"
    "        {\"path\": \"/PLC1/SETPOINT\", \"addr\": \"D200\","
    "         \"writable\": true}"
    "      ] }"
    "  ]"
    "}";

static test_point_map *make_map(void)
{
    test_point_map *manager = test_point_map_create(ncl_mock_driver_create);
    ncl_json *document = ncl_json_parse_cstr(kConfig, NULL);
    ncl_strbuf err;

    if (manager == NULL || document == NULL) {
        ncl_json_free(document);
        test_point_map_free(manager);
        return NULL;
    }
    ncl_strbuf_init(&err);
    if (test_point_map_add_json(manager, document, &err) != NCL_OK) {
        printf("    add_json said: %s\n", ncl_strbuf_cstr(&err));
        ncl_strbuf_free(&err);
        ncl_json_free(document);
        test_point_map_free(manager);
        return NULL;
    }
    ncl_strbuf_free(&err);
    ncl_json_free(document);
    return manager;
}

static void test_counters(void)
{
    ncl_audit_options options;
    test_point_map *manager;
    ncl_json *value = NULL;
    ncl_json *written;
    ncl_json *stats;

    NCL_TEST_CASE("the trail counts requests, writes and failures by tier");
    ncl_audit_options_default(&options);
    options.operator_name = "commissioning";
    ncl_audit_init(&options);
    ncl_audit_reset_stats();
    NCL_CHECK(ncl_audit_enabled());
    NCL_CHECK(!ncl_audit_wants_raw());

    manager = make_map();
    NCL_CHECK(manager != NULL);
    if (manager == NULL) {
        return;
    }
    /* two good reads, one miss, one good write, one write to a path that has
     * no point at all: a write is audited whether or not it happened. The
 * "a point must be marked writable" rule of §7 is the host's job, not the
 * manager's - test_host_tool.c covers it where the device model lives. */
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/PLC1/STATUS", &value),
                     NCL_OK);
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/PLC1/SETPOINT", &value),
                     NCL_OK);
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/PLC1/NOPE", &value),
                     NCL_ERR_NOT_FOUND);
    written = ncl_json_new_int(42);
    NCL_CHECK_EQ_INT(test_point_map_write(manager, "/PLC1/SETPOINT", written),
                     NCL_OK); /* the write borrows the value */
    ncl_json_free(written);
    written = ncl_json_new_int(1);
    NCL_CHECK_EQ_INT(test_point_map_write(manager, "/PLC1/NOPE", written),
                     NCL_ERR_NOT_FOUND); /* no such point */
    ncl_json_free(written);

    stats = ncl_audit_stats();
    NCL_CHECK(stats != NULL);
    if (stats != NULL) {
        char *text = ncl_json_write_string(stats);

        NCL_CHECK(text != NULL);
        if (text != NULL) {
            /* 3 reads + 2 writes */
            NCL_CHECK(strstr(text, "\"requests\":5") != NULL);
            NCL_CHECK(strstr(text, "\"writes\":2") != NULL);
            NCL_CHECK(strstr(text, "\"/PLC1/SETPOINT\"") != NULL);
            NCL_CHECK(strstr(text, "\"from\":\"0\"") != NULL);
            NCL_CHECK(strstr(text, "\"to\":\"42\"") != NULL);
            /* the failed write is in the trail as well, with no old value */
            NCL_CHECK(strstr(text, "\"path\":\"/PLC1/NOPE\"") != NULL);
            NCL_CHECK(strstr(text, "\"from\":\"-\"") != NULL);
            NCL_CHECK(strstr(text, "\"to\":\"1\"") != NULL);
            /* and both misses share one code bucket in the histogram */
            NCL_CHECK(strstr(text, "\"count\":2") != NULL);
        }
        ncl_free_safe(text);
        ncl_json_free(stats);
    }

    NCL_TEST_CASE("resetting the counters leaves the log alone");
    ncl_audit_reset_stats();
    stats = ncl_audit_stats();
    NCL_CHECK(stats != NULL);
    if (stats != NULL) {
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(stats, "requests", -1), 0);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(stats, "writes", -1), 0);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_obj_get(stats, "lastWrites")), 0);
        ncl_json_free(stats);
    }

    test_point_map_free(manager);
}

/* --------------------------------------------------------------- frames -- */

/*
 * A driver with a wire but no socket: what the audit needs is only that a
 * driver can hand over the bytes it exchanged. §6 wants the raw frame of every
 * request on demand - a full hex log is not what a production line runs with.
 */
static const uint8_t kRawRequest[] = {0x02, 0x01, 0xA0};
static const uint8_t kRawReply[] = {0x02, 0x01, 0x02, 0x00, 0x07};

static ncl_err raw_read_batch(ncl_driver *self, const ncl_address *addresses,
                              size_t count, ncl_json **values)
{
    ncl_json *array = ncl_json_new_array();
    size_t i;

    (void)self;
    (void)addresses;
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < count; i++) {
        if (ncl_json_arr_push(array, ncl_json_new_int(7)) != NCL_OK) {
            ncl_json_free(array);
            return NCL_ERR_NOMEM;
        }
    }
    *values = array;
    return NCL_OK;
}

static void raw_last_raw(const ncl_driver *self, ncl_driver_raw *out)
{
    (void)self;
    out->request = kRawRequest;
    out->request_len = sizeof(kRawRequest);
    out->reply = kRawReply;
    out->reply_len = sizeof(kRawReply);
}

static void raw_destroy(ncl_driver *self)
{
    ncl_free_safe(self);
}

/** The ops in order: protocol, create, open, close, is_connected, read_batch,
 *  write_batch, read_raw, write_raw, call, attach_event, destroy, last_raw. */
static ncl_driver *raw_factory(void)
{
    static const ncl_driver_ops kOps = {
        "rawmock", NULL,
        NULL,      NULL,
        NULL,      raw_read_batch,
        NULL,      NULL,
        NULL,      NULL,
        NULL,      raw_destroy,
        raw_last_raw,
    };

    return ncl_driver_new(&kOps, NULL);
}

/** The whole file as text, or NULL. */
static char *read_text_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    char *text;
    size_t size;

    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    size = (size_t)ftell(file);
    rewind(file);
    text = (char *)ncl_mem_alloc(size + 1u);
    if (text == NULL) {
        fclose(file);
        return NULL;
    }
    if (size > 0 && fread(text, 1, size, file) != size) {
        ncl_free_safe(text);
        fclose(file);
        return NULL;
    }
    text[size] = '\0';
    fclose(file);
    return text;
}

static void test_raw_frames(void)
{
    ncl_audit_options options;
    test_point_map *manager;
    ncl_json *config;
    ncl_json *value = NULL;
    ncl_strbuf err;
    char *log;

    NCL_TEST_CASE("§6: the frames of a request reach the log when asked for");
    (void)remove(RAW_LOG_DIR "/out.txt");
    NCL_CHECK(ncl_log_init(RAW_LOG_DIR));
    ncl_log_set_console(false); /* keep the test output readable */
    ncl_log_set_level(NCL_LOG_DEBUG);

    ncl_audit_options_default(&options);
    options.raw = true; /* the switch of §6: off unless someone asks for it */
    ncl_audit_init(&options);
    NCL_CHECK(ncl_audit_wants_raw());

    manager = test_point_map_create(raw_factory);
    NCL_CHECK(manager != NULL);
    if (manager != NULL) {
        config = ncl_json_parse_cstr(
            "{\"id\":\"raw1\",\"path\":\"/RAW1\",\"type\":\"rawmock\","
            "\"points\":[{\"path\":\"/RAW1/X\",\"addr\":\"D1\"}]}",
            NULL);
        NCL_CHECK(config != NULL);
        ncl_strbuf_init(&err);
        NCL_CHECK_EQ_INT(test_point_map_add_json(manager, config, &err),
                         NCL_OK);
        ncl_strbuf_free(&err);
        ncl_json_free(config);
        NCL_CHECK_EQ_INT(test_point_map_read(manager, "/RAW1/X", &value),
                         NCL_OK);
        ncl_json_free(value);
        test_point_map_free(manager);
    }

    ncl_log_shutdown();
    ncl_log_set_level(NCL_LOG_INFO);
    ncl_log_set_console(true);
    log = read_text_file(RAW_LOG_DIR "/out.txt");
    NCL_CHECK(log != NULL);
    if (log != NULL) {
        NCL_CHECK(strstr(log, "request 3 bytes 0201a0") != NULL);
        NCL_CHECK(strstr(log, "reply 5 bytes 0201020007") != NULL);
        ncl_free_safe(log);
    }
    ncl_audit_init(NULL); /* the defaults again, for whatever runs next */
}

static void test_tiers(void)
{
    test_point_map *manager;
    ncl_json *params;
    ncl_driver *driver;
    ncl_json *value = NULL;
    ncl_json *stats;

    NCL_TEST_CASE("a failing driver lands in its own tier and code bucket");
    ncl_audit_init(NULL);
    ncl_audit_reset_stats();
    manager = test_point_map_create(ncl_mock_driver_create);
    NCL_CHECK(manager != NULL);
    if (manager == NULL) {
        return;
    }
    /* A mock that fails the first two reads with a protocol code. */
    params = ncl_json_parse_cstr(
        "{\"id\":\"bad\",\"path\":\"/BAD\",\"type\":\"mock\","
        "\"parameters\":{\"fail\":{\"code\":536870932,\"count\":2}},"
        "\"points\":[{\"path\":\"/BAD/D1\",\"addr\":\"D1\"}]}",
        NULL);
    NCL_CHECK_EQ_INT(test_point_map_add_json(manager, params, NULL), NCL_OK);
    ncl_json_free(params);

    driver = test_point_map_driver(manager);
    NCL_CHECK(driver != NULL);
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/BAD/D1", &value),
                     NCL_DRV_ERR_PROTOCOL(20));
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/BAD/D1", &value),
                     NCL_DRV_ERR_PROTOCOL(20));
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/BAD/D1", &value), NCL_OK);
    ncl_json_free(value);
    value = NULL;

    stats = ncl_audit_stats();
    NCL_CHECK(stats != NULL);
    if (stats != NULL) {
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(stats, "requests", -1), 3);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(stats, "protocol", -1), 2);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(stats, "transport", -1), 0);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(stats, "business", -1), 0);
        {
            ncl_json *errors = ncl_json_obj_get(stats, "errors");
            ncl_json *first = ncl_json_arr_get(errors, 0);

            NCL_CHECK(first != NULL);
            if (first != NULL) {
                NCL_CHECK_EQ_INT(ncl_json_obj_get_int(first, "count", -1), 2);
                NCL_CHECK_EQ_STR(ncl_json_obj_get_string(first, "tier"), "protocol");
                NCL_CHECK_EQ_INT(ncl_json_obj_get_int(first, "code", 0),
                                 NCL_DRV_ERR_PROTOCOL(20));
            }
        }
        ncl_json_free(stats);
    }
    test_point_map_free(manager);
}

static void test_sessions(void)
{
    test_point_map *manager;
    ncl_strbuf err;
    ncl_json *stats;

    NCL_TEST_CASE("opening and closing the links is part of the trail");
    ncl_audit_init(NULL);
    ncl_audit_reset_stats();
    manager = make_map();
    NCL_CHECK(manager != NULL);
    if (manager == NULL) {
        return;
    }
    ncl_strbuf_init(&err);
    NCL_CHECK_EQ_INT(test_point_map_open(manager), NCL_OK);
    ncl_strbuf_free(&err);
    test_point_map_close(manager);
    stats = ncl_audit_stats();
    NCL_CHECK(stats != NULL);
    if (stats != NULL) {
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(stats, "sessions", -1), 2);
        ncl_json_free(stats);
    }
    test_point_map_free(manager);

    NCL_TEST_CASE("the trail can be switched off");
    ncl_audit_reset_stats();
    {
        ncl_audit_options options;

        ncl_audit_options_default(&options);
        options.enabled = false;
        ncl_audit_init(&options);
    }
    NCL_CHECK(!ncl_audit_enabled());
    manager = make_map();
    if (manager != NULL) {
        ncl_json *value = NULL;

        (void)test_point_map_read(manager, "/PLC1/STATUS", &value);
        ncl_json_free(value);
        test_point_map_free(manager);
    }
    NCL_CHECK_EQ_INT(ncl_audit_enabled(), 0);
    ncl_audit_init(NULL); /* back to the defaults for anything that follows */
}

NCL_TEST_MAIN_BEGIN()
    test_counters();
    test_raw_frames();
    test_tiers();
    test_sessions();
NCL_TEST_MAIN_END()
