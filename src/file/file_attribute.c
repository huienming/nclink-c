/*
 * NC-Link core - file attributes, checksums and transfer helpers.
 */
#include "nclink/ncl_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"

/* Extensions whose content is compressed on transfer. */
static const char *const k_compress_extensions[] = {
    ".txt",  ".log",  ".xml",  ".json", ".csv",  ".doc",  ".docx", ".xls",
    ".xlsx", ".pdf",  ".java", ".cpp",  ".py",   ".html", ".css",  ".js"};

/* ---------------------------------------------------------- attribute ----- */

ncl_file_attribute *ncl_file_attribute_new(void)
{
    ncl_file_attribute *attribute =
        (ncl_file_attribute *)calloc(1, sizeof(*attribute));
    return attribute;
}

void ncl_file_attribute_free(ncl_file_attribute *attribute)
{
    if (attribute == NULL) {
        return;
    }
    free(attribute->file_name);
    free(attribute->checksum);
    free(attribute->parant_dir);
    free(attribute);
}

void ncl_file_attribute_release(void *attribute)
{
    ncl_file_attribute_free((ncl_file_attribute *)attribute);
}

bool ncl_file_attribute_is_dir(const ncl_file_attribute *attribute)
{
    return attribute != NULL && attribute->file_type == 1;
}

/* Property order: fileName, fileType, fileSize, totalChunks, compressed,
 * checksum, parantDir, modifyTime. Date fields serialise as epoch millis, and
 * a null field is omitted. */
ncl_json *ncl_file_attribute_to_json(const ncl_file_attribute *attribute)
{
    ncl_json *json;

    if (attribute == NULL) {
        return NULL;
    }
    json = ncl_json_new_object();
    if (json == NULL) {
        return NULL;
    }
    if (attribute->file_name != NULL) {
        ncl_json_obj_set_string(json, "fileName", attribute->file_name);
    }
    ncl_json_obj_set_int(json, "fileType", attribute->file_type);
    ncl_json_obj_set_int(json, "fileSize", attribute->file_size);
    ncl_json_obj_set_int(json, "totalChunks", attribute->total_chunks);
    ncl_json_obj_set_bool(json, "compressed", attribute->compressed);
    if (attribute->checksum != NULL) {
        ncl_json_obj_set_string(json, "checksum", attribute->checksum);
    }
    if (attribute->parant_dir != NULL) {
        ncl_json_obj_set_string(json, "parantDir", attribute->parant_dir);
    }
    if (attribute->modify_time != 0) {
        ncl_json_obj_set_int(json, "modifyTime", attribute->modify_time);
    }
    return json;
}

ncl_file_attribute *ncl_file_attribute_from_json(const ncl_json *json)
{
    ncl_file_attribute *attribute;
    const char *text;

    if (json == NULL || ncl_json_type_of(json) != NCL_JSON_OBJECT) {
        return NULL;
    }
    attribute = ncl_file_attribute_new();
    if (attribute == NULL) {
        return NULL;
    }
    text = ncl_json_obj_get_string(json, "fileName");
    attribute->file_name = text != NULL ? ncl_strdup(text) : NULL;
    attribute->file_type =
        (int)ncl_json_obj_get_int(json, "fileType", 0);
    attribute->file_size = ncl_json_obj_get_int(json, "fileSize", 0);
    attribute->total_chunks = (int)ncl_json_obj_get_int(json, "totalChunks", 0);
    attribute->compressed =
        ncl_json_obj_get_bool(json, "compressed", false);
    text = ncl_json_obj_get_string(json, "checksum");
    attribute->checksum = text != NULL ? ncl_strdup(text) : NULL;
    text = ncl_json_obj_get_string(json, "parantDir");
    attribute->parant_dir = text != NULL ? ncl_strdup(text) : NULL;
    attribute->modify_time = ncl_json_obj_get_int(json, "modifyTime", 0);
    return attribute;
}

ncl_json *ncl_file_attributes_to_json(const ncl_ptrvec *attributes)
{
    ncl_json *array;
    size_t i;

    array = ncl_json_new_array();
    if (array == NULL) {
        return NULL;
    }
    for (i = 0; attributes != NULL && i < attributes->len; i++) {
        const ncl_file_attribute *attribute =
            (const ncl_file_attribute *)attributes->items[i];
        ncl_json *item = ncl_file_attribute_to_json(attribute);
        if (item == NULL) {
            ncl_json_free(array);
            return NULL;
        }
        ncl_json_arr_push(array, item);
    }
    return array;
}

/* --------------------------------------------------------------- Utils ---- */

bool ncl_file_need_compression(const char *file_name)
{
    size_t i;

    if (file_name == NULL) {
        return false;
    }
    for (i = 0; i < sizeof(k_compress_extensions) / sizeof(k_compress_extensions[0]);
         i++) {
        size_t name_len = strlen(file_name);
        size_t ext_len = strlen(k_compress_extensions[i]);
        if (name_len < ext_len) {
            continue;
        }
        if (ncl_strcasecmp(file_name + name_len - ext_len,
                           k_compress_extensions[i]) == 0) {
            return true;
        }
    }
    return false;
}

