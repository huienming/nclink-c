/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - file transfer.
 *
 * Bulk data travels outside of MQTT: a control message carries a small
 * "/temp/<name>" path token and the bytes travel over FTP.
 *
 *   ncl_server_file_tool - the device connects out to the peer's FTP server,
 *                          uploads (<root>/uploadFile/<path>) and downloads
 *                          files.
 *   ncl_file_client_tool - speaks the NC-Link "get_value / set_value / add /
 *                          delete on /CONTROLLER/FILE" protocol over MQTT, and
 *                          stores the result under <cwd>/<sn>/<path>.
 *   ncl_server_register_file_tool - installs the "file" tool on the server:
 *                          write, read, ll, mkdir, delete, bound to
 *                          /CONTROLLER/FILE.
 *
 * A protocol message cannot carry a file, so a method parameter or result value
 * that names one is expressed as an explicit marker object:
 *
 *   {"@file": "<local path>"}      as a method parameter or result value
 *
 * ncl_server_invoke_method_call() replaces such a result with the "/temp/<name>"
 * token and lists the key in "fileKeys"; ncl_client_method_call_file() performs
 * the mirror operation.
 */
#ifndef NCL_FILE_H
#define NCL_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_client.h"
#include "nclink/ncl_common.h"
#include "nclink/ncl_ftp.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Upload chunk size in bytes. */
#define NCL_FILE_CHUNK_SIZE (1024 * 256)
/** Number of chunks transferred in parallel. */
#define NCL_FILE_PARALLEL_CHUNKS 5
/** Timeout of one file channel operation, milliseconds. */
#define NCL_FILE_OPERATION_TIMEOUT 5000

/** Protocol directory used for the MQTT file hand-off. */
#define NCL_FILE_TEMP_DIR "/temp"
/** Local directory the device writes fetched files into. */
#define NCL_FILE_UPLOAD_DIR "/uploadFile"
/** Key of the marker object that stands in for a file value. */
#define NCL_FILE_MARKER "@file"

/* ====================================================== file attribute ==== */

/** Attributes of a file or directory. */
typedef struct {
    char     *file_name;   /**< fileName   */
    int       file_type;   /**< fileType, 1 = directory                     */
    long long file_size;   /**< fileSize                                   */
    int       total_chunks;/**< totalChunks                                */
    bool      compressed;  /**< compressed                                 */
    char     *checksum;    /**< checksum (SHA-256 hex, or the modification
                                timestamp for entries read over FTP)        */
    char     *parant_dir;  /**< parantDir (the upstream spelling)          */
    int64_t   modify_time; /**< modifyTime, epoch milliseconds             */
} ncl_file_attribute;

ncl_file_attribute *ncl_file_attribute_new(void);
void                ncl_file_attribute_free(ncl_file_attribute *attribute);
/** ncl_free_fn compatible destructor for ncl_ptrvec. */
void                ncl_file_attribute_release(void *attribute);

/** True when the attribute describes a directory. */
bool ncl_file_attribute_is_dir(const ncl_file_attribute *attribute);

/** Property order: fileName, fileType, fileSize, totalChunks, compressed,
 *  checksum, parantDir, modifyTime (null valued entries are omitted). */
ncl_json           *ncl_file_attribute_to_json(const ncl_file_attribute *attribute);
ncl_file_attribute *ncl_file_attribute_from_json(const ncl_json *json);

/** Serialise a list of attributes into a JSON array. */
ncl_json *ncl_file_attributes_to_json(const ncl_ptrvec *attributes);

/* ============================================================ checksum ==== */

/** SHA-256 of @p len bytes at @p data, lower case hex into a heap string. */
ncl_err ncl_sha256_hex(const void *data, size_t len, char **out_hex);

/* ====================================================== file utilities ==== */

/** True when the extension of @p file_name is compressed on transfer. */
bool ncl_file_need_compression(const char *file_name);

/** ceil(size / NCL_FILE_CHUNK_SIZE), 0 for a size of 0. */
int ncl_file_total_chunks(long long size);

/** SHA-256 over the contents of @p path. */
ncl_err ncl_file_checksum(const char *path, char **out_hex);

/**
 * Attribute of the local file or directory @p path: a directory yields type 1,
 * 0 chunks and an empty checksum; a file yields its size, chunk count,
 * compression flag and SHA-256.
 */
ncl_err ncl_file_attribute_of(const char *path, const char *parent,
                              ncl_file_attribute **out);

/**
 * Attribute of the remote @p entry reported by the peer's FTP server. For a
 * file the checksum field receives the *modification time* formatted as
 * "yyyy-MM-dd HH:mm:ss", not a hash: FTP LIST gives no way to read the file.
 */
ncl_err ncl_file_attribute_from_entry(const ncl_ftp_entry *entry,
                                      const char *parent,
                                      ncl_file_attribute **out);

/* ========================================================= ftpResponse ==== */

/** Contents of bin/ftp.txt: FTP credentials, port and interface map. */
typedef struct {
    char     *user_name;
    char     *password;
    int       port;
    ncl_json *ip_map; /**< object of interface -> IPv4, may be NULL */
} ncl_ftp_response;

/** Populate from bin/ftp.txt, creating it with the built in defaults (admin /
 *  123456 / 2121 plus the local interface map) when the file is missing. */
ncl_err ncl_ftp_info_read(ncl_ftp_response *out);

/** Persist to bin/ftp.txt. */
ncl_err ncl_ftp_info_write(const ncl_ftp_response *info);

void ncl_ftp_info_free(ncl_ftp_response *info);

/* ================================================= FTP client (device) ==== */

/**
 * The FTP client the device uses to move files to the peer: two attempts per
 * operation and a remote layout of "/<sn>/<remoteDir>".
 */
