/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - driver configuration, point map and path dispatch.
 *
 * One entry per configured device link: the driver instance, the model path
 * prefix it answers for and the point map that turns an NC-Link model path into
 * a unified address.
 *
 * Points are stored in driver-relative form ("/STATUS" under the "/PLC1" link),
 * which is what makes the lookup a single string compare no matter how the
 * configuration wrote it - absolute ("/PLC1/STATUS") or relative ("STATUS").
 */

#include "nclink_adapter/ncl_driver_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_platform.h"

#include "core/adapter_text.h"
#include "nclink_adapter/ncl_audit.h"

#if defined(NCL_OS_WINDOWS)
#include <windows.h>
#else
#include <dirent.h>
#endif

typedef struct {
    char        *key;  /**< relative to the link prefix, starts with '/' */
    char        *path; /**< absolute model path, as the configuration wrote it */
    ncl_address  address;
    bool         writable;
    bool         sampled;
} driver_point;

typedef struct {
    char        *id;
    char        *prefix; /**< "/PLC1", or "/" for the catch-all link */
    char        *protocol;
    ncl_driver  *driver;
    driver_point *points;
    size_t        point_count;
    size_t        point_cap;
} driver_entry;

struct ncl_driver_manager {
    driver_entry *entries;
    size_t        count;
    size_t        cap;
};

/* --------------------------------------------------------------- helpers -- */

static void err_append(ncl_strbuf *err, const char *text)
{
    if (err == NULL) {
        return;
    }
    if (err->len > 0) {
        (void)ncl_strbuf_puts(err, "; ");
    }
    (void)ncl_strbuf_puts(err, text);
}

static void err_append1(ncl_strbuf *err, const char *fmt, const char *arg)
{
    if (err == NULL) {
        return;
    }
    if (err->len > 0) {
        (void)ncl_strbuf_puts(err, "; ");
    }
    (void)ncl_strbuf_printf(err, fmt, arg);
}

/** "a" + "/" + "b", collapsing a duplicated separator. Heap string or NULL. */
static char *path_join(const char *a, const char *b)
{
    ncl_strbuf sb;
    char *out;

    if (ncl_str_is_empty(a)) {
        return ncl_strdup(b != NULL ? b : "");
    }
    if (ncl_str_is_empty(b)) {
        return ncl_strdup(a);
    }
    while (*b == '/') {
        b++;
    }
    ncl_strbuf_init(&sb);
    (void)ncl_strbuf_puts(&sb, a);
    if (a[strlen(a) - 1] != '/') {
        (void)ncl_strbuf_putc(&sb, '/');
    }
    (void)ncl_strbuf_puts(&sb, b);
    out = ncl_strbuf_detach(&sb);
    ncl_strbuf_free(&sb);
    return out;
}

/** "/PLC1/" -> "/PLC1"; "" or NULL -> "/"; "/PLC1" -> "/PLC1". */
static char *normalise_prefix(const char *path)
{
    char *out;
    size_t len;

    if (ncl_str_is_blank(path)) {
        return ncl_strdup("/");
    }
    out = path[0] == '/' ? ncl_strdup(path) : path_join("/", path);
    if (out == NULL) {
        return NULL;
    }
    len = strlen(out);
    while (len > 1 && out[len - 1] == '/') {
        out[--len] = '\0';
    }
    return out;
}

/** True when @p prefix covers @p path: "/PLC1" covers "/PLC1/x", not "/PLC10". */
static bool prefix_covers(const char *prefix, const char *path)
{
    size_t len;

    if (strcmp(prefix, "/") == 0) {
        return true;
    }
    len = strlen(prefix);
    if (strncmp(prefix, path, len) != 0) {
        return false;
    }
    return path[len] == '\0' || path[len] == '/';
}

/** What @p path is called inside @p prefix ("/x" for "/PLC1/x"). Heap or NULL. */
static char *relative_key(const char *prefix, const char *path)
{
    if (strcmp(prefix, "/") == 0) {
        return path[0] == '/' ? ncl_strdup(path) : path_join("/", path);
    }
    path += strlen(prefix);
    return path[0] == '/' ? ncl_strdup(path) : path_join("/", path);
}

