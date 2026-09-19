/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Driver configuration and path dispatch: the point map, the longest-prefix
 * rule with its "/" catch-all, and the read/write/call helpers that the daemon
 * (and every tool method) goes through.
 */
#include <stdio.h>

#include "ncl_test.h"

#include "nclink/ncl_env.h"
#include "nclink_adapter/ncl_driver_manager.h"

#define CFG_DIR "ncl_driver_cfg_test"

static const char kConfig[] =
    "{"
    "  \"drivers\": ["
    "    {"
    "      \"id\": \"plc1\","
    "      \"path\": \"/PLC1/\","
    "      \"type\": \"mock\","
    "      \"parameters\": { \"points\": ["
    "        {\"area\": \"D\", \"offset\": 100, \"dtype\": \"int16\","
    "         \"value\": 7},"
    "        {\"area\": \"M\", \"offset\": 10, \"dtype\": \"int16\","
    "         \"value\": 0}]},"
    "      \"points\": ["
    "        {\"path\": \"/PLC1/STATUS\", \"addr\": \"D100\"},"
    "        {\"id\": \"POWER\", \"addr\": {\"area\": \"D\", \"offset\": 101},"
    "         \"dtype\": \"float32\"},"
    "        {\"id\": \"BITS\", \"addr\": \"M10\", \"length\": 4}"
    "      ]"
    "    },"
    "    {"
    "      \"id\": \"fallback\","
    "      \"path\": \"/\","
    "      \"type\": \"mock\","
    "      \"parameters\": { \"points\": ["
    "        {\"area\": \"C\", \"offset\": 0, \"dtype\": \"int16\","
    "         \"value\": 99}]},"
    "      \"points\": [ {\"path\": \"/ANYTHING\", \"addr\": \"C0\"} ]"
    "    }"
    "  ]"
    "}";

static ncl_driver_manager *make_manager(void)
{
    ncl_driver_manager *manager = ncl_driver_manager_create();
    ncl_json *document = ncl_json_parse_cstr(kConfig, NULL);
    ncl_strbuf err;

    if (manager == NULL || document == NULL) {
        ncl_json_free(document);
        ncl_driver_manager_free(manager);
        return NULL;
    }
    ncl_strbuf_init(&err);
    if (ncl_driver_manager_add_json(manager, document, &err) != NCL_OK) {
        printf("    config error: %s\n", ncl_strbuf_cstr(&err));
        ncl_strbuf_free(&err);
        ncl_json_free(document);
        ncl_driver_manager_free(manager);
        return NULL;
    }
    ncl_strbuf_free(&err);
    ncl_json_free(document);
    return manager;
}

static long long read_int(ncl_driver_manager *manager, const char *path,
                          ncl_err *err)
{
    ncl_json *value = NULL;
    long long number = -1;

    *err = ncl_driver_manager_read(manager, path, &value);
    if (*err == NCL_OK && !ncl_json_as_int(value, &number)) {
        *err = NCL_ERR_INVALID_VALUE;
    }
    ncl_json_free(value);
    return number;
}

/* -------------------------------------------------------------- contents -- */

