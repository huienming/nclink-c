/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * JSON Schema validation tests: the regular expression engine behind
 * `pattern`, every supported keyword, and the one shot entry points.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ncl_test.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_schema.h"

/* ------------------------------------------------------------ regex tests -- */

static void test_regex(void)
{
    static const struct {
        const char *pattern;
        const char *text;
        bool        matches;
    } cases[] = {
        {"abc", "xxabcyy", true},
        {"abc", "abd", false},
        {"^abc$", "abc", true},
        {"^abc$", "abcc", false},
        {"a.c", "abc", true},
        {"a.c", "ac", false},
        {"[0-9]+", "id-42", true},
        {"^[0-9]+$", "id-42", false},
        {"^[0-9]+$", "0042", true},
        {"[^0-9]", "12345", false},
        {"[^0-9]", "12a45", true},
        {"colou?r", "color", true},
        {"colou?r", "colour", true},
        {"ab*c", "ac", true},
        {"ab*c", "abbbc", true},
        {"ab+c", "ac", false},
        {"a{3}", "caaab", true},
        {"a{3}", "aab", false},
        {"a{2,3}", "aa", true},
        {"a{2,3}", "aaaa", true},   /* unanchored search finds "aaa" */
        {"^a{2,3}$", "aaaa", false},
        {"^a{2,3}$", "aaa", true},
        {"a{2,}", "aaaaa", true},
        {"^(cat|dog)$", "dog", true},
        {"^(cat|dog)$", "cow", false},
        {"^(a|b|c)+$", "abcab", true},
        {"^\\d{4}-\\d{2}-\\d{2}$", "2026-09-15", true},
        {"^\\d{4}-\\d{2}-\\d{2}$", "2026-9-15", false},
        {"^\\w+@\\w+\\.\\w+$", "a@b.c", true},
        {"\\bfoo\\b", "a foo b", true},
        {"\\bfoo\\b", "afoob", false},
        {"^(ab)+$", "ababab", true},
        {"^(ab)+$", "aba", false},
        {"(a|ab)c", "abc", true},
        {"^[A-Za-z_][A-Za-z0-9_]*$", "_sn123", true},
        {"^[A-Za-z_][A-Za-z0-9_]*$", "1sn", false},
        {"a{1,2}b", "aab", true},
        {"(?:xy)+", "xyxy", true},
    };
    size_t i;

    NCL_TEST_CASE("regular expression engine");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *error = NULL;
        ncl_regex *regex = ncl_regex_compile(cases[i].pattern, &error);
        NCL_CHECK(regex != NULL);
        if (regex != NULL) {
            bool got = ncl_regex_search(regex, cases[i].text,
                                        strlen(cases[i].text));
            if (got != cases[i].matches) {
                printf("      pattern=%s text=%s got=%d want=%d\n",
                       cases[i].pattern, cases[i].text, (int)got,
                       (int)cases[i].matches);
            }
            NCL_CHECK_EQ_INT(got, cases[i].matches);
            ncl_regex_free(regex);
        }
        ncl_free_safe(error);
    }

    NCL_TEST_CASE("unsupported expressions are rejected");
    {
        char *error = NULL;
        ncl_regex *regex = ncl_regex_compile("[a-", &error);
        NCL_CHECK(regex == NULL);
        NCL_CHECK(error != NULL);
        ncl_free_safe(error);
    }
}

/* ----------------------------------------------------------- schema tests -- */

static int count_errors(const char *json, const char *schema)
{
    ncl_strvec errors;
    int count;

    ncl_strvec_init(&errors);
    ncl_json_schema_validate(json, schema, &errors);
    count = (int)errors.len;
    if (count > 0) {
        size_t i;
        for (i = 0; i < errors.len; i++) {
            printf("        %s\n", ncl_strvec_at(&errors, i));
        }
    }
    ncl_strvec_free(&errors);
    return count;
}

