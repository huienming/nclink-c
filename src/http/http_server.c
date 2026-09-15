/* NC-Link core - HTTP/1.1 server foundation. */
#include "nclink/ncl_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_thread.h"

#define NCL_HTTP_MAX_HEADER_BYTES 16384
#define NCL_HTTP_MAX_BODY_BYTES (4 * 1024 * 1024)
#define NCL_HTTP_MAX_HEADERS 32
#define NCL_HTTP_MAX_ROUTES 64
#define NCL_HTTP_READ_TIMEOUT_MS 5000

typedef struct {
    char *name;
    char *value;
} ncl_http_kv;

struct ncl_http_request {
    char  *method;
    char  *target;      /**< path + optional query, as sent */
    char  *path;
    char  *query;
    ncl_http_kv headers[NCL_HTTP_MAX_HEADERS];
    size_t header_count;
    char  *body;
    size_t body_len;
    /* Lazily decoded values, owned by the request. */
    ncl_ptrvec decoded; /**< char* owned */
};

struct ncl_http_response {
    int          status;
    ncl_http_kv  headers[NCL_HTTP_MAX_HEADERS];
    size_t       header_count;
    char        *body;
    size_t       body_len;
    bool         body_owned;
};

typedef struct {
    char            *method; /**< NULL or "*" matches any */
    char            *path;   /**< exact path, or the prefix of a catch-all */
    size_t           prefix_len; /**< non-zero for a catch-all route */
    ncl_http_handler handler;
    void            *user;
} ncl_http_route;

struct ncl_http_server {
    unsigned        port;
    ncl_socket     *listener;
    ncl_thread     *thread;
    volatile bool   running;
    volatile bool   stopping;
    ncl_http_route  routes[NCL_HTTP_MAX_ROUTES];
    size_t          route_count;
    ncl_mutex      *mutex;
    size_t          request_count;
    bool            cors;
};

/* =============================================================== helpers == */

const char *ncl_http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

char *ncl_http_url_decode(const char *value)
{
    ncl_strbuf out;
    size_t i;

    if (value == NULL) {
        return NULL;
    }
    ncl_strbuf_init(&out);
    for (i = 0; value[i] != '\0'; i++) {
        if (value[i] == '+') {
            ncl_strbuf_putc(&out, ' ');
        } else if (value[i] == '%' && value[i + 1] != '\0' &&
                   value[i + 2] != '\0') {
            int hi = 0;
            int lo = 0;
            int j;
            for (j = 0; j < 2; j++) {
                char c = value[i + 1 + (size_t)j];
                int digit;
                if (c >= '0' && c <= '9') {
                    digit = c - '0';
                } else if (c >= 'a' && c <= 'f') {
                    digit = c - 'a' + 10;
                } else if (c >= 'A' && c <= 'F') {
                    digit = c - 'A' + 10;
                } else {
                    digit = -1;
                }
                if (digit < 0) {
                    break;
                }
                if (j == 0) {
                    hi = digit;
                } else {
                    lo = digit;
                }
            }
            if (j == 2) {
                ncl_strbuf_putc(&out, (char)((hi << 4) | lo));
                i += 2;
            } else {
                ncl_strbuf_putc(&out, value[i]);
            }
        } else {
            ncl_strbuf_putc(&out, value[i]);
        }
    }
    return ncl_strbuf_detach(&out);
}

char *ncl_http_url_encode(const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    ncl_strbuf out;
    size_t i;

    if (value == NULL) {
        return NULL;
    }
    ncl_strbuf_init(&out);
    for (i = 0; value[i] != '\0'; i++) {
        unsigned char c = (unsigned char)value[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            ncl_strbuf_putc(&out, (char)c);
        } else if (c == ' ') {
            ncl_strbuf_putc(&out, '+');
        } else {
            ncl_strbuf_putc(&out, '%');
            ncl_strbuf_putc(&out, hex[(c >> 4) & 0x0F]);
            ncl_strbuf_putc(&out, hex[c & 0x0F]);
        }
    }
    return ncl_strbuf_detach(&out);
}

