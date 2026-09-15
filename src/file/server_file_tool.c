/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - device side file tool.
 *
 * The device side helper: an FTP client that mirrors files under the peer's
 * "/<sn>/..." tree and keeps a local copy under <root>/uploadFile.
 */
#include "nclink/ncl_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"

#if !defined(NCL_OS_WINDOWS)
#  include <dirent.h>
#endif

struct ncl_server_file_tool {
    ncl_mutex     *lock;
    ncl_ftp_client *client;
    char          *ip;
    unsigned       port;
    char          *user;
    char          *password;
    char          *sn;
};

/* --------------------------------------------------------------- helpers -- */

/** "/" separated remote path for a local file name. */
static ncl_err remote_join(char *out, size_t out_len, const char *dir,
                           const char *name)
{
    size_t used;
    int rc;

    if (dir == NULL || dir[0] == '\0') {
        rc = snprintf(out, out_len, "/%s", name);
        return (rc > 0 && (size_t)rc < out_len) ? NCL_OK : NCL_ERR_RANGE;
    }
    rc = snprintf(out, out_len, "%s", dir);
    if (rc <= 0 || (size_t)rc >= out_len) {
        return NCL_ERR_RANGE;
    }
    used = (size_t)rc;
    while (used > 1 && out[used - 1] == '/') {
        used--;
    }
    rc = snprintf(out + used, out_len - used, "/%s", name);
    if (rc <= 0 || (size_t)rc >= out_len - used) {
        return NCL_ERR_RANGE;
    }
    return NCL_OK;
}

/** <root>/uploadFile/<remote path> with the platform separator. */
static void local_upload_path(char *out, size_t out_len, const char *remote)
{
    size_t used = 0;
    int rc;
    const char *cursor = remote != NULL ? remote : "";

    rc = snprintf(out, out_len, "%s", ncl_env_root());
    if (rc <= 0 || (size_t)rc >= out_len) {
        out[0] = '\0';
        return;
    }
    used = (size_t)rc;
    if (used > 0 && (out[used - 1] == '/' || out[used - 1] == '\\')) {
        used--;
    }
    out[used] = '\0';
    for (; *cursor != '\0'; cursor++) {
        char c = (*cursor == '/' || *cursor == '\\') ? NCL_PATH_SEP : *cursor;
        if (used + 2 >= out_len) {
            out[0] = '\0';
            return;
        }
        out[used++] = c;
        out[used] = '\0';
    }
}

/** <root>/uploadFile + @p remote. */
static void local_mirror_path(char *out, size_t out_len, const char *remote)
{
    char joined[NCL_PATH_MAX_BUF];
    size_t i;

    snprintf(joined, sizeof(joined), "%s%s", NCL_FILE_UPLOAD_DIR,
             remote != NULL ? remote : "");
    for (i = 0; joined[i] != '\0'; i++) {
        if (joined[i] == '/') {
            joined[i] = NCL_PATH_SEP;
        }
    }
    local_upload_path(out, out_len, joined);
}

/** Create every parent directory of @p path. */
static void ensure_parent_dir(const char *path)
{
    char buffer[NCL_PATH_MAX_BUF];
    char *slash;

    snprintf(buffer, sizeof(buffer), "%s", path);
    slash = strrchr(buffer, NCL_PATH_SEP);
    if (slash == NULL) {
        slash = strrchr(buffer, '/');
    }
    if (slash == NULL) {
        return;
    }
    *slash = '\0';
    if (buffer[0] != '\0') {
        ncl_mkdir_p(buffer);
    }
}

/* ------------------------------------------------------------ life cycle -- */

ncl_server_file_tool *ncl_server_file_tool_create(const char *ip, unsigned port,
                                                  const char *user,
                                                  const char *password,
                                                  const char *sn)
{
    ncl_server_file_tool *tool;

    if (ip == NULL || sn == NULL) {
        return NULL;
    }
    tool = (ncl_server_file_tool *)calloc(1, sizeof(*tool));
    if (tool == NULL) {
        return NULL;
    }
    tool->lock = ncl_mutex_create();
    tool->ip = ncl_strdup(ip);
    tool->port = port != 0 ? port : (unsigned)NCL_FTP_CLIENT_HOLDER_PORT;
    tool->user = ncl_strdup(user != NULL ? user : NCL_FTP_DEFAULT_USER);
    tool->password =
        ncl_strdup(password != NULL ? password : NCL_FTP_DEFAULT_PASSWORD);
    tool->sn = ncl_strdup(sn);
    if (tool->lock == NULL || tool->ip == NULL || tool->user == NULL ||
        tool->password == NULL || tool->sn == NULL) {
        ncl_server_file_tool_free(tool);
        return NULL;
    }
    return tool;
}

