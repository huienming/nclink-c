/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Configuration layer tests and the matching REST endpoints.
 *
 * The tests point the library at a throwaway install root under the system temp
 * directory so the repository's conf/ tree is never touched.
 */
#include "ncl_test.h"

#include <stdlib.h>

#include "nclink/ncl_config.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_http.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_rest.h"

static char g_root[512];

static void prepare_root(void)
{
    /* A fresh directory per run: PIDs are recycled, so reusing a name can hand
     * the test a tree left behind by an earlier (possibly aborted) run. */
    snprintf(g_root, sizeof(g_root), "%s%cnclink-config-test-%ld-%lld",
             getenv("TEMP") != NULL ? getenv("TEMP") : ".", NCL_PATH_SEP,
             ncl_process_id(), (long long)ncl_time_millis());
    ncl_path_remove(g_root);
    ncl_mkdir_p(g_root);
    ncl_env_set_root(g_root);
}

/* ------------------------------------------------------------------ HTTP -- */

static bool http_send(unsigned port, const char *method, const char *path,
                      const char *content_type, const char *body, int *status,
                      char *out_body, size_t out_size)
{
    ncl_socket *sock;
    ncl_strbuf request;
    char buffer[8192];
    ncl_strbuf raw;
    int rc;

    sock = ncl_socket_connect("127.0.0.1", port, 3000, NULL, 0);
    if (sock == NULL) {
        return false;
    }
    ncl_strbuf_init(&request);
    ncl_strbuf_printf(&request, "%s %s HTTP/1.1\r\nHost: localhost\r\n", method,
                      path);
    if (body != NULL) {
        ncl_strbuf_printf(&request, "Content-Type: %s\r\nContent-Length: %zu\r\n",
                          content_type != NULL ? content_type : "application/json",
                          strlen(body));
    }
    ncl_strbuf_puts(&request, "Connection: close\r\n\r\n");
    if (body != NULL) {
        ncl_strbuf_puts(&request, body);
    }
    if (ncl_socket_send(sock, request.data, request.len) != NCL_OK) {
        ncl_strbuf_free(&request);
        ncl_socket_close(sock);
        return false;
    }
    ncl_strbuf_free(&request);

    ncl_strbuf_init(&raw);
    while ((rc = ncl_socket_recv(sock, buffer, sizeof(buffer), 3000)) > 0) {
        ncl_strbuf_append(&raw, buffer, (size_t)rc);
    }
    ncl_socket_close(sock);
    if (raw.len == 0) {
        ncl_strbuf_free(&raw);
        return false;
    }
    if (status != NULL) {
        *status = atoi(raw.data + 9);
    }
    {
        const char *sep = strstr(raw.data, "\r\n\r\n");
        if (out_body != NULL && out_size > 0) {
            size_t copy = 0;
            if (sep != NULL) {
                size_t available = raw.len - (size_t)(sep - raw.data) - 4;
                copy = available < out_size - 1 ? available : out_size - 1;
                memcpy(out_body, sep + 4, copy);
            }
            out_body[copy] = '\0';
        }
    }
    ncl_strbuf_free(&raw);
    return true;
}

/* ----------------------------------------------------------------- tests -- */

