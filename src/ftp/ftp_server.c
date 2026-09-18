/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - FTP server.
 *
 * A single-user, single-directory FTP server covering the command subset the
 * NC-Link file channel uses. The login root is the only visible directory tree,
 * and paths are normalised so that ".." can never escape it.
 */
#include "ftp_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"

#if !defined(NCL_OS_WINDOWS)
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <dirent.h>
#  include <unistd.h>
#endif

/* -------------------------------------------------------- local file sys -- */

typedef struct {
    bool    is_dir;
    bool    exists;
    long long size;
    int64_t mtime_ms;
} ncl_fs_info;

static void fs_child_path(char *out, size_t out_len, const char *dir,
                          const char *name)
{
    size_t len = strlen(dir);
    while (len > 1 && (dir[len - 1] == '/' || dir[len - 1] == '\\')) {
        len--;
    }
    snprintf(out, out_len, "%.*s%c%s", (int)len, dir, NCL_PATH_SEP, name);
}

static bool fs_stat(const char *path, ncl_fs_info *info)
{
    memset(info, 0, sizeof(*info));
#if defined(NCL_OS_WINDOWS)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
            return false;
        }
        info->exists = true;
        info->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        info->size = ((long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        {
            ULARGE_INTEGER u;
            u.LowPart = data.ftLastWriteTime.dwLowDateTime;
            u.HighPart = data.ftLastWriteTime.dwHighDateTime;
            info->mtime_ms = (int64_t)(u.QuadPart / 10000ULL) - 11644473600000LL;
        }
    }
#else
    {
        struct stat st;
        if (stat(path, &st) != 0) {
            return false;
        }
        info->exists = true;
        info->is_dir = S_ISDIR(st.st_mode);
        info->size = (long long)st.st_size;
        info->mtime_ms = (int64_t)st.st_mtime * 1000;
    }
#endif
    return true;
}

static ncl_err fs_mkdir_p(const char *path)
{
    return ncl_mkdir_p(path);
}

static bool fs_remove_recursive(const char *path)
{
    ncl_fs_info info;

    if (!fs_stat(path, &info)) {
        return true; /* already gone */
    }
    if (!info.is_dir) {
        return remove(path) == 0;
    }
#if defined(NCL_OS_WINDOWS)
    {
        WIN32_FIND_DATAA entry;
        HANDLE handle;
        char pattern[NCL_FTP_PATH_MAX];
        bool ok = true;

        fs_child_path(pattern, sizeof(pattern), path, "*");
        handle = FindFirstFileA(pattern, &entry);
        if (handle != INVALID_HANDLE_VALUE) {
            do {
                char child[NCL_FTP_PATH_MAX];
                if (strcmp(entry.cFileName, ".") == 0 ||
                    strcmp(entry.cFileName, "..") == 0) {
                    continue;
                }
                fs_child_path(child, sizeof(child), path, entry.cFileName);
                if (!fs_remove_recursive(child)) {
                    ok = false;
                }
            } while (FindNextFileA(handle, &entry));
            FindClose(handle);
        }
        if (!ok && !fs_stat(path, &info)) {
            return false;
        }
        return RemoveDirectoryA(path) != 0;
    }
#else
    {
        DIR *dir = opendir(path);
        struct dirent *entry;
        if (dir != NULL) {
            while ((entry = readdir(dir)) != NULL) {
                char child[NCL_FTP_PATH_MAX];
                if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                fs_child_path(child, sizeof(child), path, entry->d_name);
                fs_remove_recursive(child);
            }
            closedir(dir);
        }
        return rmdir(path) == 0;
    }
#endif
}

/**
 * Append the directory listing of @p dir to @p out.
 * @param long_format produce "ls -l" lines (LIST) instead of bare names (NLST).
 */
static ncl_err fs_list_dir(const char *dir, bool long_format, ncl_strbuf *out)
{
#if defined(NCL_OS_WINDOWS)
    WIN32_FIND_DATAA entry;
    HANDLE handle;
    char pattern[NCL_FTP_PATH_MAX];

    fs_child_path(pattern, sizeof(pattern), dir, "*");
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        return NCL_ERR_NOT_FOUND;
    }
    do {
        ncl_fs_info info;
        char child[NCL_FTP_PATH_MAX];

        if (strcmp(entry.cFileName, ".") == 0 ||
            strcmp(entry.cFileName, "..") == 0) {
            continue;
        }
        fs_child_path(child, sizeof(child), dir, entry.cFileName);
        if (!fs_stat(child, &info)) {
            continue;
        }
        if (long_format) {
            ncl_ftp_format_list_line(out, entry.cFileName, info.is_dir,
                                     info.size, info.mtime_ms);
        } else {
            ncl_strbuf_puts(out, entry.cFileName);
        }
        ncl_strbuf_puts(out, "\r\n");
    } while (FindNextFileA(handle, &entry));
    FindClose(handle);
    return NCL_OK;
