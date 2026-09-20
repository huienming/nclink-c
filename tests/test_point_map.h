/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * A tiny point map for the protocol tests.
 *
 * The adapter host has no point map any more: its points come from a module's
 * declaration (nclink/ncl_tool.h). A protocol test, though, still wants the old
 * shape - "here is a driver configuration with points, read me this path" - and
 * this is exactly that, and nothing else: a driver, a list of {path, address},
 * and reads and writes through ncl_driver_read_one()/ncl_driver_write_one().
 *
 * Header only and static, so a test just includes it. How an address is written
 * ("D32", "M3", {"area":..,"offset":..}) stays the driver's business:
 * ncl_address_from_json() is what parses it.
 *
 * The driver comes either from a factory the test passes, or - when it passes
 * NULL - from the configuration's "type" through the driver registry, which is
 * what a test that registers its own protocol needs.
 */
#ifndef NCL_TEST_POINT_MAP_H
#define NCL_TEST_POINT_MAP_H

#include <stdio.h>
#include <string.h>

#include "nclink/ncl_common.h"
#include "nclink/ncl_json.h"
#include "nclink_adapter/ncl_audit.h"
#include "nclink_adapter/ncl_driver.h"

typedef struct {
    char        *key; /**< the model path the test asks for */
    ncl_address  address;
} test_point;

typedef struct {
    ncl_driver  *driver;
    char        *prefix;
    char        *id;     /**< the link's name, for the audit trail (§6) */
    test_point  *points;
    size_t       count;
    size_t       capacity;
} test_point_map;

/**
 * Make the map. With @p factory the driver is built from it; with NULL the
 * driver is built later, from the configuration's "type" (the registry).
 */
static test_point_map *test_point_map_create(ncl_driver_factory factory)
{
    test_point_map *map = (test_point_map *)ncl_mem_calloc(1, sizeof(*map));

    if (map == NULL) {
        return NULL;
    }
    if (factory != NULL) {
        map->driver = factory();
        if (map->driver == NULL) {
            ncl_mem_free(map);
            return NULL;
        }
    }
    return map;
}

static void test_point_map_free(test_point_map *map)
{
    size_t i;

    if (map == NULL) {
        return;
    }
    for (i = 0; i < map->count; i++) {
        ncl_free_safe(map->points[i].key);
        ncl_address_clear(&map->points[i].address);
    }
    ncl_mem_free(map->points);
    ncl_free_safe(map->prefix);
    ncl_free_safe(map->id);
    if (map->driver != NULL && map->driver->ops != NULL &&
        map->driver->ops->destroy != NULL) {
        map->driver->ops->destroy(map->driver);
    }
    ncl_mem_free(map);
}

/** "/PLC" + "TEMP", or the path as it stands when it is already absolute. */
static char *test_point_absolute(const char *prefix, const char *path)
{
    char buffer[512];

    if (path[0] == '/' || ncl_str_is_blank(prefix)) {
        return ncl_strdup(path);
    }
    snprintf(buffer, sizeof(buffer), "%s%s", prefix, path);
    return ncl_strdup(buffer);
}

/**
 * The link inside @p config: a bare link object, the first entry of a
 * "drivers" array, or the first entry of an array - the three shapes a test
 * writes ("here is one link").
 */
static const ncl_json *test_point_map_link(const ncl_json *config)
{
    const ncl_json *drivers;

    if (ncl_json_type_of(config) == NCL_JSON_ARRAY) {
        return ncl_json_arr_get(config, 0);
    }
    drivers = ncl_json_obj_get(config, "drivers");
    if (ncl_json_type_of(drivers) == NCL_JSON_ARRAY) {
        return ncl_json_arr_get(drivers, 0);
    }
    return config;
}

/**
 * Configure the driver from @p config's "parameters" and take its "points":
 *
 *   {"path": "/PLC", "parameters": {...},
 *    "points": [{"path": "/PLC/TEMP", "addr": "D32"}, ...]}
 *
 * A point may carry its own "dtype" and "length". A point whose key is already
 * there is replaced, so a later file may override an earlier one.
 */
