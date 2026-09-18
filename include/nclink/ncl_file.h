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
/** Longest channel id a file channel handshake accepts. */
#define NCL_FILE_CHANNEL_ID_MAX 96

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

/**
 * Streaming SHA-256, for files too big to hand over as one buffer: create,
 * feed pieces, take the hex digest (heap, caller frees), release.
 */
typedef struct ncl_sha256 ncl_sha256;

ncl_sha256 *ncl_sha256_new(void);
ncl_err ncl_sha256_update(ncl_sha256 *ctx, const void *data, size_t len);
/** Hex digest (heap, ncl_free_safe()); NULL when the context is unusable. */
char *ncl_sha256_finish(ncl_sha256 *ctx);
void ncl_sha256_free(ncl_sha256 *ctx);

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

/**
 * Use passive mode (PASV) instead of the default active mode (PORT) for the FTP
 * transfers of this tool. Peers behind NAT, a container or a firewall cannot be
 * dialled back by the file server, so passive is what a cross-network channel
 * needs; on a LAN the default is fine. Takes effect on the next transfer.
 */
void ncl_server_file_tool_set_passive(ncl_server_file_tool *tool,
                                      bool passive);

/** True when the tool transfers in passive mode. */
bool ncl_server_file_tool_is_passive(const ncl_server_file_tool *tool);

/**
 * Bytes this tool has put on / taken off the wire so far (data connections
 * only). A resumed transfer sends only the remainder, so these counters are
 * what tells a resumed transfer from a full one - and what a throughput report
 * quotes.
 */
long long ncl_server_file_tool_bytes_sent(const ncl_server_file_tool *tool);
long long ncl_server_file_tool_bytes_received(const ncl_server_file_tool *tool);

/** Validate the link with NOOP first, reconnecting when it is gone. */
bool ncl_server_file_tool_detect(ncl_server_file_tool *tool);

/** Log out and drop the control connection. */
void ncl_server_file_tool_disconnect(ncl_server_file_tool *tool);

/** Upload @p local_path into "/<sn><remoteDir>/". */
bool ncl_server_file_tool_write(ncl_server_file_tool *tool,
                                const char *local_path, const char *remote_dir);

/**
 * Download "/<sn>/<remoteFilePath>" to <root>/uploadFile/<remoteFilePath> and
 * return that library owned path (NULL on failure). Release it with
 * ncl_free_safe().
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

/**
 * Start the process wide FTP server that receives the files a device pushes.
 *
 * The default (ncl_client_holder_start_ftp) listens on port 2323, is rooted at
 * ncl_env_root() and accepts admin / 123456. The _ex form overrides any of it
 * (port 0 / NULL keep the default) — use it when 2323 is taken or the peer
 * should connect somewhere else. Safe to call twice: an already running
 * endpoint is returned as is.
 */
ncl_err ncl_client_holder_start_ftp_ex(unsigned port, const char *root,
                                      const char *user, const char *password);

ncl_err ncl_client_holder_start_ftp(void);

/** Stop the process wide FTP server. */
void ncl_client_holder_stop_ftp(void);

/**
 * The process wide FTP endpoint (borrowed, NULL when it is not running). Handy
 * for monitoring: ncl_ftp_server_bytes_sent/received() then report what the
 * file channel of this process has moved.
 */
ncl_ftp_server *ncl_client_holder_ftp_endpoint(void);

/**
 * Rebuild the process wide client from conf/mqtt.cfg: the running connection is
 * dropped and a new one is established with the values now on disk. Use this
 * after changing the broker at runtime. Returns NCL_OK when a connection is
 * re-established.
 */
ncl_err ncl_client_holder_restart(void);

/** Broker URL the process wide client was initialised with; NULL before
 *  ncl_client_holder_init*() succeeds. */
const char *ncl_client_holder_server_uri(void);

/* ========================================= client side file channel ======= */

/**
 * Settings of the client side FTP endpoint and of the channel it advertises,
 * read from `<root>/conf/ftp.txt`.
 *
 * Everything in it is optional, and the file itself is: a missing file, a
 * missing key or an empty value leaves that setting at its default, so a stale
 * or half-written file can never make the library advertise a broken endpoint.
 * It is read on every ncl_client_open_file_channel() and every
 * ncl_client_holder_start_ftp*(), so an edit takes effect without a restart.
 *
 * ```json
 * { "host": "10.0.0.7",      // address a device dials; absent = derived from the broker
 *   "port": 2323,            // endpoint port; absent/0 = NCL_FTP_CLIENT_HOLDER_PORT
 *   "advertisePort": 4023,   // port handed to the device; absent/0 = "port" (port mapping)
 *   "root": "D:/share",      // endpoint login root; absent = ncl_env_root()
 *   "userName": "nclink",    // endpoint account, also handed to the device;
 *   "password": "secret",    //   absent = the library mints a per-channel account
 *   "path": "V200583BC87",   // remote prefix the device works under; absent = its own SN
 *   "passive": true,         // device transfers in PASV mode (peer behind NAT)
 *   "force": true }          // replace a channel that another peer holds
 * ```
 *
 * Callers override any of it through the ncl_file_channel_options they pass;
 * the order is options > conf/ftp.txt > derived defaults.
 */
typedef struct {
    unsigned port;           /**< endpoint port; 0 = the holder default (2323)   */
    unsigned advertise_port; /**< port handed to the device; 0 = @p port          */
    char    *root;           /**< endpoint login root; NULL = ncl_env_root()      */
    char    *user;           /**< endpoint account / login handed to the device   */
    char    *password;       /**< password of @p user; never NULL when user is    */
    char    *host;           /**< address the device dials; NULL = derive it      */
    char    *path;           /**< remote prefix; NULL = the device's own SN       */
    bool     force;          /**< replace a channel that is already open          */
    bool     passive;        /**< ask the device for passive-mode transfers       */
} ncl_file_channel_config;

