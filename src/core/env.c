/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* NC-Link core - environment paths, mqtt.cfg, sn.txt, file helpers. */
#include "nclink/ncl_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"

#if !defined(NCL_OS_WINDOWS)
#  include <errno.h>
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#else
#  include <io.h> /* _chsize_s / _fileno */
#endif

#include "nclink/ncl_logger.h"

char *ncl_sn_generate(void)
{
    static const char kHex[] = "0123456789ABCDEF";
    unsigned char raw[9];
    char out[12];
    bool has_letter = false;
    size_t i;

    if (!ncl_random_bytes(raw, sizeof(raw))) {
        return NULL;
    }
    /* "V2" followed by nine upper case hex digits, one random nibble per
     * digit: the serial *is* hexadecimal text, not a number printed in hex. */
    out[0] = 'V';
    out[1] = '2';
    for (i = 0; i < sizeof(raw); i++) {
        char digit = kHex[raw[i] & 0x0F];
        out[2 + i] = digit;
        if (digit > '9') {
            has_letter = true;
        }
    }
    /* Nine random hex digits are all decimal digits about 1 % of the time.
     * Force a letter in that case, so a fresh SN never reads as a plain
     * number (nor does the run-time <cwd>/<sn> directory it creates). */
    if (!has_letter) {
        out[2 + sizeof(raw) - 1] = kHex[10 + ((raw[0] >> 4) % 6)];
    }
    out[2 + sizeof(raw)] = '\0';

    return ncl_strdup(out);
}

static char *g_root = NULL;
static char *g_conf = NULL;
static char *g_run = NULL;
static char *g_driver = NULL;
static char *g_plugin = NULL;
static char *g_log = NULL;
static char *g_log_file = NULL;
static char *g_mqtt_cfg = NULL;
static char *g_model_file = NULL;
static char *g_driver_cfg = NULL;
static char *g_sn_file = NULL;
static ncl_ptrvec g_server_list;
static bool g_server_list_ready = false;

static void ncl_env_invalidate_paths(void)
{
    ncl_mem_free(g_conf); g_conf = NULL;
    ncl_mem_free(g_run); g_run = NULL;
    ncl_mem_free(g_driver); g_driver = NULL;
    ncl_mem_free(g_log); g_log = NULL;
    ncl_mem_free(g_log_file); g_log_file = NULL;
    ncl_mem_free(g_mqtt_cfg); g_mqtt_cfg = NULL;
    ncl_mem_free(g_model_file); g_model_file = NULL;
    ncl_mem_free(g_driver_cfg); g_driver_cfg = NULL;
    ncl_mem_free(g_sn_file); g_sn_file = NULL;
}

void ncl_env_set_root(const char *path)
{
    ncl_mem_free(g_root);
    g_root = NULL;
    if (path != NULL && path[0] != '\0') {
        g_root = ncl_strdup(path);
    }
    ncl_env_invalidate_paths();
}

const char *ncl_env_root(void)
{
    if (g_root == NULL) {
        char buf[4096];
        size_t len;
#if defined(NCL_OS_WINDOWS)
        DWORD n = GetCurrentDirectoryA((DWORD)sizeof(buf), buf);
        if (n == 0 || n >= sizeof(buf)) {
            return ".";
        }
#else
        if (getcwd(buf, sizeof(buf)) == NULL) {
            return ".";
        }
#endif
        len = strlen(buf);
        while (len > 1 && (buf[len - 1] == '\\' || buf[len - 1] == '/')) {
            buf[--len] = '\0';
        }
        g_root = ncl_strdup(buf);
        if (g_root == NULL) {
            return ".";
        }
    }
    return g_root;
}

static const char *ncl_env_join(char **slot, const char *base, const char *leaf)
{
    if (*slot == NULL) {
        if (ncl_asprintf(slot, "%s%c%s", base, NCL_PATH_SEP, leaf) != NCL_OK) {
            return NULL;
        }
    }
    return *slot;
}

const char *ncl_env_conf_path(void)
{
    return ncl_env_join(&g_conf, ncl_env_root(), "conf");
}

const char *ncl_env_run_path(void)
{
    return ncl_env_join(&g_run, ncl_env_root(), "bin");
}

const char *ncl_env_driver_path(void)
{
    return ncl_env_join(&g_driver, ncl_env_root(), "drivers");
}

