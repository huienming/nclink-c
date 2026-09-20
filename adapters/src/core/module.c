/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - loading vendor adapters (see ncl_module.h).
 *
 * The host keeps one set of loaded modules for the life of the process: the
 * libraries stay open (their factories are function pointers into them), and
 * registering a module is a one way door - the driver registry is global and
 * has no "unregister". So the order is always
 *
 *     load (files) -> register (protocols) -> build the device from config
 *
 * and a module that fails to load is reported with the platform's own reason
 * instead of a silent "no driver for protocol focas".
 */

#include "nclink_adapter/ncl_module.h"

#include <stdio.h>
#include <string.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_library.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"

#if defined(_WIN32) || defined(_WIN64)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dirent.h>
#endif

/**
 * Both generations start with the ABI word, so the loader can tell them apart
 * before it knows which struct it is looking at.
 */
typedef const unsigned *(*ncl_module_probe_fn)(void);

typedef struct {
    ncl_library              *library;
    const ncl_adapter_module_desc *module; /**< borrowed: it lives in @p library */
    const ncl_tool_module_desc    *tool;   /**< generation 2, NULL for a driver */
    unsigned                  abi;     /**< 1 = driver factory, 2 = tool declaration */
    char                     *path;   /**< owned */
    bool                      registered;
} ncl_loaded_module;

struct ncl_module_set {
    ncl_loaded_module *items;
    size_t             count;
    size_t             capacity;
};

#define NCL_MODULE_SUFFIX_DLL ".dll"
#define NCL_MODULE_SUFFIX_SO ".so"

static void err_append(ncl_strbuf *err, const char *text)
{
    if (err == NULL || text == NULL) {
        return;
    }
    if (err->len > 0) {
        (void)ncl_strbuf_puts(err, "; ");
    }
    (void)ncl_strbuf_puts(err, text);
}

static void err_append_module(ncl_strbuf *err, const char *what,
                              const char *path)
{
    if (err == NULL) {
        return;
    }
    if (err->len > 0) {
        (void)ncl_strbuf_puts(err, "; ");
    }
    (void)ncl_strbuf_printf(err, "%s：%s", what, path);
}

ncl_module_set *ncl_modules_create(void)
{
    return (ncl_module_set *)ncl_mem_calloc(1, sizeof(ncl_module_set));
}

/** Name of the module at @p entry, whichever generation it is. */
static const char *module_name_of(const ncl_loaded_module *entry)
{
    if (entry == NULL) {
        return NULL;
    }
    return entry->abi == NCL_TOOL_MODULE_ABI ? entry->tool->name
                                             : entry->module->name;
}

static bool module_name_taken(const ncl_module_set *set, const char *name)
{
    size_t i;

    for (i = 0; i < set->count; i++) {
        const char *taken = module_name_of(&set->items[i]);

        if (taken != NULL && ncl_strcasecmp(taken, name) == 0) {
            return true;
        }
    }
    return false;
}

/** Put the module file name into @p buffer (dir/name), or just the name. */
static const char *module_file_path(char *buffer, size_t size, const char *dir,
                                    const char *name_or_file, char **heap_name)
{
    char *file = ncl_library_file_name(name_or_file);

    *heap_name = file;
    if (file == NULL) {
        return NULL;
    }
    if (ncl_str_is_blank(dir)) {
        /* A bare name with no directory is left to the platform's search
         * order; that is what a caller asking for it means. */
        snprintf(buffer, size, "%s", file);
        return buffer;
    }
    snprintf(buffer, size, "%s%c%s", dir, NCL_PATH_SEP, file);
    return buffer;
}

