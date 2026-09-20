/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * A module fixture whose ABI generation is from the future: the host must
 * refuse it with a message that names both numbers, instead of calling into a
 * struct it does not understand.
 */
#include "nclink_adapter/ncl_module.h"

static ncl_driver *bad_create(void)
{
    return NULL;
}

static const ncl_adapter_module_desc kModule = {
    99u, /* not NCL_ADAPTER_MODULE_ABI */
    "from-the-future",
    "9.9.9",
    "a module the host must refuse",
    bad_create,
    NULL,
};

/* The prototype (and the export) is in ncl_module.h. */
const ncl_adapter_module_desc *ncl_adapter_module(void)
{
    return &kModule;
}
