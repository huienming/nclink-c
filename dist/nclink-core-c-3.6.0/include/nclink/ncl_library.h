/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - dynamic libraries.
 *
 * A device program assembles itself at run time: the core library and the
 * adapter host are one program, and the vendor protocol code arrives as a
 * module that is loaded on start-up. This is the small platform seam that
 * makes that possible - `LoadLibrary`/`GetProcAddress` on Windows,
 * `dlopen`/`dlsym` elsewhere - plus the file name convention a module is
 * looked up by.
 *
 * What a module contains and how its entry point is called is not a core
 * concern: the adapter layer defines that ABI (nclink/ncl_module.h).
 */
#ifndef NCL_LIBRARY_H
#define NCL_LIBRARY_H

#include "nclink/ncl_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ncl_library ncl_library;

/**
 * Load the dynamic library in @p path.
 *
 * On failure NULL comes back and, when @p error is not NULL, *error receives a
 * heap message the caller releases with ncl_free_safe(). The message carries
 * the platform's own complaint (a missing file, a wrong architecture, a
 * missing dependency), because that is what a site needs to see.
 */
ncl_library *ncl_library_open(const char *path, char **error);

/** Address of the exported symbol @p name, or NULL. */
void *ncl_library_symbol(const ncl_library *library, const char *name);

/** Release the library. NULL is accepted. */
void ncl_library_close(ncl_library *library);

/** Path this library was loaded from (borrowed), or NULL. */
const char *ncl_library_path(const ncl_library *library);

/**
 * Platform file name of a module: "focas" becomes "ncl_driver_focas.dll" on
 * Windows and "libncl_driver_focas.so" on Linux/macOS.
 *
 * A name that already looks like a file (it contains a path separator or the
 * platform's module suffix) is returned unchanged, so a configuration may give
 * either a protocol name or the module's file name.
 *
 * Returns a heap string (ncl_free_safe()), or NULL.
 */
char *ncl_library_file_name(const char *name);

/** True when @p name already looks like a module file name. */
bool ncl_library_name_is_file(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* NCL_LIBRARY_H */
