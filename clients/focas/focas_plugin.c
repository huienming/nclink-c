/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the FANUC FOCAS module.
 *
 * This is the whole module: it hands the host the driver factory and says what
 * it answers to. Building it produces
 *
 *     plugins/ncl_driver_focas.dll      (Windows)
 *     plugins/libncl_driver_focas.so    (Linux)
 *
 * which a device program picks up at start-up (see ncl_module.h), so "does this
 * box speak FANUC" is decided by what is in the plugin directory rather than
 * at build time.
 *
 * Two things it deliberately does *not* contain:
 *
 *   - the point map. Which FOCAS item goes to which model path is site data:
 *     it lives in the device configuration (conf/fanuc.json), where it can be
 *     edited without a compiler.
 *   - a registration call. The host owns the driver registry; a module only
 *     hands its factory over, because a module built against the static core
 *     has its own copy of that registry.
 */

#include "nclink_adapter/ncl_module.h"

#include "focas/ncl_focas_driver.h"

/** "fanuc" is what a site is likely to write in a configuration. */
static const char *const kAliases[] = {"fanuc", NULL};

static const ncl_adapter_module_desc kModule = {
    NCL_ADAPTER_MODULE_ABI,
    "focas",
    "1.0.0",
    "FANUC FOCAS / Fwlib32 over TCP, read only (01 册 §2.1-§2.3)",
    ncl_focas_create,
    kAliases,
};

/* The prototype (and the export) is in ncl_module.h. */
const ncl_adapter_module_desc *ncl_adapter_module(void)
{
    return &kModule;
}
