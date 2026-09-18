/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * File transfer tests: attributes and helpers, the SHA-256 checksum, bin/ftp.txt,
 * the FTP file tool against a real FTP server, and the whole MQTT + FTP file
 * channel end to end (client file tool <-> server "file" tool).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ncl_test.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_server.h"
#include "nclink/ncl_socket.h"

#if defined(NCL_OS_WINDOWS)
#  include <direct.h>
#  define test_chdir(p) _chdir(p)
#else
#  include <unistd.h>
#  define test_chdir(p) chdir(p)
#endif

#define TEST_ROOT "ncl_file_test_root"
#define TEST_SN "V203243111F"
#define TEST_FTP_PORT 12345u

static const char *kModelJson =
    "{\"name\":\"nclink\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\","
    "\"devices\":[{\"id\":\"02\",\"type\":\"PLC\",\"configs\":[],"
    "\"dataItems\":[{\"id\":\"030001\",\"type\":\"STATUS\"}],"
    "\"version\":\"2.0\"}]}";

/* ------------------------------------------------------------- helpers ---- */

static void make_payload(char *buffer, size_t len, unsigned seed)
{
    size_t i;
    for (i = 0; i < len; i++) {
        buffer[i] = (char)((i * 37 + seed) & 0xFF);
    }
    buffer[0] = 'p'; /* keep it printable-ish at the front */
    buffer[len - 1] = '\0';
}

static bool read_file(const char *path, char *buffer, size_t len,
                      size_t *out_len)
{
    char *data = NULL;
    size_t data_len = 0;

    if (ncl_file_read_all(path, &data, &data_len) != NCL_OK) {
        return false;
    }
    if (data_len > len) {
        ncl_free_safe(data);
        return false;
    }
    memcpy(buffer, data, data_len);
    ncl_free_safe(data);
    if (out_len != NULL) {
        *out_len = data_len;
    }
    return true;
}

/* ==================================================== unit level checks === */

static void test_checksum(void)
{
    static const struct {
        const char *input;
        const char *hex;
    } vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc",
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    size_t i;

    NCL_TEST_CASE("SHA-256 known vectors");
    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        char *hex = NULL;
        NCL_CHECK_EQ_INT(ncl_sha256_hex(vectors[i].input,
                                        strlen(vectors[i].input), &hex),
                         NCL_OK);
        NCL_CHECK_EQ_STR(hex, vectors[i].hex);
        ncl_free_safe(hex);
    }
    NCL_TEST_CASE("SHA-256 over a long input");
    {
        char block[4096];
        char *hex = NULL;
        memset(block, 'a', sizeof(block));
        NCL_CHECK_EQ_INT(ncl_sha256_hex(block, sizeof(block), &hex), NCL_OK);
        NCL_CHECK_EQ_STR(hex,
                         "c93eee2d0db02f10acc7460d9576e122dcf8cd53c4bf8dfcae1b"
                         "3e74ebcfff5a");
        ncl_free_safe(hex);
    }
}

static void test_utils(void)
{
    NCL_TEST_CASE("the compression decision follows the extension");
    NCL_CHECK(ncl_file_need_compression("a.txt"));
    NCL_CHECK(ncl_file_need_compression("REPORT.JSON"));
    NCL_CHECK(ncl_file_need_compression("main.cpp"));
    NCL_CHECK(!ncl_file_need_compression("frame.bin"));
    NCL_CHECK(!ncl_file_need_compression("movie.mp4"));
    NCL_CHECK(!ncl_file_need_compression("archive.tar.gz"));
    NCL_CHECK(!ncl_file_need_compression(NULL));

    NCL_TEST_CASE("the chunk count follows the chunk size");
    NCL_CHECK_EQ_INT(ncl_file_total_chunks(0), 0);
    NCL_CHECK_EQ_INT(ncl_file_total_chunks(1), 1);
    NCL_CHECK_EQ_INT(ncl_file_total_chunks(NCL_FILE_CHUNK_SIZE), 1);
    NCL_CHECK_EQ_INT(ncl_file_total_chunks(NCL_FILE_CHUNK_SIZE + 1), 2);
    NCL_CHECK_EQ_INT(ncl_file_total_chunks(NCL_FILE_CHUNK_SIZE * 5), 5);
}