void ncl_server_file_tool_disconnect(ncl_server_file_tool *tool)
{
    if (tool == NULL) {
        return;
    }
    ncl_mutex_lock(tool->lock);
    if (tool->client != NULL) {
        ncl_ftp_client_free(tool->client);
        tool->client = NULL;
    }
    ncl_mutex_unlock(tool->lock);
}

void ncl_server_file_tool_free(ncl_server_file_tool *tool)
{
    if (tool == NULL) {
        return;
    }
    ncl_server_file_tool_disconnect(tool);
    free(tool->ip);
    free(tool->user);
    free(tool->password);
    free(tool->sn);
    if (tool->lock != NULL) {
        ncl_mutex_destroy(tool->lock);
    }
    free(tool);
}

/** A live session is validated with NOOP. */
bool ncl_server_file_tool_detect(ncl_server_file_tool *tool)
{
    bool ok = false;

    if (tool == NULL) {
        return false;
    }
    ncl_mutex_lock(tool->lock);
    if (tool->client != NULL &&
        ncl_ftp_client_is_connected(tool->client) &&
        ncl_ftp_client_noop(tool->client)) {
        ok = true;
    } else {
        if (tool->client != NULL) {
            ncl_ftp_client_free(tool->client);
            tool->client = NULL;
        }
        tool->client = ncl_ftp_client_create(tool->ip, tool->port, tool->user,
                                             tool->password);
        if (tool->client != NULL) {
            ok = ncl_ftp_client_detect(tool->client);
            if (!ok) {
                ncl_ftp_client_free(tool->client);
                tool->client = NULL;
            }
        }
    }
    ncl_mutex_unlock(tool->lock);
    return ok;
}

/* ----------------------------------------------------------------- write -- */

bool ncl_server_file_tool_write(ncl_server_file_tool *tool,
                                const char *local_path, const char *remote_dir)
{
    char remote[NCL_PATH_MAX_BUF];
    int attempt;
    bool ok = false;

    if (tool == NULL || local_path == NULL) {
        return false;
    }
    if (!ncl_path_exists(local_path) || ncl_path_is_dir(local_path)) {
        ncl_log_error("local file does not exist: %s", local_path);
        return false;
    }
    for (attempt = 0; attempt < 2 && !ok; attempt++) {
        if (!ncl_server_file_tool_detect(tool)) {
            ncl_log_error("ftp connect failed, attempt %d", attempt + 1);
            continue;
        }
        /* remoteDir is normalised to "/<remoteDir>" then prefixed with the
         * serial number. */
        {
            char normalised[NCL_PATH_MAX_BUF];
            const char *dir = remote_dir != NULL ? remote_dir : "/";
            size_t i;

            for (i = 0; dir[i] != '\0' && i + 1 < sizeof(normalised); i++) {
                normalised[i] = (dir[i] == '\\') ? '/' : dir[i];
            }
            normalised[i] = '\0';
            {
                char with_sn[NCL_PATH_MAX_BUF];
                if (normalised[0] == '/') {
                    snprintf(with_sn, sizeof(with_sn), "/%s%s", tool->sn,
                             normalised);
                } else {
                    snprintf(with_sn, sizeof(with_sn), "/%s/%s", tool->sn,
                             normalised);
                }
                snprintf(remote, sizeof(remote), "%s", with_sn);
            }
        }
        ncl_mutex_lock(tool->lock);
        if (tool->client != NULL) {
            if (ncl_ftp_client_chdir(tool->client, remote) != NCL_OK) {
                if (ncl_ftp_client_mkdir(tool->client, remote) == NCL_OK) {
                    ncl_ftp_client_chdir(tool->client, remote);
                } else {
                    ncl_log_error("cannot create the remote directory %s",
                                  remote);
                    ncl_mutex_unlock(tool->lock);
                    return false;
                }
            }
            {
                const char *name = local_path;
                const char *slash = strrchr(local_path, NCL_PATH_SEP);
                if (slash == NULL) {
                    slash = strrchr(local_path, '/');
                }
                if (slash != NULL) {
                    name = slash + 1;
                }
                ok = ncl_ftp_client_store_file(tool->client, name,
                                               local_path) == NCL_OK;
                if (ok) {
                    ncl_log_info("file upload successful: %s", name);
                } else {
                    ncl_log_error("file upload failed: %s (reply %d)", name,
                                  ncl_ftp_client_reply_code(tool->client));
                    /* A dead connection is dropped so the retry reconnects. */
                    if (!ncl_ftp_client_noop(tool->client)) {
                        ncl_ftp_client_free(tool->client);
                        tool->client = NULL;
                    }
                }
            }
        }
        ncl_mutex_unlock(tool->lock);
    }
    return ok;
}