#else
    DIR *dirp = opendir(dir);
    struct dirent *entry;

    if (dirp == NULL) {
        return NCL_ERR_NOT_FOUND;
    }
    while ((entry = readdir(dirp)) != NULL) {
        ncl_fs_info info;
        char child[NCL_FTP_PATH_MAX];

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        fs_child_path(child, sizeof(child), dir, entry->d_name);
        if (!fs_stat(child, &info)) {
            continue;
        }
        if (long_format) {
            ncl_ftp_format_list_line(out, entry->d_name, info.is_dir, info.size,
                                     info.mtime_ms);
        } else {
            ncl_strbuf_puts(out, entry->d_name);
        }
        ncl_strbuf_puts(out, "\r\n");
    }
    closedir(dirp);
    return NCL_OK;
#endif
}

/* ------------------------------------------------------------- structures -- */

typedef struct ncl_ftp_session {
    struct ncl_ftp_server *server;
    ncl_socket             *ctrl;
    ncl_ftp_reader          reader;
    char                    cwd[NCL_FTP_PATH_MAX];
    bool                    logged_in;
    char                    username[64];
    char                    rnfr[NCL_FTP_PATH_MAX];
    bool                    have_rnfr;
    bool                    type_binary;
    long long               rest;

    ncl_socket             *pasv; /**< passive listener, owned by the session */
    char                    active_host[64];
    unsigned                active_port;
    bool                    have_active;

    ncl_thread             *thread;
    bool                    finished;
    bool                    quit;
} ncl_ftp_session;

struct ncl_ftp_server {
    char        *root;
    char        *user;
    char        *password;
    bool         allow_write;
    unsigned     port;
    unsigned     idle_timeout_ms;
    unsigned     data_timeout_ms;

    ncl_socket  *listener;
    ncl_thread  *acceptor;
    ncl_mutex   *lock;
    bool         running;
    bool         stop;
    size_t       session_count;
    long long    command_count;
    ncl_ptrvec   sessions; /**< ncl_ftp_session* */
};

/* --------------------------------------------------------------- helpers -- */

static void session_close_pasv(ncl_ftp_session *s)
{
    if (s->pasv != NULL) {
        ncl_socket_close(s->pasv);
        s->pasv = NULL;
    }
}

/** Translate an absolute virtual FTP path into a local file system path. */
static void session_local_path(const ncl_ftp_session *s, const char *virt,
                               char *out, size_t out_len)
{
    size_t used = 0;
    const char *cursor;
    const char *root = s->server->root;
    size_t root_len = strlen(root);

    while (root_len > 1 && (root[root_len - 1] == '/' ||
                            root[root_len - 1] == '\\')) {
        root_len--;
    }
    if (root_len >= out_len) {
        out[0] = '\0';
        return;
    }
    memcpy(out, root, root_len);
    out[root_len] = '\0';
    used = root_len;

    cursor = virt;
    while (*cursor != '\0') {
        const char *slash;
        size_t segment_len;

        while (*cursor == '/') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        slash = strchr(cursor, '/');
        segment_len = slash != NULL ? (size_t)(slash - cursor) : strlen(cursor);
        if (used + 1 + segment_len + 1 >= out_len) {
            out[0] = '\0';
            return;
        }
        out[used++] = NCL_PATH_SEP;
        memcpy(out + used, cursor, segment_len);
        used += segment_len;
        out[used] = '\0';
        if (slash == NULL) {
            break;
        }
        cursor = slash + 1;
    }
}

/**
 * Resolve @p arg against the session's working directory and translate it to a
 * local path. @p virt_out (optional) receives the normalised virtual path.
 */
static ncl_err session_resolve(ncl_ftp_session *s, const char *arg,
                               char *local_out, size_t local_len,
                               char *virt_out, size_t virt_len)
{
    char virt[NCL_FTP_PATH_MAX];
    ncl_err rc;

    rc = ncl_ftp_path_normalise(s->cwd, arg, virt, sizeof(virt));
    if (rc != NCL_OK) {
        return rc;
    }
    session_local_path(s, virt, local_out, local_len);
    if (local_out[0] == '\0') {
        return NCL_ERR_RANGE;
    }
    if (virt_out != NULL && virt_len > 0) {
        snprintf(virt_out, virt_len, "%s", virt);
    }
    return NCL_OK;
}

/* ---------------------------------------------------------- data channel -- */

/**
 * Hand back the socket for the next transfer, mirroring the FTP server side of
 * the PORT/PASV dance. Returns NULL with *permanent set when the client never
 * negotiated a data connection.
 */
static ncl_socket *session_open_data(ncl_ftp_session *s, bool *permanent)
{
    if (permanent != NULL) {
        *permanent = false;
    }
    if (s->pasv != NULL) {
        ncl_socket *conn = NULL;
        while (!s->server->stop) {
            conn = ncl_socket_accept(s->pasv, 500);
            if (conn != NULL) {
                break;
            }
        }
        session_close_pasv(s);
        if (conn == NULL && permanent != NULL) {
            *permanent = true;
        }
        return conn;
    }
    if (s->have_active) {
        char err[128];
        ncl_socket *conn;
        unsigned port = s->active_port;
        char host[64];

        snprintf(host, sizeof(host), "%s", s->active_host);
        s->have_active = false;
        err[0] = '\0';
        conn = ncl_socket_connect(host, port, s->server->data_timeout_ms, err,
                                  sizeof(err));
        if (conn == NULL && permanent != NULL) {
            *permanent = true;
        }
        return conn;
    }
    if (permanent != NULL) {
        *permanent = true;
    }
    return NULL;
}

