/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - vendor adapters as loadable modules.
 *
 * A device program is one NC-Link server: `ncl_server` plus the transports and
 * the endpoints the host brings up around it. The vendor side - FANUC over
 * FOCAS, Modbus, MC, ... - is not compiled into that program: it arrives as a
 * module the host loads at start-up, and the point map lives in the
 * configuration file next to it. So "add a brand" is "drop a module into
 * plugins/ and name it in the configuration":
 *
 *     ncl_adapter -c conf/fanuc.json            # plugins/ + "plugins": ["focas"]
 *     ncl_adapter -c conf/fanuc.json --plugin-dir plugins --plugins
 *
 * What a module exports (this is the whole ABI):
 *
 *     const ncl_adapter_module_desc *ncl_adapter_module(void);  // NCL_ADAPTER_MODULE_ENTRY
 *
 * and the struct it returns carries the driver factory. The *host* registers
 * that factory into its own driver registry, which is why a module never
 * touches global state of its own: the registry, the logger and the point map
 * all stay on the host's side of the boundary.
 *
 * Two consequences of that split are worth knowing before writing one:
 *
 *   - a module is built against the static core, so it carries its own copy of
 *     the small helpers (ncl_mem_*, ncl_json_*, the logger). With the default
 *     heap build that is harmless - both sides allocate from the C runtime
 *     heap. With NCL_STATIC_MEM it is not: two pools cannot free each other's
 *     blocks, so plugin modules and the static-memory build do not mix.
 *   - the module must not call ncl_driver_register_protocol() itself (its copy
 *     of the registry is not the host's); it hands the factory over and the
 *     host registers it. ncl_module.h's loader does that.
 */
#ifndef NCL_MODULE_H
#define NCL_MODULE_H

#include <stdbool.h>
#include <stddef.h>

#include "nclink/ncl_json.h"
#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/** ABI generation of the module descriptor. A host refuses anything else. */
#define NCL_ADAPTER_MODULE_ABI 1u

/* The entry point is a function named ncl_adapter_module(), so the struct it
 * returns cannot carry that name as well - in C a typedef and a function share
 * one namespace (MSVC: error C2365). The descriptor is therefore
 * ncl_adapter_module_desc; the *symbol* keeps the documented name. */

/** Entry point every module exports (a function returning the struct below). */
#define NCL_ADAPTER_MODULE_ENTRY "ncl_adapter_module"

#if defined(_WIN32) || defined(_WIN64)
#  define NCL_MODULE_EXPORT __declspec(dllexport)
#else
#  define NCL_MODULE_EXPORT __attribute__((visibility("default")))
#endif

/** What a module tells the host about itself. */
typedef struct {
    unsigned    abi;         /**< NCL_ADAPTER_MODULE_ABI                    */
    const char *name;        /**< protocol name it answers to ("focas")     */
    const char *version;     /**< module version, for `--plugins`           */
    const char *description; /**< one line, for `--plugins`                 */
    /** The driver factory the host registers under @p name. Required. */
    ncl_driver *(*create)(void);
    /** Optional extra protocol names, NULL terminated ("fanuc", ...). */
    const char *const *aliases;
} ncl_adapter_module_desc;

/** What a module's entry point looks like: a static struct it hands over. */
typedef const ncl_adapter_module_desc *(*ncl_adapter_module_fn)(void);

/**
 * The entry point itself: a module defines exactly this function, and the host
 * finds it by that name (NCL_ADAPTER_MODULE_ENTRY). Declared - not defined -
 * here so a module's own definition has a prototype to match.
 */
NCL_MODULE_EXPORT const ncl_adapter_module_desc *ncl_adapter_module(void);

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

/**
 * Register every loaded module in this process's driver registry: its protocol
 * name (and aliases) become usable in a driver configuration from here on.
 */
ncl_err ncl_modules_register(ncl_module_set *set, ncl_strbuf *err);

size_t ncl_module_count(const ncl_module_set *set);
/** Protocol name of module @p index, or NULL. */
const char *ncl_module_name(const ncl_module_set *set, size_t index);
/** Version / description of module @p index, or NULL. */
const char *ncl_module_version(const ncl_module_set *set, size_t index);
const char *ncl_module_description(const ncl_module_set *set, size_t index);
/** File the module was loaded from, or NULL. */
const char *ncl_module_path(const ncl_module_set *set, size_t index);
/** True when module @p index has registered its protocol already. */
bool ncl_module_registered(const ncl_module_set *set, size_t index);

/** Unload every module (the factories are gone afterwards). */
void ncl_modules_free(ncl_module_set *set);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MODULE_H */