static ncl_err module_attach(ncl_module_set *set, ncl_library *library,
                             const char *path, bool *added, ncl_strbuf *err)
{
    ncl_adapter_module_fn entry;
    const ncl_adapter_module_desc *module;
    const ncl_tool_module_desc *tool;
    const unsigned *head;
    ncl_loaded_module *slot;
    unsigned abi;

    *added = false;
    entry = (ncl_adapter_module_fn)ncl_library_symbol(library,
                                                      NCL_ADAPTER_MODULE_ENTRY);
    if (entry == NULL) {
        err_append_module(err, "模块未导出 " NCL_ADAPTER_MODULE_ENTRY "()", path);
        return NCL_ERR_INVALID_ARG;
    }
    head = ((ncl_module_probe_fn)entry)();
    if (head == NULL) {
        err_append_module(err, "模块的入口返回空", path);
        return NCL_ERR_INVALID_ARG;
    }
    abi = *head;
    module = NULL;
    tool = NULL;
    if (abi == NCL_ADAPTER_MODULE_ABI) {
        module = (const ncl_adapter_module_desc *)head;
        if (ncl_str_is_blank(module->name) || module->create == NULL) {
            err_append_module(err, "模块缺少 name 或 create", path);
            return NCL_ERR_INVALID_ARG;
        }
    } else if (abi == NCL_TOOL_MODULE_ABI) {
        tool = (const ncl_tool_module_desc *)head;
        if (ncl_str_is_blank(tool->name)) {
            err_append_module(err, "模块缺少 name", path);
            return NCL_ERR_INVALID_ARG;
        }
        if (ncl_tool_validate(&tool->decl, err) != NCL_OK) {
            err_append_module(err, "模块声明的点位不合法", path);
            return NCL_ERR_INVALID_ARG;
        }
    } else {
        char text[256];

        snprintf(text, sizeof(text),
                 "模块 %s 的 ABI 是 %u，本宿主只认 %u（驱动工厂）和 %u（工具声明）",
                 path, abi, (unsigned)NCL_ADAPTER_MODULE_ABI,
                 (unsigned)NCL_TOOL_MODULE_ABI);
        err_append(err, text);
        return NCL_ERR_INVALID_ARG;
    }
    if (module_name_taken(set, module != NULL ? module->name : tool->name)) {
        return NCL_OK; /* already loaded: not an error, @p added stays false */
    }
    if (set->count == set->capacity) {
        size_t capacity = set->capacity == 0 ? 4 : set->capacity * 2;
        ncl_loaded_module *grown = (ncl_loaded_module *)ncl_mem_realloc(
            set->items, capacity * sizeof(*grown));

        if (grown == NULL) {
            return NCL_ERR_NOMEM;
        }
        set->items = grown;
        set->capacity = capacity;
    }
    slot = &set->items[set->count];
    memset(slot, 0, sizeof(*slot));
    slot->path = ncl_strdup(path);
    if (slot->path == NULL) {
        return NCL_ERR_NOMEM;
    }
    slot->library = library;
    slot->module = module;
    slot->tool = tool;
    slot->abi = abi;
    set->count++;
    *added = true;
    return NCL_OK;
}

/** Load one module file; a module already in the set is not loaded twice. */
static ncl_err module_load_file(ncl_module_set *set, const char *path,
                                bool required, ncl_strbuf *err)
{
    ncl_library *library;
    char *reason = NULL;
    bool added = false;
    ncl_err rc;

    library = ncl_library_open(path, &reason);
    if (library == NULL) {
        if (err != NULL) {
            if (err->len > 0) {
                (void)ncl_strbuf_puts(err, "; ");
            }
            (void)ncl_strbuf_printf(err, "模块装载失败: %s",
                                    reason != NULL ? reason : path);
        }
        ncl_free_safe(reason);
        return required ? NCL_ERR_IO : NCL_OK;
    }
    ncl_free_safe(reason);
    rc = module_attach(set, library, path, &added, err);
    if (rc != NCL_OK || !added) {
        ncl_library_close(library); /* it complained, or the name is taken */
        return required && rc != NCL_OK ? rc : NCL_OK;
    }
    return NCL_OK;
}

ncl_err ncl_modules_add(ncl_module_set *set, const char *name_or_file,
                        const char *dir, ncl_strbuf *err)
{
    char path[NCL_PATH_MAX_BUF];
    char *file = NULL;

    if (set == NULL || ncl_str_is_blank(name_or_file)) {
        return NCL_ERR_INVALID_ARG;
    }
    if (module_file_path(path, sizeof(path), dir, name_or_file, &file) == NULL) {
        err_append(err, "模块名无效");
        return NCL_ERR_INVALID_ARG;
    }
    ncl_free_safe(file);
    return module_load_file(set, path, true, err);
}