/**
 * The absolute form of a point path: "/PLC1/STATUS" is already absolute, while
 * "STATUS" and "/STATUS" are relative to the link and become "/PLC1/STATUS".
 */
static char *point_absolute(const char *prefix, const char *path)
{
    char *as_written = path[0] == '/' ? ncl_strdup(path) : path_join("/", path);
    char *out;

    if (as_written == NULL) {
        return NULL;
    }
    if (prefix_covers(prefix, as_written)) {
        return as_written;
    }
    out = path_join(prefix, as_written);
    ncl_free_safe(as_written);
    return out;
}

/* ------------------------------------------------------------------ entry -- */

static driver_entry *manager_push(ncl_driver_manager *manager)
{
    driver_entry *entry;
    size_t cap;

    if (manager->count == manager->cap) {
        cap = manager->cap == 0 ? 4 : manager->cap * 2;
        entry = (driver_entry *)ncl_mem_realloc(manager->entries,
                                                cap * sizeof(*entry));
        if (entry == NULL) {
            return NULL;
        }
        manager->entries = entry;
        manager->cap = cap;
    }
    entry = &manager->entries[manager->count];
    memset(entry, 0, sizeof(*entry));
    return entry;
}

static void entry_release(driver_entry *entry)
{
    size_t i;

    if (entry->driver != NULL && entry->driver->ops != NULL &&
        entry->driver->ops->destroy != NULL) {
        entry->driver->ops->destroy(entry->driver);
    }
    for (i = 0; i < entry->point_count; i++) {
        ncl_free_safe(entry->points[i].key);
        ncl_free_safe(entry->points[i].path);
        ncl_address_clear(&entry->points[i].address);
    }
    ncl_free_safe(entry->points);
    ncl_free_safe(entry->id);
    ncl_free_safe(entry->prefix);
    ncl_free_safe(entry->protocol);
    memset(entry, 0, sizeof(*entry));
}

static const ncl_address *entry_find_point(const driver_entry *entry,
                                           const char *key)
{
    size_t i;

    for (i = 0; i < entry->point_count; i++) {
        if (strcmp(entry->points[i].key, key) == 0) {
            return &entry->points[i].address;
        }
    }
    return NULL;
}

/**
 * Add one point. The point path is absolute ("/PLC1/STATUS") or relative to the
 * link ("STATUS" / "/STATUS"); an existing key is replaced, so a later file may
 * override an earlier one.
 */