int ncl_file_total_chunks(long long size)
{
    if (size <= 0) {
        return 0;
    }
    return (int)(size / NCL_FILE_CHUNK_SIZE +
                 (size % NCL_FILE_CHUNK_SIZE > 0 ? 1 : 0));
}

ncl_err ncl_file_checksum(const char *path, char **out_hex)
{
    ncl_strbuf sb;
    FILE *fp;
    char chunk[8192];
    ncl_err rc;

    if (path == NULL || out_hex == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *out_hex = NULL;
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return NCL_ERR_IO;
    }
    ncl_strbuf_init(&sb);
    for (;;) {
        size_t got = fread(chunk, 1, sizeof(chunk), fp);
        if (got > 0 && ncl_strbuf_append(&sb, chunk, got) != NCL_OK) {
            fclose(fp);
            ncl_strbuf_free(&sb);
            return NCL_ERR_NOMEM;
        }
        if (got < sizeof(chunk)) {
            break;
        }
    }
    fclose(fp);
    rc = ncl_sha256_hex(sb.data, sb.len, out_hex);
    ncl_strbuf_free(&sb);
    return rc;
}

/** Basename of @p path (both separators accepted). */
static const char *ncl_file_basename(const char *path)
{
    const char *slash;
    const char *back;

    if (path == NULL) {
        return NULL;
    }
    slash = strrchr(path, '/');
    back = strrchr(path, '\\');
    if (back != NULL && (slash == NULL || back > slash)) {
        slash = back;
    }
    return slash != NULL ? slash + 1 : path;
}

/** Parent of @p path, or @p fallback; writes into @p out. */
static const char *ncl_file_parent_of(const char *path, char *out,
                                      size_t out_len)
{
    char *slash;

    if (path == NULL || out_len == 0) {
        return NULL;
    }
    snprintf(out, out_len, "%s", path);
    slash = strrchr(out, '/');
    if (slash == NULL) {
        slash = strrchr(out, '\\');
    }
    if (slash == NULL) {
        return NULL;
    }
    if (slash == out) {
        out[1] = '\0';
        return out;
    }
    *slash = '\0';
    return out;
}

ncl_err ncl_file_attribute_of(const char *path, const char *parent,
                              ncl_file_attribute **out)
{
    ncl_file_attribute *attribute;
    char parent_buf[NCL_PATH_MAX_BUF];
    bool is_dir;
    long long size;

    if (path == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!ncl_path_exists(path)) {
        return NCL_ERR_NOT_FOUND;
    }
    is_dir = ncl_path_is_dir(path);
    size = is_dir ? 0 : ncl_file_size(path);
    if (size < 0) {
        size = 0;
    }
    attribute = ncl_file_attribute_new();
    if (attribute == NULL) {
        return NCL_ERR_NOMEM;
    }
    attribute->file_name = ncl_strdup(ncl_file_basename(path));
    attribute->file_type = is_dir ? 1 : 0;
    attribute->file_size = size;
    attribute->total_chunks = is_dir ? 0 : ncl_file_total_chunks(size);
    attribute->compressed =
        is_dir ? false : ncl_file_need_compression(attribute->file_name);
    if (is_dir) {
        attribute->checksum = ncl_strdup("");
    } else {
        if (ncl_file_checksum(path, &attribute->checksum) != NCL_OK) {
            attribute->checksum = ncl_strdup("");
        }
    }
    {
        const char *dir = parent;
        if (dir == NULL) {
            dir = ncl_file_parent_of(path, parent_buf, sizeof(parent_buf));
        }
        attribute->parant_dir = dir != NULL ? ncl_strdup(dir) : NULL;
    }
    attribute->modify_time = ncl_file_mtime_ms(path);
    if (attribute->file_name == NULL) {
        ncl_file_attribute_free(attribute);
        return NCL_ERR_NOMEM;
    }
    *out = attribute;
    return NCL_OK;
}

/**
 * Attribute of a remote entry as reported by LIST. For a file the formatted
 * modification time goes into the checksum field, which is what the receiving
 * side compares against its own copy.
 */