static void test_config(void)
{
    ncl_driver_manager *manager = make_manager();
    ncl_json *value = NULL;
    long long number = 0;
    ncl_err err;

    NCL_TEST_CASE("the configuration builds one entry per driver");
    NCL_CHECK(manager != NULL);
    if (manager == NULL) {
        return;
    }
    NCL_CHECK_EQ_INT(ncl_driver_manager_count(manager), 2);
    NCL_CHECK_EQ_STR(ncl_driver_manager_id_at(manager, 0), "plc1");
    NCL_CHECK_EQ_STR(ncl_driver_manager_path_at(manager, 0), "/PLC1");
    NCL_CHECK_EQ_STR(ncl_driver_manager_protocol_at(manager, 0), "mock");
    NCL_CHECK_EQ_STR(ncl_driver_manager_id_at(manager, 1), "fallback");
    NCL_CHECK_EQ_STR(ncl_driver_manager_path_at(manager, 1), "/");
    NCL_CHECK(ncl_driver_manager_driver_at(manager, 0) != NULL);
    NCL_CHECK(ncl_driver_manager_id_at(manager, 9) == NULL);

    NCL_TEST_CASE("a point maps to an address relative to the link");
    {
        const ncl_address *address = ncl_driver_manager_address_of(
            manager, "/PLC1/STATUS");

        NCL_CHECK(address != NULL);
        if (address != NULL) {
            NCL_CHECK_EQ_STR(address->area, "D");
            NCL_CHECK_EQ_INT(address->offset, 100);
            NCL_CHECK_EQ_INT(address->dtype, NCL_DTYPE_INT16);
        }
        address = ncl_driver_manager_address_of(manager, "/PLC1/BITS");
        NCL_CHECK(address != NULL);
        if (address != NULL) {
            NCL_CHECK_EQ_INT(address->length, 4);
        }
        NCL_CHECK(ncl_driver_manager_address_of(manager, "/PLC1/NOPE") == NULL);
        NCL_CHECK(ncl_driver_manager_address_of(manager, "/ANYTHING") != NULL);
    }

    NCL_TEST_CASE("reads and writes go through the point map");
    number = read_int(manager, "/PLC1/STATUS", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 7);

    NCL_CHECK_EQ_INT(ncl_driver_manager_write(manager, "/PLC1/STATUS",
                                              ncl_json_new_int(55)),
                     NCL_OK);
    number = read_int(manager, "/PLC1/STATUS", &err);
    NCL_CHECK_EQ_INT(number, 55);

    /* the type declared by the point is what the driver sees */
    NCL_CHECK_EQ_INT(ncl_driver_manager_write(manager, "/PLC1/POWER",
                                              ncl_json_new_double(1.5)),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_driver_manager_read(manager, "/PLC1/POWER", &value),
                     NCL_OK);
    {
        double real = 0;
        const ncl_address *address =
            ncl_driver_manager_address_of(manager, "/PLC1/POWER");

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == 1.5);
        NCL_CHECK(address != NULL);
        if (address != NULL) {
            NCL_CHECK_EQ_INT(address->dtype, NCL_DTYPE_FLOAT32);
        }
    }
    ncl_json_free(value);

    NCL_TEST_CASE("the catch-all link answers what nothing else claims");
    number = read_int(manager, "/ANYTHING", &err);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(number, 99);
    number = read_int(manager, "/SOMEWHERE/ELSE", &err);
    NCL_CHECK_EQ_INT(err, NCL_ERR_NOT_FOUND); /* the "/" link has no point */

    NCL_TEST_CASE("the longest prefix wins, on segment boundaries");
    NCL_CHECK(ncl_driver_manager_driver_of(manager, "/PLC1/STATUS") ==
              ncl_driver_manager_driver_at(manager, 0));
    /* "/PLC10" is not inside "/PLC1" */
    NCL_CHECK(ncl_driver_manager_driver_of(manager, "/PLC10/STATUS") ==
              ncl_driver_manager_driver_at(manager, 1));
    NCL_CHECK(ncl_driver_manager_address_of(manager, "/PLC10/STATUS") == NULL);
    NCL_CHECK(ncl_driver_manager_driver_of(manager, "/PLC1") ==
              ncl_driver_manager_driver_at(manager, 0));
    NCL_CHECK(ncl_driver_manager_driver_of(manager, "/") ==
              ncl_driver_manager_driver_at(manager, 1));

    ncl_driver_manager_free(manager);
}

/* --------------------------------------------------------------- methods -- */

static void test_calls_and_events(void)
{
    ncl_driver_manager *manager = make_manager();
    ncl_json *echo = ncl_json_parse_cstr("{\"a\":[1,2]}", NULL);
    ncl_json *result = NULL;

    NCL_TEST_CASE("call() reaches the driver that owns the path");
    NCL_CHECK(manager != NULL);
    if (manager == NULL) {
        ncl_json_free(echo);
        return;
    }
    NCL_CHECK_EQ_INT(ncl_driver_manager_call(manager, "/PLC1/POWER", "echo",
                                             echo, &result),
                     NCL_OK);
    NCL_CHECK(result != NULL && ncl_json_equals(result, echo));
    ncl_json_free(result);
    result = NULL;

    NCL_CHECK_EQ_INT(ncl_driver_manager_call(manager, "/ANYTHING", "echo", echo,
                                             &result),
                     NCL_OK);
    ncl_json_free(result);
    NCL_CHECK_EQ_INT(ncl_driver_manager_call(manager, "/ANYTHING", "nonsense",
                                             NULL, &result),
                     NCL_DRV_ERR_PROTOCOL(1));
    NCL_CHECK_EQ_INT(ncl_driver_manager_call(manager, "/ANYTHING", "", NULL,
                                             &result),
                     NCL_ERR_INVALID_ARG);
    ncl_json_free(echo);
    ncl_driver_manager_free(manager);
}

/* ---------------------------------------------------------------- files --- */