/* ------------------------------------------------------------------ read -- */

char *ncl_server_file_tool_read(ncl_server_file_tool *tool,
                                const char *remote_file_path)
{
    char local[NCL_PATH_MAX_BUF];
    char remote[NCL_PATH_MAX_BUF];
    int attempt;

    if (tool == NULL || remote_file_path == NULL) {
        return NULL;
    }
    /* <root>/uploadFile/<remote path> with local separators. */
    local_mirror_path(local, sizeof(local), remote_file_path);
    snprintf(remote, sizeof(remote), "/%s/%s", tool->sn, remote_file_path);
    ensure_parent_dir(local);

    for (attempt = 0; attempt < 2; attempt++) {
        bool ok;
        int code = 0;
        char text[256];

        text[0] = '\0';
        if (!ncl_server_file_tool_detect(tool)) {
            ncl_log_error("ftp connect failed, attempt %d", attempt + 1);
            continue;
        }
        ncl_mutex_lock(tool->lock);
        ok = tool->client != NULL &&
             ncl_ftp_client_retrieve_file(tool->client, remote, local) == NCL_OK;
        if (tool->client != NULL) {
            code = ncl_ftp_client_reply_code(tool->client);
            snprintf(text, sizeof(text), "%s",
                     ncl_ftp_client_reply_text(tool->client));
        }
        if (!ok && tool->client != NULL &&
            !ncl_ftp_client_noop(tool->client)) {
            ncl_ftp_client_free(tool->client);
            tool->client = NULL;
        }
        ncl_mutex_unlock(tool->lock);
        if (ok) {
            ncl_log_info("file download successful: %s", local);
            return ncl_strdup(local);
        }
        ncl_log_error("file download failed: %s (reply %d %s)", remote_file_path,
                      code, text);
        remove(local); /* drop the empty file storeFile may have created */
    }
    return NULL;
}

/* -------------------------------------------------------------------- ll -- */