static ncl_err test_point_map_add_json(test_point_map *map,
                                       const ncl_json *config, ncl_strbuf *err)
{
    const ncl_json *parameters;
    const ncl_json *points;
    config = test_point_map_link(config);
    if (ncl_json_type_of(config) != NCL_JSON_OBJECT) {
        return NCL_ERR_INVALID_ARG;
    }
    parameters = ncl_json_obj_get(config, "parameters");
    points = ncl_json_obj_get(config, "points");
    const ncl_driver_ops *ops;
    const char *prefix = ncl_json_obj_get_string(config, "path");
    size_t i;
    ncl_err rc;

    if (map->driver == NULL) {
        const char *protocol = ncl_json_obj_get_string(config, "type");

        /* The built-in protocols (mock and friends) are always available. */
        ncl_driver_register_builtin();
        map->driver = protocol != NULL ? ncl_driver_create(protocol) : NULL;
        if (map->driver == NULL) {
            if (err != NULL) {
                (void)ncl_strbuf_printf(err, "no driver for \"%s\"",
                                        protocol != NULL ? protocol : "?");
            }
            return NCL_ERR_NOT_FOUND;
        }
    }
    ops = ncl_driver_ops_of(map->driver);
    if (ops == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (ops->create != NULL) {
        rc = ops->create(map->driver, parameters);
        if (rc != NCL_OK) {
            if (err != NULL) {
                (void)ncl_strbuf_puts(err, "the driver refused its parameters");
            }
            return rc;
        }
    }
    if (ncl_json_type_of(points) != NCL_JSON_ARRAY) {
        return NCL_OK;
    }
    ncl_free_safe(map->prefix);
    map->prefix = ncl_strdup(prefix != NULL ? prefix : "");
    ncl_free_safe(map->id);
    map->id = ncl_strdup(ncl_json_obj_get_string(config, "id") != NULL
                             ? ncl_json_obj_get_string(config, "id")
                             : "test");
    for (i = 0; i < ncl_json_arr_len(points); i++) {
        const ncl_json *point = ncl_json_arr_get(points, i);
        const char *path = ncl_json_obj_get_string(point, "path");
        const ncl_json *addr = ncl_json_obj_get(point, "addr");
        ncl_address address;
        ncl_dtype dtype;
        char *absolute;
        size_t at;

        if (addr == NULL) {
            addr = ncl_json_obj_get(point, "address");
        }
        if (ncl_str_is_blank(path)) {
            path = ncl_json_obj_get_string(point, "id");
        }
        if (ncl_str_is_blank(path) || addr == NULL) {
            if (err != NULL) {
                (void)ncl_strbuf_puts(err, "a point needs \"path\" and \"addr\"");
            }
            return NCL_ERR_INVALID_ARG;
        }
        absolute = test_point_absolute(map->prefix, path);
        rc = ncl_address_from_json(addr, &address);
        if (rc != NCL_OK) {
            if (err != NULL) {
                (void)ncl_strbuf_printf(err, "the point %s has an unusable address",
                                        absolute);
            }
            ncl_free_safe(absolute);
            return rc;
        }
        if (address.bit < 0 &&
            ncl_dtype_from_object(point, "dtype", &dtype) == NCL_OK) {
            address.dtype = dtype;
        }
        if (address.length == 1 && ncl_json_obj_has(point, "length")) {
            long long length = ncl_json_obj_get_int(point, "length", 1);

            if (length > 0 && length <= 65536) {
                address.length = (int)length;
            }
        }
        at = map->count;
        if (at == map->capacity) {
            size_t capacity = map->capacity == 0 ? 8 : map->capacity * 2;
            test_point *grown = (test_point *)ncl_mem_realloc(
                map->points, capacity * sizeof(*grown));

            if (grown == NULL) {
                ncl_free_safe(absolute);
                ncl_address_clear(&address);
                return NCL_ERR_NOMEM;
            }
            map->points = grown;
            map->capacity = capacity;
        }
        map->points[at].key = absolute;
        map->points[at].address = address;
        map->count++;
    }
    return NCL_OK;
}

static const ncl_address *test_point_map_find(const test_point_map *map,
                                              const char *path)
{
    size_t i;

    for (i = 0; map != NULL && i < map->count; i++) {
        if (strcmp(map->points[i].key, path) == 0) {
            return &map->points[i].address;
        }
    }
    return NULL;
}

/** The frames the driver just exchanged, when the audit wants them (§6). */
static const ncl_driver_raw *test_point_map_raw(const ncl_driver *driver,
                                                ncl_driver_raw *out)
{
    if (driver == NULL || !ncl_audit_wants_raw()) {
        return NULL;
    }
    ncl_driver_last_raw(driver, out);
    return out->request != NULL || out->reply != NULL ? out : NULL;
}

/**
 * Read a path. Like the point map the adapter host used to carry, this keeps
 * the §6 trail: what was asked, which address it answered for, the outcome and
 * the frames (when the audit asked for them).
 */
static ncl_err test_point_map_read(test_point_map *map, const char *path,
                                   ncl_json **value)
{
    const ncl_address *address = test_point_map_find(map, path);
    ncl_driver_raw raw;
    int64_t started;
    ncl_err rc;

    if (address == NULL) {
        ncl_audit_request(map != NULL ? map->id : "-", path, "-",
                          NCL_ERR_NOT_FOUND, 0, NULL);
        return NCL_ERR_NOT_FOUND;
    }
    started = ncl_time_monotonic_millis();
    rc = ncl_driver_read_one(map->driver, address, value);
    {
        char *text = ncl_address_to_text(address);

        ncl_audit_request(map->id, path, text != NULL ? text : "?", rc,
                          ncl_time_monotonic_millis() - started,
                          test_point_map_raw(map->driver, &raw));
        ncl_free_safe(text);
    }
    return rc;
}

/** Write a path, with the old value read first so the trail can say what
 *  changed (§6: "改了哪个地址、旧值、新值、操作者"). */
static ncl_err test_point_map_write(test_point_map *map, const char *path,
                                    const ncl_json *value)
{
    const ncl_address *address = test_point_map_find(map, path);
    ncl_json *old_value = NULL;
    int64_t started;
    char *text;
    ncl_err rc;

    if (address == NULL) {
        ncl_audit_write(map != NULL ? map->id : "-", path, "-", NULL, value,
                        NCL_ERR_NOT_FOUND);
        ncl_audit_request(map != NULL ? map->id : "-", path, "-",
                          NCL_ERR_NOT_FOUND, 0, NULL);
        return NCL_ERR_NOT_FOUND;
    }
    text = ncl_address_to_text(address);
    if (ncl_audit_enabled()) {
        (void)ncl_driver_read_one(map->driver, address, &old_value);
    }
    started = ncl_time_monotonic_millis();
    rc = ncl_driver_write_one(map->driver, address, value);
    ncl_audit_write(map->id, path, text != NULL ? text : "?", old_value, value,
                    rc);
    {
        ncl_driver_raw raw;

        ncl_audit_request(map->id, path, text != NULL ? text : "?", rc,
                          ncl_time_monotonic_millis() - started,
                          test_point_map_raw(map->driver, &raw));
    }
    ncl_json_free(old_value);
    ncl_free_safe(text);
    return rc;
}

static size_t test_point_map_count(const test_point_map *map)
{
    return map != NULL ? map->count : 0;
}

/** The driver itself, for a test that wants to poke at the ops. */
static ncl_driver *test_point_map_driver(const test_point_map *map)
{
    return map != NULL ? map->driver : NULL;
}

static ncl_err test_point_map_open(test_point_map *map)
{
    const ncl_driver_ops *ops = ncl_driver_ops_of(map->driver);
    ncl_err rc;

    rc = ops != NULL && ops->open != NULL ? ops->open(map->driver) : NCL_OK;
    ncl_audit_session(map != NULL ? map->id : "-", "open",
                      rc == NCL_OK ? NULL : "failed");
    return rc;
}

static void test_point_map_close(test_point_map *map)
{
    const ncl_driver_ops *ops = ncl_driver_ops_of(map->driver);

    if (ops != NULL && ops->close != NULL) {
        ops->close(map->driver);
    }
    ncl_audit_session(map != NULL ? map->id : "-", "close", NULL);
}

#endif /* NCL_TEST_POINT_MAP_H */