/** Read the whole data connection into @p out and close it. */
static ncl_err session_read_data(ncl_ftp_session *s, ncl_socket *conn,
                                 ncl_strbuf *out)
{
    char chunk[8192];

    for (;;) {
        int got = ncl_socket_recv(conn, chunk, sizeof(chunk),
                                  s->server->data_timeout_ms);
        if (got == NCL_SOCKET_TIMEOUT) {
            continue;
        }
        if (got < 0) {
            return NCL_ERR_IO;
        }
        if (got == 0) {
            return NCL_OK;
        }
        if (ncl_strbuf_append(out, chunk, (size_t)got) != NCL_OK) {
            return NCL_ERR_NOMEM;
        }
    }
}

/* ------------------------------------------------------------- commands -- */

static void session_reply(ncl_ftp_session *s, int code, const char *text)
{
    ncl_ftp_reply(s->ctrl, code, text);
}

static void session_replyf(ncl_ftp_session *s, int code, const char *fmt, ...)
{
    va_list ap;
    char *body = NULL;

    va_start(ap, fmt);
    if (ncl_vasprintf(&body, fmt, ap) == NCL_OK) {
        ncl_ftp_reply(s->ctrl, code, body);
        ncl_mem_free(body);
    }
    va_end(ap);
}

static bool session_require_login(ncl_ftp_session *s)
{
    if (s->logged_in) {
        return true;
    }
    session_reply(s, 530, "Please login with USER and PASS.");
    return false;
}

static bool session_require_write(ncl_ftp_session *s)
{
    if (s->server->allow_write) {
        return true;
    }
    session_reply(s, 550, "Permission denied.");
    return false;
}

static void session_handle_pasv(ncl_ftp_session *s)
{
    char host[64];
    char arg[64];
    unsigned port;

    session_close_pasv(s);
    s->have_active = false;
    s->pasv = ncl_socket_listen(0, NULL, 0);
    if (s->pasv == NULL) {
        session_reply(s, 425, "Cannot open a passive data connection.");
        return;
    }
    port = ncl_socket_local_port(s->pasv);
    if (ncl_socket_local_ip(s->ctrl, host, sizeof(host)) != NCL_OK) {
        snprintf(host, sizeof(host), "127.0.0.1");
    }
    if (ncl_ftp_format_port_arg(arg, sizeof(arg), host, port) != NCL_OK) {
        session_close_pasv(s);
        session_reply(s, 425, "Cannot advertise the passive port.");
        return;
    }
    session_replyf(s, 227, "Entering Passive Mode (%s).", arg);
}

static void session_handle_epsv(ncl_ftp_session *s)
{
    unsigned port;

    session_close_pasv(s);
    s->have_active = false;
    s->pasv = ncl_socket_listen(0, NULL, 0);
    if (s->pasv == NULL) {
        session_reply(s, 425, "Cannot open a passive data connection.");
        return;
    }
    port = ncl_socket_local_port(s->pasv);
    session_replyf(s, 229, "Entering Extended Passive Mode (|||%u|).", port);
}

static void session_handle_port(ncl_ftp_session *s, const char *arg)
{
    unsigned v[6];
    int i;
    const char *cursor = arg;

    for (i = 0; i < 6; i++) {
        char *end = NULL;
        unsigned long value;
        while (*cursor == ' ') {
            cursor++;
        }
        value = strtoul(cursor, &end, 10);
        if (end == cursor || value > 255) {
            session_reply(s, 501, "Illegal PORT command.");
            return;
        }
        v[i] = (unsigned)value;
        cursor = end;
        if (*cursor == ',') {
            cursor++;
        }
    }
    session_close_pasv(s);
    snprintf(s->active_host, sizeof(s->active_host), "%u.%u.%u.%u", v[0], v[1],
             v[2], v[3]);
    s->active_port = v[4] * 256 + v[5];
    s->have_active = s->active_port != 0;
    session_reply(s, 200, "PORT command successful.");
}

static void session_handle_eprt(ncl_ftp_session *s, const char *arg)
{
    const char *cursor = arg;
    char delim;
    char host[64];
    unsigned port = 0;
    size_t used = 0;

    if (cursor == NULL || *cursor == '\0') {
        session_reply(s, 501, "Illegal EPRT command.");
        return;
    }
    delim = *cursor++;
    /* Format: |af|address|port| */
    while (*cursor != '\0' && *cursor != delim) {
        cursor++; /* address family */
    }
    if (*cursor == delim) {
        cursor++;
    }
    while (*cursor != '\0' && *cursor != delim && used + 1 < sizeof(host)) {
        host[used++] = *cursor++;
    }
    host[used] = '\0';
    if (*cursor == delim) {
        cursor++;
    }
    port = (unsigned)strtoul(cursor, NULL, 10);
    if (host[0] == '\0' || port == 0) {
        session_reply(s, 501, "Illegal EPRT command.");
        return;
    }
    session_close_pasv(s);
    snprintf(s->active_host, sizeof(s->active_host), "%s", host);
    s->active_port = port;
    s->have_active = true;
    session_reply(s, 200, "EPRT command successful.");
}