static void test_attribute_json(void)
{
    ncl_file_attribute attribute;
    ncl_json *json;
    char *text;
    ncl_file_attribute *round_trip;

    NCL_TEST_CASE("file attributes serialise in the documented order");
    memset(&attribute, 0, sizeof(attribute));
    attribute.file_name = (char *)"nclink.json";
    attribute.file_type = 0;
    attribute.file_size = 1234;
    attribute.total_chunks = 1;
    attribute.compressed = true;
    attribute.checksum = (char *)"abc";
    attribute.parant_dir = (char *)"/conf/model";
    attribute.modify_time = 1700000000000LL;

    json = ncl_file_attribute_to_json(&attribute);
    NCL_CHECK(json != NULL);
    text = ncl_json_write_string(json);
    NCL_CHECK_EQ_STR(
        text,
        "{\"fileName\":\"nclink.json\",\"fileType\":0,\"fileSize\":1234,"
        "\"totalChunks\":1,\"compressed\":true,\"checksum\":\"abc\","
        "\"parantDir\":\"/conf/model\",\"modifyTime\":1700000000000}");
    ncl_json_free(json);

    NCL_TEST_CASE("file attributes parse back");
    json = ncl_json_parse_cstr(text, NULL);
    ncl_free_safe(text);
    round_trip = ncl_file_attribute_from_json(json);
    NCL_CHECK(round_trip != NULL);
    if (round_trip != NULL) {
        NCL_CHECK_EQ_STR(round_trip->file_name, "nclink.json");
        NCL_CHECK_EQ_INT(round_trip->file_type, 0);
        NCL_CHECK_EQ_INT(round_trip->file_size, 1234);
        NCL_CHECK_EQ_INT(round_trip->total_chunks, 1);
        NCL_CHECK(round_trip->compressed);
        NCL_CHECK_EQ_STR(round_trip->checksum, "abc");
        NCL_CHECK_EQ_STR(round_trip->parant_dir, "/conf/model");
        NCL_CHECK_EQ_INT(round_trip->modify_time, 1700000000000LL);
        NCL_CHECK(!ncl_file_attribute_is_dir(round_trip));
    }
    ncl_file_attribute_free(round_trip);
    ncl_json_free(json);

    NCL_TEST_CASE("a null file name is omitted, like NON_NULL inclusion");
    memset(&attribute, 0, sizeof(attribute));
    attribute.file_name = NULL;
    attribute.checksum = NULL;
    attribute.parant_dir = NULL;
    json = ncl_file_attribute_to_json(&attribute);
    text = ncl_json_write_string(json);
    NCL_CHECK_EQ_STR(text,
                     "{\"fileType\":0,\"fileSize\":0,\"totalChunks\":0,"
                     "\"compressed\":false}");
    ncl_free_safe(text);
    ncl_json_free(json);
}

static void test_local_attribute(void)
{
    char path[512];
    char payload[5000];
    ncl_file_attribute *attribute = NULL;
    char *checksum = NULL;

    NCL_TEST_CASE("attributes of a local file");
    make_payload(payload, sizeof(payload), 3);
    snprintf(path, sizeof(path), "sample.log");
    NCL_CHECK_EQ_INT(ncl_file_write_all(path, payload, sizeof(payload)), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_file_checksum(path, &checksum), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_file_attribute_of(path, "/data", &attribute), NCL_OK);
    NCL_CHECK(attribute != NULL);
    if (attribute != NULL) {
        NCL_CHECK_EQ_STR(attribute->file_name, "sample.log");
        NCL_CHECK_EQ_INT(attribute->file_type, 0);
        NCL_CHECK_EQ_INT(attribute->file_size, (long long)sizeof(payload));
        NCL_CHECK_EQ_INT(attribute->total_chunks,
                         ncl_file_total_chunks((long long)sizeof(payload)));
        NCL_CHECK(attribute->compressed);
        NCL_CHECK_EQ_STR(attribute->checksum, checksum);
        NCL_CHECK_EQ_STR(attribute->parant_dir, "/data");
        NCL_CHECK(attribute->modify_time > 0);
    }
    ncl_file_attribute_free(attribute);
    ncl_free_safe(checksum);

    NCL_TEST_CASE("directories report type 1 and no checksum");
    attribute = NULL;
    NCL_CHECK_EQ_INT(ncl_mkdir_p("adir"), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_file_attribute_of("adir", NULL, &attribute), NCL_OK);
    NCL_CHECK(attribute != NULL);
    if (attribute != NULL) {
        NCL_CHECK(ncl_file_attribute_is_dir(attribute));
        NCL_CHECK_EQ_INT(attribute->file_type, 1);
        NCL_CHECK_EQ_INT(attribute->total_chunks, 0);
        NCL_CHECK(!attribute->compressed);
        NCL_CHECK_EQ_STR(attribute->checksum, "");
    }
    ncl_file_attribute_free(attribute);
}

static void test_ftp_info(void)
{
    ncl_ftp_response info;
    ncl_ftp_response back;

    NCL_TEST_CASE("bin/ftp.txt defaults and round trip");
    NCL_CHECK_EQ_INT(ncl_ftp_info_read(&info), NCL_OK);
    NCL_CHECK_EQ_STR(info.user_name, "admin");
    NCL_CHECK_EQ_STR(info.password, "123456");
    NCL_CHECK_EQ_INT(info.port, 2121);

    info.port = 3131;
    ncl_ftp_info_write(&info);
    memset(&back, 0, sizeof(back));
    NCL_CHECK_EQ_INT(ncl_ftp_info_read(&back), NCL_OK);
    NCL_CHECK_EQ_INT(back.port, 3131);
    NCL_CHECK_EQ_STR(back.user_name, "admin");
    ncl_ftp_info_free(&info);
    ncl_ftp_info_free(&back);

    NCL_TEST_CASE("the interface map is reported");
    {
        char *map = ncl_net_ip_map_json();
        NCL_CHECK(map != NULL);
        ncl_free_safe(map);
    }
}

/* ================================================ FTP file tool (+FTP) ===== */

