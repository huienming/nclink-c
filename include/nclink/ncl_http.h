/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - HTTP/1.1 server foundation.
 *
 * Scope: connection handling, request parsing (request line, query string,
 * headers, body) and response writing, plus a route table that dispatches on
 * "exact path + method" for the /api/... endpoints.
 *
 * Connections are served one at a time on the accept thread with socket
 * timeouts, and every response closes the connection (Connection: close). That
 * is sufficient for the configuration style API and avoids a per-connection
 * thread; a thread-per-connection variant can be swapped in behind the same API.
 */
#ifndef NCL_HTTP_H
#define NCL_HTTP_H

#include <stdbool.h>

#include "nclink/ncl_common.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_socket.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ncl_http_server  ncl_http_server;
typedef struct ncl_http_request ncl_http_request;
typedef struct ncl_http_response ncl_http_response;

/** Route handler; fills @p response (a default 200 with no body is pre-set). */
typedef void (*ncl_http_handler)(ncl_http_request *request,
                                 ncl_http_response *response, void *user);

/** Common status codes. */
#define NCL_HTTP_OK 200
#define NCL_HTTP_BAD_REQUEST 400
#define NCL_HTTP_NOT_FOUND 404
#define NCL_HTTP_METHOD_NOT_ALLOWED 405
#define NCL_HTTP_PAYLOAD_TOO_LARGE 413
#define NCL_HTTP_INTERNAL_ERROR 500
#define NCL_HTTP_NOT_IMPLEMENTED 501

/* ---------------------------------------------------------------- server -- */

/** Create a server bound to @p port once started (0 = ephemeral). */
ncl_http_server *ncl_http_server_create(unsigned port);
void             ncl_http_server_free(ncl_http_server *server);

/**
 * Register a handler for an exact path. @p method may be NULL or "*" to match
 * any method. The handler runs on the accept thread.
 *
 * A path ending in "/\*" registers a prefix route instead: "/api/\*" accepts
 * every path that starts with "/api/". Exact routes always win over prefix
 * routes, no matter which was registered first.
 */
ncl_err ncl_http_server_route(ncl_http_server *server, const char *method,
                              const char *path, ncl_http_handler handler,
                              void *user);

/** Bind and start serving. Returns NCL_OK once the listener is ready. */
ncl_err ncl_http_server_start(ncl_http_server *server);

/** Shut the listener down and join the accept thread. */
void ncl_http_server_stop(ncl_http_server *server);

/** Port actually bound (useful when creating with port 0). */
unsigned ncl_http_server_port(const ncl_http_server *server);

/** Number of requests handled so far. */
size_t ncl_http_server_request_count(const ncl_http_server *server);

/** Enable or disable the Access-Control-Allow-Origin: * header (default on). */
void ncl_http_server_set_cors(ncl_http_server *server, bool enabled);

/* --------------------------------------------------------------- request -- */

const char *ncl_http_method(const ncl_http_request *request);
/** Path without the query string. */
const char *ncl_http_path(const ncl_http_request *request);
/** Raw query string (without '?'), or "". */
const char *ncl_http_query_string(const ncl_http_request *request);
/** Value of a query parameter, or NULL. */
const char *ncl_http_query(const ncl_http_request *request, const char *name);
/** Header lookup, case insensitive. */
const char *ncl_http_header(const ncl_http_request *request, const char *name);
const char *ncl_http_body(const ncl_http_request *request);
size_t      ncl_http_body_len(const ncl_http_request *request);
/**
 * Parse the body as JSON. Returns a new document the caller frees, or NULL when
 * the body is empty or malformed.
 */
ncl_json   *ncl_http_json_body(const ncl_http_request *request);
/** Value of a form field in an application/x-www-form-urlencoded body. */
const char *ncl_http_form_field(const ncl_http_request *request, const char *name);

/* -------------------------------------------------------------- response -- */

void ncl_http_set_status(ncl_http_response *response, int status);
void ncl_http_set_header(ncl_http_response *response, const char *name,
                         const char *value);

/** Set the body. The content type defaults to text/plain; charset=utf-8. */
void ncl_http_reply(ncl_http_response *response, int status,
                    const char *content_type, const char *body, size_t body_len);
void ncl_http_reply_text(ncl_http_response *response, int status,
                         const char *text);
void ncl_http_reply_json(ncl_http_response *response, int status,
                         const ncl_json *json);

/* ------------------------------------------------------------- utilities -- */

/** Reason phrase for a status code. */
const char *ncl_http_status_text(int status);

/**
 * URL-decode @p value into a heap string ('+' becomes a space). Used for query
 * parameters and form fields.
 */
char *ncl_http_url_decode(const char *value);

/** Percent-encode @p value for use in a query string. */
char *ncl_http_url_encode(const char *value);

#ifdef __cplusplus
}
#endif

#endif /* NCL_HTTP_H */