/** Split "a=1&b=2" and return the decoded value of @p name. */
static char *ncl_http_param_lookup(const char *query, const char *name)
{
    size_t name_len;
    const char *cursor;

    if (query == NULL || name == NULL) {
        return NULL;
    }
    name_len = strlen(name);
    cursor = query;
    while (*cursor != '\0') {
        const char *pair_end = strchr(cursor, '&');
        const char *eq;
        if (pair_end == NULL) {
            pair_end = cursor + strlen(cursor);
        }
        eq = memchr(cursor, '=', (size_t)(pair_end - cursor));
        {
            size_t key_len = eq != NULL ? (size_t)(eq - cursor)
                                        : (size_t)(pair_end - cursor);
            if (key_len == name_len && strncmp(cursor, name, name_len) == 0) {
                const char *value = eq != NULL ? eq + 1 : pair_end;
                char raw[1024];
                size_t len = (size_t)(pair_end - value);
                if (len >= sizeof(raw)) {
                    len = sizeof(raw) - 1;
                }
                memcpy(raw, value, len);
                raw[len] = '\0';
                return ncl_http_url_decode(raw);
            }
        }
        if (*pair_end == '\0') {
            break;
        }
        cursor = pair_end + 1;
    }
    return NULL;
}

/* =============================================================== request == */

const char *ncl_http_method(const ncl_http_request *request)
{
    return request != NULL ? request->method : NULL;
}

const char *ncl_http_path(const ncl_http_request *request)
{
    return request != NULL ? request->path : NULL;
}

const char *ncl_http_query_string(const ncl_http_request *request)
{
    return request != NULL && request->query != NULL ? request->query : "";
}

const char *ncl_http_header(const ncl_http_request *request, const char *name)
{
    size_t i;
    if (request == NULL || name == NULL) {
        return NULL;
    }
    for (i = 0; i < request->header_count; i++) {
        if (ncl_streq_ignore_case(request->headers[i].name, name)) {
            return request->headers[i].value;
        }
    }
    return NULL;
}

const char *ncl_http_body(const ncl_http_request *request)
{
    return request != NULL ? request->body : NULL;
}

size_t ncl_http_body_len(const ncl_http_request *request)
{
    return request != NULL ? request->body_len : 0;
}

ncl_json *ncl_http_json_body(const ncl_http_request *request)
{
    if (request == NULL || request->body == NULL || request->body_len == 0) {
        return NULL;
    }
    return ncl_json_parse(request->body, request->body_len, NULL);
}

const char *ncl_http_form_field(const ncl_http_request *request, const char *name)
{
    char *value;
    /* The decoded-value cache is mutable bookkeeping behind a logically const
     * request, so the cast is intentional. */
    ncl_http_request *mutable_request = (ncl_http_request *)request;
    if (request == NULL || request->body == NULL || name == NULL) {
        return NULL;
    }
    value = ncl_http_param_lookup(request->body, name);
    if (value == NULL) {
        return NULL;
    }
    /* Keep the decoded string alive for the lifetime of the request. */
    if (ncl_ptrvec_push_owned(&mutable_request->decoded, value) != NCL_OK) {
        free(value);
        return NULL;
    }
    return value;
}

const char *ncl_http_query(const ncl_http_request *request, const char *name)
{
    char *value;
    ncl_http_request *mutable_request = (ncl_http_request *)request;
    if (request == NULL || request->query == NULL || name == NULL) {
        return NULL;
    }
    value = ncl_http_param_lookup(request->query, name);
    if (value == NULL) {
        return NULL;
    }
    if (ncl_ptrvec_push_owned(&mutable_request->decoded, value) != NCL_OK) {
        free(value);
        return NULL;
    }
    return value;
}

static void ncl_http_request_free(ncl_http_request *request)
{
    size_t i;
    if (request == NULL) {
        return;
    }
    free(request->method);
    free(request->target);
    free(request->path);
    free(request->query);
    for (i = 0; i < request->header_count; i++) {
        free(request->headers[i].name);
        free(request->headers[i].value);
    }
    free(request->body);
    ncl_ptrvec_free(&request->decoded);
    free(request);
}

