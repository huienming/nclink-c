/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * A driver module, generation 1: it hands the host the FOCAS factory, which is
 * what the FANUC plugin used to do before that adapter moved over to a
 * declaration (clients/focas/focas_tool.c).
 *
 * It exists so the legacy path stays covered - a driver module plus the point
 * map in the configuration - while a site can already use the declared tool.
 * The FOCAS frames the test answers are the captured ones, so this also keeps
 * the driver honest against a fake machine.
 */
#include "nclink_adapter/ncl_module.h"

#include "focas/ncl_focas_driver.h"

/** "fanuc" is what a site is likely to write in a configuration. */
static const char *const kAliases[] = {"fanuc", NULL};

static const ncl_adapter_module_desc kModule = {
    NCL_ADAPTER_MODULE_ABI,
    "focas",
    "1.0.0-fixture",
    "FANUC FOCAS / Fwlib32 over TCP, read only (fixture driver module)",
    ncl_focas_create,
    kAliases,
};

/* The prototype (and the export) is in ncl_module.h. */
const ncl_adapter_module_desc *ncl_adapter_module(void)
{
    return &kModule;
}
