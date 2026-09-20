/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 huienming
 *
 * NC-Link core - dynamic libraries (see ncl_library.h).
 *
 * Two rules keep a module load predictable at a site:
 *
 *   1. the path is always resolved by the caller (host: the plugin directory
 *      from the configuration) - never left to the loader's search order, so
 *      "which DLL did it pick" is never a question;
 *   2. the platform's own error text travels back with the failure. A module
 *      that will not load is usually a missing C++ runtime or a 32/64 bit
 *      mismatch, and that is exactly what the message says.
 */

#include "nclink/ncl_library.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

struct ncl_library {
    char *path;  /**< owned, what was asked for */
#if defined(_WIN32) || defined(_WIN64)
    HMODULE handle;
#else
    void *handle;
#endif
};

/** "ncl_driver_focas.dll" / "libncl_driver_focas.so" for the protocol "focas". */
#define NCL_LIBRARY_PREFIX "ncl_driver_"

#if defined(_WIN32) || defined(_WIN64)
#  define NCL_LIBRARY_SUFFIX ".dll"
#else
#  define NCL_LIBRARY_SUFFIX ".so"
#endif

static char *platform_error_text(void)
{
#if defined(_WIN32) || defined(_WIN64)
    DWORD code = GetLastError();
    char *message = NULL;
    DWORD length;

    length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                FORMAT_MESSAGE_FROM_SYSTEM |
                                FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL, code, 0, (LPSTR)&message, 0, NULL);
    if (length > 0 && message != NULL) {
        char *copy;
        size_t i;

        /* The system text ends with CRLF; trim it. */
        for (i = strlen(message); i > 0; i--) {
            if (message[i - 1] == '\r' || message[i - 1] == '\n' ||
                message[i - 1] == ' ') {
                message[i - 1] = '\0';
            } else {
                break;
            }
        }
        if (ncl_asprintf(&copy, "%s (code %lu)", message, (unsigned long)code) ==
            NCL_OK) {
            LocalFree(message);
            return copy;
        }
        LocalFree(message);
        return NULL;
    }
    if (message != NULL) {
        LocalFree(message);
    }
    {
        char *text = NULL;

        if (ncl_asprintf(&text, "LoadLibrary failed (code %lu)",
                         (unsigned long)code) == NCL_OK) {
            return text;
        }
    }
    return NULL;
#else
    const char *text = dlerror();

    return text != NULL ? ncl_strdup(text) : NULL;
#endif
}

ncl_library *ncl_library_open(const char *path, char **error)
{
    ncl_library *library;

    if (error != NULL) {
        *error = NULL;
    }
    if (ncl_str_is_blank(path)) {
        if (error != NULL) {
            *error = ncl_strdup("empty module path");
        }
        return NULL;
    }
    library = (ncl_library *)ncl_mem_calloc(1, sizeof(*library));
    if (library == NULL) {
        return NULL;
    }
    library->path = ncl_strdup(path);
    if (library->path == NULL) {
        ncl_mem_free(library);
        return NULL;
    }
#if defined(_WIN32) || defined(_WIN64)
    library->handle = LoadLibraryA(path);
    if (library->handle == NULL) {
#else
    library->handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (library->handle == NULL) {
#endif
        if (error != NULL) {
            char *reason = platform_error_text();

            if (reason != NULL) {
                (void)ncl_asprintf(error, "%s: %s", path, reason);
                ncl_free_safe(reason);
            } else {
                (void)ncl_asprintf(error, "%s: cannot load the module", path);
            }
        }
        ncl_mem_free(library->path);
        ncl_mem_free(library);
        return NULL;
    }
    return library;
}

void *ncl_library_symbol(const ncl_library *library, const char *name)
{
    if (library == NULL || ncl_str_is_blank(name)) {
        return NULL;
    }
#if defined(_WIN32) || defined(_WIN64)
    return (void *)GetProcAddress(library->handle, name);
#else
    return dlsym(library->handle, name);
#endif
}

void ncl_library_close(ncl_library *library)
{
    if (library == NULL) {
        return;
    }
    if (library->handle != NULL) {
#if defined(_WIN32) || defined(_WIN64)
        FreeLibrary(library->handle);
#else
        dlclose(library->handle);
#endif
    }
    ncl_mem_free(library->path);
    ncl_mem_free(library);
}

const char *ncl_library_path(const ncl_library *library)
{
    return library != NULL ? library->path : NULL;
}

bool ncl_library_name_is_file(const char *name)
{
    static const char *const suffixes[] = { ".dll", ".so", ".dylib" };
    size_t i;

    if (ncl_str_is_blank(name)) {
        return false;
    }
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        return true;
    }
    /* Any of the three suffixes counts on every platform: a configuration that
     * spells the file out ("focas.dll") means that file, and it should not be
     * rewritten into "libfocas.dll.so" on the way. */
    for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if (strstr(name, suffixes[i]) != NULL) {
            return true;
        }
    }
    return false;
}

char *ncl_library_file_name(const char *name)
{
    char *file = NULL;

    if (ncl_str_is_blank(name)) {
        return NULL;
    }
    if (ncl_library_name_is_file(name)) {
        return ncl_strdup(name);
    }
#if defined(_WIN32) || defined(_WIN64)
    if (ncl_asprintf(&file, NCL_LIBRARY_PREFIX "%s" NCL_LIBRARY_SUFFIX, name) !=
        NCL_OK) {
        return NULL;
    }
#else
    if (ncl_asprintf(&file, "lib" NCL_LIBRARY_PREFIX "%s" NCL_LIBRARY_SUFFIX,
                     name) != NCL_OK) {
        return NULL;
    }
#endif
    return file;
}