/** Where the adapter host picks up its modules ("plugins"). */
const char *ncl_env_plugin_path(void)
{
    return ncl_env_join(&g_plugin, ncl_env_root(), "plugins");
}

const char *ncl_env_log_path(void)
{
    return ncl_env_join(&g_log, ncl_env_root(), "log");
}

const char *ncl_env_log_file(void)
{
    if (g_log_file == NULL) {
        const char *dir = ncl_env_log_path();
        if (dir == NULL ||
            ncl_asprintf(&g_log_file, "%s%cout.txt", dir, NCL_PATH_SEP) != NCL_OK) {
            return NULL;
        }
    }
    return g_log_file;
}

const char *ncl_env_mqtt_cfg_file(void)
{
    if (g_mqtt_cfg == NULL) {
        const char *dir = ncl_env_conf_path();
        if (dir == NULL ||
            ncl_asprintf(&g_mqtt_cfg, "%s%cmqtt.cfg", dir, NCL_PATH_SEP) != NCL_OK) {
            return NULL;
        }
    }
    return g_mqtt_cfg;
}

const char *ncl_env_model_file(void)
{
    if (g_model_file == NULL) {
        const char *dir = ncl_env_conf_path();
        if (dir == NULL ||
            ncl_asprintf(&g_model_file, "%s%cmodel%cnclink.json", dir,
                         NCL_PATH_SEP, NCL_PATH_SEP) != NCL_OK) {
            return NULL;
        }
    }
    return g_model_file;
}

const char *ncl_env_driver_cfg_file(void)
{
    if (g_driver_cfg == NULL) {
        const char *dir = ncl_env_conf_path();
        if (dir == NULL ||
            ncl_asprintf(&g_driver_cfg, "%s%cdriver%cdriver.json", dir,
                         NCL_PATH_SEP, NCL_PATH_SEP) != NCL_OK) {
            return NULL;
        }
    }
    return g_driver_cfg;
}

const char *ncl_env_sn_file(void)
{
    if (g_sn_file == NULL) {
        const char *dir = ncl_env_run_path();
        if (dir == NULL ||
            ncl_asprintf(&g_sn_file, "%s%csn.txt", dir, NCL_PATH_SEP) != NCL_OK) {
            return NULL;
        }
    }
    return g_sn_file;
}

void ncl_env_set_server_list(const char *const *servers, size_t count)
{
    size_t i;
    if (!g_server_list_ready) {
        ncl_ptrvec_init(&g_server_list, ncl_mem_free);
        g_server_list_ready = true;
    }
    ncl_ptrvec_clear(&g_server_list);
    for (i = 0; i < count; i++) {
        char *copy = ncl_strdup(servers[i]);
        if (copy == NULL || ncl_ptrvec_push(&g_server_list, copy) != NCL_OK) {
            ncl_mem_free(copy);
            break;
        }
    }
}

size_t ncl_env_server_count(void)
{
    return g_server_list_ready ? ncl_ptrvec_len(&g_server_list) : 0;
}

const char *ncl_env_server_at(size_t index)
{
    if (!g_server_list_ready) {
        return NULL;
    }
    return (const char *)ncl_ptrvec_at(&g_server_list, index);
}

void ncl_env_shutdown(void)
{
    ncl_mem_free(g_root);
    g_root = NULL;
    ncl_env_invalidate_paths();
    if (g_server_list_ready) {
        ncl_ptrvec_free(&g_server_list);
        g_server_list_ready = false;
    }
}

/* ---------------------------------------------------------------- files -- */

bool ncl_path_exists(const char *path)
{
    FILE *fp;
    if (path == NULL) {
        return false;
    }
#if defined(NCL_OS_WINDOWS)
    /* fopen() cannot see directories, so ask the filesystem directly. */
    {
        DWORD attributes = GetFileAttributesA(path);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            return true;
        }
    }
#else
    {
        struct stat info;
        if (stat(path, &info) == 0) {
            return true;
        }
    }
#endif
    fp = fopen(path, "rb");
    if (fp != NULL) {
        fclose(fp);
        return true;
    }
    return false;
}