typedef struct ncl_server_file_tool ncl_server_file_tool;

ncl_server_file_tool *ncl_server_file_tool_create(const char *ip, unsigned port,
                                                  const char *user,
                                                  const char *password,
                                                  const char *sn);
void ncl_server_file_tool_free(ncl_server_file_tool *tool);

/** Validate the link with NOOP first, reconnecting when it is gone. */
bool ncl_server_file_tool_detect(ncl_server_file_tool *tool);

/** Log out and drop the control connection. */
void ncl_server_file_tool_disconnect(ncl_server_file_tool *tool);

/** Upload @p local_path into "/<sn><remoteDir>/". */
bool ncl_server_file_tool_write(ncl_server_file_tool *tool,
                                const char *local_path, const char *remote_dir);

/**
 * Download "/<sn>/<remoteFilePath>" to <root>/uploadFile/<remoteFilePath> and
 * return that heap path (NULL on failure). Free with free().
 */
char *ncl_server_file_tool_read(ncl_server_file_tool *tool,
                                const char *remote_file_path);

/** List @p remote_dir into @p out (initialize with
 *  ncl_ptrvec_init(out, ncl_file_attribute_release)). */
ncl_err ncl_server_file_tool_ll(ncl_server_file_tool *tool,
                                const char *remote_dir, ncl_ptrvec *out);

/** Create @p remote_dir on the peer. */
bool ncl_server_file_tool_mkdir(ncl_server_file_tool *tool,
                                const char *remote_dir);

/** Delete @p remote_file_path, recursive for directories. */
bool ncl_server_file_tool_delete(ncl_server_file_tool *tool,
                                 const char *remote_file_path);

/* ================================================= FTP client (tool) ===== */

/**
 * Talks to the peer's /CONTROLLER/FILE node over the NC-Link protocol. Files
 * live under <cwd>/<sn>/<path>.
 */
typedef struct ncl_file_client_tool ncl_file_client_tool;

ncl_file_client_tool *ncl_file_client_tool_create(ncl_client *client);
void                  ncl_file_client_tool_free(ncl_file_client_tool *tool);

/** Create <cwd>/<sn>. */
bool ncl_file_client_tool_detect(ncl_file_client_tool *tool);

/** Upload @p local_file_path with "set" on /CONTROLLER/FILE. */
bool ncl_file_client_tool_write(ncl_file_client_tool *tool,
                                const char *local_file_path);

/** Download @p remote_file_path with "get_value"; returns the heap local path
 *  under <cwd>/<sn><remoteFilePath>, or NULL. */
char *ncl_file_client_tool_read(ncl_file_client_tool *tool,
                                const char *remote_file_path);

/** List @p remote_dir with "get_attributes". */
ncl_err ncl_file_client_tool_ll(ncl_file_client_tool *tool,
                                const char *remote_dir, ncl_ptrvec *out);

/** Create @p remote_dir with "add". */
bool ncl_file_client_tool_mkdir(ncl_file_client_tool *tool,
                                const char *remote_dir);

/** Delete @p remote_file_path with "delete". */
bool ncl_file_client_tool_delete(ncl_file_client_tool *tool,
                                 const char *remote_file_path);

/* ================================================ client integration ===== */

/**
 * Method call with file parameters: every entry of @p file_paths is copied to
 * <cwd>/<sn>/temp/<name>, uploaded through the file channel, and the matching
 * params[key] is replaced by "/temp/<name>".
 * After the response, "fileKeys" entries are downloaded through the file
 * channel and replaced by their local path.
 *
 * Takes ownership of @p request and stores the response in *out.
 */
ncl_err ncl_client_method_call_file(ncl_client *client, ncl_message *request,
                                    const char *const *file_keys,
                                    const char *const *file_paths,
                                    size_t file_count, unsigned timeout_ms,
                                    ncl_message **out);

/** Upload @p local_file_path through the file channel installed by
 *  ncl_file_client_tool_create(); false when no channel is installed. */
ncl_err ncl_client_write(ncl_client *client, const char *local_file_path);

/** Download @p remote_file_path; returns a heap local path or NULL. */
char *ncl_client_read(ncl_client *client, const char *remote_file_path);

/** List @p remote_dir on the peer. */
ncl_err ncl_client_ll(ncl_client *client, const char *remote_dir,
                      ncl_ptrvec *out);

/** Install the file channel of @p client, or remove it when @p tool is NULL. */
void ncl_client_set_file_tool(ncl_client *client, ncl_file_client_tool *tool);
ncl_file_client_tool *ncl_client_file_tool(ncl_client *client);

/** Start the process wide FTP server on 127.0.0.1:2323 rooted at the current
 *  working directory with user admin / 123456. Safe to call twice. */
ncl_err ncl_client_holder_start_ftp(void);

/** Stop the process wide FTP server. */
void ncl_client_holder_stop_ftp(void);

/**
 * Rebuild the process wide client from conf/mqtt.cfg: the running connection is
 * dropped and a new one is established with the values now on disk. Use this
 * after changing the broker at runtime. Returns NCL_OK when a connection is
 * re-established.
 */
ncl_err ncl_client_holder_restart(void);

/* ================================================= server integration ==== */

/** Register the built in "file" tool on @p server. */
ncl_err ncl_server_register_file_tool(ncl_server *server);

/**
 * Start the server side FTP endpoint: read bin/ftp.txt for the port and
 * credentials and serve ncl_env_root() as the login root.
 */
ncl_err ncl_server_start_ftp(ncl_server *server);

/** Stop the server side FTP endpoint. */
void ncl_server_stop_ftp(ncl_server *server);

#ifdef __cplusplus
}
#endif

#endif /* NCL_FILE_H */