/** LIST / NLST */
static void session_handle_list(ncl_ftp_session *s, const char *arg,
                                bool long_format)
{
    char local[NCL_FTP_PATH_MAX];
    char virt[NCL_FTP_PATH_MAX];
    ncl_fs_info info;
    ncl_strbuf listing;
    ncl_socket *conn;
    bool permanent = false;
    ncl_err rc;
    const char *target = (arg != NULL && arg[0] != '\0') ? arg : NULL;

    if (!session_require_login(s)) {
        return;
    }
    rc = session_resolve(s, target != NULL ? target : ".", local,
                         sizeof(local), virt, sizeof(virt));
    if (rc != NCL_OK || !fs_stat(local, &info)) {
        session_reply(s, 550, "No such file or directory.");
        return;
    }
    ncl_strbuf_init(&listing);
    if (info.is_dir) {
        fs_list_dir(local, long_format, &listing);
    } else {
        const char *name = strrchr(virt, '/');
        name = name != NULL ? name + 1 : virt;
        if (long_format) {
            ncl_ftp_format_list_line(&listing, name, false, info.size,
                                     info.mtime_ms);
        } else {
            ncl_strbuf_puts(&listing, name);
        }
        ncl_strbuf_puts(&listing, "\r\n");
    }

    session_reply(s, 150, "Here comes the directory listing.");
    conn = session_open_data(s, &permanent);
    if (conn == NULL) {
        if (permanent) {
            session_reply(s, 425, "Use PORT or PASV first.");
        }
        ncl_strbuf_free(&listing);
        return;
    }
    if (listing.len > 0) {
        ncl_socket_send(conn, listing.data, listing.len);
    }
    ncl_socket_close(conn);
    ncl_strbuf_free(&listing);
    session_reply(s, 226, "Directory send OK.");
}

/** RETR */
static void session_handle_retr(ncl_ftp_session *s, const char *arg)
{
    char local[NCL_FTP_PATH_MAX];
    ncl_fs_info info;
    ncl_socket *conn;
    bool permanent = false;
    void *data = NULL;
    size_t len = 0;
    ncl_err rc;

    if (!session_require_login(s) || arg == NULL || arg[0] == '\0') {
        session_reply(s, 501, "Illegal RETR command.");
        return;
    }
    rc = session_resolve(s, arg, local, sizeof(local), NULL, 0);
    if (rc != NCL_OK || !fs_stat(local, &info) || info.is_dir) {
        session_reply(s, 550, "Failed to open file.");
        return;
    }
    rc = ncl_file_read_all(local, (char **)&data, &len);
    if (rc != NCL_OK) {
        session_reply(s, 550, "Failed to open file.");
        return;
    }
    session_replyf(s, 150, "Opening BINARY mode data connection for %s (%lld bytes).",
                   arg, info.size);
    conn = session_open_data(s, &permanent);
    if (conn == NULL) {
        if (permanent) {
            session_reply(s, 425, "Use PORT or PASV first.");
        }
        ncl_mem_free(data);
        return;
    }
    if (len > 0) {
        ncl_socket_send(conn, (const char *)data + (size_t)s->rest,
                        len > (size_t)s->rest ? len - (size_t)s->rest : 0);
    }
    s->rest = 0;
    ncl_socket_close(conn);
    ncl_mem_free(data);
    session_reply(s, 226, "Transfer complete.");
}

/** STOR / APPE */
static void session_handle_stor(ncl_ftp_session *s, const char *arg,
                                bool append)
{
    char local[NCL_FTP_PATH_MAX];
    ncl_fs_info info;
    ncl_socket *conn;
    bool permanent = false;
    ncl_strbuf payload;
    ncl_err rc;
    char parent[NCL_FTP_PATH_MAX];
    char *slash;

    if (!session_require_login(s) || !session_require_write(s)) {
        return;
    }
    if (arg == NULL || arg[0] == '\0') {
        session_reply(s, 501, "Illegal STOR command.");
        return;
    }
    rc = session_resolve(s, arg, local, sizeof(local), NULL, 0);
    if (rc != NCL_OK) {
        session_reply(s, 550, "Invalid path.");
        return;
    }
    if (fs_stat(local, &info) && info.is_dir) {
        session_reply(s, 550, "Is a directory.");
        return;
    }
    snprintf(parent, sizeof(parent), "%s", local);
    slash = strrchr(parent, NCL_PATH_SEP);
    if (slash != NULL) {
        *slash = '\0';
        fs_mkdir_p(parent);
    }

    session_reply(s, 150, "Ok to send data.");
    conn = session_open_data(s, &permanent);
    if (conn == NULL) {
        if (permanent) {
            session_reply(s, 425, "Use PORT or PASV first.");
        }
        return;
    }
    ncl_strbuf_init(&payload);
    rc = session_read_data(s, conn, &payload);
    ncl_socket_close(conn);
    if (rc != NCL_OK) {
        ncl_strbuf_free(&payload);
        session_reply(s, 550, "Failed to receive data.");
        return;
    }
    if (append && fs_stat(local, &info) && !info.is_dir) {
        rc = ncl_file_append(local, payload.data, payload.len);
    } else {
        rc = ncl_file_write_all(local, payload.data, payload.len);
    }
    ncl_strbuf_free(&payload);
    if (rc != NCL_OK) {
        session_reply(s, 550, "Failed to write file.");
        return;
    }
    session_reply(s, 226, "Transfer complete.");
}