static void test_keywords(void)
{
    NCL_TEST_CASE("type");
    NCL_CHECK_EQ_INT(count_errors("\"abc\"", "{\"type\":\"string\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("123", "{\"type\":\"string\"}"), 1);
    NCL_CHECK_EQ_INT(count_errors("123", "{\"type\":\"integer\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("1.5", "{\"type\":\"integer\"}"), 1);
    NCL_CHECK_EQ_INT(count_errors("1.5", "{\"type\":\"number\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("true", "{\"type\":\"boolean\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("null", "{\"type\":\"null\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("[]", "{\"type\":\"array\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("{}", "{\"type\":\"object\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("\"a\"", "{\"type\":[\"string\",\"null\"]}"), 0);
    NCL_CHECK_EQ_INT(count_errors("7", "{\"type\":[\"string\",\"null\"]}"), 1);

    NCL_TEST_CASE("required and additionalProperties");
    {
        const char *schema =
            "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},"
            "\"value\":{\"type\":\"string\"}},"
            "\"required\":[\"key\",\"value\"],\"additionalProperties\":false}";
        NCL_CHECK_EQ_INT(
            count_errors("{\"key\":\"a\",\"value\":\"b\"}", schema), 0);
        NCL_CHECK_EQ_INT(count_errors("{\"key\":\"a\"}", schema), 1);
        NCL_CHECK_EQ_INT(
            count_errors("{\"key\":\"a\",\"value\":\"b\",\"c\":1}", schema), 1);
        NCL_CHECK_EQ_INT(count_errors("{\"key\":1,\"value\":\"b\"}", schema), 1);
    }

    NCL_TEST_CASE("string keywords");
    NCL_CHECK_EQ_INT(count_errors("\"abc\"", "{\"minLength\":2}"), 0);
    NCL_CHECK_EQ_INT(count_errors("\"a\"", "{\"minLength\":2}"), 1);
    NCL_CHECK_EQ_INT(count_errors("\"abcd\"", "{\"maxLength\":3}"), 1);
    NCL_CHECK_EQ_INT(count_errors("\"42\"", "{\"pattern\":\"^[0-9]+$\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("\"4a\"", "{\"pattern\":\"^[0-9]+$\"}"), 1);

    NCL_TEST_CASE("number keywords");
    NCL_CHECK_EQ_INT(count_errors("5", "{\"minimum\":5}"), 0);
    NCL_CHECK_EQ_INT(count_errors("4", "{\"minimum\":5}"), 1);
    NCL_CHECK_EQ_INT(count_errors("5", "{\"maximum\":5}"), 0);
    NCL_CHECK_EQ_INT(count_errors("6", "{\"maximum\":5}"), 1);
    NCL_CHECK_EQ_INT(count_errors("5", "{\"exclusiveMinimum\":5}"), 1);
    NCL_CHECK_EQ_INT(count_errors("6", "{\"exclusiveMinimum\":5}"), 0);
    NCL_CHECK_EQ_INT(count_errors("10", "{\"multipleOf\":5}"), 0);
    NCL_CHECK_EQ_INT(count_errors("11", "{\"multipleOf\":5}"), 1);
    NCL_CHECK_EQ_INT(
        count_errors("5", "{\"minimum\":1,\"maximum\":10,\"multipleOf\":5}"), 0);

    NCL_TEST_CASE("array keywords");
    NCL_CHECK_EQ_INT(count_errors("[1,2]", "{\"minItems\":2}"), 0);
    NCL_CHECK_EQ_INT(count_errors("[1]", "{\"minItems\":2}"), 1);
    NCL_CHECK_EQ_INT(count_errors("[1,2,3]", "{\"maxItems\":2}"), 1);
    NCL_CHECK_EQ_INT(
        count_errors("[1,2]", "{\"items\":{\"type\":\"integer\"}}"), 0);
    NCL_CHECK_EQ_INT(
        count_errors("[1,\"a\"]", "{\"items\":{\"type\":\"integer\"}}"), 1);
    NCL_CHECK_EQ_INT(count_errors("[1,1]", "{\"uniqueItems\":true}"), 1);
    NCL_CHECK_EQ_INT(count_errors("[1,2]", "{\"uniqueItems\":true}"), 0);
    NCL_CHECK_EQ_INT(
        count_errors("[1,\"a\"]",
                     "{\"items\":[{\"type\":\"integer\"},{\"type\":\"string\"}]}"),
        0);
    NCL_CHECK_EQ_INT(
        count_errors("[\"a\",1]",
                     "{\"items\":[{\"type\":\"integer\"},{\"type\":\"string\"}]}"),
        2);

    NCL_TEST_CASE("enum and const");
    NCL_CHECK_EQ_INT(count_errors("\"ok\"", "{\"enum\":[\"ok\",\"ng\"]}"), 0);
    NCL_CHECK_EQ_INT(count_errors("\"xx\"", "{\"enum\":[\"ok\",\"ng\"]}"), 1);
    NCL_CHECK_EQ_INT(count_errors("\"ok\"", "{\"const\":\"ok\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("\"ng\"", "{\"const\":\"ok\"}"), 1);

    NCL_TEST_CASE("combinators");
    NCL_CHECK_EQ_INT(
        count_errors("5", "{\"allOf\":[{\"minimum\":1},{\"maximum\":10}]}"), 0);
    NCL_CHECK_EQ_INT(
        count_errors("50", "{\"allOf\":[{\"minimum\":1},{\"maximum\":10}]}"), 1);
    NCL_CHECK_EQ_INT(
        count_errors("\"a\"", "{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"integer\"}]}"),
        0);
    NCL_CHECK_EQ_INT(
        count_errors("true", "{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"integer\"}]}"),
        1);
    NCL_CHECK_EQ_INT(
        count_errors("5", "{\"oneOf\":[{\"type\":\"integer\"},{\"type\":\"string\"}]}"),
        0);
    NCL_CHECK_EQ_INT(
        count_errors("5", "{\"oneOf\":[{\"minimum\":1},{\"maximum\":10}]}"), 1);
    NCL_CHECK_EQ_INT(count_errors("5", "{\"not\":{\"type\":\"string\"}}"), 0);
    NCL_CHECK_EQ_INT(count_errors("\"a\"", "{\"not\":{\"type\":\"string\"}}"), 1);
    NCL_CHECK_EQ_INT(
        count_errors("{\"a\":1}",
                     "{\"if\":{\"required\":[\"a\"]},\"then\":{\"required\":[\"b\"]}}"),
        1);
    NCL_CHECK_EQ_INT(
        count_errors("{\"a\":1,\"b\":2}",
                     "{\"if\":{\"required\":[\"a\"]},\"then\":{\"required\":[\"b\"]}}"),
        0);
    NCL_CHECK_EQ_INT(
        count_errors("{}",
                     "{\"if\":{\"required\":[\"a\"]},\"else\":{\"required\":[\"c\"]}}"),
        1);

    NCL_TEST_CASE("$ref");
    {
        const char *schema =
            "{\"definitions\":{\"name\":{\"type\":\"string\",\"minLength\":1}},"
            "\"type\":\"object\",\"properties\":{\"name\":{\"$ref\":\"#/definitions/name\"}},"
            "\"required\":[\"name\"]}";
        NCL_CHECK_EQ_INT(count_errors("{\"name\":\"x\"}", schema), 0);
        NCL_CHECK_EQ_INT(count_errors("{\"name\":\"\"}", schema), 1);
        NCL_CHECK_EQ_INT(count_errors("{}", schema), 1);
    }
    NCL_CHECK_EQ_INT(
        count_errors("1", "{\"$ref\":\"#/definitions/missing\"}"), 1);

    NCL_TEST_CASE("format");
    NCL_CHECK_EQ_INT(
        count_errors("\"2026-09-15T08:00:00Z\"", "{\"format\":\"date-time\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("\"nope\"", "{\"format\":\"date-time\"}"), 1);
    NCL_CHECK_EQ_INT(count_errors("\"2026-09-15\"", "{\"format\":\"date\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("\"192.168.1.1\"", "{\"format\":\"ipv4\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("\"999.1.1.1\"", "{\"format\":\"ipv4\"}"), 1);
    NCL_CHECK_EQ_INT(count_errors("\"a@b.com\"", "{\"format\":\"email\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("\"a-b\"", "{\"format\":\"email\"}"), 1);
    NCL_CHECK_EQ_INT(
        count_errors("\"3f2504e0-4f89-11d3-9a0c-0305e82c3301\"",
                     "{\"format\":\"uuid\"}"), 0);
    NCL_CHECK_EQ_INT(count_errors("\"whatever\"", "{\"format\":\"unknown\"}"), 0);
}

static void test_entry_points(void)
{
    NCL_TEST_CASE("validate a JSON document against a schema");
    {
        ncl_strvec errors;

        ncl_strvec_init(&errors);
        NCL_CHECK_EQ_INT(
            ncl_json_schema_validate(
                "{\"name\":\"plc\"}",
                "{\"type\":\"object\",\"required\":[\"name\"]}", &errors),
            NCL_OK);
        NCL_CHECK_EQ_INT(errors.len, 0);

        ncl_strvec_clear(&errors);
        NCL_CHECK_EQ_INT(
            ncl_json_schema_validate(
                "{\"other\":1}",
                "{\"type\":\"object\",\"required\":[\"name\"]}", &errors),
            NCL_OK);
        NCL_CHECK_EQ_INT(errors.len, 1);
        NCL_CHECK(strstr(ncl_strvec_at(&errors, 0), "name") != NULL);
        ncl_strvec_free(&errors);
    }

    NCL_TEST_CASE("a broken document is reported, not thrown");
    {
        ncl_strvec errors;
        ncl_strvec_init(&errors);
        NCL_CHECK_EQ_INT(
            ncl_json_schema_validate("{not json", "{\"type\":\"object\"}",
                                     &errors),
            NCL_ERR_PARSE);
        NCL_CHECK_EQ_INT(errors.len, 1);
        NCL_CHECK(strstr(ncl_strvec_at(&errors, 0), "校验过程发生错误") != NULL);
        ncl_strvec_free(&errors);
    }

    NCL_TEST_CASE("a broken schema is reported, not thrown");
    {
        ncl_strvec errors;
        ncl_strvec_init(&errors);
        NCL_CHECK_EQ_INT(ncl_json_schema_validate("{}", "{oops", &errors),
                         NCL_ERR_PARSE);
        NCL_CHECK_EQ_INT(errors.len, 1);
        ncl_strvec_free(&errors);
    }

    NCL_TEST_CASE("the error list renders as [a, b]");
    {
        ncl_strvec errors;
        char *text;
        ncl_strvec_init(&errors);
        ncl_strvec_push(&errors, "a");
        ncl_strvec_push(&errors, "b");
        text = ncl_schema_join_errors(&errors);
        NCL_CHECK_EQ_STR(text, "[a, b]");
        ncl_free_safe(text);
        ncl_strvec_free(&errors);
    }

    NCL_TEST_CASE("an NC-Link parameter schema");
    {
        /* The shape a tool method declares for the file tool's write(). */
        const char *schema =
            "{\"type\":\"object\","
            "\"properties\":{\"key\":{\"type\":\"string\",\"minLength\":1},"
            "\"value\":{\"type\":\"string\"},"
            "\"offset\":{\"type\":\"integer\",\"minimum\":0},"
            "\"length\":{\"type\":\"integer\",\"minimum\":0}},"
            "\"required\":[\"key\"]}";
        NCL_CHECK_EQ_INT(
            count_errors("{\"key\":\"/data/a.txt\",\"value\":\"x\"}", schema), 0);
        NCL_CHECK_EQ_INT(count_errors("{\"value\":\"x\"}", schema), 1);
        NCL_CHECK_EQ_INT(count_errors("{\"key\":\"\"}", schema), 1);
        NCL_CHECK_EQ_INT(
            count_errors("{\"key\":\"a\",\"offset\":-1}", schema), 1);
        NCL_CHECK_EQ_INT(
            count_errors("{\"key\":\"a\",\"offset\":\"1\"}", schema), 1);
    }
}

/* ==================================================================== main */

NCL_TEST_MAIN_BEGIN()

    test_regex();
    test_keywords();
    test_entry_points();

NCL_TEST_MAIN_END()