/* ============================================================== response == */

void ncl_http_set_status(ncl_http_response *response, int status)
{
    if (response != NULL) {
        response->status = status;
    }
}

void ncl_http_set_header(ncl_http_response *response, const char *name,
                         const char *value)
{
    size_t i;

    if (response == NULL || name == NULL) {
        return;
    }
    for (i = 0; i < response->header_count; i++) {
        if (ncl_streq_ignore_case(response->headers[i].name, name)) {
            free(response->headers[i].value);
            response->headers[i].value = value != NULL ? ncl_strdup(value) : NULL;
            return;
        }
    }
    if (response->header_count >= NCL_HTTP_MAX_HEADERS) {
        return;
    }
    response->headers[response->header_count].name = ncl_strdup(name);
    response->headers[response->header_count].value =
        value != NULL ? ncl_strdup(value) : NULL;
    response->header_count++;
}

void ncl_http_reply(ncl_http_response *response, int status,
                    const char *content_type, const char *body, size_t body_len)
{
    if (response == NULL) {
        return;
    }
    response->status = status;
    free(response->body);
    response->body = NULL;
    response->body_len = 0;
    response->body_owned = true;
    if (body_len > 0) {
        response->body = (char *)malloc(body_len + 1);
        if (response->body == NULL) {
            response->status = NCL_HTTP_INTERNAL_ERROR;
            return;
        }
        memcpy(response->body, body, body_len);
        response->body[body_len] = '\0';
        response->body_len = body_len;
    }
    ncl_http_set_header(response, "Content-Type",
                        content_type != NULL ? content_type
                                             : "text/plain; charset=utf-8");
}

void ncl_http_reply_text(ncl_http_response *response, int status,
                         const char *text)
{
    ncl_http_reply(response, status, "text/plain; charset=utf-8", text,
                   text != NULL ? strlen(text) : 0);
}

void ncl_http_reply_json(ncl_http_response *response, int status,
                         const ncl_json *json)
{
    char *text = json != NULL ? ncl_json_write_string(json) : NULL;
    if (text == NULL) {
        ncl_http_reply_text(response, NCL_HTTP_INTERNAL_ERROR, "null");
        return;
    }
    ncl_http_reply(response, status, "application/json; charset=utf-8", text,
                   strlen(text));
    free(text);
}

static void ncl_http_response_free(ncl_http_response *response)
{
    size_t i;
    if (response == NULL) {
        return;
    }
    for (i = 0; i < response->header_count; i++) {
        free(response->headers[i].name);
        free(response->headers[i].value);
    }
    free(response->body);
    free(response);
}

/* ================================================================ parsing = */

/** Read the header block; returns the number of bytes consumed (0 on error). */
static size_t ncl_http_read_head(ncl_socket *sock, char *buffer, size_t capacity)
{
    size_t used = 0;

    while (used + 1 < capacity) {
        int rc = ncl_socket_recv(sock, buffer + used, 1, NCL_HTTP_READ_TIMEOUT_MS);
        if (rc <= 0) {
            return 0;
        }
        used++;
        buffer[used] = '\0';
        if (used >= 4 && memcmp(buffer + used - 4, "\r\n\r\n", 4) == 0) {
            return used;
        }
        /* Tolerate bare LF line endings. */
        if (used >= 2 && memcmp(buffer + used - 2, "\n\n", 2) == 0) {
            return used;
        }
    }
    return 0;
}

static void ncl_http_trim(char *text)
{
    size_t len;
    char *start = text;

    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                       text[len - 1] == '\r' || text[len - 1] == '\n')) {
        text[--len] = '\0';
    }
}