static void session_handle_dele(ncl_ftp_session *s, const char *arg)
{
    char local[NCL_FTP_PATH_MAX];
    ncl_fs_info info;

    if (!session_require_login(s) || !session_require_write(s)) {
        return;
    }
    if (session_resolve(s, arg, local, sizeof(local), NULL, 0) != NCL_OK ||
        !fs_stat(local, &info) || info.is_dir) {
        session_reply(s, 550, "No such file.");
        return;
    }
    if (remove(local) != 0) {
        session_reply(s, 550, "Delete failed.");
        return;
    }
    session_reply(s, 250, "Delete operation successful.");
}

static void session_handle_mkd(ncl_ftp_session *s, const char *arg)
{
    char local[NCL_FTP_PATH_MAX];
    char virt[NCL_FTP_PATH_MAX];

    if (!session_require_login(s) || !session_require_write(s)) {
        return;
    }
    if (session_resolve(s, arg, local, sizeof(local), virt, sizeof(virt)) !=
        NCL_OK) {
        session_reply(s, 550, "Invalid path.");
        return;
    }
    if (fs_mkdir_p(local) != NCL_OK) {
        session_reply(s, 550, "Create directory failed.");
        return;
    }
    session_replyf(s, 257, "\"%s\" directory created.", virt);
}

static void session_handle_rmd(ncl_ftp_session *s, const char *arg)
{
    char local[NCL_FTP_PATH_MAX];
    ncl_fs_info info;

    if (!session_require_login(s) || !session_require_write(s)) {
        return;
    }
    if (session_resolve(s, arg, local, sizeof(local), NULL, 0) != NCL_OK ||
        !fs_stat(local, &info) || !info.is_dir) {
        session_reply(s, 550, "No such directory.");
        return;
    }
    if (!fs_remove_recursive(local)) {
        session_reply(s, 550, "Remove directory failed.");
        return;
    }
    session_reply(s, 250, "Remove directory operation successful.");
}

static void session_handle_cwd(ncl_ftp_session *s, const char *arg)
{
    char local[NCL_FTP_PATH_MAX];
    char virt[NCL_FTP_PATH_MAX];
    ncl_fs_info info;

    if (!session_require_login(s)) {
        return;
    }
    if (session_resolve(s, arg, local, sizeof(local), virt, sizeof(virt)) !=
        NCL_OK || !fs_stat(local, &info) || !info.is_dir) {
        session_reply(s, 550, "Failed to change directory.");
        return;
    }
    snprintf(s->cwd, sizeof(s->cwd), "%s", virt);
    session_reply(s, 250, "Directory successfully changed.");
}

static void session_handle_size(ncl_ftp_session *s, const char *arg)
{
    char local[NCL_FTP_PATH_MAX];
    ncl_fs_info info;

    if (!session_require_login(s)) {
        return;
    }
    if (session_resolve(s, arg, local, sizeof(local), NULL, 0) != NCL_OK ||
        !fs_stat(local, &info) || info.is_dir) {
        session_reply(s, 550, "Could not get file size.");
        return;
    }
    session_replyf(s, 213, "%lld", info.size);
}

static void session_handle_mdtm(ncl_ftp_session *s, const char *arg)
{
    char local[NCL_FTP_PATH_MAX];
    ncl_fs_info info;
    char stamp[32];

    if (!session_require_login(s)) {
        return;
    }
    if (session_resolve(s, arg, local, sizeof(local), NULL, 0) != NCL_OK ||
        !fs_stat(local, &info)) {
        session_reply(s, 550, "Could not get file modification time.");
        return;
    }
    ncl_ftp_format_mdtm(info.mtime_ms, stamp, sizeof(stamp));
    session_replyf(s, 213, "%s", stamp);
}

/* ------------------------------------------------------------- dispatch -- */