static void test_files(void)
{
    ncl_driver_manager *manager = ncl_driver_manager_create();
    ncl_strbuf err;
    const char *one =
        "{\"id\":\"a\",\"path\":\"/A\",\"type\":\"mock\","
        "\"points\":[{\"path\":\"/A/X\",\"addr\":\"D1\"}]}";
    const char *two =
        "{\"id\":\"b\",\"path\":\"/B\",\"type\":\"mock\","
        "\"points\":[{\"path\":\"/B/X\",\"addr\":\"D2\"}]}";

    NCL_TEST_CASE("a directory of configuration files loads in name order");
    NCL_CHECK(manager != NULL);
    NCL_CHECK_EQ_INT(ncl_mkdir_p(CFG_DIR), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_file_write_all(CFG_DIR "/10-a.json", one,
                                        strlen(one)),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_file_write_all(CFG_DIR "/20-b.json", two,
                                        strlen(two)),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_file_write_all(CFG_DIR "/notes.txt", "not json", 8),
                     NCL_OK);

    ncl_strbuf_init(&err);
    NCL_CHECK_EQ_INT(ncl_driver_manager_load_dir(manager, CFG_DIR, &err), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_driver_manager_count(manager), 2);
    NCL_CHECK_EQ_STR(ncl_driver_manager_id_at(manager, 0), "a");
    NCL_CHECK_EQ_STR(ncl_driver_manager_id_at(manager, 1), "b");
    NCL_CHECK(ncl_driver_manager_address_of(manager, "/A/X") != NULL);
    NCL_CHECK(ncl_driver_manager_address_of(manager, "/B/X") != NULL);
    ncl_strbuf_free(&err);

    NCL_TEST_CASE("a missing directory is reported, not fatal");
    ncl_strbuf_init(&err);
    NCL_CHECK_EQ_INT(ncl_driver_manager_load_dir(manager, "no-such-dir-here",
                                                 &err),
                     NCL_ERR_NOT_FOUND);
    NCL_CHECK(err.len > 0);
    ncl_strbuf_free(&err);

    NCL_TEST_CASE("a single file loads the same way");
    ncl_strbuf_init(&err);
    NCL_CHECK_EQ_INT(ncl_driver_manager_load_file(manager, CFG_DIR "/10-a.json",
                                                  &err),
                     NCL_ERR_EXISTS); /* the id is taken */
    NCL_CHECK(err.len > 0);
    NCL_CHECK_EQ_INT(ncl_driver_manager_count(manager), 2);
    ncl_strbuf_free(&err);
    ncl_driver_manager_free(manager);
}

static void test_bom(void)
{
    /* A UTF-8 byte order mark is what a Windows editor writes by default and
     * must not stop the adapter from starting. */
    static const char kWithBom[] = "\xEF\xBB\xBF"
                                   "{\"id\":\"bom\",\"path\":\"/BOM\","
                                   "\"type\":\"mock\"}";
    ncl_driver_manager *manager = ncl_driver_manager_create();
    ncl_strbuf err;

    NCL_TEST_CASE("a configuration file with a byte order mark still loads");
    NCL_CHECK(manager != NULL);
    /* Its own directory: the scan in the test above reads the top level. */
    NCL_CHECK_EQ_INT(ncl_mkdir_p(CFG_DIR "/bom"), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_file_write_all(CFG_DIR "/bom/bom.json", kWithBom,
                                        sizeof(kWithBom) - 1),
                     NCL_OK);
    ncl_strbuf_init(&err);
    NCL_CHECK_EQ_INT(ncl_driver_manager_load_file(manager,
                                                  CFG_DIR "/bom/bom.json", &err),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_driver_manager_count(manager), 1);
    NCL_CHECK_EQ_STR(ncl_driver_manager_id_at(manager, 0), "bom");
    ncl_strbuf_free(&err);
    ncl_driver_manager_free(manager);
}

static void test_bad_config(void)
{
    ncl_driver_manager *manager = ncl_driver_manager_create();
    ncl_strbuf err;
    ncl_json *document;

    NCL_TEST_CASE("a configuration that cannot be honoured says why");
    NCL_CHECK(manager != NULL);

    ncl_strbuf_init(&err);
    document = ncl_json_parse_cstr("{\"path\":\"/X\"}", NULL);
    NCL_CHECK_EQ_INT(ncl_driver_manager_add_json(manager, document, &err),
                     NCL_ERR_INVALID_ARG);
    NCL_CHECK(err.len > 0);
    ncl_json_free(document);
    ncl_strbuf_reset(&err);

    document = ncl_json_parse_cstr("{\"type\":\"no_such_protocol\"}", NULL);
    NCL_CHECK_EQ_INT(ncl_driver_manager_add_json(manager, document, &err),
                     NCL_ERR_NOT_FOUND);
    ncl_json_free(document);
    ncl_strbuf_reset(&err);

    document = ncl_json_parse_cstr(
        "{\"type\":\"mock\",\"points\":[{\"id\":\"X\",\"addr\":42}]}", NULL);
    NCL_CHECK_EQ_INT(ncl_driver_manager_add_json(manager, document, &err),
                     NCL_ERR_PARSE);
    ncl_json_free(document);
    ncl_strbuf_reset(&err);

    document = ncl_json_parse_cstr(
        "{\"type\":\"mock\",\"points\":[{\"id\":\"X\"}]}", NULL);
    NCL_CHECK_EQ_INT(ncl_driver_manager_add_json(manager, document, &err),
                     NCL_ERR_INVALID_ARG);
    ncl_json_free(document);
    ncl_strbuf_reset(&err);

    document = ncl_json_parse_cstr("{\"id\":\"dup\",\"type\":\"mock\"}", NULL);
    NCL_CHECK_EQ_INT(ncl_driver_manager_add_json(manager, document, &err),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_driver_manager_add_json(manager, document, &err),
                     NCL_ERR_EXISTS);
    ncl_json_free(document);
    ncl_strbuf_reset(&err);

    /* a failed entry must not be left half registered */
    NCL_CHECK_EQ_INT(ncl_driver_manager_count(manager), 1);
    ncl_strbuf_free(&err);
    ncl_driver_manager_free(manager);
}

NCL_TEST_MAIN_BEGIN()
    test_config();
    test_calls_and_events();
    test_files();
    test_bom();
    test_bad_config();
NCL_TEST_MAIN_END()