static ncl_err entry_add_point(driver_entry *entry, const ncl_json *point,
                               ncl_strbuf *err)
{
    const char *path = ncl_json_obj_get_string(point, "path");
    const char *id = ncl_json_obj_get_string(point, "id");
    ncl_json *addr_node = ncl_json_obj_get(point, "addr");
    char *absolute;
    char *key;
    ncl_address address;
    ncl_dtype dtype;
    ncl_err result;
    size_t i;

    if (addr_node == NULL) {
        addr_node = ncl_json_obj_get(point, "address");
    }
    if (ncl_str_is_blank(path)) {
        if (ncl_str_is_blank(id)) {
            err_append(err, "point without a \"path\" or an \"id\"");
            return NCL_ERR_INVALID_ARG;
        }
        path = id;
    }
    if (addr_node == NULL) {
        err_append1(err, "point %s has no \"addr\"", path);
        return NCL_ERR_INVALID_ARG;
    }
    absolute = point_absolute(entry->prefix, path);
    if (absolute == NULL) {
        return NCL_ERR_NOMEM;
    }

    result = ncl_address_from_json(addr_node, &address);
    if (result != NCL_OK) {
        err_append1(err, "point %s has an unusable address", absolute);
        ncl_free_safe(absolute);
        return result;
    }
    /* The point may carry the type and the length itself. */
    if (address.bit < 0 &&
        ncl_dtype_from_object(point, "dtype", &dtype) == NCL_OK) {
        address.dtype = dtype;
    }
    if (address.length == 1 && ncl_json_obj_has(point, "length")) {
        long long length = ncl_json_obj_get_int(point, "length", 1);

        if (length < 1 || length > 65536) {
            err_append1(err, "point %s has a bad \"length\"", absolute);
            ncl_address_clear(&address);
            ncl_free_safe(absolute);
            return NCL_ERR_RANGE;
        }
        address.length = (int)length;
    }

    key = relative_key(entry->prefix, absolute);
    if (key == NULL) {
        ncl_free_safe(absolute);
        ncl_address_clear(&address);
        return NCL_ERR_NOMEM;
    }

    for (i = 0; i < entry->point_count; i++) {
        if (strcmp(entry->points[i].key, key) == 0) {
            ncl_address_clear(&entry->points[i].address);
            ncl_free_safe(entry->points[i].path);
            entry->points[i].address = address;
            entry->points[i].path = absolute;
            entry->points[i].writable =
                ncl_json_obj_get_bool(point, "writable", false);
            entry->points[i].sampled =
                ncl_json_obj_get_bool(point, "sample", true);
            ncl_free_safe(key);
            return NCL_OK;
        }
    }
    if (entry->point_count == entry->point_cap) {
        size_t cap = entry->point_cap == 0 ? 8 : entry->point_cap * 2;
        driver_point *grown = (driver_point *)ncl_mem_realloc(
            entry->points, cap * sizeof(*grown));

        if (grown == NULL) {
            ncl_address_clear(&address);
            ncl_free_safe(absolute);
            ncl_free_safe(key);
            return NCL_ERR_NOMEM;
        }
        entry->points = grown;
        entry->point_cap = cap;
    }
    entry->points[entry->point_count].key = key;
    entry->points[entry->point_count].path = absolute;
    entry->points[entry->point_count].address = address;
    entry->points[entry->point_count].writable =
        ncl_json_obj_get_bool(point, "writable", false);
    entry->points[entry->point_count].sampled =
        ncl_json_obj_get_bool(point, "sample", true);
    entry->point_count++;
    return NCL_OK;
}

static ncl_err manager_add_one(ncl_driver_manager *manager,
                               const ncl_json *config, ncl_strbuf *err)
{
    const char *id = ncl_json_obj_get_string(config, "id");
    const char *path = ncl_json_obj_get_string(config, "path");
    const char *protocol = ncl_json_obj_get_string(config, "type");
    const ncl_json *parameters = ncl_json_obj_get(config, "parameters");
    const ncl_json *points;
    driver_entry *entry;
    ncl_driver *driver;
    ncl_err result;
    size_t i;

    if (ncl_str_is_blank(protocol)) {
        err_append(err, "a driver entry needs a \"type\"");
        return NCL_ERR_INVALID_ARG;
    }
    /* Ids are the key of the configuration: a duplicate is a mistake, not an
     * override, so a half merged set of links cannot go unnoticed. */
    if (!ncl_str_is_blank(id)) {
        for (i = 0; i < manager->count; i++) {
            if (strcmp(manager->entries[i].id, id) == 0) {
                err_append1(err, "two drivers share the id \"%s\"", id);
                return NCL_ERR_EXISTS;
            }
        }
    }
    driver = ncl_driver_create(protocol);
    if (driver == NULL) {
        err_append1(err, "unknown protocol \"%s\"", protocol);
        return NCL_ERR_NOT_FOUND;
    }
    if (driver->ops->create != NULL) {
        result = driver->ops->create(driver, parameters);
        if (result != NCL_OK) {
            err_append1(err, "protocol \"%s\" refused its parameters", protocol);
            driver->ops->destroy(driver);
            return result;
        }
    }
    entry = manager_push(manager);
    if (entry == NULL) {
        driver->ops->destroy(driver);
        return NCL_ERR_NOMEM;
    }
    entry->driver = driver;
    entry->protocol = ncl_strdup(ncl_driver_protocol(driver));
    entry->id = ncl_strdup(ncl_str_is_blank(id) ? ncl_driver_protocol(driver) : id);
    entry->prefix = normalise_prefix(path);
    if (entry->protocol == NULL || entry->id == NULL || entry->prefix == NULL) {
        entry_release(entry);
        return NCL_ERR_NOMEM;
    }

    points = ncl_json_obj_get(config, "points");
    for (i = 0; i < ncl_json_arr_len(points); i++) {
        result = entry_add_point(entry, ncl_json_arr_get(points, i), err);
        if (result != NCL_OK) {
            entry_release(entry);
            return result;
        }
    }
    manager->count++;
    return NCL_OK;
}