static void session_dispatch(ncl_ftp_session *s, const char *verb,
                             const char *arg)
{
    ncl_mutex_lock(s->server->lock);
    s->server->command_count++;
    ncl_mutex_unlock(s->server->lock);

    if (ncl_streq_ignore_case(verb, "USER")) {
        snprintf(s->username, sizeof(s->username), "%s", arg != NULL ? arg : "");
        s->logged_in = false;
        if (ncl_streq_ignore_case(s->username, s->server->user) ||
            strcmp(s->username, "anonymous") == 0) {
            session_reply(s, 331, "Please specify the password.");
        } else {
            session_reply(s, 530, "Invalid user name.");
        }
    } else if (ncl_streq_ignore_case(verb, "PASS")) {
        if (ncl_streq_ignore_case(s->username, s->server->user) &&
            strcmp(arg != NULL ? arg : "", s->server->password) == 0) {
            s->logged_in = true;
            session_reply(s, 230, "Login successful.");
        } else if (strcmp(s->username, "anonymous") == 0) {
            s->logged_in = true;
            session_reply(s, 230, "Login successful.");
        } else {
            session_reply(s, 530, "Login incorrect.");
        }
    } else if (ncl_streq_ignore_case(verb, "ACCT")) {
        session_reply(s, 202, "Account not required.");
    } else if (ncl_streq_ignore_case(verb, "SYST")) {
        session_reply(s, 215, "UNIX Type: L8");
    } else if (ncl_streq_ignore_case(verb, "FEAT")) {
        ncl_ftp_reply_multiline(s->ctrl, 211,
                                "Features:\n SIZE\n MDTM\n REST STREAM\n "
                                "UTF8\nEnd");
    } else if (ncl_streq_ignore_case(verb, "OPTS")) {
        session_reply(s, 200, "OPTS command successful.");
    } else if (ncl_streq_ignore_case(verb, "NOOP")) {
        session_reply(s, 200, "NOOP ok.");
    } else if (ncl_streq_ignore_case(verb, "PWD") ||
               ncl_streq_ignore_case(verb, "XPWD")) {
        if (session_require_login(s)) {
            session_replyf(s, 257, "\"%s\" is the current directory.", s->cwd);
        }
    } else if (ncl_streq_ignore_case(verb, "CWD") ||
               ncl_streq_ignore_case(verb, "XCWD")) {
        session_handle_cwd(s, arg);
    } else if (ncl_streq_ignore_case(verb, "CDUP") ||
               ncl_streq_ignore_case(verb, "XCUP")) {
        session_handle_cwd(s, "..");
    } else if (ncl_streq_ignore_case(verb, "TYPE")) {
        s->type_binary = (arg != NULL && (arg[0] == 'I' || arg[0] == 'i'));
        session_reply(s, 200, "Switching to requested type.");
    } else if (ncl_streq_ignore_case(verb, "MODE") ||
               ncl_streq_ignore_case(verb, "STRU")) {
        session_reply(s, 200, "Command okay.");
    } else if (ncl_streq_ignore_case(verb, "PASV")) {
        session_handle_pasv(s);
    } else if (ncl_streq_ignore_case(verb, "EPSV")) {
        session_handle_epsv(s);
    } else if (ncl_streq_ignore_case(verb, "PORT")) {
        session_handle_port(s, arg);
    } else if (ncl_streq_ignore_case(verb, "EPRT")) {
        session_handle_eprt(s, arg);
    } else if (ncl_streq_ignore_case(verb, "LIST")) {
        session_handle_list(s, arg, true);
    } else if (ncl_streq_ignore_case(verb, "NLST")) {
        session_handle_list(s, arg, false);
    } else if (ncl_streq_ignore_case(verb, "RETR")) {
        session_handle_retr(s, arg);
    } else if (ncl_streq_ignore_case(verb, "STOR")) {
        session_handle_stor(s, arg, false);
    } else if (ncl_streq_ignore_case(verb, "APPE")) {
        session_handle_stor(s, arg, true);
    } else if (ncl_streq_ignore_case(verb, "DELE")) {
        session_handle_dele(s, arg);
    } else if (ncl_streq_ignore_case(verb, "MKD") ||
               ncl_streq_ignore_case(verb, "XMKD")) {
        session_handle_mkd(s, arg);
    } else if (ncl_streq_ignore_case(verb, "RMD") ||
               ncl_streq_ignore_case(verb, "XRMD")) {
        session_handle_rmd(s, arg);
    } else if (ncl_streq_ignore_case(verb, "RNFR")) {
        if (!session_require_login(s)) {
            return;
        }
        if (session_resolve(s, arg, s->rnfr, sizeof(s->rnfr), NULL, 0) ==
            NCL_OK) {
            s->have_rnfr = true;
            session_reply(s, 350, "Ready for RNTO.");
        } else {
            s->have_rnfr = false;
            session_reply(s, 550, "RNFR command failed.");
        }
    } else if (ncl_streq_ignore_case(verb, "RNTO")) {
        char local[NCL_FTP_PATH_MAX];

        if (!session_require_login(s)) {
            return;
        }
        if (!s->have_rnfr ||
            session_resolve(s, arg, local, sizeof(local), NULL, 0) != NCL_OK) {
            session_reply(s, 503, "RNFR required first.");
            return;
        }
        s->have_rnfr = false;
        if (rename(s->rnfr, local) != 0) {
            session_reply(s, 550, "Rename failed.");
            return;
        }
        session_reply(s, 250, "Rename successful.");
    } else if (ncl_streq_ignore_case(verb, "SIZE")) {
        session_handle_size(s, arg);
    } else if (ncl_streq_ignore_case(verb, "MDTM")) {
        session_handle_mdtm(s, arg);
    } else if (ncl_streq_ignore_case(verb, "REST")) {
        if (!session_require_login(s)) {
            return;
        }
        s->rest = strtoll(arg != NULL ? arg : "0", NULL, 10);
        session_replyf(s, 350, "Restarting at %lld.", s->rest);
    } else if (ncl_streq_ignore_case(verb, "ALLO")) {
        session_reply(s, 202, "ALLO command ignored.");
    } else if (ncl_streq_ignore_case(verb, "ABOR")) {
        session_close_pasv(s);
        s->have_active = false;
        session_reply(s, 226, "Abort successful.");
    } else if (ncl_streq_ignore_case(verb, "STAT")) {
        if (session_require_login(s)) {
            session_replyf(s, 211, "FTP server status: connected, cwd %s",
                           s->cwd);
        }
    } else if (ncl_streq_ignore_case(verb, "HELP")) {
        session_reply(s, 214, "No help available.");
    } else if (ncl_streq_ignore_case(verb, "SITE")) {
        session_reply(s, 500, "SITE command not supported.");
    } else if (ncl_streq_ignore_case(verb, "AUTH") ||
               ncl_streq_ignore_case(verb, "PBSZ") ||
               ncl_streq_ignore_case(verb, "PROT")) {
        session_reply(s, 502, "TLS is not supported.");
    } else if (ncl_streq_ignore_case(verb, "QUIT")) {
        session_reply(s, 221, "Goodbye.");
        s->quit = true;
    } else {
        session_replyf(s, 500, "Unknown command: %s", verb);
    }
}

