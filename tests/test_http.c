/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * HTTP server tests. A raw socket client drives the server so that the wire
 * format (status line, headers, body) is verified, not just the handler API.
 */
#include "ncl_test.h"

#include "nclink/ncl_http.h"
#include "nclink/ncl_platform.h"

/* ------------------------------------------------------------ test routes */

static char g_last_query[128];

static void route_ping(ncl_http_request *request, ncl_http_response *response,
                       void *user)
{
    (void)request;
    (void)user;
    ncl_http_reply_text(response, NCL_HTTP_OK, "pong");
}

static void route_json(ncl_http_request *request, ncl_http_response *response,
                       void *user)
{
    ncl_json *body;
    const char *device;
    (void)user;

    device = ncl_http_query(request, "device");
    snprintf(g_last_query, sizeof(g_last_query), "%s", device != NULL ? device : "");

    body = ncl_json_new_object();
    ncl_json_obj_set_string(body, "device", device != NULL ? device : "");
    ncl_json_obj_set_int(body, "value", 42);
    ncl_http_reply_json(response, NCL_HTTP_OK, body);
    ncl_json_free(body);
}

static void route_echo(ncl_http_request *request, ncl_http_response *response,
                       void *user)
{
    ncl_json *body = ncl_http_json_body(request);
    (void)user;

    if (body == NULL) {
        ncl_http_reply_text(response, NCL_HTTP_BAD_REQUEST, "invalid json");
        return;
    }
    ncl_http_reply_json(response, NCL_HTTP_OK, body);
    ncl_json_free(body);
}

static void route_form(ncl_http_request *request, ncl_http_response *response,
                       void *user)
{
    const char *sn = ncl_http_form_field(request, "sn");
    (void)user;
    ncl_http_reply_text(response, NCL_HTTP_OK, sn != NULL ? sn : "");
}

static void route_boom(ncl_http_request *request, ncl_http_response *response,
                       void *user)
{
    (void)request;
    (void)user;
    ncl_http_reply_text(response, NCL_HTTP_INTERNAL_ERROR, "boom");
}

/* ------------------------------------------------------------ HTTP client */

typedef struct {
    int    status;
    char   headers[2048];
    char   body[4096];
    size_t body_len;
} http_reply;

static bool http_send(unsigned port, const char *request_text,
                      http_reply *reply)
{
    ncl_socket *sock;
    char buffer[8192];
    ncl_strbuf raw;
    size_t header_end;
    int rc;

    memset(reply, 0, sizeof(*reply));
    sock = ncl_socket_connect("127.0.0.1", port, 3000, NULL, 0);
    if (sock == NULL) {
        return false;
    }
    if (ncl_socket_send(sock, request_text, strlen(request_text)) != NCL_OK) {
        ncl_socket_close(sock);
        return false;
    }
    ncl_strbuf_init(&raw);
    for (;;) {
        rc = ncl_socket_recv(sock, buffer, sizeof(buffer), 3000);
        if (rc <= 0) {
            break;
        }
        ncl_strbuf_append(&raw, buffer, (size_t)rc);
    }
    ncl_socket_close(sock);

    if (raw.len == 0) {
        ncl_strbuf_free(&raw);
        return false;
    }
    /* Status line. */
    if (sscanf(raw.data, "HTTP/1.1 %d", &reply->status) != 1) {
        ncl_strbuf_free(&raw);
        return false;
    }
    header_end = (size_t)(strstr(raw.data, "\r\n\r\n") - raw.data) + 4;
    if (header_end > 4 && header_end <= raw.len) {
        size_t copy = header_end - 4 < sizeof(reply->headers) - 1
                          ? header_end - 4
                          : sizeof(reply->headers) - 1;
        memcpy(reply->headers, raw.data, copy);
        reply->headers[copy] = '\0';
        reply->body_len = raw.len - header_end;
        if (reply->body_len >= sizeof(reply->body)) {
            reply->body_len = sizeof(reply->body) - 1;
        }
        memcpy(reply->body, raw.data + header_end, reply->body_len);
        reply->body[reply->body_len] = '\0';
    }
    ncl_strbuf_free(&raw);
    return true;
}

/* -------------------------------------------------------------------- tests */