/**
 * Read `conf/ftp.txt` into @p out (memset first, then the file's values).
 * Always leaves @p out usable: an absent or unparseable file yields the
 * defaults and a warning, not an error. Release the strings with
 * ncl_file_channel_config_free().
 */
ncl_err ncl_file_channel_config_read(ncl_file_channel_config *out);

/** Write @p config to `conf/ftp.txt` (keys with a value only). */
ncl_err ncl_file_channel_config_write(const ncl_file_channel_config *config);

/** Release the strings of @p config and zero it. */
void ncl_file_channel_config_free(ncl_file_channel_config *config);

/**
 * Where a channel tells the device to dial (file/openFileChannel).
 *
 * Every field is optional: NULL / 0 / false keep the default, so
 * ncl_client_open_file_channel(client, NULL) is the whole story for a device
 * that can reach this machine. The library then advertises the process wide FTP
 * endpoint and mints a login of its own for the channel, so a revoked channel
 * cannot be used to touch the other peers of the endpoint.
 */
typedef struct {
    /**
     * Address the device dials. NULL asks the routing table for the local
     * address that reaches the broker (ncl_socket_local_ip_toward()); when that
     * is a loopback address (broker on this machine) the first non-loopback
     * IPv4 of the machine is used instead, and 127.0.0.1 is the last resort.
     * Set it explicitly when the device reaches this machine through another
     * address or a port mapping.
     */
    const char *host;
    /** Port on @p host; 0 = the process wide FTP endpoint's port. */
    unsigned    port;
    /**
     * Login handed to the device. NULL = the library adds an account to the
     * endpoint (random password) and revokes it when the channel closes. Set it
     * together with @p password to advertise an FTP server of your own.
     */
    const char *user;
    /** Password for @p user; NULL = generated with the account. */
    const char *password;
    /**
     * Lease name. NULL = derived from the serial number and the process id.
     * Opening twice with the same name reuses the channel (the device keeps the
     * endpoint it already has) instead of replacing it.
     */
    const char *channel_id;
    /** Remote prefix the device works under; NULL = the device's own SN. */
    const char *path;
    /**
     * Replace a channel that another lease holds. False is the polite default:
     * the device refuses the handshake instead of dropping a peer that may be
     * transferring right now.
     */
    bool        force;
    /**
     * Ask the device to transfer in passive mode (PASV). A controller behind
     * NAT, in a container or behind a firewall cannot be dialled back by the
     * device's file server, so its channels need this; on a LAN the active
     * default is fine.
     */
    bool        passive;
    /** Method call timeout; 0 = NCL_CLIENT_OPERATION_TIMEOUT. */
    unsigned    timeout_ms;
} ncl_file_channel_options;

/** Zero @p options and install the documented defaults. */
void ncl_file_channel_options_default(ncl_file_channel_options *options);

/**
 * Open (or refresh) the file channel of @p client: make sure the process wide
 * FTP endpoint is running, then hand the device its endpoint with
 * file/openFileChannel. @p options may be NULL (every default).
 *
 * The channel is a lease on the device: it stays until
 * ncl_client_close_file_channel(), another peer replaces it (force), or the
 * device restarts. Returns NCL_ERR_NO_CHANNEL when the device refuses the
 * handshake (the log carries its reason).
 */
ncl_err ncl_client_open_file_channel(ncl_client *client,
                                     const ncl_file_channel_options *options);

/**
 * Drop the channel of @p client: file/closeFileChannel, then revoke the login
 * the library minted for it (which closes the device's live FTP session).
 * Idempotent: no channel is not an error. The process wide FTP endpoint itself
 * stays up until ncl_client_holder_stop_ftp() / shutdown.
 */
ncl_err ncl_client_close_file_channel(ncl_client *client);

/** True when @p client holds a file channel. */
bool ncl_client_file_channel_is_open(ncl_client *client);

/** Copy the lease name of the channel into @p out; false when there is none. */
bool ncl_client_file_channel_id(ncl_client *client, char *out, size_t out_len);

/* ================================================= server integration ==== */

/**
 * Register the built in "file" tool on @p server.
 *
 * The tool exposes write / read / ll / mkdir / delete plus the channel
 * handshake (file/openFileChannel, file/closeFileChannel). Registering it does
 * not open a channel and does not start an FTP endpoint: until a peer opens a
 * channel (or ncl_server_set_file_peer() names one), the file methods answer
 * NCL_ERR_NO_CHANNEL.
 */
ncl_err ncl_server_register_file_tool(ncl_server *server);

/**
 * Point the device at a fixed FTP endpoint (the "static peer"), for hosts that
 * do not want the channel handshake.
 *
 * This is the manual alternative to file/openFileChannel: the peer must be
 * running an FTP server of its own (ncl_client_holder_start_ftp*()) and its
 * layout has to match "/<sn>/...". Nothing is derived from conf/mqtt.cfg — that
 * implicit path was removed in 3.4.0. host is required; port 0, user or
 * password NULL keep the defaults. It replaces the FTP connection that is
 * already open, and a later openFileChannel takes precedence over it.
 */
ncl_err ncl_server_set_file_peer(ncl_server *server, const char *host, unsigned port,
                                 const char *user, const char *password);

/** True when a channel is open (handshake) or a static peer is configured. */
bool ncl_server_file_channel_is_open(ncl_server *server);

/** Copy the open channel's id into @p out; false when no channel is open. */
bool ncl_server_file_channel_id(ncl_server *server, char *out, size_t out_len);

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