static ncl_err manager_add_document(ncl_driver_manager *manager,
                                    const ncl_json *document, ncl_strbuf *err)
{
    const ncl_json *list;
    size_t i;

    if (ncl_json_type_of(document) == NCL_JSON_ARRAY) {
        for (i = 0; i < ncl_json_arr_len(document); i++) {
            ncl_err result = manager_add_one(manager,
                                             ncl_json_arr_get(document, i), err);

            if (result != NCL_OK) {
                return result;
            }
        }
        return NCL_OK;
    }
    if (ncl_json_type_of(document) != NCL_JSON_OBJECT) {
        err_append(err, "a driver configuration must be an object or an array");
        return NCL_ERR_PARSE;
    }
    list = ncl_json_obj_get(document, "drivers");
    if (list != NULL) {
        return manager_add_document(manager, list, err);
    }
    return manager_add_one(manager, document, err);
}

/* ----------------------------------------------------------------- public -- */

ncl_driver_manager *ncl_driver_manager_create(void)
{
    ncl_driver_register_builtin();
    return (ncl_driver_manager *)ncl_mem_calloc(1, sizeof(ncl_driver_manager));
}

void ncl_driver_manager_free(ncl_driver_manager *manager)
{
    size_t i;

    if (manager == NULL) {
        return;
    }
    for (i = 0; i < manager->count; i++) {
        entry_release(&manager->entries[i]);
    }
    ncl_free_safe(manager->entries);
    ncl_free_safe(manager);
}

ncl_err ncl_driver_manager_add_json(ncl_driver_manager *manager,
                                    const ncl_json *document, ncl_strbuf *err)
{
    if (manager == NULL || document == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return manager_add_document(manager, document, err);
}

ncl_err ncl_driver_manager_load_file(ncl_driver_manager *manager,
                                     const char *path, ncl_strbuf *err)
{
    ncl_json *document;
    ncl_err result;

    if (manager == NULL || ncl_str_is_blank(path)) {
        return NCL_ERR_INVALID_ARG;
    }
    document = ncl_adapter_json_from_file(path, err);
    if (document == NULL) {
        return NCL_ERR_PARSE; /* the helper already described the failure */
    }
    result = manager_add_document(manager, document, err);
    ncl_json_free(document);
    return result;
}

/* -------------------------------------------------------- directory scan -- */

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/**
 * Collect the "*.json" file names of @p directory into @p names (sorted).
 * Answers NCL_ERR_NOT_FOUND when the directory cannot be opened.
 */
static ncl_err collect_json_names(const char *directory, ncl_ptrvec *names)
{
#if defined(NCL_OS_WINDOWS)
    char pattern[NCL_PATH_MAX_BUF];
    WIN32_FIND_DATAA entry;
    HANDLE handle;

    snprintf(pattern, sizeof(pattern), "%s\\*", directory);
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        return NCL_ERR_NOT_FOUND;
    }
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (ncl_str_ends_with(entry.cFileName, ".json")) {
            (void)ncl_ptrvec_push(names, ncl_strdup(entry.cFileName));
        }
    } while (FindNextFileA(handle, &entry));
    FindClose(handle);
#else
    DIR *dir = opendir(directory);
    struct dirent *entry;

    if (dir == NULL) {
        return NCL_ERR_NOT_FOUND;
    }
    while ((entry = readdir(dir)) != NULL) {
        if (ncl_str_ends_with(entry->d_name, ".json")) {
            (void)ncl_ptrvec_push(names, ncl_strdup(entry->d_name));
        }
    }
    closedir(dir);
