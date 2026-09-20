/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - vendor adapters as loadable modules.
 *
 * A device program is one NC-Link server: `ncl_server` plus the transports and
 * the endpoints the host brings up around it. The vendor side - FANUC over
 * FOCAS, Modbus, MC, ... - is not compiled into that program: it arrives as a
 * module the host loads at start-up. The points it serves are *in* that module
 * (nclink/ncl_tool.h), so "add a brand" is "drop a module into plugins/ and
 * name it in the configuration":
 *
 *     ncl_adapter -c conf/fanuc.json            # plugins/ + "plugins": ["focas"]
 *     ncl_adapter -c conf/fanuc.json --plugin-dir plugins --plugins
 *
 * What a module exports (this is the whole ABI):
 *
 *     const ncl_tool_module_desc *ncl_adapter_module(void);  // NCL_TOOL_MODULE_ENTRY
 *
 * - a structure whose `decl` field is the declaration (points, periods,
 *   open()/close()). The host builds the model, the sample channel and the
 *   bindings from it, so a module never touches global state of its own: the
 *   registry, the logger and the model all stay on the host's side.
 *
 * Two consequences are worth knowing before writing one:
 *
 *   - a module is built against the static core, so it carries its own copy of
 *     the small helpers (ncl_mem_*, ncl_json_*, the logger). With the default
 *     heap build that is harmless - both sides allocate from the C runtime
 *     heap. With NCL_STATIC_MEM it is not: two pools cannot free each other's
 *     blocks, so plugin modules and the static-memory build do not mix.
 *   - the old shape - a module handing the host a *driver factory* to register
 *     (ABI generation 1) - is refused, with a message that says so: a driver
 *     module is rewritten as a declaration (see clients/focas/focas_tool.c for
 *     a worked example).
 */
#ifndef NCL_MODULE_H
#define NCL_MODULE_H

#include <stdbool.h>
#include <stddef.h>

#include "nclink/ncl_tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ host -- */

typedef struct ncl_module_set ncl_module_set;

/** An empty set. Never NULL (an empty set is a valid "no modules" answer). */
ncl_module_set *ncl_modules_create(void);

/**
 * Load one module. @p name_or_file is either a module file ("ncl_driver_focas
 * .dll", "plugins/x.so") or a protocol name ("focas"), which is completed with
 * the platform's file name and looked up in @p dir.
 *
 * A module that is already in the set (same protocol name) is skipped, so
 * "auto" plus an explicit list is not an error. On failure a diagnostic is
 * appended to @p err.
 */
ncl_err ncl_modules_add(ncl_module_set *set, const char *name_or_file,
                        const char *dir, ncl_strbuf *err);

/**
 * Load every module file in @p dir that is not loaded yet. A directory that
 * does not exist is not an error: that is simply "no modules".
 * *loaded, when not NULL, receives how many were loaded here.
 */
ncl_err ncl_modules_add_dir(ncl_module_set *set, const char *dir, size_t *loaded,
                            ncl_strbuf *err);

/**
 * Apply the "plugins" section of a configuration: load what it names and, when
 * it asks for it, everything the directory holds. Modules already in the set
 * are skipped, so a hand written list next to "auto" is not an error.
 *
 * Accepted shapes:
 *
 *   "plugins": ["focas", "ncl_driver_mc.dll"]
 *   "plugins": { "dir": "plugins", "load": ["focas"], "auto": true }
 *   no "plugins" section at all: @p default_dir, loaded automatically
 *
 * Diagnostics for a module that could not be loaded are appended to @p err;
 * an explicitly named module that fails is an error, a stray file caught by
 * the directory scan is not (it is reported and skipped).
 */
ncl_err ncl_modules_add_config(ncl_module_set *set, const ncl_json *config,
                               const char *default_dir, ncl_strbuf *err);

/** Directory the "plugins" section asks for, or @p default_dir. Heap string. */
char *ncl_modules_dir_from_config(const ncl_json *config,
                                  const char *default_dir);


size_t ncl_module_count(const ncl_module_set *set);
/** Tool name of module @p index, or NULL. */
const char *ncl_module_name(const ncl_module_set *set, size_t index);
/** ABI generation of module @p index (0 when the index is out of range). */
unsigned ncl_module_abi(const ncl_module_set *set, size_t index);
/**
 * Declaration of the module at @p index (never NULL for a loaded module).
 * Borrowed; it lives in the module.
 */
const ncl_tool_decl *ncl_module_tool(const ncl_module_set *set, size_t index);
/** Version / description of module @p index, or NULL. */
const char *ncl_module_version(const ncl_module_set *set, size_t index);
const char *ncl_module_description(const ncl_module_set *set, size_t index);
/** File the module was loaded from, or NULL. */
const char *ncl_module_path(const ncl_module_set *set, size_t index);

/** Unload every module (the declarations are gone afterwards). */
void ncl_modules_free(ncl_module_set *set);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MODULE_H */