/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * A module fixture whose ABI generation the host does not know: the loader must
 * refuse it with a message that names the number it saw, instead of reading a
 * struct whose layout it cannot trust.
 *
 * It also stands in for the old shape - a module that hands over a driver
 * factory, which used to be generation 1. That one gets its own hint in the
 * message ("老式驱动模块请改写成声明式适配器").
 *
 * The declaration below is local on purpose: the fixture must not depend on any
 * header the host could change under it, because its whole point is to be
 * unreadable to the host.
 */
#include <stddef.h>

#if defined(_WIN32) || defined(_WIN64)
#  define FIXTURE_EXPORT __declspec(dllexport)
#else
#  define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct {
    unsigned    abi;
    const char *name;
    const char *version;
    const char *description;
    void       *payload;
} fixture_module_desc;

static const fixture_module_desc kModule = {
    99u, /* a generation from the future */
    "from-the-future",
    "9.9.9",
    "a module the host must refuse",
    NULL,
};

FIXTURE_EXPORT const fixture_module_desc *ncl_adapter_module(void);

FIXTURE_EXPORT const fixture_module_desc *ncl_adapter_module(void)
{
    return &kModule;
}