#endif
    if (names->len > 1) {
        qsort(names->items, names->len, sizeof(names->items[0]), compare_names);
    }
    return NCL_OK;
}

ncl_err ncl_driver_manager_load_dir(ncl_driver_manager *manager,
                                    const char *directory, ncl_strbuf *err)
{
    ncl_ptrvec names;
    ncl_err result = NCL_OK;
    size_t i;

    if (manager == NULL || ncl_str_is_blank(directory)) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_ptrvec_init(&names, ncl_mem_free);
    result = collect_json_names(directory, &names);
    if (result != NCL_OK) {
        err_append1(err, "cannot read the driver directory \"%s\"", directory);
        ncl_ptrvec_free(&names);
        return result;
    }
    for (i = 0; i < names.len; i++) {
        char *file = path_join(directory, (const char *)names.items[i]);

        if (file == NULL) {
            result = NCL_ERR_NOMEM;
            break;
        }
        result = ncl_driver_manager_load_file(manager, file, err);
        ncl_free_safe(file);
        if (result != NCL_OK) {
            break;
        }
    }
    ncl_ptrvec_free(&names);
    return result;
}

/* ------------------------------------------------------------- contents -- */

size_t ncl_driver_manager_count(const ncl_driver_manager *manager)
{
    return manager != NULL ? manager->count : 0;
}

static const driver_entry *entry_at(const ncl_driver_manager *manager,
                                    size_t index)
{
    if (manager == NULL || index >= manager->count) {
        return NULL;
    }
    return &manager->entries[index];
}

const char *ncl_driver_manager_id_at(const ncl_driver_manager *manager,
                                     size_t index)
{
    const driver_entry *entry = entry_at(manager, index);

    return entry != NULL ? entry->id : NULL;
}

const char *ncl_driver_manager_path_at(const ncl_driver_manager *manager,
                                       size_t index)
{
    const driver_entry *entry = entry_at(manager, index);

    return entry != NULL ? entry->prefix : NULL;
}

const char *ncl_driver_manager_protocol_at(const ncl_driver_manager *manager,
                                           size_t index)
{
    const driver_entry *entry = entry_at(manager, index);

    return entry != NULL ? entry->protocol : NULL;
}

ncl_driver *ncl_driver_manager_driver_at(ncl_driver_manager *manager,
                                         size_t index)
{
    const driver_entry *entry = entry_at(manager, index);

    return entry != NULL ? entry->driver : NULL;
}

/** Longest prefix match; the "/" link only wins when nothing else does. */
static driver_entry *manager_lookup_entry(ncl_driver_manager *manager,
                                          const char *path)
{
    driver_entry *best = NULL;
    size_t best_len = 0;
    size_t i;

    if (manager == NULL || ncl_str_is_blank(path)) {
        return NULL;
    }
    for (i = 0; i < manager->count; i++) {
        driver_entry *entry = &manager->entries[i];
        size_t len;

        if (!prefix_covers(entry->prefix, path)) {
            continue;
        }
        len = strlen(entry->prefix);
        if (best == NULL || len > best_len) {
            best = entry;
            best_len = len;
        }
    }
    return best;
}

ncl_driver *ncl_driver_manager_driver_of(ncl_driver_manager *manager,
                                         const char *path)
{
    driver_entry *entry = manager_lookup_entry(manager, path);

    return entry != NULL ? entry->driver : NULL;
}

/**
 * Find the entry and address behind @p path. When @p address is not NULL it
 * receives the point's address, or NULL when the link has no such point.
 */
static driver_entry *manager_lookup(ncl_driver_manager *manager, const char *path,
                                    const ncl_address **address)
{
    driver_entry *entry = manager_lookup_entry(manager, path);
    char *key;

    if (address != NULL) {
        *address = NULL;
    }
    if (entry == NULL) {
        return NULL;
    }
    key = relative_key(entry->prefix, path);
    if (key == NULL) {
        return entry;
    }
    if (address != NULL) {
        *address = entry_find_point(entry, key);
    }
    ncl_free_safe(key);
    return entry;
}

