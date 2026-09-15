/* Unit tests for ncl_common (error names, strings, strbuf, ptrvec). */
#include "ncl_test.h"

#include "nclink/ncl_common.h"

static void test_error_names(void)
{
    NCL_TEST_CASE("error names are stable");
    NCL_CHECK_EQ_STR(ncl_err_name(NCL_OK), "OK");
    NCL_CHECK_EQ_STR(ncl_err_name(NCL_ERR_INVALID_MESSAGE), "InvalidMessageException");
    NCL_CHECK_EQ_STR(ncl_err_name(NCL_ERR_TIMEOUT), "TimeoutException");
}

static void test_strings(void)
{
    char *t;

    NCL_TEST_CASE("trim / blank / prefix helpers");
    t = ncl_str_trim_dup("  hello \r\n");
    NCL_CHECK_EQ_STR(t, "hello");
    free(t);
    NCL_CHECK(ncl_str_is_blank(" \t\r\n"));
    NCL_CHECK(!ncl_str_is_blank(" x "));
    NCL_CHECK(ncl_str_is_blank(NULL));
    NCL_CHECK(ncl_str_starts_with("Query/Response/V1", "Query/Response/"));
    NCL_CHECK(!ncl_str_starts_with("Query/Request/V1", "Query/Response/"));
    NCL_CHECK(ncl_str_ends_with("a.txt", ".txt"));

    NCL_TEST_CASE("case-insensitive compare matches compareToIgnoreCase");
    NCL_CHECK(ncl_streq_ignore_case("ok", "OK"));
    NCL_CHECK(!ncl_streq_ignore_case("ok", "NG"));
    NCL_CHECK(!ncl_streq_ignore_case(NULL, "NG"));
}

static void test_asprintf(void)
{
    char *s = NULL;
    NCL_TEST_CASE("asprintf allocates and formats");
    NCL_CHECK_EQ_INT(ncl_asprintf(&s, "%s/%d", "Ping", 7), NCL_OK);
    NCL_CHECK_EQ_STR(s, "Ping/7");
    free(s);
}

static void test_strbuf(void)
{
    ncl_strbuf sb;
    char *detached;

    NCL_TEST_CASE("strbuf grows and appends mixed content");
    ncl_strbuf_init(&sb);
    ncl_strbuf_puts(&sb, "abc");
    ncl_strbuf_putc(&sb, '-');
    ncl_strbuf_printf(&sb, "%d-%s", 42, "x");
    NCL_CHECK_EQ_STR(ncl_strbuf_cstr(&sb), "abc-42-x");
    ncl_strbuf_reset(&sb);
    NCL_CHECK_EQ_INT(sb.len, 0);
    ncl_strbuf_puts(&sb, "keep");
    detached = ncl_strbuf_detach(&sb);
    NCL_CHECK_EQ_STR(detached, "keep");
    NCL_CHECK_EQ_INT(sb.len, 0);
    free(detached);
    ncl_strbuf_free(&sb);
}

static void test_ptrvec(void)
{
    ncl_ptrvec v;
    int *a = (int *)malloc(sizeof(int));
    int *b = (int *)malloc(sizeof(int));

    *a = 1;
    *b = 2;

    NCL_TEST_CASE("ptrvec keeps order and frees through free_fn");
    ncl_ptrvec_init(&v, free);
    NCL_CHECK_EQ_INT(ncl_ptrvec_push(&v, a), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_ptrvec_push(&v, b), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_ptrvec_len(&v), 2);
    NCL_CHECK_EQ_INT(*(int *)ncl_ptrvec_at(&v, 0), 1);
    NCL_CHECK_EQ_INT(*(int *)ncl_ptrvec_at(&v, 1), 2);

    NCL_TEST_CASE("take transfers ownership out of the vector");
    {
        int *taken = (int *)ncl_ptrvec_take(&v, 0);
        NCL_CHECK_EQ_INT(*taken, 1);
        NCL_CHECK_EQ_INT(ncl_ptrvec_len(&v), 1);
        free(taken);
    }
    ncl_ptrvec_free(&v);
}

NCL_TEST_MAIN_BEGIN()
    test_error_names();
    test_strings();
    test_asprintf();
    test_strbuf();
    test_ptrvec();
NCL_TEST_MAIN_END()