ncl_err ncl_server_file_tool_ll(ncl_server_file_tool *tool,
                                const char *remote_dir, ncl_ptrvec *out)
{
    char local[NCL_PATH_MAX_BUF];

    if (tool == NULL || out == NULL || remote_dir == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    /* <root> + remoteDir with local separators */
    local_mirror_path(local, sizeof(local), remote_dir);
    if (!ncl_path_exists(local)) {
        return NCL_OK;
    }
    if (!ncl_path_is_dir(local)) {
        ncl_file_attribute *attribute = NULL;
        char parent[NCL_PATH_MAX_BUF];
        char *slash;
        snprintf(parent, sizeof(parent), "%s", local);
        slash = strrchr(parent, NCL_PATH_SEP);
        if (slash != NULL) {
            *slash = '\0';
        }
        if (ncl_file_attribute_of(local, parent, &attribute) == NCL_OK) {
            ncl_ptrvec_push(out, attribute);
        }
        return NCL_OK;
    }
#if defined(NCL_OS_WINDOWS)
    {
        WIN32_FIND_DATAA entry;
        HANDLE handle;
        char pattern[NCL_PATH_MAX_BUF];

        snprintf(pattern, sizeof(pattern), "%s\\*", local);
        handle = FindFirstFileA(pattern, &entry);
        if (handle == INVALID_HANDLE_VALUE) {
            return NCL_OK;
        }
        do {
            char child[NCL_PATH_MAX_BUF];
            ncl_file_attribute *attribute = NULL;

            if (strcmp(entry.cFileName, ".") == 0 ||
                strcmp(entry.cFileName, "..") == 0) {
                continue;
            }
            snprintf(child, sizeof(child), "%s\\%s", local, entry.cFileName);
            if (ncl_file_attribute_of(child, local, &attribute) == NCL_OK) {
                ncl_ptrvec_push(out, attribute);
            }
        } while (FindNextFileA(handle, &entry));
        FindClose(handle);
    }
#else
    {
        DIR *dir = opendir(local);
        struct dirent *entry;

        if (dir == NULL) {
            return NCL_OK;
        }
        while ((entry = readdir(dir)) != NULL) {
            char child[NCL_PATH_MAX_BUF];
            ncl_file_attribute *attribute = NULL;

            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            snprintf(child, sizeof(child), "%s/%s", local, entry->d_name);
            if (ncl_file_attribute_of(child, local, &attribute) == NCL_OK) {
                ncl_ptrvec_push(out, attribute);
            }
        }
        closedir(dir);
    }
#endif
    return NCL_OK;
}

/* ----------------------------------------------------------------- mkdir -- */

bool ncl_server_file_tool_mkdir(ncl_server_file_tool *tool,
                                const char *remote_dir)
{
    char remote[NCL_PATH_MAX_BUF];
    char current[NCL_PATH_MAX_BUF];
    char local[NCL_PATH_MAX_BUF];
    int attempt;
    bool ok = false;

    if (tool == NULL || remote_dir == NULL) {
        return false;
    }
    local_mirror_path(local, sizeof(local), remote_dir);
    /* The local directory is created first, unconditionally. */
    if (!ncl_path_exists(local)) {
        ncl_mkdir_p(local);
    }

    for (attempt = 0; attempt < 2 && !ok; attempt++) {
        const char *cursor;

        if (!ncl_server_file_tool_detect(tool)) {
            ncl_log_error("ftp connect failed, attempt %d", attempt + 1);
            continue;
        }
        snprintf(remote, sizeof(remote), "/%s%s", tool->sn,
                 remote_dir[0] == '/' ? remote_dir : "/");
        if (remote_dir[0] != '/') {
            snprintf(remote, sizeof(remote), "/%s/%s", tool->sn, remote_dir);
        }
        current[0] = '\0';
        cursor = remote;
        ok = true;
        ncl_mutex_lock(tool->lock);
        while (*cursor != '\0' && ok) {
            const char *slash;
            size_t segment_len;
            size_t current_len;

            while (*cursor == '/') {
                cursor++;
            }
            if (*cursor == '\0') {
                break;
            }
            slash = strchr(cursor, '/');
            segment_len = slash != NULL ? (size_t)(slash - cursor)
                                        : strlen(cursor);
            current_len = strlen(current);
            snprintf(current + current_len, sizeof(current) - current_len,
                     "/%.*s", (int)segment_len, cursor);
            if (tool->client == NULL ||
                ncl_ftp_client_chdir(tool->client, current) != NCL_OK) {
                if (tool->client == NULL ||
                    ncl_ftp_client_mkdir(tool->client, current) != NCL_OK) {
                    ok = false;
                    break;
                }
                ncl_ftp_client_chdir(tool->client, current);
            }
            if (slash == NULL) {
                break;
            }
            cursor = slash + 1;
        }
        if (!ok && tool->client != NULL) {
            ncl_ftp_client_free(tool->client);
            tool->client = NULL;
        }
        ncl_mutex_unlock(tool->lock);
    }
    return ok;
}

/* ---------------------------------------------------------------- delete -- */

/** Recursive RMD over the FTP session. */
static bool ftp_delete_directory(ncl_ftp_client *client, const char *remote)
{
    ncl_ptrvec entries;
    size_t i;
    bool ok = true;

    ncl_ptrvec_init(&entries, ncl_ftp_entry_free);
    if (ncl_ftp_client_list(client, remote, &entries) == NCL_OK) {
        for (i = 0; i < entries.len && ok; i++) {
            const ncl_ftp_entry *entry =
                (const ncl_ftp_entry *)entries.items[i];
            char child[NCL_PATH_MAX_BUF];
            if (entry == NULL || entry->name == NULL) {
                continue;
            }
            if (strcmp(entry->name, ".") == 0 ||
                strcmp(entry->name, "..") == 0) {
                continue;
            }
            if (remote_join(child, sizeof(child), remote, entry->name) !=
                NCL_OK) {
                ok = false;
                break;
            }
            if (ncl_ftp_entry_is_dir(entry)) {
                ok = ftp_delete_directory(client, child);
            } else {
                ok = ncl_ftp_client_delete(client, child) == NCL_OK;
            }
        }
    }
    ncl_ptrvec_free(&entries);
    if (!ok) {
        return false;
    }
    return ncl_ftp_client_rmdir(client, remote) == NCL_OK;
}

bool ncl_server_file_tool_delete(ncl_server_file_tool *tool,
                                 const char *remote_file_path)
{
    char local[NCL_PATH_MAX_BUF];
    char remote[NCL_PATH_MAX_BUF];
    bool was_dir;
    bool ok;

    if (tool == NULL || remote_file_path == NULL) {
        return false;
    }
    local_mirror_path(local, sizeof(local), remote_file_path);
    if (ncl_path_exists(local)) {
        was_dir = ncl_path_is_dir(local);
        ncl_path_remove(local);
    } else {
        was_dir = false;
    }
    if (!ncl_server_file_tool_detect(tool)) {
        /* The local delete already succeeded, so report success. */
        return true;
    }
    snprintf(remote, sizeof(remote), "/%s%s", tool->sn, remote_file_path);
    ncl_mutex_lock(tool->lock);
    if (tool->client == NULL) {
        ok = true;
    } else if (was_dir) {
        ok = ftp_delete_directory(tool->client, remote);
    } else {
        ok = ncl_ftp_client_delete(tool->client, remote) == NCL_OK;
    }
    ncl_mutex_unlock(tool->lock);
    return ok;
}