const ncl_address *ncl_driver_manager_address_of(ncl_driver_manager *manager,
                                                 const char *path)
{
    const ncl_address *address = NULL;

    (void)manager_lookup(manager, path, &address);
    return address;
}

/* ----------------------------------------------------------------- points -- */

static const driver_point *point_at(const ncl_driver_manager *manager,
                                    size_t index, size_t point)
{
    const driver_entry *entry = entry_at(manager, index);

    if (entry == NULL || point >= entry->point_count) {
        return NULL;
    }
    return &entry->points[point];
}

size_t ncl_driver_manager_point_count(const ncl_driver_manager *manager,
                                      size_t index)
{
    const driver_entry *entry = entry_at(manager, index);

    return entry != NULL ? entry->point_count : 0;
}

const char *ncl_driver_manager_point_path(const ncl_driver_manager *manager,
                                          size_t index, size_t point)
{
    const driver_point *p = point_at(manager, index, point);

    return p != NULL ? p->path : NULL;
}

const ncl_address *ncl_driver_manager_point_address(
    const ncl_driver_manager *manager, size_t index, size_t point)
{
    const driver_point *p = point_at(manager, index, point);

    return p != NULL ? &p->address : NULL;
}

bool ncl_driver_manager_point_writable(const ncl_driver_manager *manager,
                                       size_t index, size_t point)
{
    const driver_point *p = point_at(manager, index, point);

    return p != NULL && p->writable;
}

bool ncl_driver_manager_point_sampled(const ncl_driver_manager *manager,
                                      size_t index, size_t point)
{
    const driver_point *p = point_at(manager, index, point);

    return p != NULL && p->sampled;
}

/* -------------------------------------------------------------- traffic -- */

/**
 * The frames @p driver exchanged just now, or NULL when the audit is not
 * collecting them: §6 of the spec wants the bytes only on demand, and reading
 * them out is the caller's job so a driver keeps its single-exchange contract.
 */
static const ncl_driver_raw *audit_raw(const ncl_driver *driver,
                                       ncl_driver_raw *scratch)
{
    if (!ncl_audit_wants_raw()) {
        return NULL;
    }
    ncl_driver_last_raw(driver, scratch);
    return scratch->request_len > 0 || scratch->reply_len > 0 ? scratch : NULL;
}

