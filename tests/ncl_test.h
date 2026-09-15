/*
 * Tiny test harness for the NC-Link C port.
 * Header-only, no external dependencies, works with CTest.
 */
#ifndef NCL_TEST_H
#define NCL_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ncl_test_failures = 0;
static int ncl_test_checks = 0;
static const char *ncl_test_current = "";

#define NCL_TEST_CASE(name)                                                   \
    do {                                                                      \
        ncl_test_current = (name);                                            \
        printf("  - %s\n", ncl_test_current);                                 \
    } while (0)

#define NCL_CHECK(cond)                                                       \
    do {                                                                      \
        ncl_test_checks++;                                                    \
        if (!(cond)) {                                                        \
            ncl_test_failures++;                                              \
            printf("    FAIL %s:%d [%s] %s\n", __FILE__, __LINE__,            \
                   ncl_test_current, #cond);                                  \
        }                                                                     \
    } while (0)

#define NCL_CHECK_EQ_INT(actual, expected)                                    \
    do {                                                                      \
        long long ncl_a = (long long)(actual);                                \
        long long ncl_e = (long long)(expected);                              \
        ncl_test_checks++;                                                    \
        if (ncl_a != ncl_e) {                                                 \
            ncl_test_failures++;                                              \
            printf("    FAIL %s:%d [%s] %s == %s (got %lld, want %lld)\n",    \
                   __FILE__, __LINE__, ncl_test_current, #actual, #expected,  \
                   ncl_a, ncl_e);                                             \
        }                                                                     \
    } while (0)

#define NCL_CHECK_EQ_STR(actual, expected)                                    \
    do {                                                                      \
        const char *ncl_a = (actual);                                         \
        const char *ncl_e = (expected);                                       \
        ncl_test_checks++;                                                    \
        if (ncl_a == NULL || ncl_e == NULL || strcmp(ncl_a, ncl_e) != 0) {    \
            ncl_test_failures++;                                              \
            printf("    FAIL %s:%d [%s] %s == %s\n      got  \"%s\"\n"        \
                   "      want \"%s\"\n",                                     \
                   __FILE__, __LINE__, ncl_test_current, #actual, #expected,  \
                   ncl_a != NULL ? ncl_a : "(null)",                          \
                   ncl_e != NULL ? ncl_e : "(null)");                         \
        }                                                                     \
    } while (0)

#define NCL_TEST_MAIN_BEGIN()                                                 \
    int main(void)                                                            \
    {                                                                         \
        /* Unbuffered so a crash still shows how far the run got. */           \
        setvbuf(stdout, NULL, _IONBF, 0);                                      \
        printf("running %s\n", __FILE__);

#define NCL_TEST_MAIN_END()                                                   \
    printf("%s: %d checks, %d failures\n", __FILE__, ncl_test_checks,          \
           ncl_test_failures);                                                \
    return ncl_test_failures == 0 ? 0 : 1;                                    \
    }

#endif /* NCL_TEST_H */
