/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * A fixture module for tests/test_library.c: it stands in for a vendor
 * adapter, so the loader is exercised against a real dynamic library (and its
 * failure paths) without dragging the adapter layer in.
 *
 * It deliberately exports just two symbols: "the entry point returns a struct"
 * is the shape the adapter ABI uses, and the second one proves a plain
 * function pointer resolves too.
 */
#include <stddef.h>

/* A fixture also has to behave like a real module where it matters: on Windows
 * nothing is exported unless it says so. */
#if defined(_WIN32) || defined(_WIN64)
#  define TEST_MODULE_EXPORT __declspec(dllexport)
#else
#  define TEST_MODULE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct {
    unsigned    abi;
    int         value;
    const char *name;
} test_module_v1;

static const test_module_v1 kModule = {1u, 4242, "fixture"};

TEST_MODULE_EXPORT const test_module_v1 *test_module_entry(void);
TEST_MODULE_EXPORT int test_module_answer(void);

TEST_MODULE_EXPORT const test_module_v1 *test_module_entry(void)
{
    return &kModule;
}

TEST_MODULE_EXPORT int test_module_answer(void)
{
    return 42;
}
