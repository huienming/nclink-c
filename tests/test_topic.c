/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* Unit tests for the topic builders and the serial number extraction. */
#include "ncl_test.h"

#include "nclink/ncl_common.h"
#include "nclink/ncl_topic.h"

static void test_builders(void)
{
    char *t;

    NCL_TEST_CASE("request/response topic shapes");
    t = ncl_topic_query_request("V203243111F", NULL);
    NCL_CHECK_EQ_STR(t, "Query/Request/V203243111F");
    ncl_free_safe(t);

    t = ncl_topic_query_response("V203243111F", NULL);
    NCL_CHECK_EQ_STR(t, "Query/Response/V203243111F");
    ncl_free_safe(t);

    NCL_TEST_CASE("client id variant appends a second segment");
    t = ncl_topic_probe_query_request("V1", "client-9");
    NCL_CHECK_EQ_STR(t, "Probe/Query/Request/V1/client-9");
    ncl_free_safe(t);

    NCL_TEST_CASE("register topic is a constant");
    t = ncl_topic_register_request();
    NCL_CHECK_EQ_STR(t, "Register/Request");
    ncl_free_safe(t);

    NCL_TEST_CASE("ping / pong carry the serial number");
    t = ncl_topic_ping("V42");
    NCL_CHECK_EQ_STR(t, "Ping/V42");
    ncl_free_safe(t);
    t = ncl_topic_pong("V42");
    NCL_CHECK_EQ_STR(t, "Pong/V42");
    ncl_free_safe(t);

}

static void test_extract_sn(void)
{
    char *sn;

    NCL_TEST_CASE("ordinary topics end with the serial number");
    sn = ncl_topic_extract_sn("Query/Response/V203243111F");
    NCL_CHECK_EQ_STR(sn, "V203243111F");
    ncl_free_safe(sn);

    sn = ncl_topic_extract_sn("Query/Response/V203243111F/extra");
    NCL_CHECK_EQ_STR(sn, "extra");
    ncl_free_safe(sn);

    NCL_TEST_CASE("Sample topics carry the serial number before the last segment");
    sn = ncl_topic_extract_sn("Sample//V203243111F/DATA@01");
    NCL_CHECK_EQ_STR(sn, "V203243111F");
    ncl_free_safe(sn);

    NCL_TEST_CASE("degenerate topics");
    NCL_CHECK(ncl_topic_extract_sn("") == NULL);
    NCL_CHECK(ncl_topic_extract_sn(NULL) == NULL);
    sn = ncl_topic_extract_sn("Register/Request");
    NCL_CHECK_EQ_STR(sn, "Request");
    ncl_free_safe(sn);
}

NCL_TEST_MAIN_BEGIN()
    test_builders();
    test_extract_sn();
NCL_TEST_MAIN_END()
