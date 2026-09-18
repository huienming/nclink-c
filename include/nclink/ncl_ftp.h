/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - FTP transport.
 *
 * NC-Link moves bulk data over FTP and needs both halves of the protocol, so
 * this module implements client and server against the subset of RFC 959/2389
 * that the protocol uses:
 *
 *   client - USER PASS ACCT SYST FEAT OPTS PWD CWD CDUP TYPE MODE STRU PORT
 *            EPRT PASV EPSV LIST NLST RETR STOR APPE DELE RMD MKD RNFR RNTO
 *            SIZE MDTM REST ABOR NOOP STAT ALLO HELP QUIT
 *   server - the same command set, backing onto the local file system with the
 *            configured directory as the login root.
 *
 * A session is connect, login, binary TYPE and a transfer mode; every transfer
 * then goes through PORT/PASV + STOR/RETR/LIST.
 */
#ifndef NCL_FTP_H
#define NCL_FTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default credentials and ports of the NC-Link FTP endpoints. */
#define NCL_FTP_DEFAULT_USER       "admin"
#define NCL_FTP_DEFAULT_PASSWORD   "123456"
#define NCL_FTP_SERVER_PORT        2121
#define NCL_FTP_CLIENT_HOLDER_PORT 2323

/** Default control connection (TCP connect) timeout. */
#define NCL_FTP_CONNECT_TIMEOUT_MS 5000
/** Default data connection and read timeout (setDataTimeout/setDefaultTimeout). */
#define NCL_FTP_DATA_TIMEOUT_MS    30000

/* ============================================================ file entry == */

/**
 * One directory entry as reported by LIST (name, type, size, timestamp).
 */
typedef struct {
    char     *name;        /**< entry name, never a full path */
    int       type;        /**< 0 = file, 1 = directory                     */
    long long size;        /**< size in bytes (0 for directories)            */
    int64_t   modify_time; /**< epoch milliseconds, 0 when the peer was vague */
} ncl_ftp_entry;

/** Release an entry allocated by ncl_ftp_client_list(). */
void ncl_ftp_entry_free(void *entry);

/** True when @p entry is a directory. */
bool ncl_ftp_entry_is_dir(const ncl_ftp_entry *entry);

/* ============================================================== client ==== */

typedef struct ncl_ftp_client ncl_ftp_client;

typedef struct {
    /** TCP connect timeout; 0 picks NCL_FTP_CONNECT_TIMEOUT_MS. */
    unsigned connect_timeout_ms;
    /** Data connection and command read timeout; 0 picks
     *  NCL_FTP_DATA_TIMEOUT_MS. */
    unsigned io_timeout_ms;
    /**
     * Use passive transfers. The default (false) is active mode, which is what
     * the NC-Link file channel uses.
     */
    bool passive;
} ncl_ftp_client_options;

/** Create a client with the defaults (active mode, 5 s / 30 s timeouts). */
ncl_ftp_client *ncl_ftp_client_create(const char *host, unsigned port,
                                      const char *user, const char *password);

ncl_ftp_client *ncl_ftp_client_create_ex(const char *host, unsigned port,
                                         const char *user, const char *password,
                                         const ncl_ftp_client_options *options);

void ncl_ftp_client_free(ncl_ftp_client *client);

/** Host the client was created for. */
const char *ncl_ftp_client_host(const ncl_ftp_client *client);
unsigned    ncl_ftp_client_port(const ncl_ftp_client *client);

/**
 * Validate the current connection with NOOP, or (re)connect, log in, switch to
 * binary and select the transfer mode.
 * Returns true when the session is usable.
 */
bool ncl_ftp_client_detect(ncl_ftp_client *client);

/** Log out and drop the control connection. */
void ncl_ftp_client_disconnect(ncl_ftp_client *client);

/** True when a control connection is open (no NOOP is sent). */
bool ncl_ftp_client_is_connected(const ncl_ftp_client *client);

/** Send NOOP over the control connection. */
bool ncl_ftp_client_noop(ncl_ftp_client *client);

/** Last reply code seen on the control connection (0 before the first reply). */
int ncl_ftp_client_reply_code(const ncl_ftp_client *client);

/** Last reply text, or "" when there is none. */
const char *ncl_ftp_client_reply_text(const ncl_ftp_client *client);

/** Switch between active (PORT/EPRT) and passive (PASV/EPSV) transfers. */
void ncl_ftp_client_set_passive(ncl_ftp_client *client, bool passive);
bool ncl_ftp_client_is_passive(const ncl_ftp_client *client);

/* Directory and meta operations ------------------------------------------- */

ncl_err ncl_ftp_client_pwd(ncl_ftp_client *client, char *buf, size_t buf_len);
ncl_err ncl_ftp_client_chdir(ncl_ftp_client *client, const char *path);
ncl_err ncl_ftp_client_mkdir(ncl_ftp_client *client, const char *path);
ncl_err ncl_ftp_client_rmdir(ncl_ftp_client *client, const char *path);
ncl_err ncl_ftp_client_delete(ncl_ftp_client *client, const char *path);
ncl_err ncl_ftp_client_rename(ncl_ftp_client *client, const char *from,
                              const char *to);

/** SIZE. Returns NCL_ERR_NOT_SUPPORTED when the server refuses (text mode). */
ncl_err ncl_ftp_client_size(ncl_ftp_client *client, const char *path,
                            long long *out_size);