static void test_server_file_tool(void)
{
    ncl_ftp_server_options options;
    ncl_ftp_server *ftp;
    ncl_server_file_tool *tool;
    char payload[5000];
    char buffer[5000];
    size_t len = 0;
    char path[512];
    char *downloaded;

    NCL_TEST_CASE("the file tool talks to a real FTP server");
    memset(&options, 0, sizeof(options));
    options.port = TEST_FTP_PORT;
    options.root = ".";
    options.allow_write = true;
    options.idle_timeout_ms = 5000;
    ftp = ncl_ftp_server_create_ex(&options);
    NCL_CHECK(ftp != NULL);
    if (ftp == NULL) {
        return;
    }
    NCL_CHECK_EQ_INT(ncl_ftp_server_start(ftp), NCL_OK);

    tool = ncl_server_file_tool_create("127.0.0.1", TEST_FTP_PORT, "admin",
                                       "123456", TEST_SN);
    NCL_CHECK(tool != NULL);
    if (tool == NULL) {
        ncl_ftp_server_free(ftp);
        return;
    }
    NCL_CHECK(ncl_server_file_tool_detect(tool));

    make_payload(payload, sizeof(payload), 11);
    NCL_CHECK_EQ_INT(ncl_mkdir_p("uploadFile"), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_mkdir_p("uploadFile/data"), NCL_OK);
    NCL_CHECK_EQ_INT(
        ncl_file_write_all("uploadFile/data/source.txt", payload,
                           sizeof(payload)),
        NCL_OK);

    NCL_TEST_CASE("write uploads to /<sn>/<remoteDir>");
    NCL_CHECK(ncl_server_file_tool_write(tool, "uploadFile/data/source.txt",
                                         "/data"));
    snprintf(path, sizeof(path), "%s%cdata%csource.txt", TEST_SN, NCL_PATH_SEP,
             NCL_PATH_SEP);
    NCL_CHECK(ncl_path_exists(path));
    NCL_CHECK(read_file(path, buffer, sizeof(buffer), &len));
    NCL_CHECK_EQ_INT(len, sizeof(payload));
    NCL_CHECK(memcmp(buffer, payload, len) == 0);

    NCL_TEST_CASE("ll lists the local mirror");
    {
        ncl_ptrvec attributes;
        size_t i;
        bool found = false;

        ncl_ptrvec_init(&attributes, ncl_file_attribute_release);
        NCL_CHECK_EQ_INT(
            ncl_server_file_tool_ll(tool, "/data", &attributes), NCL_OK);
        for (i = 0; i < ncl_ptrvec_len(&attributes); i++) {
            const ncl_file_attribute *attribute =
                (const ncl_file_attribute *)ncl_ptrvec_at(&attributes, i);
            if (attribute != NULL && attribute->file_name != NULL &&
                strcmp(attribute->file_name, "source.txt") == 0) {
                found = true;
                NCL_CHECK_EQ_INT(attribute->file_size,
                                 (long long)sizeof(payload));
                NCL_CHECK_EQ_INT(strlen(attribute->checksum), 64);
            }
        }
        NCL_CHECK(found);
        ncl_ptrvec_free(&attributes);
    }

    NCL_TEST_CASE("a multi-chunk upload lands byte for byte");
    {
        /* 5 片（每片 256 KB）：分片与并行分片只在 >1 片时才走，5 片正好把
         * "并行传 5 片"那条路也带进来。 */
        enum { BIG_SIZE = 4 * NCL_FILE_CHUNK_SIZE + 12345 };
        static char big_payload[BIG_SIZE];
        static char big_back[BIG_SIZE];
        size_t i;

        for (i = 0; i < sizeof(big_payload); i++) {
            big_payload[i] = (char)((i * 7 + 3) & 0xFF);
        }
        NCL_CHECK_EQ_INT(ncl_file_write_all("uploadFile/data/big.bin",
                                            big_payload, sizeof(big_payload)),
                         NCL_OK);
        NCL_CHECK(ncl_server_file_tool_write(tool, "uploadFile/data/big.bin",
                                            "/data"));
        snprintf(path, sizeof(path), "%s%cdata%cbig.bin", TEST_SN, NCL_PATH_SEP,
                 NCL_PATH_SEP);
        NCL_CHECK(ncl_path_exists(path));
        NCL_CHECK(read_file(path, big_back, sizeof(big_back), &len));
        NCL_CHECK_EQ_INT(len, sizeof(big_payload));
        NCL_CHECK(memcmp(big_back, big_payload, sizeof(big_payload)) == 0);
        NCL_CHECK_EQ_INT(ncl_path_remove("uploadFile/data/big.bin"), NCL_OK);
    }

    NCL_TEST_CASE("mkdir creates local and remote folders");
    NCL_CHECK(ncl_server_file_tool_mkdir(tool, "/data/nested"));
    NCL_CHECK(ncl_path_is_dir("uploadFile/data/nested"));
    snprintf(path, sizeof(path), "%s%cdata%cnested", TEST_SN, NCL_PATH_SEP,
             NCL_PATH_SEP);
    NCL_CHECK(ncl_path_is_dir(path));

    NCL_TEST_CASE("read downloads back from the peer");
    NCL_CHECK_EQ_INT(ncl_path_remove("uploadFile/data/source.txt"), NCL_OK);
    NCL_CHECK(!ncl_path_exists("uploadFile/data/source.txt"));
    downloaded = ncl_server_file_tool_read(tool, "/data/source.txt");
    NCL_CHECK(downloaded != NULL);
    if (downloaded != NULL) {
        NCL_CHECK(read_file(downloaded, buffer, sizeof(buffer), &len));
        NCL_CHECK_EQ_INT(len, sizeof(payload));
        NCL_CHECK(memcmp(buffer, payload, len) == 0);
        ncl_free_safe(downloaded);
    }

    NCL_TEST_CASE("delete removes both copies");
    NCL_CHECK(ncl_server_file_tool_delete(tool, "/data/source.txt"));
    NCL_CHECK(!ncl_path_exists("uploadFile/data/source.txt"));
    snprintf(path, sizeof(path), "%s%cdata%csource.txt", TEST_SN, NCL_PATH_SEP,
             NCL_PATH_SEP);
    NCL_CHECK(!ncl_path_exists(path));

    ncl_server_file_tool_free(tool);
    ncl_ftp_server_free(ftp);
}