ncl_err ncl_modules_add_dir(ncl_module_set *set, const char *dir, size_t *loaded,
                            ncl_strbuf *err)
{
    size_t before = set != NULL ? set->count : 0;

    if (set == NULL || ncl_str_is_blank(dir)) {
        return NCL_ERR_INVALID_ARG;
    }
    if (!ncl_path_exists(dir)) {
        if (loaded != NULL) {
            *loaded = 0;
        }
        return NCL_OK; /* no plugin directory: no modules, not an error */
    }
#if defined(_WIN32) || defined(_WIN64)
    {
        char pattern[NCL_PATH_MAX_BUF];
        WIN32_FIND_DATAA found;
        HANDLE handle;

        snprintf(pattern, sizeof(pattern), "%s%c*%s", dir, NCL_PATH_SEP,
                 NCL_MODULE_SUFFIX_DLL);
        handle = FindFirstFileA(pattern, &found);
        if (handle == INVALID_HANDLE_VALUE) {
            return NCL_OK;
        }
        do {
            char path[NCL_PATH_MAX_BUF];

            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                continue;
            }
            snprintf(path, sizeof(path), "%s%c%s", dir, NCL_PATH_SEP,
                     found.cFileName);
            /* A stray DLL is reported, not fatal: the directory may hold more
             * than this program's modules. */
            (void)module_load_file(set, path, false, err);
        } while (FindNextFileA(handle, &found));
        FindClose(handle);
    }
#else
    {
        DIR *handle = opendir(dir);
        struct dirent *entry;

        if (handle == NULL) {
            return NCL_OK;
        }
        while ((entry = readdir(handle)) != NULL) {
            size_t len = strlen(entry->d_name);
            char path[NCL_PATH_MAX_BUF];

            if (len < strlen(NCL_MODULE_SUFFIX_SO) ||
                strcmp(entry->d_name + len - strlen(NCL_MODULE_SUFFIX_SO),
                       NCL_MODULE_SUFFIX_SO) != 0) {
                continue;
            }
            snprintf(path, sizeof(path), "%s%c%s", dir, NCL_PATH_SEP,
                     entry->d_name);
            (void)module_load_file(set, path, false, err);
        }
        closedir(handle);
    }
#endif
    if (loaded != NULL) {
        *loaded = set->count - before;
    }
    return NCL_OK;
}

char *ncl_modules_dir_from_config(const ncl_json *config, const char *default_dir)
{
    const ncl_json *plugins = ncl_json_obj_get(config, "plugins");
    const char *dir = NULL;

    if (plugins != NULL && ncl_json_type_of(plugins) == NCL_JSON_OBJECT) {
        dir = ncl_json_obj_get_string(plugins, "dir");
    }
    if (!ncl_str_is_blank(dir)) {
        return ncl_strdup(dir);
    }
    return ncl_strdup(default_dir != NULL ? default_dir : "");
}

