/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * The dynamic library loader: the seam a device program uses to pick up its
 * vendor protocol code at run time.
 *
 * The fixture module next to this test plays the part of an adapter, so the
 * success path is a real dlopen/LoadLibrary, and the two failures a site
 * actually sees - the file is not there, the symbol is not exported - are
 * checked for their message as well.
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/ncl_library.h"

#if defined(_WIN32) || defined(_WIN64)
#  define TEST_MODULE_SUFFIX ".dll"
#else
#  define TEST_MODULE_SUFFIX ".so"
#endif

typedef struct {
    unsigned    abi;
    int         value;
    const char *name;
} test_module_v1;

static void test_file_name(void)
{
    char *file;

    NCL_TEST_CASE("a protocol name becomes the platform's module file name");
    file = ncl_library_file_name("focas");
    NCL_CHECK(file != NULL);
#if defined(_WIN32) || defined(_WIN64)
    NCL_CHECK_EQ_STR(file, "ncl_driver_focas.dll");
#else
    NCL_CHECK_EQ_STR(file, "libncl_driver_focas.so");
#endif
    ncl_free_safe(file);

    NCL_TEST_CASE("a name that is already a file is left alone");
    file = ncl_library_file_name("plugins/ncl_driver_focas.dll");
    NCL_CHECK_EQ_STR(file, "plugins/ncl_driver_focas.dll");
    ncl_free_safe(file);
    NCL_CHECK(ncl_library_name_is_file("plugins\\ncl_driver_focas.dll"));
    NCL_CHECK(ncl_library_name_is_file("libncl_driver_focas.so"));
    NCL_CHECK(!ncl_library_name_is_file("focas"));
    NCL_CHECK(!ncl_library_name_is_file(""));
    NCL_CHECK(ncl_library_file_name(NULL) == NULL);
}

static void test_missing_module(void)
{
    char *error = NULL;
    ncl_library *library;

    NCL_TEST_CASE("a module that is not there fails with a reason");
    library = ncl_library_open("definitely/not/a/module" TEST_MODULE_SUFFIX,
                               &error);
    NCL_CHECK(library == NULL);
    NCL_CHECK(error != NULL && error[0] != '\0');
    if (error != NULL) {
        printf("      message: %s\n", error);
    }
    ncl_free_safe(error);

    NCL_TEST_CASE("an empty path is refused before the platform is asked");
    NCL_CHECK(ncl_library_open("", &error) == NULL);
    NCL_CHECK(error != NULL);
    ncl_free_safe(error);
}

static void test_load_fixture(void)
{
    /* CMake hands the module directory over with forward slashes on every
     * platform, and both LoadLibrary and dlopen accept them. */
    const char *dir = NCL_TEST_MODULE_DIR;
    char path[4096];
    char *error = NULL;
    ncl_library *library;
    const test_module_v1 *(*entry)(void);
    const test_module_v1 *module;
    int (*answer)(void);

    snprintf(path, sizeof(path), "%s/ncl_test_module" TEST_MODULE_SUFFIX, dir);

    NCL_TEST_CASE("the fixture module loads");
    library = ncl_library_open(path, &error);
    if (library == NULL) {
        printf("      cannot load %s: %s\n", path, error != NULL ? error : "?");
    }
    NCL_CHECK(library != NULL);
    if (library == NULL) {
        ncl_free_safe(error);
        return;
    }
    NCL_CHECK_EQ_STR(ncl_library_path(library), path);

    NCL_TEST_CASE("its entry point resolves and carries what it should");
    entry = (const test_module_v1 *(*)(void))ncl_library_symbol(
        library, "test_module_entry");
    NCL_CHECK(entry != NULL);
    if (entry != NULL) {
        module = entry();
        NCL_CHECK(module != NULL);
        if (module != NULL) {
            NCL_CHECK_EQ_INT(module->abi, 1u);
            NCL_CHECK_EQ_INT(module->value, 4242);
            NCL_CHECK_EQ_STR(module->name, "fixture");
        }
    }

    NCL_TEST_CASE("a plain function resolves too");
    answer = (int (*)(void))ncl_library_symbol(library, "test_module_answer");
    NCL_CHECK(answer != NULL);
    if (answer != NULL) {
        NCL_CHECK_EQ_INT(answer(), 42);
    }

    NCL_TEST_CASE("a symbol the module does not export is NULL, not a crash");
    NCL_CHECK(ncl_library_symbol(library, "no_such_symbol") == NULL);
    NCL_CHECK(ncl_library_symbol(library, "") == NULL);

    ncl_library_close(library);
    ncl_free_safe(error);

    NCL_TEST_CASE("closing twice over NULL is safe");
    ncl_library_close(NULL);
}

NCL_TEST_MAIN_BEGIN()
    test_file_name();
    test_missing_module();
    test_load_fixture();
NCL_TEST_MAIN_END()