/* ===================================================== MQTT end to end === */

/*
 * A direct message channel: requests the client publishes are dispatched to the
 * ncl_server in-process and the response is handed straight back to the client's
 * inbound path. That keeps the file test hermetic (no broker, no second MQTT
 * session) while still exercising the real client/server/file-tool/FTP code.
 */
typedef struct {
    ncl_message_channel channel; /**< must be first */
    ncl_server         *server;
    ncl_client         *client;
    int                 published;
} direct_channel;

static ncl_err direct_publish(ncl_message_channel *self, const char *topic,
                              const ncl_message *message, int qos,
                              const ncl_mqtt_properties *properties)
{
    direct_channel *link = (direct_channel *)self;
    ncl_message *response;
    char response_topic[512];
    const char *marker;

    (void)qos;
    (void)properties;
    link->published++;
    response = ncl_server_dispatch(link->server, topic, message);
    if (response == NULL) {
        return NCL_ERR;
    }
    /* Response topic: "Request" becomes "Response". */
    marker = strstr(topic, "Request");
    if (marker != NULL) {
        snprintf(response_topic, sizeof(response_topic), "%.*sResponse%s",
                 (int)(marker - topic), topic, marker + 7);
    } else {
        snprintf(response_topic, sizeof(response_topic), "%s", topic);
    }
    ncl_client_on_message(link->client, response_topic, response);
    return NCL_OK;
}

static ncl_err direct_subscribe(ncl_message_channel *self, const char *topic,
                                int qos)
{
    (void)self;
    (void)topic;
    (void)qos;
    return NCL_OK;
}

static ncl_err direct_unsubscribe(ncl_message_channel *self, const char *topic)
{
    (void)self;
    (void)topic;
    return NCL_OK;
}

static ncl_err marker_echo(void *instance, const ncl_json *params,
                           ncl_json **result, char **reason)
{
    const char *key = ncl_params_string(params, "key");
    ncl_json *out = ncl_json_new_object();
    ncl_json *marker = ncl_json_new_object();
    char path[512];

    (void)instance;
    (void)reason;
    /* The file tool mirrored the parameter to <root>/uploadFile/<key>. */
    snprintf(path, sizeof(path), "uploadFile%s", key != NULL ? key : "");
    ncl_json_obj_set_string(marker, NCL_FILE_MARKER, path);
    ncl_json_obj_set(out, "copy", marker);
    *result = out;
    return NCL_OK;
}

