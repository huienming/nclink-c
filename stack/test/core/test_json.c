/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* Unit tests for the JSON DOM (parse / write / accessors). */
#include "ncl_test.h"

#include "nclink/ncl_json.h"

static ncl_json *parse_ok(const char *text)
{
    ncl_strbuf err;
    ncl_json *j;
    ncl_strbuf_init(&err);
    j = ncl_json_parse_cstr(text, &err);
    if (j == NULL) {
        printf("    unexpected parse failure: %s\n",
               ncl_strbuf_cstr(&err));
    }
    ncl_strbuf_free(&err);
    return j;
}

static void test_parse_and_write(void)
{
    ncl_json *j;
    char *out;

    NCL_TEST_CASE("compact round trip preserves key order");
    j = parse_ok("{\"@id\":\"m1\",\"ids\":[{\"id\":\"/a\",\"params\":{\"offset\":0}}]}");
    NCL_CHECK(j != NULL);
    out = ncl_json_write_string(j);
    NCL_CHECK_EQ_STR(out, "{\"@id\":\"m1\",\"ids\":[{\"id\":\"/a\",\"params\":{\"offset\":0}}]}");
    ncl_free_safe(out);
    ncl_json_free(j);

    NCL_TEST_CASE("whitespace and trailing space are tolerated");
    j = parse_ok("  { \"a\" : [ 1 , 2 ,\t3 ] }\n");
    NCL_CHECK(j != NULL);
    NCL_CHECK_EQ_INT(ncl_json_arr_len(ncl_json_obj_get(j, "a")), 3);
    ncl_json_free(j);

    NCL_TEST_CASE("the readable writer is the same document, only laid out");
    j = parse_ok("{\"id\":\"01\",\"devices\":[{\"type\":\"MACHINE\","
                 "\"ids\":[{\"id\":\"p0\"}]}],\"configs\":[],\"empty\":{}}");
    NCL_CHECK(j != NULL);
    out = ncl_json_write_pretty_string(j);
    NCL_CHECK_EQ_STR(
        out,
        "{\n"
        "  \"id\": \"01\",\n"
        "  \"devices\": [\n"
        "    {\n"
        "      \"type\": \"MACHINE\",\n"
        "      \"ids\": [\n"
        "        {\n"
        "          \"id\": \"p0\"\n"
        "        }\n"
        "      ]\n"
        "    }\n"
        "  ],\n"
        "  \"configs\": [],\n"
        "  \"empty\": {}\n"
        "}");
    ncl_free_safe(out);
    ncl_json_free(j);
}

static void test_numbers(void)
{
    ncl_json *j = parse_ok("{\"i\":42,\"d\":1.5,\"e\":1e3,\"neg\":-7}");
    long long iv = 0;
    double dv = 0.0;
    char *out;

    NCL_TEST_CASE("integer / double accessors");
    NCL_CHECK(j != NULL);
    NCL_CHECK(ncl_json_as_int(ncl_json_obj_get(j, "i"), &iv));
    NCL_CHECK_EQ_INT(iv, 42);
    NCL_CHECK(ncl_json_as_double(ncl_json_obj_get(j, "e"), &dv));
    NCL_CHECK(dv == 1000.0);
    NCL_CHECK(ncl_json_as_int(ncl_json_obj_get(j, "neg"), &iv));
    NCL_CHECK_EQ_INT(iv, -7);

    NCL_TEST_CASE("parsed numbers keep their original literal");
    out = ncl_json_write_string(j);
    NCL_CHECK_EQ_STR(out, "{\"i\":42,\"d\":1.5,\"e\":1e3,\"neg\":-7}");
    ncl_free_safe(out);
    ncl_json_free(j);

    NCL_TEST_CASE("built doubles keep the .0 suffix (1.0, not 1)");
    j = ncl_json_new_object();
    ncl_json_obj_set_double(j, "x", 1.0);
    ncl_json_obj_set_int(j, "y", 5);
    out = ncl_json_write_string(j);
    NCL_CHECK_EQ_STR(out, "{\"x\":1.0,\"y\":5}");
    ncl_free_safe(out);
    ncl_json_free(j);
}

