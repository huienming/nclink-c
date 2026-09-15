/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - configuration file access. */
#include "nclink/ncl_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"

/* Paths --------------------------------------------------------------- */

static const char *ncl_config_model_path(void)
{
    return ncl_env_model_file();
}

static const char *ncl_config_driver_path(void)
{
    return ncl_env_driver_cfg_file();
}

static char *ncl_config_server_path(void)
{
    const char *conf = ncl_env_conf_path();
    char *path = NULL;
    if (conf == NULL) {
        return NULL;
    }
    ncl_asprintf(&path, "%s%cdriver%cserver.json", conf, NCL_PATH_SEP,
                 NCL_PATH_SEP);
    return path;
}

/** <root>/conf/ipConf.json (the ipConf.json accessors). */
static char *ncl_config_ip_path(void)
{
    const char *conf = ncl_env_conf_path();
    char *path = NULL;
    if (conf == NULL) {
        return NULL;
    }
    ncl_asprintf(&path, "%s%cipConf.json", conf, NCL_PATH_SEP);
    return path;
}

/** Read a configuration file with the size limits Configure applied. */
static ncl_err ncl_config_read(const char *path, size_t max_bytes, char **out,
                               size_t *out_len)
{
    char *text = NULL;
    size_t len = 0;

    if (path == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (!ncl_path_exists(path)) {
        return NCL_ERR_NOT_FOUND;
    }
    if (ncl_file_read_all(path, &text, &len) != NCL_OK) {
        return NCL_ERR_IO;
    }
    if (len > max_bytes) {
        free(text);
        return NCL_ERR_RANGE;
    }
    if (ncl_str_is_blank(text)) {
        free(text);
        return NCL_ERR_NOT_FOUND;
    }
    *out = text;
    if (out_len != NULL) {
        *out_len = len;
    }
    return NCL_OK;
}

/* ================================================================ init === */

/**
 * The serial number written when the caller supplies none: 32 lower case hex
 * characters (a random UUID with the dashes removed), *not* the "V2..." form
 * ncl_sn_read() produces. The init call also
 * overwrites bin/sn.txt unconditionally, so calling the /api/cfg/init endpoint
 * replaces the identity a device already has. Both quirks are reproduced
 * deliberately; see PORTING.md section 3.
 */
static char *ncl_config_random_sn(void)
{
    char uuid[37];
    char *out;
    size_t i;

    if (ncl_uuid4(uuid, sizeof(uuid)) != NCL_OK) {
        return NULL;
    }
    out = (char *)malloc(33);
    if (out == NULL) {
        return NULL;
    }
    {
        size_t written = 0;
        for (i = 0; i < 36 && written < 32; i++) {
            if (uuid[i] != '-') {
                out[written++] = uuid[i];
            }
        }
        out[written] = '\0';
    }
    return out;
}

ncl_err ncl_config_init(const char *sn, char **out_sn)
{
    const char *root = ncl_env_root();
    const char *bin_dir;
    const char *conf_dir;
    const char *log_dir;
    char *serial = NULL;
    ncl_err rc;

    if (out_sn != NULL) {
        *out_sn = NULL;
    }
    if (root == NULL) {
        return NCL_ERR_IO;
    }
    bin_dir = ncl_env_run_path();
    conf_dir = ncl_env_conf_path();
    log_dir = ncl_env_log_path();

    /* cfgInit() created bin, conf and log before touching the SN file. */
    if (bin_dir != NULL && ncl_mkdir_p(bin_dir) != NCL_OK) {
        return NCL_ERR_IO;
    }
    if (conf_dir != NULL && ncl_mkdir_p(conf_dir) != NCL_OK) {
        return NCL_ERR_IO;
    }
    if (log_dir != NULL && ncl_mkdir_p(log_dir) != NCL_OK) {
        return NCL_ERR_IO;
    }

    if (sn != NULL && sn[0] != '\0') {
        serial = ncl_strdup(sn);
    } else {
        serial = ncl_config_random_sn();
    }
    if (serial == NULL) {
        return NCL_ERR_NOMEM;
    }

    {
        const char *sn_file = ncl_env_sn_file();
        if (sn_file == NULL) {
            free(serial);
            return NCL_ERR_IO;
        }
        rc = ncl_file_write_all(sn_file, serial, strlen(serial));
        if (rc != NCL_OK) {
            free(serial);
            return rc;
        }
    }
    if (out_sn != NULL) {
        *out_sn = serial;
    } else {
        free(serial);
    }
    return NCL_OK;
}

char *ncl_config_get_sn(void)
{
    /* Like ncl_sn_read(): the file is generated when it is missing. */
    return ncl_sn_read();
}

/* =============================================================== model === */

ncl_json *ncl_config_get_model(void)
{
    char *text = NULL;
    ncl_json *document;
    ncl_err rc = ncl_config_read(ncl_config_model_path(), NCL_CONFIG_MAX_FILE_BYTES,
                                 &text, NULL);

    if (rc != NCL_OK) {
        return NULL;
    }
    document = ncl_json_parse_cstr(text, NULL);
    free(text);
    return document;
}

ncl_err ncl_config_set_model(const char *json)
{
    if (json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    {
        const char *path = ncl_config_model_path();
        const char *conf = ncl_env_conf_path();
        if (path == NULL) {
            return NCL_ERR_IO;
        }
        if (conf != NULL) {
            char *model_dir = NULL;
            ncl_asprintf(&model_dir, "%s%cmodel", conf, NCL_PATH_SEP);
            if (model_dir != NULL) {
                ncl_mkdir_p(model_dir);
                free(model_dir);
            }
        }
        return ncl_file_write_all(path, json, strlen(json));
    }
}

/* ============================================================== driver === */

ncl_json *ncl_config_get_driver(void)
{
    char *text = NULL;
    ncl_json *document;
    ncl_err rc = ncl_config_read(ncl_config_driver_path(),
                                 NCL_CONFIG_MAX_FILE_BYTES, &text, NULL);

    if (rc != NCL_OK) {
        return NULL;
    }
    document = ncl_json_parse_cstr(text, NULL);
    free(text);
    return document;
}

ncl_err ncl_config_set_driver(const char *json)
{
    const char *path;
    if (json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    path = ncl_config_driver_path();
    if (path == NULL) {
        return NCL_ERR_IO;
    }
    {
        const char *conf = ncl_env_conf_path();
        if (conf != NULL) {
            char *driver_dir = NULL;
            ncl_asprintf(&driver_dir, "%s%cdriver", conf, NCL_PATH_SEP);
            if (driver_dir != NULL) {
                ncl_mkdir_p(driver_dir);
                free(driver_dir);
            }
        }
    }
    return ncl_file_write_all(path, json, strlen(json));
}

/* ========================================================= server list === */

ncl_json *ncl_config_get_server_list(void)
{
    char *path = ncl_config_server_path();
    char *text = NULL;
    ncl_json *document = NULL;

    if (path != NULL &&
        ncl_config_read(path, NCL_CONFIG_MAX_FILE_BYTES, &text, NULL) == NCL_OK) {
        document = ncl_json_parse_cstr(text, NULL);
        free(text);
    }
    free(path);
    if (document != NULL) {
        return document;
    }

    /* Fall back to the in-memory list (env.serverList). */
    document = ncl_json_new_array();
    if (document == NULL) {
        return NULL;
    }
    {
        size_t i;
        for (i = 0; i < ncl_env_server_count(); i++) {
            ncl_json_arr_push(document,
                              ncl_json_new_string(ncl_env_server_at(i)));
        }
    }
    return document;
}

ncl_err ncl_config_set_server_list(const char *json)
{
    char *path;
    ncl_err rc;

    if (json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    path = ncl_config_server_path();
    if (path == NULL) {
        return NCL_ERR_NOMEM;
    }
    {
        const char *conf = ncl_env_conf_path();
        if (conf != NULL) {
            char *driver_dir = NULL;
            ncl_asprintf(&driver_dir, "%s%cdriver", conf, NCL_PATH_SEP);
            if (driver_dir != NULL) {
                ncl_mkdir_p(driver_dir);
                free(driver_dir);
            }
        }
    }
    rc = ncl_file_write_all(path, json, strlen(json));
    free(path);
    return rc;
}

/* ============================================================ mqtt.cfg === */

/**
 * Every "key=value" line becomes a map entry, so the reply is an object with the
 * three known keys.
 */
ncl_json *ncl_config_get_mqtt(void)
{
    char *text = NULL;
    ncl_json *config;
    ncl_err rc = ncl_config_read(ncl_env_mqtt_cfg_file(),
                                 NCL_CONFIG_MAX_MQTT_BYTES, &text, NULL);

    if (rc != NCL_OK) {
        return NULL;
    }
    config = ncl_json_new_object();
    if (config == NULL) {
        free(text);
        return NULL;
    }
    {
        /* One "key=value" line per entry. */
        char *cursor = text;
        while (*cursor != '\0') {
            char *line_end = strpbrk(cursor, "\r\n");
            char *equals;
            char saved;
            if (line_end == NULL) {
                line_end = cursor + strlen(cursor);
            }
            saved = *line_end;
            *line_end = '\0';
            equals = strchr(cursor, '=');
            if (equals != NULL) {
                char *key = cursor;
                char *value = equals + 1;
                *equals = '\0';
                if (key[0] != '\0') {
                    ncl_json_obj_set_string(config, key, value);
                }
            }
            if (saved == '\0') {
                break;
            }
            cursor = line_end + 1;
        }
    }
    free(text);
    return config;
}

ncl_err ncl_config_set_mqtt(const ncl_json *config)
{
    const char *path;
    const char *url;
    const char *username;
    const char *password;
    ncl_strbuf text;
    ncl_err rc;

    if (config == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    url = ncl_json_obj_get_string(config, "url");
    username = ncl_json_obj_get_string(config, "username");
    password = ncl_json_obj_get_string(config, "password");
    if (url == NULL) {
        return NCL_ERR_INVALID_VALUE;
    }
    path = ncl_env_mqtt_cfg_file();
    if (path == NULL) {
        return NCL_ERR_IO;
    }

    /* setMqttUrl() wrote exactly this three line form. */
    ncl_strbuf_init(&text);
    ncl_strbuf_printf(&text, "url=%s\nusername=%s\npassword=%s", url,
                      username != NULL ? username : "",
                      password != NULL ? password : "");
    rc = ncl_file_write_all(path, text.data, text.len);
    ncl_strbuf_free(&text);
    return rc;
}

/* =========================================================== ipConf.json == */

ncl_json *ncl_config_get_ip_conf(void)
{
    char *path = ncl_config_ip_path();
    char *text = NULL;
    ncl_json *document = NULL;

    if (path != NULL &&
        ncl_config_read(path, NCL_CONFIG_MAX_FILE_BYTES, &text, NULL) == NCL_OK) {
        document = ncl_json_parse_cstr(text, NULL);
        free(text);
    }
    free(path);
    return document;
}

ncl_err ncl_config_set_ip_conf(const char *json)
{
    char *path;
    ncl_err rc;

    if (json == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    path = ncl_config_ip_path();
    if (path == NULL) {
        return NCL_ERR_NOMEM;
    }
    {
        const char *conf = ncl_env_conf_path();
        if (conf != NULL) {
            ncl_mkdir_p(conf);
        }
    }
    rc = ncl_file_write_all(path, json, strlen(json));
    free(path);
    return rc;
}