static void test_end_to_end(void)
{
    direct_channel link;
    ncl_server_options server_options;
    ncl_server *server;
    ncl_client *client;
    char payload[5000];
    char buffer[5000];
    size_t len = 0;
    char local[512];
    static const ncl_tool_method methods[] = {{"echo", marker_echo}};

    NCL_TEST_CASE("protocol + FTP file channel");
    memset(&link, 0, sizeof(link));
    link.channel.publish = direct_publish;
    link.channel.subscribe = direct_subscribe;
    link.channel.unsubscribe = direct_unsubscribe;

    /* ---- device side ---- */
    /* The device side file tool derives the FTP peer from the host of the MQTT
     * URL; point the configuration at the local test peer. */
    NCL_CHECK_EQ_INT(ncl_mkdir_p("conf"), NCL_OK);
    NCL_CHECK_EQ_INT(
        ncl_file_write_all("conf/mqtt.cfg",
                           "url=tcp://127.0.0.1:1883\n"
                           "username=admin\npassword=123456\n",
                           strlen("url=tcp://127.0.0.1:1883\n"
                                  "username=admin\npassword=123456\n")),
        NCL_OK);
    /* The device side file tool reads the serial number from bin/sn.txt; pin it to
     * the serial number the test's client uses so both sides address the same
     * tree. */
    NCL_CHECK_EQ_INT(ncl_mkdir_p("bin"), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_file_write_all("bin/sn.txt", TEST_SN, strlen(TEST_SN)),
                     NCL_OK);
    memset(&server_options, 0, sizeof(server_options));
    server_options.sn = TEST_SN;
    server_options.mqtt = NULL;
    server_options.model_json = kModelJson;
    server = ncl_server_create(&server_options);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    link.server = server;
    NCL_CHECK_EQ_INT(ncl_server_register_file_tool(server), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_server_register_tool(server, "filesink", server,
                                              methods,
                                              sizeof(methods) / sizeof(methods[0]),
                                              NULL, 0),
                     NCL_OK);

    /* ---- client side (its FTP endpoint on 2323 is the peer of the device) -- */
    NCL_CHECK_EQ_INT(ncl_client_holder_start_ftp(), NCL_OK);
    client = ncl_client_create(TEST_SN, &link.channel);
    NCL_CHECK(client != NULL);
    if (client == NULL) {
        ncl_server_free(server);
        return;
    }
    link.client = client;
    {
        ncl_file_client_tool *tool = ncl_file_client_tool_create(client);
        NCL_CHECK(tool != NULL);
        NCL_CHECK(ncl_file_client_tool_detect(tool));
        ncl_client_set_file_tool(client, tool);
    }
    NCL_CHECK_EQ_INT(ncl_client_subscribe(client), NCL_OK);
    NCL_CHECK(ncl_client_file_tool(client) != NULL);
    {
        ncl_ftp_client *probe = ncl_ftp_client_create(
            "127.0.0.1", NCL_FTP_CLIENT_HOLDER_PORT, "admin", "123456");
        NCL_CHECK(probe != NULL);
        NCL_CHECK(ncl_ftp_client_detect(probe));
        ncl_ftp_client_free(probe);
    }

    /* ---- the handshake: without it the device has nobody to talk to ---- */
    NCL_TEST_CASE("a file method without a channel is refused");
    NCL_CHECK(!ncl_client_file_channel_is_open(client));
    NCL_CHECK(!ncl_server_file_channel_is_open(server));
    NCL_CHECK(ncl_client_write(client, "/data/report.txt") != NCL_OK);
    {
        ncl_ptrvec none;
        ncl_ptrvec_init(&none, ncl_file_attribute_release);
        NCL_CHECK(ncl_client_ll(client, "/", &none) != NCL_OK);
        NCL_CHECK_EQ_INT(ncl_ptrvec_len(&none), 0);
        ncl_ptrvec_free(&none);
    }

    NCL_TEST_CASE("openFileChannel hands the device the endpoint");
    {
        char id[NCL_FILE_CHANNEL_ID_MAX];

        NCL_CHECK_EQ_INT(ncl_client_open_file_channel(client, NULL), NCL_OK);
        NCL_CHECK(ncl_client_file_channel_is_open(client));
        NCL_CHECK(ncl_client_file_channel_id(client, id, sizeof(id)));
        NCL_CHECK(id[0] != '\0');
        /* The device answered by dialling it: it is the FTP peer now. */
        NCL_CHECK(ncl_server_file_channel_is_open(server));
    }

    NCL_TEST_CASE("the same lease is reused, another one needs force");
    {
        ncl_file_channel_options options;
        char lease[NCL_FILE_CHANNEL_ID_MAX];
        char id[NCL_FILE_CHANNEL_ID_MAX];

        NCL_CHECK(ncl_client_file_channel_id(client, lease, sizeof(lease)));
        ncl_file_channel_options_default(&options);
        options.channel_id = lease;
        NCL_CHECK_EQ_INT(ncl_client_open_file_channel(client, &options), NCL_OK);
        NCL_CHECK(ncl_client_file_channel_id(client, id, sizeof(id)));
        NCL_CHECK_EQ_STR(id, lease);

        /* A second lease is refused, and the open one survives the refusal. */
        options.channel_id = "another-lease";
        NCL_CHECK_EQ_INT(ncl_client_open_file_channel(client, &options),
                         NCL_ERR_NO_CHANNEL);
        NCL_CHECK(ncl_client_file_channel_id(client, id, sizeof(id)));
        NCL_CHECK_EQ_STR(id, lease);
        NCL_CHECK(ncl_server_file_channel_is_open(server));

        options.force = true;
        NCL_CHECK_EQ_INT(ncl_client_open_file_channel(client, &options), NCL_OK);
        NCL_CHECK(ncl_client_file_channel_id(client, id, sizeof(id)));
        NCL_CHECK_EQ_STR(id, "another-lease");
    }

    make_payload(payload, sizeof(payload), 23);
    NCL_CHECK_EQ_INT(ncl_mkdir_p(TEST_SN "/data"), NCL_OK);
    snprintf(local, sizeof(local), "%s%cdata%creport.txt", TEST_SN,
             NCL_PATH_SEP, NCL_PATH_SEP);
    NCL_CHECK_EQ_INT(
        ncl_file_write_all(local, payload, sizeof(payload)), NCL_OK);

    NCL_TEST_CASE("the client side tool pushes a file to the device");
    NCL_CHECK_EQ_INT(ncl_client_write(client, "/data/report.txt"), NCL_OK);
    NCL_CHECK(ncl_path_exists("uploadFile/data/report.txt"));
    NCL_CHECK(read_file("uploadFile/data/report.txt", buffer, sizeof(buffer),
                        &len));
    NCL_CHECK_EQ_INT(len, sizeof(payload));
    NCL_CHECK(memcmp(buffer, payload, len) == 0);

    NCL_TEST_CASE("checksums match so a second write is a no-op");
    NCL_CHECK_EQ_INT(ncl_client_write(client, "/data/report.txt"), NCL_OK);

    NCL_TEST_CASE("the client side tool lists file attributes");
    {
        ncl_ptrvec attributes;
        size_t i;
        bool found = false;

        ncl_ptrvec_init(&attributes, ncl_file_attribute_release);
        NCL_CHECK_EQ_INT(ncl_client_ll(client, "/data/report.txt", &attributes),
                         NCL_OK);
        for (i = 0; i < ncl_ptrvec_len(&attributes); i++) {
            const ncl_file_attribute *attribute =
                (const ncl_file_attribute *)ncl_ptrvec_at(&attributes, i);
            if (attribute != NULL && attribute->file_name != NULL &&
                strcmp(attribute->file_name, "report.txt") == 0) {
                found = true;
                NCL_CHECK_EQ_INT(attribute->file_size,
                                 (long long)sizeof(payload));
            }
        }
        NCL_CHECK(found);
        ncl_ptrvec_free(&attributes);
    }

    NCL_TEST_CASE("the client side tool creates a directory");
    NCL_CHECK(ncl_file_client_tool_mkdir(ncl_client_file_tool(client),
                                         "/docs"));
    NCL_CHECK(ncl_path_is_dir("uploadFile/docs"));

    NCL_TEST_CASE("the client side tool pulls the file back over FTP");
    NCL_CHECK_EQ_INT(ncl_path_remove(local), NCL_OK);
    NCL_CHECK(!ncl_path_exists(local));
    {
        char *restored = ncl_client_read(client, "/data/report.txt");
        NCL_CHECK(restored != NULL);
        if (restored != NULL) {
            NCL_CHECK(read_file(restored, buffer, sizeof(buffer), &len));
            NCL_CHECK_EQ_INT(len, sizeof(payload));
            NCL_CHECK(memcmp(buffer, payload, len) == 0);
            ncl_free_safe(restored);
        }
    }

    NCL_TEST_CASE("the client side tool deletes a file");
    NCL_CHECK(ncl_file_client_tool_delete(ncl_client_file_tool(client),
                                          "/data/report.txt"));
    NCL_CHECK(!ncl_path_exists("uploadFile/data/report.txt"));

    NCL_TEST_CASE("a multi-chunk file round trips over the channel");
    {
        /* 5 片（每片 256 KB）+ 零头：一次把分片、并行分片和整片校验都走到。 */
        enum { BIG_SIZE = 4 * NCL_FILE_CHUNK_SIZE + 4321 };
        static char big_payload[BIG_SIZE];
        static char big_back[BIG_SIZE];
        size_t i;
        char *restored;

        for (i = 0; i < sizeof(big_payload); i++) {
            big_payload[i] = (char)((i * 13 + 5) & 0xFF);
        }
        NCL_CHECK_EQ_INT(ncl_mkdir_p(TEST_SN "/big"), NCL_OK);
        snprintf(local, sizeof(local), "%s%cbig%cbinary.bin", TEST_SN,
                 NCL_PATH_SEP, NCL_PATH_SEP);
        NCL_CHECK_EQ_INT(ncl_file_write_all(local, big_payload,
                                            sizeof(big_payload)),
                         NCL_OK);

        NCL_CHECK_EQ_INT(ncl_client_write(client, "/big/binary.bin"), NCL_OK);
        NCL_CHECK(read_file("uploadFile/big/binary.bin", big_back,
                            sizeof(big_back), &len));
        NCL_CHECK_EQ_INT(len, sizeof(big_payload));
        NCL_CHECK(memcmp(big_back, big_payload, sizeof(big_payload)) == 0);

        /* 本地删掉再拉回来，走的是设备→本机那条 FTP 方向。 */
        NCL_CHECK_EQ_INT(ncl_path_remove(local), NCL_OK);
        restored = ncl_client_read(client, "/big/binary.bin");
        NCL_CHECK(restored != NULL);
        if (restored != NULL) {
            NCL_CHECK(read_file(restored, big_back, sizeof(big_back), &len));
            NCL_CHECK_EQ_INT(len, sizeof(big_payload));
            NCL_CHECK(memcmp(big_back, big_payload, sizeof(big_payload)) == 0);
            ncl_free_safe(restored);
        }
        NCL_CHECK(ncl_file_client_tool_delete(ncl_client_file_tool(client),
                                              "/big/binary.bin"));
        NCL_CHECK(!ncl_path_exists("uploadFile/big/binary.bin"));
    }

    NCL_TEST_CASE("methodCall file parameters use the @file marker");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
        ncl_message *response = NULL;
        const char *keys[1];
        const char *paths[1];
        char staged[512];

        snprintf(staged, sizeof(staged), "%s%cdata%creport.txt", TEST_SN,
                 NCL_PATH_SEP, NCL_PATH_SEP);
        NCL_CHECK_EQ_INT(
            ncl_file_write_all(staged, payload, sizeof(payload)), NCL_OK);
        keys[0] = "key";
        paths[0] = staged;
        NCL_CHECK(request != NULL);
        ncl_message_set_method(request, "/filesink/echo");
        {
            ncl_json *params = ncl_json_new_object();
            ncl_json_obj_set_string(params, "key", "/data/report.txt");
            ncl_message_set_params(request, params);
        }
        NCL_CHECK_EQ_INT(ncl_client_method_call_file(client, request, keys,
                                                     paths, 1, 5000, &response),
                         NCL_OK);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_json *data = response->as.method_call_response.data;
            NCL_CHECK_EQ_STR(response->as.method_call_response.code,
                             NCL_KW_CODE_OK);
            NCL_CHECK(data != NULL);
            if (data != NULL) {
                ncl_json *file_keys = ncl_json_obj_get(data, "fileKeys");
                NCL_CHECK(file_keys != NULL);
                NCL_CHECK_EQ_INT(ncl_json_arr_len(file_keys), 1);
                NCL_CHECK_EQ_STR(ncl_json_as_string(ncl_json_arr_get(file_keys, 0)),
                                 "copy");
                /* The client replaced the token with its own local copy, which
                 * is the file it staged a moment ago. */
                {
                    const char *resolved =
                        ncl_json_obj_get_string(data, "copy");
                    NCL_CHECK(resolved != NULL);
                    if (resolved != NULL) {
                        NCL_CHECK(ncl_str_ends_with(resolved, "report.txt"));
                        NCL_CHECK(read_file(resolved, buffer, sizeof(buffer),
                                            &len));
                        NCL_CHECK_EQ_INT(len, sizeof(payload));
                        NCL_CHECK(memcmp(buffer, payload, len) == 0);
                    }
                }
            }
            ncl_message_free(response);
        }
    }

    NCL_TEST_CASE("closeFileChannel revokes the login of the channel");
    {
        ncl_file_channel_options options;
        ncl_ftp_client *probe;

        /* Credentials of our own, so the test can log in with them. */
        ncl_file_channel_options_default(&options);
        options.user = "chan-user";
        options.password = "chan-pass";
        options.port = NCL_FTP_CLIENT_HOLDER_PORT;
        options.force = true;
        NCL_CHECK_EQ_INT(ncl_client_open_file_channel(client, &options), NCL_OK);
        probe = ncl_ftp_client_create("127.0.0.1", NCL_FTP_CLIENT_HOLDER_PORT,
                                      "chan-user", "chan-pass");
        NCL_CHECK(probe != NULL);
        if (probe != NULL) {
            NCL_CHECK(ncl_ftp_client_detect(probe));
            ncl_ftp_client_free(probe);
        }

        NCL_CHECK_EQ_INT(ncl_client_close_file_channel(client), NCL_OK);
        NCL_CHECK(!ncl_client_file_channel_is_open(client));
        NCL_CHECK(!ncl_server_file_channel_is_open(server));
        probe = ncl_ftp_client_create("127.0.0.1", NCL_FTP_CLIENT_HOLDER_PORT,
                                      "chan-user", "chan-pass");
        NCL_CHECK(probe != NULL);
        if (probe != NULL) {
            NCL_CHECK(!ncl_ftp_client_detect(probe));
            ncl_ftp_client_free(probe);
        }
        /* Closing twice is not an error, and the device is on its own again. */
        NCL_CHECK_EQ_INT(ncl_client_close_file_channel(client), NCL_OK);
        NCL_CHECK(ncl_client_write(client, "/data/report.txt") != NCL_OK);
    }

    NCL_TEST_CASE("the device side FTP endpoint serves the installation root");
    NCL_CHECK_EQ_INT(ncl_server_start_ftp(server), NCL_OK);
    {
        /* bin/ftp.txt was written with port 3131 by the ftp.txt test. */
        ncl_ftp_client *probe =
            ncl_ftp_client_create("127.0.0.1", 3131, "admin", "123456");
        NCL_CHECK(probe != NULL);
        if (probe != NULL) {
            char cwd[256];
            NCL_CHECK(ncl_ftp_client_detect(probe));
            NCL_CHECK_EQ_INT(ncl_ftp_client_pwd(probe, cwd, sizeof(cwd)),
                             NCL_OK);
            NCL_CHECK_EQ_STR(cwd, "/");
            NCL_CHECK_EQ_INT(ncl_ftp_client_chdir(probe, "/uploadFile"),
                             NCL_OK);
            ncl_ftp_client_free(probe);
        }
    }
    ncl_server_stop_ftp(server);

    {
        ncl_file_client_tool *tool = ncl_client_file_tool(client);
        ncl_client_set_file_tool(client, NULL);
        ncl_file_client_tool_free(tool);
    }
    ncl_client_free(client);
    ncl_server_free(server);
    NCL_CHECK(link.published > 4);
}