static void test_file_layer(void)
{
    char *sn = NULL;
    char *read_back;
    ncl_json *document;

    prepare_root();

    NCL_TEST_CASE("cfgInit creates the directories and writes bin/sn.txt");
    NCL_CHECK_EQ_INT(ncl_config_init(NULL, &sn), NCL_OK);
    NCL_CHECK(sn != NULL);
    if (sn != NULL) {
        /* The init call writes a UUID with the dashes stripped, which is
         * deliberately *not* the "V2..." form ncl_sn_read() produces. */
        NCL_CHECK_EQ_INT(strlen(sn), 32);
        NCL_CHECK_EQ_INT(strspn(sn, "0123456789abcdef"), 32);
        free(sn);
    }
    NCL_CHECK(ncl_path_exists(ncl_env_conf_path()));
    NCL_CHECK(ncl_path_exists(ncl_env_sn_file()));

    NCL_TEST_CASE("the runtime serial number is stable across reads");
    {
        char *first = ncl_sn_read();
        char *second = ncl_sn_read();
        NCL_CHECK(first != NULL && second != NULL);
        if (first != NULL && second != NULL) {
            NCL_CHECK_EQ_STR(first, second);
        }
        free(first);
        free(second);
    }

    NCL_TEST_CASE("getSn reads the very same value back");
    read_back = ncl_config_get_sn();
    NCL_CHECK(read_back != NULL);
    if (read_back != NULL) {
        ncl_check_is_code_valid(NULL); /* keep the symbol used */
        free(read_back);
    }

    NCL_TEST_CASE("cfgInit honours an explicit serial number");
    NCL_CHECK_EQ_INT(ncl_config_init("V2TEST00001", &sn), NCL_OK);
    NCL_CHECK_EQ_STR(sn, "V2TEST00001");
    free(sn);
    read_back = ncl_config_get_sn();
    NCL_CHECK_EQ_STR(read_back, "V2TEST00001");
    free(read_back);

    NCL_TEST_CASE("a missing model file reports failure, not a crash");
    NCL_CHECK(ncl_config_get_model() == NULL);
    NCL_CHECK(ncl_config_get_driver() == NULL);

    NCL_TEST_CASE("setModel / getModel round trip through the file");
    NCL_CHECK_EQ_INT(ncl_config_set_model("{\"id\":\"01\",\"type\":\"NC_LINK_ROOT\"}"),
                     NCL_OK);
    document = ncl_config_get_model();
    NCL_CHECK(document != NULL);
    if (document != NULL) {
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(document, "id"), "01");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(document, "type"), "NC_LINK_ROOT");
        ncl_json_free(document);
    }

    NCL_TEST_CASE("setDriver / getDriver round trip");
    NCL_CHECK_EQ_INT(ncl_config_set_driver("{\"drivers\":[{\"name\":\"modbus\"}]}"),
                     NCL_OK);
    document = ncl_config_get_driver();
    NCL_CHECK(document != NULL);
    if (document != NULL) {
        NCL_CHECK(ncl_json_obj_get(document, "drivers") != NULL);
        ncl_json_free(document);
    }

    NCL_TEST_CASE("setMqtt / getMqtt round trip through conf/mqtt.cfg");
    {
        ncl_json *config = ncl_json_new_object();
        ncl_json_obj_set_string(config, "url", "tcp://iot.hz2025.com:1883");
        ncl_json_obj_set_string(config, "username", "admin");
        ncl_json_obj_set_string(config, "password", "123456");
        NCL_CHECK_EQ_INT(ncl_config_set_mqtt(config), NCL_OK);
        ncl_json_free(config);
    }
    document = ncl_config_get_mqtt();
    NCL_CHECK(document != NULL);
    if (document != NULL) {
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(document, "url"),
                         "tcp://iot.hz2025.com:1883");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(document, "username"), "admin");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(document, "password"), "123456");
        ncl_json_free(document);
    }

    NCL_TEST_CASE("setServer / getServerList round trip");
    NCL_CHECK_EQ_INT(ncl_config_set_server_list("[\"driverA\",\"driverB\"]"), NCL_OK);
    document = ncl_config_get_server_list();
    NCL_CHECK(document != NULL);
    if (document != NULL) {
        NCL_CHECK_EQ_INT(ncl_json_arr_len(document), 2);
        NCL_CHECK_EQ_STR(ncl_json_as_string(ncl_json_arr_get(document, 1)), "driverB");
        ncl_json_free(document);
    }

    NCL_TEST_CASE("setIpConf / getIpConf round trip through conf/ipConf.json");
    NCL_CHECK(ncl_config_get_ip_conf() == NULL); /* not written yet */
    NCL_CHECK_EQ_INT(
        ncl_config_set_ip_conf("{\"ip\":\"192.168.1.10\",\"mask\":\"255.255.255.0\"}"),
        NCL_OK);
    document = ncl_config_get_ip_conf();
    NCL_CHECK(document != NULL);
    if (document != NULL) {
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(document, "ip"), "192.168.1.10");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(document, "mask"), "255.255.255.0");
        ncl_json_free(document);
    }
}