ncl_err ncl_driver_manager_read(ncl_driver_manager *manager, const char *path,
                                ncl_json **value)
{
    const ncl_address *address = NULL;
    driver_entry *entry;
    ncl_driver_raw raw;
    int64_t started;
    ncl_err result;

    if (value != NULL) {
        *value = NULL;
    }
    if (manager == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    entry = manager_lookup(manager, path, &address);
    if (entry == NULL) {
        ncl_audit_request("-", path, "-", NCL_ERR_NOT_FOUND, 0, NULL);
        return NCL_ERR_NOT_FOUND; /* no link answers for this path */
    }
    if (address == NULL) {
        ncl_audit_request(entry->id, path, "-", NCL_ERR_NOT_FOUND, 0, NULL);
        return NCL_ERR_NOT_FOUND; /* the link has no point mapped here */
    }
    started = ncl_time_monotonic_millis();
    result = ncl_driver_read_one(entry->driver, address, value);
    {
        char text[64];
        char *address_text = ncl_address_to_text(address);

        snprintf(text, sizeof(text), "%s",
                 address_text != NULL ? address_text : "?");
        ncl_free_safe(address_text);
        ncl_audit_request(entry->id, path, text, result,
                          ncl_time_monotonic_millis() - started,
                          audit_raw(entry->driver, &raw));
    }
    return result;
}

ncl_err ncl_driver_manager_write(ncl_driver_manager *manager, const char *path,
                                 const ncl_json *value)
{
    const ncl_address *address = NULL;
    driver_entry *entry;
    ncl_json *old_value = NULL;
    char text[64];
    char *address_text;
    ncl_driver_raw raw;
    ncl_err result;
    int64_t started;

    if (manager == NULL || value == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    entry = manager_lookup(manager, path, &address);
    if (entry == NULL || address == NULL) {
        const char *link = entry != NULL ? entry->id : "-";

        ncl_audit_write(link, path, "-", NULL, value, NCL_ERR_NOT_FOUND);
        ncl_audit_request(link, path, "-", NCL_ERR_NOT_FOUND, 0, NULL);
        return NCL_ERR_NOT_FOUND;
    }
    /* The old value is read first so the audit line can say what changed (§6);
     * a point that cannot be read just shows "-" as its previous value. */
    (void)ncl_driver_read_one(entry->driver, address, &old_value);
    started = ncl_time_monotonic_millis();
    result = ncl_driver_write_one(entry->driver, address, value);
    address_text = ncl_address_to_text(address);
    snprintf(text, sizeof(text), "%s", address_text != NULL ? address_text : "?");
    ncl_free_safe(address_text);
    ncl_audit_write(entry->id, path, text, old_value, value, result);
    ncl_audit_request(entry->id, path, text, result,
                      ncl_time_monotonic_millis() - started,
                      audit_raw(entry->driver, &raw));
    ncl_json_free(old_value);
    return result;
}

ncl_err ncl_driver_manager_call(ncl_driver_manager *manager, const char *path,
                                const char *operation, const ncl_json *params,
                                ncl_json **result)
{
    driver_entry *entry;
    ncl_driver_raw raw;

    if (result != NULL) {
        *result = NULL;
    }
    if (manager == NULL || ncl_str_is_blank(operation)) {
        return NCL_ERR_INVALID_ARG;
    }
    entry = manager_lookup_entry(manager, path);
    if (entry == NULL || entry->driver->ops->call == NULL) {
        ncl_audit_request("-", path, operation, NCL_ERR_NOT_FOUND, 0, NULL);
        return NCL_ERR_NOT_FOUND;
    }
    {
        ncl_err call_result;
        int64_t started = ncl_time_monotonic_millis();

        call_result = entry->driver->ops->call(entry->driver, operation, params,
                                               result);
        ncl_audit_request(entry->id, path, operation, call_result,
                          ncl_time_monotonic_millis() - started,
                          audit_raw(entry->driver, &raw));
        return call_result;
    }
}

void ncl_driver_manager_attach_event(ncl_driver_manager *manager,
                                     ncl_driver_event_fn fn, void *user)
{
    size_t i;

    if (manager == NULL) {
        return;
    }
    for (i = 0; i < manager->count; i++) {
        driver_entry *entry = &manager->entries[i];

        if (entry->driver->ops->attach_event != NULL) {
            entry->driver->ops->attach_event(entry->driver, fn, user);
        }
    }
}

ncl_err ncl_driver_manager_open_all(ncl_driver_manager *manager, ncl_strbuf *err)
{
    ncl_err first = NCL_OK;
    size_t i;

    if (manager == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < manager->count; i++) {
        driver_entry *entry = &manager->entries[i];
        ncl_err result;

        if (entry->driver->ops->open == NULL) {
            continue;
        }
        result = entry->driver->ops->open(entry->driver);
        ncl_audit_session(entry->id, "open", result == NCL_OK ? NULL : "failed");
        if (result != NCL_OK) {
            /* An offline device is not a fatal error: the session is opened
             * again on demand. The caller decides what to do with the list. */
            err_append1(err, "link %s did not open", entry->id);
            if (first == NCL_OK) {
                first = result;
            }
        }
    }
    return first;
}

void ncl_driver_manager_close_all(ncl_driver_manager *manager)
{
    size_t i;

    if (manager == NULL) {
        return;
    }
    for (i = 0; i < manager->count; i++) {
        driver_entry *entry = &manager->entries[i];

        if (entry->driver->ops->close != NULL) {
            entry->driver->ops->close(entry->driver);
            ncl_audit_session(entry->id, "close", NULL);
        }
    }
}