/**
 * conf/ftp.txt drives the client side endpoint and the handshake: the file's
 * port and account make the endpoint listen there, its host and account go into
 * openFileChannel, and a caller's own options still win over all of it.
 */
static void test_channel_config(void)
{
    direct_channel link;
    ncl_server_options server_options;
    ncl_server *server;
    ncl_client *client;
    ncl_file_channel_config config;
    ncl_file_channel_config read_back;
    ncl_file_channel_options options;
    ncl_ftp_client *probe;
    ncl_file_client_tool *tool;
    char payload[256];
    char local[512];

    NCL_TEST_CASE("conf/ftp.txt round trips");
    memset(&config, 0, sizeof(config));
    config.host = (char *)"127.0.0.1";
    config.port = 12388;
    config.user = (char *)"cfg-user";
    config.password = (char *)"cfg-pass";
    config.force = true;
    NCL_CHECK_EQ_INT(ncl_file_channel_config_write(&config), NCL_OK);
    memset(&read_back, 0, sizeof(read_back));
    NCL_CHECK_EQ_INT(ncl_file_channel_config_read(&read_back), NCL_OK);
    NCL_CHECK_EQ_STR(read_back.host, "127.0.0.1");
    NCL_CHECK_EQ_INT((int)read_back.port, 12388);
    /* advertisePort is absent, so it follows port. */
    NCL_CHECK_EQ_INT((int)read_back.advertise_port, 12388);
    NCL_CHECK_EQ_STR(read_back.user, "cfg-user");
    NCL_CHECK_EQ_STR(read_back.password, "cfg-pass");
    NCL_CHECK(read_back.force);
    NCL_CHECK(read_back.path == NULL);
    NCL_CHECK(read_back.root == NULL);
    ncl_file_channel_config_free(&read_back);

    NCL_TEST_CASE("a broken conf/ftp.txt is ignored, not fatal");
    NCL_CHECK_EQ_INT(ncl_file_write_all("conf/ftp.txt", "{ not json", 10),
                     NCL_OK);
    memset(&read_back, 0, sizeof(read_back));
    NCL_CHECK_EQ_INT(ncl_file_channel_config_read(&read_back), NCL_OK);
    NCL_CHECK(read_back.host == NULL && read_back.user == NULL &&
              read_back.port == 0 && !read_back.force);
    ncl_file_channel_config_free(&read_back);
    NCL_CHECK_EQ_INT(ncl_file_channel_config_write(&config), NCL_OK);

    NCL_TEST_CASE("the process wide endpoint follows conf/ftp.txt");
    ncl_client_holder_stop_ftp();
    NCL_CHECK_EQ_INT(ncl_client_holder_start_ftp(), NCL_OK);
    probe = ncl_ftp_client_create("127.0.0.1", 12388, "cfg-user", "cfg-pass");
    NCL_CHECK(probe != NULL);
    if (probe != NULL) {
        NCL_CHECK(ncl_ftp_client_detect(probe));
        ncl_ftp_client_free(probe);
    }

    NCL_TEST_CASE("the handshake takes host and account from conf/ftp.txt");
    memset(&link, 0, sizeof(link));
    link.channel.publish = direct_publish;
    link.channel.subscribe = direct_subscribe;
    link.channel.unsubscribe = direct_unsubscribe;
    memset(&server_options, 0, sizeof(server_options));
    server_options.sn = TEST_SN;
    server_options.model_json = kModelJson;
    server = ncl_server_create(&server_options);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        return;
    }
    link.server = server;
    NCL_CHECK_EQ_INT(ncl_server_register_file_tool(server), NCL_OK);
    client = ncl_client_create(TEST_SN, &link.channel);
    NCL_CHECK(client != NULL);
    if (client == NULL) {
        ncl_server_free(server);
        return;
    }
    link.client = client;
    tool = ncl_file_client_tool_create(client);
    NCL_CHECK(tool != NULL);
    NCL_CHECK(ncl_file_client_tool_detect(tool));
    ncl_client_set_file_tool(client, tool);
    NCL_CHECK_EQ_INT(ncl_client_subscribe(client), NCL_OK);

    /* NULL options: the file decides the address, the port and the account. */
    NCL_CHECK_EQ_INT(ncl_client_open_file_channel(client, NULL), NCL_OK);
    NCL_CHECK(ncl_server_file_channel_is_open(server));

    /* A real transfer proves the device dialled 12388 with the file's account. */
    make_payload(payload, sizeof(payload), 41);
    NCL_CHECK_EQ_INT(ncl_mkdir_p(TEST_SN "/cfgdata"), NCL_OK);
    snprintf(local, sizeof(local), "%s%ccfgdata%creport.bin", TEST_SN,
             NCL_PATH_SEP, NCL_PATH_SEP);
    NCL_CHECK_EQ_INT(ncl_file_write_all(local, payload, sizeof(payload)),
                     NCL_OK);
    NCL_CHECK_EQ_INT(ncl_client_write(client, "/cfgdata/report.bin"), NCL_OK);
    NCL_CHECK(ncl_path_exists("uploadFile/cfgdata/report.bin"));

    NCL_TEST_CASE("explicit options win over conf/ftp.txt");
    ncl_file_channel_options_default(&options);
    options.host = "127.0.0.1";
    options.port = 12388;
    options.user = "opt-user";
    options.password = "opt-pass";
    options.force = true;
    NCL_CHECK_EQ_INT(ncl_client_open_file_channel(client, &options), NCL_OK);
    probe = ncl_ftp_client_create("127.0.0.1", 12388, "opt-user", "opt-pass");
    NCL_CHECK(probe != NULL);
    if (probe != NULL) {
        /* The caller's account was registered on our endpoint for the channel. */
        NCL_CHECK(ncl_ftp_client_detect(probe));
        ncl_ftp_client_free(probe);
    }

    NCL_TEST_CASE("close drops the channel and conf/ftp.txt stays optional");
    NCL_CHECK_EQ_INT(ncl_client_close_file_channel(client), NCL_OK);
    NCL_CHECK(!ncl_server_file_channel_is_open(server));
    NCL_CHECK_EQ_INT(ncl_path_remove("conf/ftp.txt"), NCL_OK);
    memset(&read_back, 0, sizeof(read_back));
    NCL_CHECK_EQ_INT(ncl_file_channel_config_read(&read_back), NCL_OK);
    NCL_CHECK(read_back.host == NULL && read_back.port == 0);
    ncl_file_channel_config_free(&read_back);

    {
        ncl_file_client_tool *installed = ncl_client_file_tool(client);
        ncl_client_set_file_tool(client, NULL);
        ncl_file_client_tool_free(installed);
    }
    ncl_client_free(client);
    ncl_server_free(server);
    ncl_client_holder_stop_ftp();
}

/* ==================================================================== main */

NCL_TEST_MAIN_BEGIN()

    NCL_TEST_CASE("isolated working directory");
    ncl_path_remove(TEST_ROOT);
    NCL_CHECK_EQ_INT(ncl_mkdir_p(TEST_ROOT), NCL_OK);
    /* Both roles run in one process here, so the working directory is what the
     * client file tool and the client side FTP endpoint consider <cwd>. */
    NCL_CHECK_EQ_INT(test_chdir(TEST_ROOT), 0);
    ncl_env_set_root(".");

    test_checksum();
    test_utils();
    test_attribute_json();
    test_local_attribute();
    test_ftp_info();
    test_server_file_tool();
    test_end_to_end();
    test_channel_config();

    NCL_TEST_CASE("clean up");
    ncl_client_holder_stop_ftp();
    NCL_CHECK_EQ_INT(test_chdir(".."), 0);
    ncl_env_set_root(".");
    NCL_CHECK_EQ_INT(ncl_path_remove(TEST_ROOT), NCL_OK);
    NCL_CHECK(!ncl_path_exists(TEST_ROOT));

NCL_TEST_MAIN_END()