ncl_err ncl_mkdir_p(const char *path)
{
    char *work;
    char *p;
    ncl_err rc = NCL_OK;

    if (path == NULL || path[0] == '\0') {
        return NCL_ERR_INVALID_ARG;
    }
    work = ncl_strdup(path);
    if (work == NULL) {
        return NCL_ERR_NOMEM;
    }

    for (p = work + 1; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/') {
            char saved = *p;
            *p = '\0';
#if defined(NCL_OS_WINDOWS)
            CreateDirectoryA(work, NULL);
#else
            mkdir(work, 0775);
#endif
            *p = saved;
        }
    }
#if defined(NCL_OS_WINDOWS)
    if (!CreateDirectoryA(work, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            rc = NCL_ERR_IO;
        }
    }
#else
    if (mkdir(work, 0775) != 0 && errno != EEXIST) {
        rc = NCL_ERR_IO;
    }
#endif
    ncl_mem_free(work);
    return rc;
}

ncl_err ncl_file_read_all(const char *path, char **out, size_t *out_len)
{
    FILE *fp;
    long size;
    char *buf;
    size_t got;

    if (path == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (out_len != NULL) {
        *out_len = 0;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return NCL_ERR_IO;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NCL_ERR_IO;
    }
    size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NCL_ERR_IO;
    }
    rewind(fp);

    buf = (char *)ncl_mem_alloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(fp);
        return NCL_ERR_NOMEM;
    }
    got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[got] = '\0';
    *out = buf;
    if (out_len != NULL) {
        *out_len = got;
    }
    return NCL_OK;
}