/* -------------------------------------------------------------- session -- */

static void session_thread(void *context)
{
    ncl_ftp_session *s = (ncl_ftp_session *)context;
    ncl_strbuf line;

    ncl_strbuf_init(&line);
    ncl_ftp_reply(s->ctrl, 220, "NC-Link FTP server ready.");
    while (!s->quit && !s->server->stop) {
        ncl_err rc = ncl_ftp_reader_line(&s->reader, &line,
                                         s->server->idle_timeout_ms);
        char *cursor;
        char *separator;
        const char *verb;
        const char *argument;

        if (rc == NCL_ERR_TIMEOUT) {
            ncl_ftp_reply(s->ctrl, 421, "Idle timeout, closing control connection.");
            break;
        }
        if (rc != NCL_OK) {
            break;
        }
        cursor = line.data;
        while (*cursor == ' ') {
            cursor++;
        }
        if (*cursor == '\0') {
            continue;
        }
        separator = strchr(cursor, ' ');
        if (separator != NULL) {
            *separator = '\0';
            argument = separator + 1;
            while (*argument == ' ') {
                argument++;
            }
            if (*argument == '\0') {
                argument = NULL;
            }
        } else {
            argument = NULL;
        }
        verb = cursor;
        session_dispatch(s, verb, argument);
    }
    ncl_strbuf_free(&line);

    session_close_pasv(s);
    /* The control socket stays owned by the server: whoever reaps this session
     * (the accept thread or ncl_ftp_server_stop) closes it after the join, so
     * the shutdown path can never race with a close here. */
    ncl_mutex_lock(s->server->lock);
    s->finished = true;
    ncl_mutex_unlock(s->server->lock);
}

/** Join and release sessions whose thread has already returned. */
static void server_reap_locked(ncl_ftp_server *srv)
{
    size_t i;

    for (i = srv->sessions.len; i > 0; i--) {
        ncl_ftp_session *s = (ncl_ftp_session *)srv->sessions.items[i - 1];
        if (!s->finished) {
            continue;
        }
        ncl_thread_join(s->thread);
        if (s->ctrl != NULL) {
            ncl_socket_close(s->ctrl);
            s->ctrl = NULL;
        }
        if (srv->session_count > 0) {
            srv->session_count--;
        }
        ncl_mem_free(s);
        for (; i < srv->sessions.len; i++) {
            srv->sessions.items[i - 1] = srv->sessions.items[i];
        }
        srv->sessions.len--;
    }
}

static void server_accept_thread(void *arg)
{
    ncl_ftp_server *srv = (ncl_ftp_server *)arg;

    while (!srv->stop) {
        ncl_socket *ctrl = ncl_socket_accept(srv->listener, 500);
        ncl_ftp_session *session;

        if (ctrl == NULL) {
            continue;
        }
        ncl_socket_set_nodelay(ctrl, true);
        session = (ncl_ftp_session *)ncl_mem_calloc(1, sizeof(*session));
        if (session == NULL) {
            ncl_socket_close(ctrl);
            continue;
        }
        session->server = srv;
        session->ctrl = ctrl;
        session->type_binary = true;
        snprintf(session->cwd, sizeof(session->cwd), "/");
        ncl_ftp_reader_init(&session->reader, ctrl);

        ncl_mutex_lock(srv->lock);
        server_reap_locked(srv);
        session->thread = ncl_thread_start(session_thread, session);
        if (session->thread != NULL &&
            ncl_ptrvec_push(&srv->sessions, session) == NCL_OK) {
            srv->session_count++;
            ncl_mutex_unlock(srv->lock);
        } else {
            ncl_mutex_unlock(srv->lock);
            if (session->thread != NULL) {
                ncl_thread_join(session->thread);
            }
            ncl_socket_close(ctrl);
            ncl_mem_free(session);
        }
    }
}

/* ------------------------------------------------------------ life cycle -- */

ncl_ftp_server *ncl_ftp_server_create(void)
{
    return ncl_ftp_server_create_ex(NULL);
}