/** Next CRLF/LF terminated line; strips the trailing CR. NULL at end of input. */
static char *ncl_http_next_line(char **cursor)
{
    char *start = *cursor;
    char *newline;
    size_t len;

    if (start == NULL || *start == '\0') {
        return NULL;
    }
    newline = strchr(start, '\n');
    if (newline != NULL) {
        *newline = '\0';
        *cursor = newline + 1;
    } else {
        *cursor = start + strlen(start);
    }
    len = strlen(start);
    while (len > 0 && (start[len - 1] == '\r' || start[len - 1] == ' ')) {
        start[--len] = '\0';
    }
    return start;
}

static ncl_http_request *ncl_http_parse_request(char *head, size_t head_len,
                                                ncl_socket *sock)
{
    ncl_http_request *request;
    char *cursor = head;
    char *line;
    char *rest;

    (void)head_len;
    request = (ncl_http_request *)calloc(1, sizeof(ncl_http_request));
    if (request == NULL) {
        return NULL;
    }
    ncl_ptrvec_init(&request->decoded, free);

    /* Request line: METHOD SP TARGET SP VERSION */
    line = ncl_http_next_line(&cursor);
    if (line == NULL) {
        ncl_http_request_free(request);
        return NULL;
    }
    rest = strchr(line, ' ');
    if (rest == NULL) {
        ncl_http_request_free(request);
        return NULL;
    }
    *rest = '\0';
    request->method = ncl_strdup(line);
    rest++;
    while (*rest == ' ') {
        rest++;
    }
    {
        char *space = strchr(rest, ' ');
        if (space != NULL) {
            *space = '\0'; /* drop the HTTP version */
        }
    }
    request->target = ncl_strdup(rest);
    if (request->method == NULL || request->target == NULL) {
        ncl_http_request_free(request);
        return NULL;
    }

    /* Split the target into path and query. */
    {
        char *question = strchr(request->target, '?');
        if (question != NULL) {
            *question = '\0';
            request->query = ncl_strdup(question + 1);
        }
        request->path = ncl_strdup(request->target);
    }

    /* Headers. */
    while ((line = ncl_http_next_line(&cursor)) != NULL) {
        char *colon;

        if (line[0] == '\0') {
            break; /* end of the header block */
        }
        colon = strchr(line, ':');
        if (colon == NULL || request->header_count >= NCL_HTTP_MAX_HEADERS) {
            continue;
        }
        *colon = '\0';
        request->headers[request->header_count].name = ncl_strdup(line);
        request->headers[request->header_count].value = ncl_strdup(colon + 1);
        if (request->headers[request->header_count].value != NULL) {
            ncl_http_trim(request->headers[request->header_count].value);
        }
        request->header_count++;
    }

    /* Body, when the client announced one. */
    {
        const char *length_text = ncl_http_header(request, "Content-Length");
        long long length = length_text != NULL ? strtoll(length_text, NULL, 10) : 0;
        if (length > 0 && length <= NCL_HTTP_MAX_BODY_BYTES) {
            request->body = (char *)malloc((size_t)length + 1);
            if (request->body != NULL &&
                ncl_socket_recv_exact(sock, request->body, (size_t)length,
                                      NCL_HTTP_READ_TIMEOUT_MS) == NCL_OK) {
                request->body[length] = '\0';
                request->body_len = (size_t)length;
            } else {
                free(request->body);
                request->body = NULL;
            }
        }
    }
    return request;
}

/* ============================================================== dispatch == */