ncl_err ncl_file_write_all(const char *path, const void *data, size_t len)
{
    FILE *fp;
    size_t written;

    if (path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    fp = fopen(path, "wb");
    if (fp == NULL) {
        return NCL_ERR_IO;
    }
    written = len > 0 ? fwrite(data, 1, len, fp) : 0;
    fclose(fp);
    return written == len ? NCL_OK : NCL_ERR_IO;
}

ncl_err ncl_file_append(const char *path, const void *data, size_t len)
{
    FILE *fp;
    size_t written;

    if (path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    fp = fopen(path, "ab");
    if (fp == NULL) {
        return NCL_ERR_IO;
    }
    written = len > 0 ? fwrite(data, 1, len, fp) : 0;
    fclose(fp);
    return written == len ? NCL_OK : NCL_ERR_IO;
}

/* ------------------------------------------------------ streamed files --- */

struct ncl_file_stream {
    FILE      *handle;
    long long  written; /**< writer: bytes written through this stream */
};

/* Offsets beyond 2 GB need the 64-bit seek of each platform. */
#if defined(NCL_OS_WINDOWS)
#  define stream_seek(fp, off) _fseeki64((fp), (off), SEEK_SET)
#else
#  define stream_seek(fp, off) fseeko((fp), (off), SEEK_SET)
#endif

/** Cut @p path down to @p size bytes (a short resume must not keep a tail). */
static int stream_truncate(FILE *fp, long long size)
{
#if defined(NCL_OS_WINDOWS)
    return _chsize_s(_fileno(fp), size) == 0 ? 0 : -1;
#else
    fflush(fp);
    return ftruncate(fileno(fp), (off_t)size);
#endif
}

ncl_err ncl_file_open_read(const char *path, long long offset,
                           ncl_file_stream **out)
{
    ncl_file_stream *stream;

    if (path == NULL || out == NULL || offset < 0) {
        return NCL_ERR_INVALID_ARG;
    }
    *out = NULL;
    stream = (ncl_file_stream *)ncl_mem_calloc(1, sizeof(*stream));
    if (stream == NULL) {
        return NCL_ERR_NOMEM;
    }
    stream->handle = fopen(path, "rb");
    if (stream->handle == NULL ||
        (offset > 0 && stream_seek(stream->handle, offset) != 0)) {
        if (stream->handle != NULL) {
            fclose(stream->handle);
        }
        ncl_mem_free(stream);
        return NCL_ERR_IO;
    }
    *out = stream;
    return NCL_OK;
}

size_t ncl_file_read_chunk(ncl_file_stream *stream, void *buf, size_t len)
{
    if (stream == NULL || stream->handle == NULL || buf == NULL || len == 0) {
        return 0;
    }
    return fread(buf, 1, len, stream->handle);
}

void ncl_file_close_read(ncl_file_stream *stream)
{
    if (stream == NULL) {
        return;
    }
    if (stream->handle != NULL) {
        fclose(stream->handle);
    }
    ncl_mem_free(stream);
}

ncl_err ncl_file_open_write(const char *path, long long offset, bool append,
                            ncl_file_stream **out)
{
    ncl_file_stream *stream;

    if (path == NULL || out == NULL || offset < 0) {
        return NCL_ERR_INVALID_ARG;
    }
    *out = NULL;
    stream = (ncl_file_stream *)ncl_mem_calloc(1, sizeof(*stream));
    if (stream == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (append) {
        long long existing = ncl_file_size(path);

        stream->handle = fopen(path, "ab");
        /* The close-time truncate must not cut back what was already there. */
        stream->written = existing > 0 ? existing : 0;
    } else if (offset > 0) {
        /* Resume: keep the first @p offset bytes and overwrite from there. */
        stream->handle = fopen(path, "r+b");
        if (stream->handle == NULL) {
            stream->handle = fopen(path, "w+b");
        }
        if (stream->handle != NULL && stream_seek(stream->handle, offset) != 0) {
            fclose(stream->handle);
            stream->handle = NULL;
        }
    } else {
        stream->handle = fopen(path, "wb");
    }
    if (stream->handle == NULL) {
        ncl_mem_free(stream);
        return NCL_ERR_IO;
    }
    if (!append && offset > 0) {
        stream->written = offset;
    }
    *out = stream;
    return NCL_OK;
}

ncl_err ncl_file_write_chunk(ncl_file_stream *stream, const void *buf,
                             size_t len)
{
    if (stream == NULL || stream->handle == NULL || (buf == NULL && len > 0)) {
        return NCL_ERR_INVALID_ARG;
    }
    if (len > 0 && fwrite(buf, 1, len, stream->handle) != len) {
        return NCL_ERR_IO;
    }
    stream->written += (long long)len;
    return NCL_OK;
}

ncl_err ncl_file_close_write(ncl_file_stream *stream)
{
    ncl_err rc = NCL_OK;

    if (stream == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (stream->handle != NULL) {
        if (fflush(stream->handle) != 0 ||
            stream_truncate(stream->handle, stream->written) != 0 ||
            fclose(stream->handle) != 0) {
            rc = NCL_ERR_IO;
        }
    }
    ncl_mem_free(stream);
    return rc;
}

ncl_err ncl_file_copy(const char *src, const char *dst)
{
    char chunk[64 * 1024];
    ncl_file_stream *reader = NULL;
    ncl_file_stream *writer = NULL;
    char parent[NCL_PATH_MAX_BUF];
    char *slash;
    ncl_err rc;

    if (src == NULL || dst == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    /* Copying a file onto itself is a no-op: the streamed version below would
     * truncate the destination before the reader ever saw a byte. */
    if (ncl_path_same_file(src, dst)) {
        return ncl_path_exists(dst) ? NCL_OK : NCL_ERR_NOT_FOUND;
    }
    snprintf(parent, sizeof(parent), "%s", dst);
    slash = strrchr(parent, NCL_PATH_SEP);
    if (slash == NULL) {
        slash = strrchr(parent, '/');
    }
    if (slash != NULL) {
        *slash = '\0';
        if (parent[0] != '\0') {
            ncl_mkdir_p(parent);
        }
    }
    /* Streamed in 64 KiB pieces: copying a big file costs a fixed buffer. */
    rc = ncl_file_open_read(src, 0, &reader);
    if (rc == NCL_OK) {
        rc = ncl_file_open_write(dst, 0, false, &writer);
    }
    while (rc == NCL_OK) {
        size_t got = ncl_file_read_chunk(reader, chunk, sizeof(chunk));

        if (got == 0) {
            break;
        }
        rc = ncl_file_write_chunk(writer, chunk, got);
    }
    if (reader != NULL) {
        ncl_file_close_read(reader);
    }
    if (writer != NULL) {
        ncl_err close_rc = ncl_file_close_write(writer);
        if (rc == NCL_OK) {
            rc = close_rc;
        }
    }
    return rc;
}

bool ncl_path_is_dir(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }
#if defined(NCL_OS_WINDOWS)
    {
        DWORD attributes = GetFileAttributesA(path);
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
#else
    {
        struct stat st;
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    }
#endif
}

bool ncl_path_same_file(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    /* Identity, not path text: the same file reached through a different
     * spelling ("./x" vs "x", "a/../x", a symlink) still counts. */
#if defined(NCL_OS_WINDOWS)
    {
        DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        HANDLE handle_a = CreateFileA(a, 0, share, NULL, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, NULL);
        HANDLE handle_b = CreateFileA(b, 0, share, NULL, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, NULL);
        BY_HANDLE_FILE_INFORMATION info_a;
        BY_HANDLE_FILE_INFORMATION info_b;
        bool same;

        if (handle_a != INVALID_HANDLE_VALUE &&
            handle_b != INVALID_HANDLE_VALUE &&
            GetFileInformationByHandle(handle_a, &info_a) &&
            GetFileInformationByHandle(handle_b, &info_b)) {
            same = info_a.dwVolumeSerialNumber == info_b.dwVolumeSerialNumber &&
                   info_a.nFileIndexHigh == info_b.nFileIndexHigh &&
                   info_a.nFileIndexLow == info_b.nFileIndexLow;
        } else {
            /* Not on disk: the text is all we have. */
            same = _stricmp(a, b) == 0;
        }
        if (handle_a != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_a);
        }
        if (handle_b != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_b);
        }
        return same;
    }
#else
    {
        struct stat info_a;
        struct stat info_b;

        if (stat(a, &info_a) == 0 && stat(b, &info_b) == 0) {
            return info_a.st_dev == info_b.st_dev &&
                   info_a.st_ino == info_b.st_ino;
        }
        /* Not on disk: the text is all we have. */
        return strcmp(a, b) == 0;
    }
#endif
}

long long ncl_file_size(const char *path)
{
    if (path == NULL || ncl_path_is_dir(path)) {
        return -1;
    }
#if defined(NCL_OS_WINDOWS)
    {
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) {
            return -1;
        }
        return ((long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    }
#else
    {
        struct stat st;
        if (stat(path, &st) != 0) {
            return -1;
        }
        return (long long)st.st_size;
    }
#endif
}

int64_t ncl_file_mtime_ms(const char *path)
{
#if defined(NCL_OS_WINDOWS)
    WIN32_FILE_ATTRIBUTE_DATA info;
    ULARGE_INTEGER value;

    if (path == NULL ||
        !GetFileAttributesExA(path, GetFileExInfoStandard, &info)) {
        return 0;
    }
    value.LowPart = info.ftLastWriteTime.dwLowDateTime;
    value.HighPart = info.ftLastWriteTime.dwHighDateTime;
    return (int64_t)(value.QuadPart / 10000ULL) - 11644473600000LL;
#else
    struct stat st;
    if (path == NULL || stat(path, &st) != 0) {
        return 0;
    }
    return (int64_t)st.st_mtime * 1000;
#endif
}

static ncl_err ncl_path_remove_recursive(const char *path)
{
    if (!ncl_path_exists(path)) {
        return NCL_OK;
    }
    if (!ncl_path_is_dir(path)) {
        return remove(path) == 0 ? NCL_OK : NCL_ERR_IO;
    }
#if defined(NCL_OS_WINDOWS)
    {
        WIN32_FIND_DATAA entry;
        HANDLE handle;
        char pattern[NCL_PATH_MAX_BUF];

        snprintf(pattern, sizeof(pattern), "%s\\*", path);
        handle = FindFirstFileA(pattern, &entry);
        if (handle != INVALID_HANDLE_VALUE) {
            do {
                char child[NCL_PATH_MAX_BUF];
                if (strcmp(entry.cFileName, ".") == 0 ||
                    strcmp(entry.cFileName, "..") == 0) {
                    continue;
                }
                snprintf(child, sizeof(child), "%s\\%s", path, entry.cFileName);
                ncl_path_remove_recursive(child);
            } while (FindNextFileA(handle, &entry));
            FindClose(handle);
        }
        return RemoveDirectoryA(path) ? NCL_OK : NCL_ERR_IO;
    }
#else
    {
        DIR *dir = opendir(path);
        struct dirent *entry;

        if (dir != NULL) {
            while ((entry = readdir(dir)) != NULL) {
                char child[NCL_PATH_MAX_BUF];
                if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
                ncl_path_remove_recursive(child);
            }
            closedir(dir);
        }
        return rmdir(path) == 0 ? NCL_OK : NCL_ERR_IO;
    }
#endif
}

ncl_err ncl_path_remove(const char *path)
{
    if (path == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return ncl_path_remove_recursive(path);
}

/* ----------------------------------------------------------- MqttConfig -- */

/* Returns the remainder of the line
 * that follows "<key>=". */
static char *ncl_mqtt_cfg_value(const char *text, const char *key)
{
    size_t key_len = strlen(key);
    const char *p = text;

    while (*p != '\0') {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *value = p + key_len + 1;
            const char *end = value;
            while (*end != '\0' && *end != '\r' && *end != '\n') {
                end++;
            }
            return ncl_strndup(value, (size_t)(end - value));
        }
        /* advance to the next line */
        while (*p != '\0' && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            p++;
        }
    }
    return NULL;
}

ncl_err ncl_mqtt_config_read(ncl_mqtt_config *out)
{
    const char *path;
    char *content = NULL;
    char *url = NULL;
    char *user = NULL;
    char *pwd = NULL;

    if (out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    out->url = NULL;
    out->username = NULL;
    out->password = NULL;

    path = ncl_env_mqtt_cfg_file();
    if (path == NULL) {
        return NCL_ERR_IO;
    }

    if (!ncl_path_exists(path)) {
        static const char defaults[] =
            "url=tcp://localhost:1883\r\nusername=\r\npassword=\r\n";
        const char *conf_dir = ncl_env_conf_path();
        if (conf_dir != NULL) {
            ncl_mkdir_p(conf_dir);
        }
        if (ncl_file_write_all(path, defaults, sizeof(defaults) - 1) != NCL_OK) {
            ncl_log_error("无法写入默认MQTT配置: %s", path);
        }
        out->url = ncl_strdup("tcp://localhost:1883");
        out->username = ncl_strdup("admin");
        out->password = ncl_strdup("public");
        return (out->url != NULL && out->username != NULL && out->password != NULL)
                   ? NCL_OK
                   : NCL_ERR_NOMEM;
    }

    if (ncl_file_read_all(path, &content, NULL) != NCL_OK) {
        ncl_log_error("读取MQTT配置失败: %s", path);
        return NCL_ERR_IO;
    }

    url = ncl_mqtt_cfg_value(content, "url");
    user = ncl_mqtt_cfg_value(content, "username");
    pwd = ncl_mqtt_cfg_value(content, "password");
    ncl_mem_free(content);

    if (url == NULL || url[0] == '\0') {
        ncl_mem_free(url);
        url = ncl_strdup("tcp://localhost:1883");
    }
    if (user == NULL || user[0] == '\0') {
        ncl_mem_free(user);
        user = ncl_strdup("admin");
    }
    if (pwd == NULL) {
        pwd = ncl_strdup("");
    }
    out->url = url;
    out->username = user;
    out->password = pwd;

    if (out->url == NULL || out->username == NULL || out->password == NULL) {
        ncl_mqtt_config_free(out);
        return NCL_ERR_NOMEM;
    }
    return NCL_OK;
}

void ncl_mqtt_config_free(ncl_mqtt_config *cfg)
{
    if (cfg == NULL) {
        return;
    }
    ncl_mem_free(cfg->url);
    ncl_mem_free(cfg->username);
    ncl_mem_free(cfg->password);
    cfg->url = NULL;
    cfg->username = NULL;
    cfg->password = NULL;
}

/* ------------------------------------------------------- serial number ---- */

char *ncl_sn_read(void)
{
    const char *path = ncl_env_sn_file();
    char *content = NULL;
    char *trimmed;

    if (path == NULL) {
        return NULL;
    }
    if (!ncl_path_exists(path)) {
        char *sn = ncl_sn_generate();
        const char *run_dir = ncl_env_run_path();
        if (sn == NULL) {
            return NULL;
        }
        if (run_dir != NULL) {
            ncl_mkdir_p(run_dir);
        }
        ncl_file_write_all(path, sn, strlen(sn));
        return sn;
    }
    if (ncl_file_read_all(path, &content, NULL) != NCL_OK) {
        ncl_log_error("读取SN失败: %s", path);
        return NULL;
    }
    trimmed = ncl_str_trim_dup(content);
    ncl_mem_free(content);
    return trimmed;
}