/** MDTM. *out_time receives epoch milliseconds (UTC). */
ncl_err ncl_ftp_client_mdtm(ncl_ftp_client *client, const char *path,
                            int64_t *out_time);

/** True when @p path names a directory on the server (CWD probe). */
bool ncl_ftp_client_is_dir(ncl_ftp_client *client, const char *path);

/* Transfers --------------------------------------------------------------- */

/** STOR: upload @p len bytes to @p remote. */
ncl_err ncl_ftp_client_store(ncl_ftp_client *client, const char *remote,
                             const void *data, size_t len);

/** Upload @p local_path with STOR as @p remote_name. */
ncl_err ncl_ftp_client_store_file(ncl_ftp_client *client, const char *remote,
                                  const char *local_path);

/**
 * RETR into @p out. The buffer is reset first; use ncl_ftp_client_retrieve_file
 * for large payloads.
 */
ncl_err ncl_ftp_client_retrieve(ncl_ftp_client *client, const char *remote,
                                ncl_strbuf *out);

/** Download @p remote_path with RETR into @p local_path. */
ncl_err ncl_ftp_client_retrieve_file(ncl_ftp_client *client, const char *remote,
                                     const char *local_path);

/**
 * LIST @p path into @p out, an ncl_ptrvec of ncl_ftp_entry*.
 * Initialise it with ncl_ptrvec_init(out, ncl_ftp_entry_free).
 * Reading a directory yields its children; reading a plain file yields a single
 * entry.
 */
ncl_err ncl_ftp_client_list(ncl_ftp_client *client, const char *path,
                            ncl_ptrvec *out);

/** NLST @p path: the entry names only, appended to @p out. */
ncl_err ncl_ftp_client_nlst(ncl_ftp_client *client, const char *path,
                            ncl_strvec *out);

/* ============================================================== server ==== */

typedef struct ncl_ftp_server ncl_ftp_server;

/**
 * One login of the FTP server.
 *
 * A server always accepts the account it was created with
 * (ncl_ftp_server_options.user / password / root) plus every account added
 * with ncl_ftp_server_add_account(), plus "anonymous" (which is served with the
 * server defaults, as before). Accounts exist so that a file channel can hand
 * the peer a credential of its own: revoking that one account does not touch
 * the other peers logged in to the same endpoint.
 */
typedef struct {
    const char *user;        /**< login name (required)                      */
    const char *password;    /**< password (required, may be empty)          */
    const char *root;        /**< login root; NULL = the root of the server  */
    bool        allow_write; /**< STOR/APPE/DELE/RMD/MKD for this account    */
} ncl_ftp_account;

typedef struct {
    /** Listening port. ncl_ftp_server_create() defaults to
     *  NCL_FTP_SERVER_PORT; pass 0 to let the OS pick one, then read it back
     *  with ncl_ftp_server_port(). */
    unsigned    port;
    /** Login root. NULL means ncl_env_root(). */
    const char *root;
    /** NULL means NCL_FTP_DEFAULT_USER. */
    const char *user;
    /** NULL means NCL_FTP_DEFAULT_PASSWORD. */
    const char *password;
    /** Accept STOR/APPE/DELE/RMD/MKD. When false the peer may only read. */
    bool        allow_write;
    /** Idle timeout for a control connection before it is closed. */
    unsigned    idle_timeout_ms;
} ncl_ftp_server_options;

/** Create a server with the defaults. */
ncl_ftp_server *ncl_ftp_server_create(void);

ncl_ftp_server *ncl_ftp_server_create_ex(const ncl_ftp_server_options *options);

/**
 * Stop the server if it is running and release it. The root directory is never
 * deleted.
 */
void ncl_ftp_server_free(ncl_ftp_server *server);

/** Bind and start accepting sessions. Returns NCL_ERR_CONNECT when the port is
 *  taken or binding fails. */
ncl_err ncl_ftp_server_start(ncl_ftp_server *server);

/**
 * Register @p account (or update it in place when the user name is already
 * taken). The strings are copied. Sessions that are already logged in keep the
 * settings they logged in with; the next login sees the new ones.
 */
ncl_err ncl_ftp_server_add_account(ncl_ftp_server *server,
                                   const ncl_ftp_account *account);

/**
 * Drop @p user and close its live sessions. The account the server was created
 * with and "anonymous" cannot be removed.
 */
ncl_err ncl_ftp_server_remove_account(ncl_ftp_server *server, const char *user);

/** Number of accounts added with ncl_ftp_server_add_account(). */
size_t ncl_ftp_server_account_count(ncl_ftp_server *server);

/** Stop accepting, close every session and join all threads. Idempotent. */
void ncl_ftp_server_stop(ncl_ftp_server *server);

bool     ncl_ftp_server_is_running(ncl_ftp_server *server);
unsigned ncl_ftp_server_port(const ncl_ftp_server *server);
size_t   ncl_ftp_server_session_count(ncl_ftp_server *server);
long long ncl_ftp_server_command_count(ncl_ftp_server *server);

/** Login root, as an absolute path. */
const char *ncl_ftp_server_root(const ncl_ftp_server *server);

#ifdef __cplusplus
}
#endif

#endif /* NCL_FTP_H */