static void ncl_http_write_response(ncl_socket *sock, ncl_http_response *response,
                                    bool cors)
{
    ncl_strbuf head;
    size_t i;

    ncl_strbuf_init(&head);
    ncl_strbuf_printf(&head, "HTTP/1.1 %d %s\r\n", response->status,
                      ncl_http_status_text(response->status));
    for (i = 0; i < response->header_count; i++) {
        if (response->headers[i].value != NULL) {
            ncl_strbuf_printf(&head, "%s: %s\r\n", response->headers[i].name,
                              response->headers[i].value);
        }
    }
    if (cors) {
        /* CORS header, on unless the caller turned it off. */
        ncl_strbuf_puts(&head, "Access-Control-Allow-Origin: *\r\n");
        ncl_strbuf_puts(&head,
                        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n");
        ncl_strbuf_puts(&head,
                        "Access-Control-Allow-Headers: Content-Type\r\n");
    }
    ncl_strbuf_printf(&head, "Content-Length: %zu\r\n", response->body_len);
    ncl_strbuf_puts(&head, "Connection: close\r\n\r\n");

    ncl_socket_send(sock, head.data, head.len);
    if (response->body_len > 0) {
        ncl_socket_send(sock, response->body, response->body_len);
    }
    ncl_strbuf_free(&head);
}

static bool ncl_http_method_matches(const ncl_http_route *route,
                                    const char *method)
{
    if (route->method == NULL) {
        return true;
    }
    return ncl_streq_ignore_case(route->method, method);
}

/**
 * How well @p route matches @p path: 2 = exact, 1 = catch-all prefix match,
 * 0 = no match. The most specific route wins, so an application can register
 * both "/api/cfg/getSn" and an "/api" catch-all in any order.
 */
static int ncl_http_path_match(const ncl_http_route *route, const char *path)
{
    if (route->prefix_len > 0) {
        return strncmp(route->path, path, route->prefix_len) == 0 ? 1 : 0;
    }
    return strcmp(route->path, path) == 0 ? 2 : 0;
}

static void ncl_http_handle(ncl_http_server *server, ncl_socket *sock)
{
    char head[NCL_HTTP_MAX_HEADER_BYTES];
    ncl_http_request *request;
    ncl_http_response *response;
    size_t i;
    bool path_exists = false;
    ncl_http_route *best = NULL;
    int best_score = 0;

    if (ncl_http_read_head(sock, head, sizeof(head)) == 0) {
        return;
    }
    request = ncl_http_parse_request(head, 0, sock);
    if (request == NULL) {
        ncl_http_response brief;
        memset(&brief, 0, sizeof(brief));
        brief.status = NCL_HTTP_BAD_REQUEST;
        ncl_http_write_response(sock, &brief, server->cors);
        return;
    }

    response = (ncl_http_response *)calloc(1, sizeof(ncl_http_response));
    if (response == NULL) {
        ncl_http_request_free(request);
        return;
    }
    response->status = NCL_HTTP_OK;
    ncl_http_set_header(response, "Server", "nclink-core-c");

    ncl_mutex_lock(server->mutex);
    server->request_count++;
    ncl_mutex_unlock(server->mutex);

    /* CORS preflight: answer immediately. */
    if (ncl_streq_ignore_case(request->method, "OPTIONS")) {
        ncl_http_reply(response, 204, NULL, NULL, 0);
    } else {
        for (i = 0; i < server->route_count; i++) {
            ncl_http_route *route = &server->routes[i];
            int score = ncl_http_path_match(route, request->path);
            if (score == 0) {
                continue;
            }
            path_exists = true;
            if (ncl_http_method_matches(route, request->method)) {
                if (score > best_score) {
                    best = route;
                    best_score = score;
                }
            }
        }
        if (best != NULL) {
            best->handler(request, response, best->user);
        } else if (path_exists) {
            ncl_http_reply_text(response, NCL_HTTP_METHOD_NOT_ALLOWED,
                                "method not allowed");
        } else {
            ncl_http_reply_text(response, NCL_HTTP_NOT_FOUND, "not found");
        }
    }

    ncl_http_write_response(sock, response, server->cors);
    ncl_http_response_free(response);
    ncl_http_request_free(request);
}

static void ncl_http_accept_thread(void *arg)
{
    ncl_http_server *server = (ncl_http_server *)arg;

    while (!server->stopping) {
        ncl_socket *listener = server->listener;
        ncl_socket *sock;
        if (listener == NULL) {
            break;
        }
        sock = ncl_socket_accept(listener, 200);
        if (sock == NULL) {
            continue;
        }
        ncl_socket_set_nodelay(sock, true);
        ncl_http_handle(server, sock);
        ncl_socket_close(sock);
    }
    server->running = false;
}

/* ================================================================ public == */

ncl_http_server *ncl_http_server_create(unsigned port)
{
    ncl_http_server *server = (ncl_http_server *)calloc(1, sizeof(ncl_http_server));
    if (server == NULL) {
        return NULL;
    }
    server->port = port;
    server->cors = true;
    server->mutex = ncl_mutex_create();
    if (server->mutex == NULL) {
        free(server);
        return NULL;
    }
    return server;
}

void ncl_http_server_free(ncl_http_server *server)
{
    size_t i;
    if (server == NULL) {
        return;
    }
    ncl_http_server_stop(server);
    for (i = 0; i < server->route_count; i++) {
        free(server->routes[i].method);
        free(server->routes[i].path);
    }
    ncl_mutex_destroy(server->mutex);
    free(server);
}

ncl_err ncl_http_server_route(ncl_http_server *server, const char *method,
                              const char *path, ncl_http_handler handler,
                              void *user)
{
    ncl_http_route *route;

    if (server == NULL || path == NULL || handler == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (server->route_count >= NCL_HTTP_MAX_ROUTES) {
        return NCL_ERR_NOMEM;
    }
    route = &server->routes[server->route_count];
    route->method = (method != NULL && strcmp(method, "*") != 0)
                        ? ncl_strdup(method)
                        : NULL;
    route->path = ncl_strdup(path);
    route->handler = handler;
    route->user = user;
    /* A path ending in slash + star registers a catch-all route: the trailing
     * two characters are dropped and the remainder becomes the prefix. */
    route->prefix_len = 0;
    if (strlen(path) > 2 && strcmp(path + strlen(path) - 2, "/*") == 0) {
        route->prefix_len = strlen(path) - 2;
        route->path[route->prefix_len] = '\0';
    }
    if (route->path == NULL) {
        free(route->method);
        return NCL_ERR_NOMEM;
    }
    server->route_count++;
    return NCL_OK;
}

ncl_err ncl_http_server_start(ncl_http_server *server)
{
    char err[128];

    if (server == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (server->running) {
        return NCL_OK;
    }
    err[0] = '\0';
    server->listener = ncl_socket_listen(server->port, err, sizeof(err));
    if (server->listener == NULL) {
        ncl_log_error("HTTP 服务监听失败: %s", err);
        return NCL_ERR_IO;
    }
    server->port = ncl_socket_local_port(server->listener);
    server->stopping = false;
    server->thread = ncl_thread_start(ncl_http_accept_thread, server);
    if (server->thread == NULL) {
        ncl_socket_close(server->listener);
        server->listener = NULL;
        return NCL_ERR_NOMEM;
    }
    server->running = true;
    ncl_log_info("HTTP 服务已启动: 端口 %u", server->port);
    return NCL_OK;
}

void ncl_http_server_stop(ncl_http_server *server)
{
    if (server == NULL) {
        return;
    }
    server->stopping = true;
    /* Shut the listener down first: it unblocks accept() without freeing the
     * object the accept thread is still referencing. It is released after the
     * thread has been joined. */
    if (server->listener != NULL) {
        ncl_socket_shutdown(server->listener);
    }
    if (server->thread != NULL) {
        ncl_thread_join(server->thread);
        server->thread = NULL;
    }
    if (server->listener != NULL) {
        ncl_socket_close(server->listener);
        server->listener = NULL;
    }
    server->running = false;
}

unsigned ncl_http_server_port(const ncl_http_server *server)
{
    return server != NULL ? server->port : 0;
}

size_t ncl_http_server_request_count(const ncl_http_server *server)
{
    size_t count;
    ncl_http_server *mutable_server = (ncl_http_server *)server;
    if (server == NULL) {
        return 0;
    }
    ncl_mutex_lock(mutable_server->mutex);
    count = mutable_server->request_count;
    ncl_mutex_unlock(mutable_server->mutex);
    return count;
}

void ncl_http_server_set_cors(ncl_http_server *server, bool enabled)
{
    if (server != NULL) {
        server->cors = enabled;
    }
}