static void test_strings_and_escapes(void)
{
    ncl_json *j;
    const char *s;
    char *out;

    NCL_TEST_CASE("escape decoding (\\n, \\u4e2d, surrogate pair)");
    j = parse_ok("{\"a\":\"line\\nbreak\",\"b\":\"\\u4e2d\\u6587\",\"c\":\"\\ud83d\\ude00\"}");
    NCL_CHECK(j != NULL);
    s = ncl_json_obj_get_string(j, "a");
    NCL_CHECK_EQ_STR(s, "line\nbreak");
    s = ncl_json_obj_get_string(j, "b");
    NCL_CHECK_EQ_STR(s, "\xe4\xb8\xad\xe6\x96\x87");
    s = ncl_json_obj_get_string(j, "c");
    NCL_CHECK_EQ_STR(s, "\xf0\x9f\x98\x80");

    NCL_TEST_CASE("writer escapes control characters only");
    out = ncl_json_write_string(j);
    NCL_CHECK(strstr(out, "\\n") != NULL);
    NCL_CHECK(strstr(out, "\\u4e2d") == NULL); /* raw UTF-8 on the wire */
    ncl_free_safe(out);
    ncl_json_free(j);
}

static void test_type_of_text(void)
{
    ncl_json *j = parse_ok("{\"n\":\"12\",\"b\":\"true\",\"o\":{}}");
    long long iv = 0;
    bool bv = false;
    char *text;

    NCL_TEST_CASE("numeric strings coerce to numbers");
    NCL_CHECK(j != NULL);
    NCL_CHECK(ncl_json_as_int(ncl_json_obj_get(j, "n"), &iv));
    NCL_CHECK_EQ_INT(iv, 12);
    NCL_CHECK(ncl_json_as_bool(ncl_json_obj_get(j, "b"), &bv));
    NCL_CHECK(bv == true);

    NCL_TEST_CASE("as_text renders scalars as text");
    text = ncl_json_as_text(ncl_json_obj_get(j, "n"));
    NCL_CHECK_EQ_STR(text, "12");
    ncl_free_safe(text);
    NCL_CHECK(ncl_json_as_text(ncl_json_obj_get(j, "o")) == NULL);
    ncl_json_free(j);
}

static void test_clone_equals(void)
{
    ncl_json *a = parse_ok("{\"k\":[1,2,{\"z\":null}],\"t\":\"v\"}");
    ncl_json *b;

    NCL_TEST_CASE("clone then deep equality");
    NCL_CHECK(a != NULL);
    b = ncl_json_clone(a);
    NCL_CHECK(b != NULL);
    NCL_CHECK(ncl_json_equals(a, b));
    ncl_json_obj_set_string(b, "t", "other");
    NCL_CHECK(!ncl_json_equals(a, b));
    ncl_json_free(a);
    ncl_json_free(b);
}

static void test_parse_errors(void)
{
    ncl_strbuf err;
    ncl_json *j;

    NCL_TEST_CASE("malformed input is rejected with a message");
    ncl_strbuf_init(&err);
    j = ncl_json_parse_cstr("{\"a\":1,}", &err);
    NCL_CHECK(j == NULL);
    NCL_CHECK(err.len > 0);
    ncl_strbuf_free(&err);

    ncl_strbuf_init(&err);
    j = ncl_json_parse_cstr("{\"a\":1} trailing", &err);
    NCL_CHECK(j == NULL);
    ncl_strbuf_free(&err);

    ncl_strbuf_init(&err);
    j = ncl_json_parse_cstr("", &err);
    NCL_CHECK(j == NULL);
    ncl_strbuf_free(&err);
}

static void test_object_mutation(void)
{
    ncl_json *j = ncl_json_new_object();
    char *out;

    NCL_TEST_CASE("set replaces in place, remove shifts down");
    ncl_json_obj_set_int(j, "a", 1);
    ncl_json_obj_set_int(j, "b", 2);
    ncl_json_obj_set_int(j, "a", 9);
    NCL_CHECK_EQ_INT(ncl_json_obj_len(j), 2);
    ncl_json_obj_remove(j, "b");
    NCL_CHECK_EQ_INT(ncl_json_obj_len(j), 1);
    NCL_CHECK_EQ_INT(ncl_json_obj_get_int(j, "a", 0), 9);
    out = ncl_json_write_string(j);
    NCL_CHECK_EQ_STR(out, "{\"a\":9}");
    ncl_free_safe(out);
    ncl_json_free(j);
}

NCL_TEST_MAIN_BEGIN()
    test_parse_and_write();
    test_numbers();
    test_strings_and_escapes();
    test_type_of_text();
    test_clone_equals();
    test_parse_errors();
    test_object_mutation();
NCL_TEST_MAIN_END()