ncl_ftp_server *ncl_ftp_server_create_ex(const ncl_ftp_server_options *options)
{
    ncl_ftp_server *srv = (ncl_ftp_server *)ncl_mem_calloc(1, sizeof(*srv));
    const char *root;

    if (srv == NULL) {
        return NULL;
    }
    root = (options != NULL && options->root != NULL) ? options->root
                                                      : ncl_env_root();
    srv->root = ncl_strdup(root != NULL ? root : ".");
    srv->user = ncl_strdup(options != NULL && options->user != NULL
                               ? options->user
                               : NCL_FTP_DEFAULT_USER);
    srv->password =
        ncl_strdup(options != NULL && options->password != NULL
                       ? options->password
                       : NCL_FTP_DEFAULT_PASSWORD);
    srv->port = options != NULL ? options->port : (unsigned)NCL_FTP_SERVER_PORT;
    srv->allow_write = options != NULL ? options->allow_write : true;
    srv->idle_timeout_ms =
        (options != NULL && options->idle_timeout_ms != 0)
            ? options->idle_timeout_ms
            : 300000u;
    srv->data_timeout_ms = 30000u;
    srv->lock = ncl_mutex_create();
    ncl_ptrvec_init(&srv->sessions, NULL);
    if (srv->root == NULL || srv->user == NULL || srv->password == NULL ||
        srv->lock == NULL) {
        ncl_ftp_server_free(srv);
        return NULL;
    }
    return srv;
}

ncl_err ncl_ftp_server_start(ncl_ftp_server *srv)
{
    char err[256];

    if (srv == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (srv->running) {
        return NCL_OK;
    }
    err[0] = '\0';
    srv->listener = ncl_socket_listen(srv->port, err, sizeof(err));
    if (srv->listener == NULL) {
        ncl_log_error("ftp server cannot listen on port %u: %s", srv->port,
                      err);
        return NCL_ERR_CONNECT;
    }
    srv->port = ncl_socket_local_port(srv->listener);
    srv->stop = false;
    srv->acceptor = ncl_thread_start(server_accept_thread, srv);
    if (srv->acceptor == NULL) {
        ncl_socket_close(srv->listener);
        srv->listener = NULL;
        return NCL_ERR;
    }
    srv->running = true;
    ncl_log_info("ftp server listening on port %u, root %s", srv->port,
                 srv->root);
    return NCL_OK;
}

void ncl_ftp_server_stop(ncl_ftp_server *srv)
{
    size_t i;

    if (srv == NULL || !srv->running) {
        return;
    }
    ncl_mutex_lock(srv->lock);
    srv->stop = true;
    ncl_mutex_unlock(srv->lock);

    ncl_socket_shutdown(srv->listener);
    ncl_thread_join(srv->acceptor);
    srv->acceptor = NULL;
    ncl_socket_close(srv->listener);
    srv->listener = NULL;

    ncl_mutex_lock(srv->lock);
    for (i = 0; i < srv->sessions.len; i++) {
        ncl_ftp_session *s = (ncl_ftp_session *)srv->sessions.items[i];
        if (s->ctrl != NULL) {
            ncl_socket_shutdown(s->ctrl);
        }
    }
    ncl_mutex_unlock(srv->lock);

    for (i = 0; i < srv->sessions.len; i++) {
        ncl_ftp_session *s = (ncl_ftp_session *)srv->sessions.items[i];
        ncl_thread_join(s->thread);
        if (s->ctrl != NULL) {
            ncl_socket_close(s->ctrl);
        }
        ncl_mem_free(s);
    }
    ncl_ptrvec_clear(&srv->sessions);
    srv->session_count = 0;
    srv->running = false;
}

void ncl_ftp_server_free(ncl_ftp_server *srv)
{
    if (srv == NULL) {
        return;
    }
    ncl_ftp_server_stop(srv);
    ncl_ptrvec_free(&srv->sessions);
    ncl_mem_free(srv->root);
    ncl_mem_free(srv->user);
    ncl_mem_free(srv->password);
    if (srv->lock != NULL) {
        ncl_mutex_destroy(srv->lock);
    }
    ncl_mem_free(srv);
}

bool ncl_ftp_server_is_running(ncl_ftp_server *srv)
{
    bool running;

    if (srv == NULL) {
        return false;
    }
    ncl_mutex_lock(srv->lock);
    running = srv->running;
    ncl_mutex_unlock(srv->lock);
    return running;
}

unsigned ncl_ftp_server_port(const ncl_ftp_server *srv)
{
    return srv != NULL ? srv->port : 0;
}

size_t ncl_ftp_server_session_count(ncl_ftp_server *srv)
{
    size_t count;

    if (srv == NULL) {
        return 0;
    }
    ncl_mutex_lock(srv->lock);
    server_reap_locked(srv);
    count = srv->session_count;
    ncl_mutex_unlock(srv->lock);
    return count;
}

long long ncl_ftp_server_command_count(ncl_ftp_server *srv)
{
    long long count;

    if (srv == NULL) {
        return 0;
    }
    ncl_mutex_lock(srv->lock);
    count = srv->command_count;
    ncl_mutex_unlock(srv->lock);
    return count;
}

const char *ncl_ftp_server_root(const ncl_ftp_server *srv)
{
    return srv != NULL ? srv->root : NULL;
}
