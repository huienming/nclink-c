/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * A module fixture that exports no entry point at all: a stray DLL next to the
 * real modules, and the loader must say so ("未导出 ncl_adapter_module()")
 * rather than treat it as a module that has nothing to offer.
 */
#include <stddef.h>

/* Declared first: a stray DLL next to the real modules exports something, just
 * not the module entry point. */
int ncl_test_not_a_module(void);

int ncl_test_not_a_module(void)
{
    return 7;
}