static void test_http_server(void)
{
    ncl_http_server *server;
    unsigned port;
    http_reply reply;
    char request[2048];

    server = ncl_http_server_create(0);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    NCL_CHECK_EQ_INT(ncl_http_server_route(server, "GET", "/api/ping", route_ping,
                                           NULL),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_http_server_route(server, "GET", "/api/value", route_json,
                                           NULL),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_http_server_route(server, "POST", "/api/echo", route_echo,
                                           NULL),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_http_server_route(server, "POST", "/api/form", route_form,
                                           NULL),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_http_server_route(server, "*", "/api/any", route_ping,
                                           NULL),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_http_server_route(server, "GET", "/api/boom", route_boom,
                                           NULL),
                     NCL_OK);

    NCL_TEST_CASE("server starts on an ephemeral port");
    NCL_CHECK_EQ_INT(ncl_http_server_start(server), NCL_OK);
    port = ncl_http_server_port(server);
    NCL_CHECK(port != 0);

    NCL_TEST_CASE("GET returns the handler body with correct headers");
    snprintf(request, sizeof(request),
             "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    NCL_CHECK(http_send(port, request, &reply));
    NCL_CHECK_EQ_INT(reply.status, 200);
    NCL_CHECK_EQ_STR(reply.body, "pong");
    NCL_CHECK(strstr(reply.headers, "Content-Type: text/plain") != NULL);
    NCL_CHECK(strstr(reply.headers, "Content-Length: 4") != NULL);
    NCL_CHECK(strstr(reply.headers, "Access-Control-Allow-Origin: *") != NULL);

    NCL_TEST_CASE("query parameters are decoded");
    snprintf(request, sizeof(request),
             "GET /api/value?device=V203243111F&x=1 HTTP/1.1\r\nHost: x\r\n\r\n");
    NCL_CHECK(http_send(port, request, &reply));
    NCL_CHECK_EQ_INT(reply.status, 200);
    NCL_CHECK_EQ_STR(g_last_query, "V203243111F");
    NCL_CHECK(strstr(reply.headers, "Content-Type: application/json") != NULL);
    NCL_CHECK_EQ_STR(reply.body, "{\"device\":\"V203243111F\",\"value\":42}");

    NCL_TEST_CASE("URL encoded query values are decoded");
    snprintf(request, sizeof(request),
             "GET /api/value?device=A%%2FB%%20C HTTP/1.1\r\nHost: x\r\n\r\n");
    NCL_CHECK(http_send(port, request, &reply));
    NCL_CHECK_EQ_STR(g_last_query, "A/B C");

    NCL_TEST_CASE("POST with a JSON body is parsed and echoed");
    {
        const char *payload = "{\"sn\":\"V1\",\"value\":7}";
        snprintf(request, sizeof(request),
                 "POST /api/echo HTTP/1.1\r\nHost: x\r\n"
                 "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                 strlen(payload), payload);
        NCL_CHECK(http_send(port, request, &reply));
        NCL_CHECK_EQ_INT(reply.status, 200);
        NCL_CHECK_EQ_STR(reply.body, payload);
    }

    NCL_TEST_CASE("POST form fields are decoded");
    {
        const char *payload = "sn=V203243111F&other=1";
        snprintf(request, sizeof(request),
                 "POST /api/form HTTP/1.1\r\nHost: x\r\n"
                 "Content-Type: application/x-www-form-urlencoded\r\n"
                 "Content-Length: %zu\r\n\r\n%s",
                 strlen(payload), payload);
        NCL_CHECK(http_send(port, request, &reply));
        NCL_CHECK_EQ_INT(reply.status, 200);
        NCL_CHECK_EQ_STR(reply.body, "V203243111F");
    }

    NCL_TEST_CASE("unknown paths answer 404 and wrong methods 405");
    snprintf(request, sizeof(request), "GET /api/nope HTTP/1.1\r\nHost: x\r\n\r\n");
    NCL_CHECK(http_send(port, request, &reply));
    NCL_CHECK_EQ_INT(reply.status, 404);

    snprintf(request, sizeof(request), "DELETE /api/ping HTTP/1.1\r\nHost: x\r\n\r\n");
    NCL_CHECK(http_send(port, request, &reply));
    NCL_CHECK_EQ_INT(reply.status, 405);

    NCL_TEST_CASE("a wildcard route accepts any method");
    snprintf(request, sizeof(request), "PUT /api/any HTTP/1.1\r\nHost: x\r\n\r\n");
    NCL_CHECK(http_send(port, request, &reply));
    NCL_CHECK_EQ_INT(reply.status, 200);
    NCL_CHECK_EQ_STR(reply.body, "pong");

    NCL_TEST_CASE("CORS preflight is answered without running the handler");
    snprintf(request, sizeof(request),
             "OPTIONS /api/ping HTTP/1.1\r\nHost: x\r\n"
             "Origin: http://localhost\r\n\r\n");
    NCL_CHECK(http_send(port, request, &reply));
    NCL_CHECK_EQ_INT(reply.status, 204);
    NCL_CHECK(strstr(reply.headers, "Access-Control-Allow-Methods") != NULL);

    NCL_TEST_CASE("handler chosen status codes reach the client");
    snprintf(request, sizeof(request), "GET /api/boom HTTP/1.1\r\nHost: x\r\n\r\n");
    NCL_CHECK(http_send(port, request, &reply));
    NCL_CHECK_EQ_INT(reply.status, 500);
    NCL_CHECK_EQ_STR(reply.body, "boom");

    NCL_TEST_CASE("a malformed request line is rejected");
    NCL_CHECK(http_send(port, "GARBAGE\r\n\r\n", &reply));
    NCL_CHECK_EQ_INT(reply.status, 400);

    NCL_TEST_CASE("requests are counted");
    NCL_CHECK(ncl_http_server_request_count(server) >= 9);

    NCL_TEST_CASE("stop shuts the listener down");
    ncl_http_server_stop(server);
    NCL_CHECK(!http_send(port, "GET /api/ping HTTP/1.1\r\n\r\n", &reply));
    ncl_http_server_free(server);
}

static void test_utilities(void)
{
    char *decoded;
    char *encoded;

    NCL_TEST_CASE("URL decoding handles %XX, '+' and malformed input");
    decoded = ncl_http_url_decode("A%2FB+C");
    NCL_CHECK_EQ_STR(decoded, "A/B C");
    free(decoded);
    decoded = ncl_http_url_decode("100%");
    NCL_CHECK_EQ_STR(decoded, "100%");
    free(decoded);
    decoded = ncl_http_url_decode(NULL);
    NCL_CHECK(decoded == NULL);

    NCL_TEST_CASE("URL encoding escapes reserved characters");
    encoded = ncl_http_url_encode("a b/c?d=e");
    NCL_CHECK_EQ_STR(encoded, "a+b%2Fc%3Fd%3De");
    free(encoded);

    NCL_TEST_CASE("status text lookup");
    NCL_CHECK_EQ_STR(ncl_http_status_text(200), "OK");
    NCL_CHECK_EQ_STR(ncl_http_status_text(404), "Not Found");
    NCL_CHECK_EQ_STR(ncl_http_status_text(500), "Internal Server Error");
}

NCL_TEST_MAIN_BEGIN()
    test_http_server();
    test_utilities();
NCL_TEST_MAIN_END()
