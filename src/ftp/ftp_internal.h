/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - shared FTP helpers used by the client and the server.
 * Not installed; meant for the sources under src/ftp only.
 */
#ifndef NCL_FTP_INTERNAL_H
#define NCL_FTP_INTERNAL_H

#include <time.h>

#include "nclink/ncl_common.h"
#include "nclink/ncl_ftp.h"
#include "nclink/ncl_socket.h"

#define NCL_FTP_PATH_MAX 4096
#define NCL_FTP_LINE_MAX 4096

/* --------------------------------------------------------------- reading -- */

/** Line buffered reader over a socket, shared by both roles. */
typedef struct {
    ncl_socket *sock;
    char        buf[NCL_FTP_LINE_MAX];
    size_t      len;
    size_t      pos;
} ncl_ftp_reader;

void ncl_ftp_reader_init(ncl_ftp_reader *reader, ncl_socket *sock);

/**
 * Read one CRLF (or bare LF) terminated line into @p out, which is reset first.
 * Returns NCL_OK, NCL_ERR_TIMEOUT, or NCL_ERR_IO on EOF/error.
 */
ncl_err ncl_ftp_reader_line(ncl_ftp_reader *reader, ncl_strbuf *out,
                            unsigned timeout_ms);

/**
 * Read a complete FTP reply. Multi line replies ("123-text" ... "123 end") are
 * coalesced into @p text, one line per entry.
 */
ncl_err ncl_ftp_reader_reply(ncl_ftp_reader *reader, int *code,
                             ncl_strbuf *text, unsigned timeout_ms);

/** Append "line\r\n" to the socket. */
ncl_err ncl_ftp_send_line(ncl_socket *sock, const char *line);

/** printf a command line: "@p fmt" followed by CRLF. */
ncl_err ncl_ftp_send_command(ncl_socket *sock, const char *fmt, ...);

/* --------------------------------------------------------------- replies -- */

/** Send "<code> <text>\r\n". */
ncl_err ncl_ftp_reply(ncl_socket *sock, int code, const char *text);
ncl_err ncl_ftp_replyf(ncl_socket *sock, int code, const char *fmt, ...);

/**
 * Send a multiline reply: "<code>-line\r\n" for each @p text line (split on
 * '\n') followed by "<code> <last line>\r\n". Text without '\n' becomes the
 * single terminating line.
 */
ncl_err ncl_ftp_reply_multiline(ncl_socket *sock, int code, const char *text);

/** Code of a reply as an int, -1 when malformed. */
int ncl_ftp_code_of(const char *line);

/** True for 2xx/3xx (FTPReply.isPositiveCompletion/isPositiveIntermediate). */
bool ncl_ftp_reply_is_completion(int code);
bool ncl_ftp_reply_is_intermediate(int code);
/** Mirrors FTPReply.isPositivePreliminary (1xx). */
bool ncl_ftp_reply_is_preliminary(int code);

/** "h1,h2,h3,h4,p1,p2" for @p host:@p port, e.g. "227 ... (127,0,0,1,4,1)". */
ncl_err ncl_ftp_format_port_arg(char *out, size_t out_len, const char *host,
                                unsigned port);

/** Parse the "(h1,h2,h3,h4,p1,p2)" part of a 227 reply. */
ncl_err ncl_ftp_parse_pasv_reply(const char *text, char *host, size_t host_len,
                                 unsigned *port);

/* -------------------------------------------------------------- listings -- */

/** Build one "ls -l" style line. */
ncl_err ncl_ftp_format_list_line(ncl_strbuf *out, const char *name, bool is_dir,
                                 long long size, int64_t mtime_ms);

/**
 * Parse an "ls -l" line into an entry. @p out->name receives a heap copy and
 * must be released with ncl_ftp_entry_free(). Returns NCL_ERR_PARSE for lines
 * that carry no name (e.g. "total 4").
 */
ncl_err ncl_ftp_parse_list_line(const char *line, ncl_ftp_entry *out);

/* --------------------------------------------------------------- time ----- */

/** "YYYYMMDDHHMMSS" (UTC) -> epoch milliseconds, 0 when unparsable. */
int64_t ncl_ftp_parse_mdtm(const char *text);

/** epoch milliseconds -> "YYYYMMDDHHMMSS" (UTC). */
void ncl_ftp_format_mdtm(int64_t ms, char *out, size_t out_len);

/** Epoch milliseconds -> local calendar time. */
void ncl_ftp_localtime(int64_t ms, struct tm *out);

/** Local broken-down time -> epoch milliseconds (0 on failure). */
int64_t ncl_ftp_mktime_local(const struct tm *tm);

/** Civil date/time in UTC -> Unix seconds, or -1 when the fields are invalid. */
int64_t ncl_ftp_utc_to_epoch(int year, int month, int day, int hour, int minute,
                             int second);

/* ---------------------------------------------------------------- paths --- */

/** Path of @p name inside @p dir, using '/' as the separator. */
ncl_err ncl_ftp_path_join(char *out, size_t out_len, const char *dir,
                          const char *name);

/**
 * Normalise @p path against @p cwd into an absolute virtual FTP path: leading
 * '/' added, "." dropped, ".." resolved. Returns NCL_ERR_INVALID_ARG when the
 * result would escape the root.
 */
ncl_err ncl_ftp_path_normalise(const char *cwd, const char *path, char *out,
                               size_t out_len);

#endif /* NCL_FTP_INTERNAL_H */