ncl_err ncl_modules_add_config(ncl_module_set *set, const ncl_json *config,
                               const char *default_dir, ncl_strbuf *err)
{
    const ncl_json *plugins;
    const ncl_json *list = NULL;
    char *dir = NULL;
    bool auto_load = false;
    ncl_err rc = NCL_OK;
    size_t i;

    if (set == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    plugins = config != NULL ? ncl_json_obj_get(config, "plugins") : NULL;
    if (plugins == NULL) {
        dir = ncl_strdup(default_dir != NULL ? default_dir : "");
        auto_load = true;
    } else if (ncl_json_type_of(plugins) == NCL_JSON_ARRAY) {
        dir = ncl_strdup(default_dir != NULL ? default_dir : "");
        list = plugins;
    } else if (ncl_json_type_of(plugins) == NCL_JSON_OBJECT) {
        list = ncl_json_obj_get(plugins, "load");
        auto_load = ncl_json_obj_get_bool(plugins, "auto", false);
        dir = ncl_modules_dir_from_config(config, default_dir);
    } else {
        err_append(err, "\"plugins\" 应当是数组或对象");
        return NCL_ERR_INVALID_ARG;
    }
    if (dir == NULL) {
        return NCL_ERR_NOMEM;
    }

    if (ncl_json_type_of(list) == NCL_JSON_ARRAY) {
        for (i = 0; i < ncl_json_arr_len(list); i++) {
            const char *entry = ncl_json_as_string(ncl_json_arr_get(list, i));

            if (ncl_str_is_blank(entry)) {
                continue;
            }
            if (ncl_modules_add(set, entry, dir, err) != NCL_OK) {
                rc = NCL_ERR_IO;
            }
        }
    }
    if (auto_load) {
        (void)ncl_modules_add_dir(set, dir, NULL, err);
    }
    ncl_free_safe(dir);
    return rc;
}

ncl_err ncl_modules_register(ncl_module_set *set, ncl_strbuf *err)
{
    size_t i;
    ncl_err first = NCL_OK;

    if (set == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < set->count; i++) {
        ncl_loaded_module *entry = &set->items[i];
        const ncl_adapter_module_desc *module = entry->module;

        if (entry->registered) {
            continue;
        }
        /* A tool module has no driver factory to register: the host reads its
         * declaration instead (ncl_module_tool()). */
        if (entry->abi == NCL_TOOL_MODULE_ABI) {
            /* Values and methods are counted by the host when it lists what is
             * loaded; here the interesting part is that a tool needs no driver
             * registration at all. */
            ncl_log_info("适配器模块 %s：工具 \"%s\"（%s）",
                         ncl_library_path(entry->library), entry->tool->name,
                         !ncl_str_is_blank(entry->tool->version)
                             ? entry->tool->version
                             : "?");
            continue;
        }
        if (ncl_driver_register_protocol(module->name, module->create) != NCL_OK) {
            err_append_module(err, "协议已被注册（内置驱动或另一个模块）",
                              module->name);
            if (first == NCL_OK) {
                first = NCL_ERR_EXISTS;
            }
            continue;
        }
        ncl_log_info("适配器模块 %s：注册协议 \"%s\"（%s）",
                     ncl_library_path(entry->library), module->name,
                     !ncl_str_is_blank(module->version) ? module->version : "?");
        if (module->aliases != NULL) {
            size_t alias;

            for (alias = 0; module->aliases[alias] != NULL; alias++) {
                if (ncl_driver_register_protocol(module->aliases[alias],
                                                 module->create) != NCL_OK) {
                    err_append_module(err, "别名已被注册",
                                      module->aliases[alias]);
                } else {
                    ncl_log_info("  （别名 \"%s\"）", module->aliases[alias]);
                }
            }
        }
        entry->registered = true;
    }
    return first;
}

size_t ncl_module_count(const ncl_module_set *set)
{
    return set != NULL ? set->count : 0;
}

static const ncl_loaded_module *module_at(const ncl_module_set *set,
                                          size_t index)
{
    if (set == NULL || index >= set->count) {
        return NULL;
    }
    return &set->items[index];
}

static const char *module_version_of(const ncl_loaded_module *entry)
{
    if (entry == NULL) {
        return NULL;
    }
    return entry->abi == NCL_TOOL_MODULE_ABI ? entry->tool->version
                                             : entry->module->version;
}

static const char *module_description_of(const ncl_loaded_module *entry)
{
    if (entry == NULL) {
        return NULL;
    }
    return entry->abi == NCL_TOOL_MODULE_ABI ? entry->tool->description
                                             : entry->module->description;
}

const char *ncl_module_name(const ncl_module_set *set, size_t index)
{
    return module_name_of(module_at(set, index));
}

const char *ncl_module_version(const ncl_module_set *set, size_t index)
{
    return module_version_of(module_at(set, index));
}

const char *ncl_module_description(const ncl_module_set *set, size_t index)
{
    return module_description_of(module_at(set, index));
}

unsigned ncl_module_abi(const ncl_module_set *set, size_t index)
{
    const ncl_loaded_module *entry = module_at(set, index);

    return entry != NULL ? entry->abi : 0u;
}

const ncl_tool_decl *ncl_module_tool(const ncl_module_set *set, size_t index)
{
    const ncl_loaded_module *entry = module_at(set, index);

    if (entry == NULL || entry->abi != NCL_TOOL_MODULE_ABI) {
        return NULL;
    }
    return &entry->tool->decl;
}

const char *ncl_module_path(const ncl_module_set *set, size_t index)
{
    const ncl_loaded_module *entry = module_at(set, index);

    return entry != NULL ? entry->path : NULL;
}

bool ncl_module_registered(const ncl_module_set *set, size_t index)
{
    const ncl_loaded_module *entry = module_at(set, index);

    return entry != NULL && entry->registered;
}

void ncl_modules_free(ncl_module_set *set)
{
    size_t i;

    if (set == NULL) {
        return;
    }
    /* Reverse order: a module may have been built on another's symbols. */
    for (i = set->count; i > 0; i--) {
        ncl_free_safe(set->items[i - 1].path);
        ncl_library_close(set->items[i - 1].library);
    }
    ncl_mem_free(set->items);
    ncl_mem_free(set);
}