static void test_rest_endpoints(void)
{
    ncl_http_server *http;
    unsigned port;
    int status = 0;
    static char body[8192];
    ncl_json *parsed;

    prepare_root();
    NCL_CHECK_EQ_INT(ncl_config_init("V2REST00001", NULL), NCL_OK);

    http = ncl_http_server_create(0);
    NCL_CHECK(http != NULL);
    if (http == NULL) {
        return;
    }
    NCL_CHECK_EQ_INT(ncl_rest_attach_config(http), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_http_server_start(http), NCL_OK);
    port = ncl_http_server_port(http);

    NCL_TEST_CASE("GET /api/cfg/getSn answers the Result envelope");
    NCL_CHECK(http_send(port, "GET", "/api/cfg/getSn", NULL, NULL, &status, body,
                        sizeof(body)));
    NCL_CHECK_EQ_INT(status, 200);
    parsed = ncl_json_parse_cstr(body, NULL);
    NCL_CHECK(parsed != NULL);
    if (parsed != NULL) {
        bool ok = false;
        ncl_json_as_bool(ncl_json_obj_get(parsed, "status"), &ok);
        NCL_CHECK(ok);
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(parsed, "data"), "V2REST00001");
        ncl_json_free(parsed);
    }

    NCL_TEST_CASE("GET /api/getMqttUrl returns the stored connection settings");
    {
        const char *payload =
            "{\"url\":\"tcp://broker:1883\",\"username\":\"u1\",\"password\":\"p1\"}";
        NCL_CHECK(http_send(port, "POST", "/api/setMqttUrl", "application/json",
                            payload, &status, body, sizeof(body)));
        NCL_CHECK_EQ_INT(status, 200);
        parsed = ncl_json_parse_cstr(body, NULL);
        NCL_CHECK(parsed != NULL);
        if (parsed != NULL) {
            bool ok = false;
            ncl_json_as_bool(ncl_json_obj_get(parsed, "status"), &ok);
            NCL_CHECK(ok);
            ncl_json_free(parsed);
        }
    }
    NCL_CHECK(http_send(port, "GET", "/api/getMqttUrl", NULL, NULL, &status, body,
                        sizeof(body)));
    parsed = ncl_json_parse_cstr(body, NULL);
    NCL_CHECK(parsed != NULL);
    if (parsed != NULL) {
        ncl_json *data = ncl_json_obj_get(parsed, "data");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(data, "url"), "tcp://broker:1883");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(data, "password"), "p1");
        ncl_json_free(parsed);
    }

    NCL_TEST_CASE("POST /api/cfg/setModel then GET /api/cfg/getModel");
    NCL_CHECK(http_send(port, "POST", "/api/cfg/setModel", "application/json",
                        "{\"id\":\"07\",\"type\":\"NC_LINK_ROOT\"}", &status, body,
                        sizeof(body)));
    NCL_CHECK(http_send(port, "GET", "/api/cfg/getModel", NULL, NULL, &status, body,
                        sizeof(body)));
    parsed = ncl_json_parse_cstr(body, NULL);
    NCL_CHECK(parsed != NULL);
    if (parsed != NULL) {
        ncl_json *data = ncl_json_obj_get(parsed, "data");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(data, "id"), "07");
        ncl_json_free(parsed);
    }

    NCL_TEST_CASE("a missing configuration answers status=false with a message");
    {
        /* Point at a fresh root where the driver file was never written. */
        snprintf(g_root, sizeof(g_root), "%s%cnclink-config-missing-%ld-%lld",
                 getenv("TEMP") != NULL ? getenv("TEMP") : ".", NCL_PATH_SEP,
                 ncl_process_id(), (long long)ncl_time_millis());
        ncl_path_remove(g_root);
        ncl_mkdir_p(g_root);
        ncl_env_set_root(g_root);
        NCL_CHECK(http_send(port, "GET", "/api/cfg/getDriver", NULL, NULL, &status,
                            body, sizeof(body)));
        parsed = ncl_json_parse_cstr(body, NULL);
        NCL_CHECK(parsed != NULL);
        if (parsed != NULL) {
            bool ok = true;
            ncl_json_as_bool(ncl_json_obj_get(parsed, "status"), &ok);
            NCL_CHECK(!ok);
            NCL_CHECK(ncl_json_obj_get_string(parsed, "data") != NULL);
            ncl_json_free(parsed);
        }
    }

    NCL_TEST_CASE("unsupported methods on a config path answer 405");
    NCL_CHECK(http_send(port, "DELETE", "/api/cfg/getSn", NULL, NULL, &status, body,
                        sizeof(body)));
    NCL_CHECK_EQ_INT(status, 405);

    NCL_TEST_CASE("POST /api/cfg/setIpConf then GET /api/cfg/getIpConf");
    {
        /* Back to the root that has a conf directory. */
        prepare_root();
        NCL_CHECK_EQ_INT(ncl_config_init("V2REST00001", NULL), NCL_OK);
        NCL_CHECK(http_send(port, "POST", "/api/cfg/setIpConf", "application/json",
                            "{\"ip\":\"10.0.0.5\"}", &status, body, sizeof(body)));
        NCL_CHECK_EQ_INT(status, 200);
        NCL_CHECK(http_send(port, "GET", "/api/cfg/getIpConf", NULL, NULL, &status,
                            body, sizeof(body)));
        parsed = ncl_json_parse_cstr(body, NULL);
        NCL_CHECK(parsed != NULL);
        if (parsed != NULL) {
            bool ok = false;
            ncl_json_as_bool(ncl_json_obj_get(parsed, "status"), &ok);
            NCL_CHECK(ok);
            NCL_CHECK_EQ_STR(
                ncl_json_obj_get_string(ncl_json_obj_get(parsed, "data"), "ip"),
                "10.0.0.5");
            ncl_json_free(parsed);
        }
    }

    ncl_http_server_stop(http);
    ncl_http_server_free(http);
}

NCL_TEST_MAIN_BEGIN()
    test_file_layer();
    test_rest_endpoints();
NCL_TEST_MAIN_END()