ncl_err ncl_file_attribute_from_entry(const ncl_ftp_entry *entry,
                                      const char *parent,
                                      ncl_file_attribute **out)
{
    ncl_file_attribute *attribute;
    struct tm tm;
    char stamp[32];
    time_t seconds;

    if (entry == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *out = NULL;
    attribute = ncl_file_attribute_new();
    if (attribute == NULL) {
        return NCL_ERR_NOMEM;
    }
    attribute->file_name = ncl_strdup(entry->name);
    attribute->file_type = entry->type;
    attribute->file_size = ncl_ftp_entry_is_dir(entry) ? 0 : entry->size;
    attribute->total_chunks =
        ncl_ftp_entry_is_dir(entry) ? 0 : ncl_file_total_chunks(entry->size);
    attribute->compressed =
        ncl_ftp_entry_is_dir(entry)
            ? false
            : ncl_file_need_compression(entry->name);
    attribute->parant_dir = parent != NULL ? ncl_strdup(parent) : NULL;
    attribute->modify_time = entry->modify_time;

    seconds = (time_t)(entry->modify_time / 1000);
    memset(&tm, 0, sizeof(tm));
#if defined(NCL_OS_WINDOWS)
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec);
    attribute->checksum =
        ncl_strdup(ncl_ftp_entry_is_dir(entry) ? "" : stamp);

    if (attribute->file_name == NULL || attribute->checksum == NULL) {
        ncl_file_attribute_free(attribute);
        return NCL_ERR_NOMEM;
    }
    *out = attribute;
    return NCL_OK;
}

/* --------------------------------------------------------- ftpResponse ---- */

void ncl_ftp_info_free(ncl_ftp_response *info)
{
    if (info == NULL) {
        return;
    }
    free(info->user_name);
    free(info->password);
    ncl_json_free(info->ip_map);
    memset(info, 0, sizeof(*info));
}

ncl_err ncl_ftp_info_read(ncl_ftp_response *out)
{
    char path[NCL_PATH_MAX_BUF];
    char *text = NULL;
    ncl_json *json = NULL;
    ncl_err rc;

    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    snprintf(path, sizeof(path), "%s%cbin%cftp.txt", ncl_env_root(),
             NCL_PATH_SEP, NCL_PATH_SEP);
    if (ncl_path_exists(path) &&
        ncl_file_read_all(path, &text, NULL) == NCL_OK && text != NULL) {
        json = ncl_json_parse_cstr(text, NULL);
        free(text);
    }
    if (json != NULL) {
        const char *value;
        out->user_name = NULL;
        value = ncl_json_obj_get_string(json, "userName");
        if (value == NULL) {
            value = ncl_json_obj_get_string(json, "username");
        }
        out->user_name =
            ncl_strdup(value != NULL ? value : NCL_FTP_DEFAULT_USER);
        value = ncl_json_obj_get_string(json, "password");
        out->password =
            ncl_strdup(value != NULL ? value : NCL_FTP_DEFAULT_PASSWORD);
        out->port = (int)ncl_json_obj_get_int(json, "port", NCL_FTP_SERVER_PORT);
        out->ip_map = ncl_json_obj_get(json, "ipMap");
        out->ip_map = out->ip_map != NULL ? ncl_json_clone(out->ip_map) : NULL;
        ncl_json_free(json);
        if (out->user_name == NULL || out->password == NULL) {
            ncl_ftp_info_free(out);
            return NCL_ERR_NOMEM;
        }
        return NCL_OK;
    }
    /* Built in credentials plus the local interface map, then persist. */
    out->user_name = ncl_strdup(NCL_FTP_DEFAULT_USER);
    out->password = ncl_strdup(NCL_FTP_DEFAULT_PASSWORD);
    out->port = NCL_FTP_SERVER_PORT;
    {
        char *map = ncl_net_ip_map_json();
        out->ip_map = map != NULL ? ncl_json_parse_cstr(map, NULL) : NULL;
        free(map);
    }
    if (out->user_name == NULL || out->password == NULL) {
        ncl_ftp_info_free(out);
        return NCL_ERR_NOMEM;
    }
    rc = ncl_ftp_info_write(out);
    return rc == NCL_OK ? NCL_OK : rc;
}

ncl_err ncl_ftp_info_write(const ncl_ftp_response *info)
{
    char path[NCL_PATH_MAX_BUF];
    char dir[NCL_PATH_MAX_BUF];
    ncl_json *json;
    char *text;
    ncl_err rc;

    if (info == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    json = ncl_json_new_object();
    if (json == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (info->user_name != NULL) {
        ncl_json_obj_set_string(json, "userName", info->user_name);
    }
    if (info->password != NULL) {
        ncl_json_obj_set_string(json, "password", info->password);
    }
    ncl_json_obj_set_int(json, "port", info->port);
    if (info->ip_map != NULL) {
        ncl_json_obj_set(json, "ipMap", ncl_json_clone(info->ip_map));
    }
    text = ncl_json_write_string(json);
    ncl_json_free(json);
    if (text == NULL) {
        return NCL_ERR_NOMEM;
    }
    snprintf(dir, sizeof(dir), "%s%cbin", ncl_env_root(), NCL_PATH_SEP);
    snprintf(path, sizeof(path), "%s%cftp.txt", dir, NCL_PATH_SEP);
    ncl_mkdir_p(dir);
    rc = ncl_file_write_all(path, text, strlen(text));
    free(text);
    return rc;
}
