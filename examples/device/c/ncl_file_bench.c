/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * File channel throughput bench / cross-host test rig.
 *
 * Two roles, meant to run on two machines (or in a container and on the host)
 * with one broker in between:
 *
 *   ncl_file_bench device <broker> <sn> [seconds]
 *       A device: registers the "file" tool, subscribes, and waits. It prints
 *       what its own FTP client moved when it exits.
 *
 *   ncl_file_bench client <broker> <sn> <size_mb> [host] [port] [passive]
 *       A controller: opens the file channel ... by default with the derived
 *       address; pass host/port for the device to dial (host.docker.internal,
 *       a LAN address, a NAT mapping), and "passive" when this side cannot be
 *       dialled back by the device.
 *       It uploads and downloads <size_mb> MiB and prints one row per
 *       direction: seconds, MiB/s and the bytes the endpoint really moved.
 *       A "resume" row pre-seeds half of the file and reports what the resumed
 *       transfer costs.
 *
 * Example (two machines, controller behind NAT):
 *   device:  ./ncl_file_bench device tcp://broker:1883 V2BENCH0001 120
 *   client:  ./ncl_file_bench client tcp://broker:1883 V2BENCH0001 64 10.0.0.7 2323 passive
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_mqtt.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_server.h"

#include "device_model.h"

/* --------------------------------------------------------------- helpers -- */

static double now_ms(void)
{
    return (double)ncl_time_monotonic_millis();
}

/** The pattern both sides use, fed to the streamed SHA-256. */
static void pattern_block(char *block, size_t len, unsigned long long offset)
{
    size_t i;

    for (i = 0; i < len; i++) {
        block[i] = (char)(((offset + i) * 17 + 3) & 0xFF);
    }
}

/** Expected SHA-256 of a file of @p size bytes (heap hex, caller frees). */
static char *pattern_checksum(long long size)
{
    enum { STEP = 256 * 1024 };
    char block[STEP];
    ncl_sha256 *ctx = ncl_sha256_new();
    char *hex = NULL;
    long long done = 0;

    if (ctx == NULL) {
        return NULL;
    }
    while (done < size) {
        long long left = size - done;
        size_t chunk = left < (long long)sizeof(block) ? (size_t)left
                                                      : sizeof(block);
        pattern_block(block, chunk, (unsigned long long)done);
        ncl_sha256_update(ctx, block, chunk);
        done += (long long)chunk;
    }
    hex = ncl_sha256_finish(ctx);
    ncl_sha256_free(ctx);
    return hex;
}

/** True when @p path hashes to @p expected. */
static bool file_matches(const char *path, const char *expected)
{
    char *hex = NULL;
    bool same = false;

    if (ncl_file_checksum(path, &hex) == NCL_OK && hex != NULL) {
        same = strcmp(hex, expected) == 0;
    }
    ncl_free_safe(hex);
    return same;
}

static void report(const char *what, long long bytes, double ms,
                   long long wire_bytes)
{
    double seconds = ms / 1000.0;
    double mib = (double)bytes / (1024.0 * 1024.0);

    printf("%-18s %8.2f MiB  %7.3f s  %8.2f MiB/s  线上 %lld 字节\n", what, mib,
           seconds, seconds > 0.0 ? mib / seconds : 0.0, wire_bytes);
    fflush(stdout);
}

/** A file of @p size bytes in <cwd>/<sn>/<name>, as the channel expects. */
static bool make_local(const char *sn, const char *name, long long size,
                       char *out, size_t out_len)
{
    enum { STEP = 256 * 1024 };
    static char block[STEP];
    FILE *fp;
    long long written = 0;
    size_t i;
    char dir[NCL_PATH_MAX_BUF];

    snprintf(dir, sizeof(dir), "%s%c%s", ".", NCL_PATH_SEP, sn);
    ncl_mkdir_p(dir);
    snprintf(out, out_len, "%s%c%s", dir, NCL_PATH_SEP, name);
    (void)i;
    fp = fopen(out, "wb");
    if (fp == NULL) {
        return false;
    }
    while (written < size) {
        long long left = size - written;
        size_t chunk = left < (long long)sizeof(block) ? (size_t)left
                                                      : sizeof(block);

        pattern_block(block, chunk, (unsigned long long)written);
        if (fwrite(block, 1, chunk, fp) != chunk) {
            fclose(fp);
            return false;
        }
        written += (long long)chunk;
    }
    return fclose(fp) == 0;
}

/* ------------------------------------------------------------------ device -- */

/** The device role's server: the MQTT callback needs it, so it is global. */
static ncl_server *g_bench_server;

/** Inbound MQTT publish -> the server's dispatch (never block this thread). */
static void on_mqtt_message(void *user, const ncl_mqtt_publish *publish)
{
    ncl_message *request;

    (void)user;
    if (publish->topic == NULL) {
        return;
    }
    request = ncl_message_parse(publish->topic,
                                (const char *)publish->payload,
                                publish->payload_len);
    if (request == NULL) {
        return;
    }
    ncl_server_on_message(g_bench_server, publish->topic, request);
}

static int run_device(const char *broker, const char *sn, int seconds)
{
    ncl_mqtt_client_options mqtt_options;
    ncl_mqtt_client *mqtt;
    ncl_server_options options;
    ncl_server *server;
    int i;

    ncl_mqtt_client_options_default(&mqtt_options);
    mqtt_options.url = broker;
    mqtt_options.client_id = sn;
    mqtt_options.keep_alive_seconds = 60;
    mqtt_options.automatic_reconnect = true;
    mqtt_options.on_message = on_mqtt_message;
    mqtt = ncl_mqtt_client_create(&mqtt_options);
    if (mqtt == NULL || ncl_mqtt_client_connect(mqtt) != NCL_OK) {
        ncl_log_error("MQTT 连接失败: %s",
                      mqtt != NULL ? ncl_mqtt_client_last_error(mqtt) : "?");
        return 1;
    }
    memset(&options, 0, sizeof(options));
    options.sn = sn;
    options.mqtt = mqtt;
    options.model_json = ncl_demo_device_model();
    server = ncl_server_create(&options);
    if (server == NULL) {
        ncl_log_error("设备端创建失败");
        return 1;
    }
    g_bench_server = server;
    ncl_server_register_file_tool(server);
    ncl_server_subscribe(server);
    printf("device ready: sn=%s, %d s\n", sn, seconds);
    fflush(stdout);
    for (i = 0; i < seconds * 10; i++) {
        ncl_sleep_millis(100);
    }
    printf("device done: 通道状态 %s\n",
           ncl_server_file_channel_is_open(server) ? "开" : "关");
    ncl_server_free(server);
    g_bench_server = NULL;
    ncl_mqtt_client_disconnect(mqtt);
    ncl_mqtt_client_destroy(mqtt);
    return 0;
}

/* ------------------------------------------------------------------ client -- */

static int run_client(const char *broker, const char *sn, int size_mb,
                      const char *host, unsigned port, bool passive)
{
    ncl_file_channel_options options;
    ncl_client *client;
    ncl_ftp_server *endpoint;
    char local[NCL_PATH_MAX_BUF];
    char remote_name[64];
    char remote[NCL_PATH_MAX_BUF];
    long long size = (long long)size_mb * 1024 * 1024;
    long long wire_before;
    double started;
    char *back;
    char *expected;

    if (ncl_client_holder_init(broker, NULL, NULL) != NCL_OK) {
        ncl_log_error("连 broker 失败: %s", broker);
        return 1;
    }
    client = ncl_client_holder_get(sn);
    if (client == NULL) {
        ncl_log_error("取不到设备客户端: %s", sn);
        return 1;
    }
    ncl_file_channel_options_default(&options);
    options.host = host;
    options.port = port;
    options.passive = passive;
    /* Take the channel over: a bench run is a fresh controller, and a lease
     * left by the previous run (which exited without closing) would refuse. */
    options.force = true;
    if (ncl_client_open_file_channel(client, &options) != NCL_OK) {
        ncl_log_error("开文件通道失败");
        return 1;
    }
    endpoint = ncl_client_holder_ftp_endpoint();
    /* One name per size: a leftover mirror from an earlier run would otherwise
     * turn the transfer into a resume and skew the numbers. */
    snprintf(remote_name, sizeof(remote_name), "bench-%d.bin", size_mb);
    snprintf(remote, sizeof(remote), "/%s", remote_name);
    if (!make_local(sn, remote_name, size, local, sizeof(local))) {
        ncl_log_error("造测试文件失败: %s", local);
        return 1;
    }
    printf("size=%d MiB  host=%s port=%u passive=%s\n", size_mb,
           host != NULL ? host : "(推导)", port, passive ? "yes" : "no");
    expected = pattern_checksum(size);
    if (expected == NULL) {
        ncl_log_error("算不出期望校验和");
        return 1;
    }

    /* upload */
    /* The device pulls the file, so the endpoint *serves* those bytes. */
    wire_before = ncl_ftp_server_bytes_sent(endpoint);
    started = now_ms();
    if (ncl_client_write(client, remote) != NCL_OK) {
        ncl_log_error("上传失败");
        return 1;
    }
    report("upload", size, now_ms() - started,
           ncl_ftp_server_bytes_sent(endpoint) - wire_before);

    /* download */
    remove(local);
    /* The device pushes the file to us, so the endpoint receives them. */
    wire_before = ncl_ftp_server_bytes_received(endpoint);
    started = now_ms();
    back = ncl_client_read(client, remote);
    if (back == NULL) {
        ncl_log_error("下载失败");
        return 1;
    }
    report("download", size, now_ms() - started,
           ncl_ftp_server_bytes_received(endpoint) - wire_before);
    printf("  verify download: %s\n",
           file_matches(back, expected) ? "byte-identical" : "MISMATCH");
    ncl_free_safe(back);

    /* resume: hand the peer the first half and time the remainder */
    remove(local);
    if (make_local(sn, remote_name, size / 2, local, sizeof(local))) {
        /* The local mirror now holds the first half of the pattern, and the
         * FTP server serves that path: the device's upload sees SIZE = size/2
         * and only sends the rest. */
        wire_before = ncl_ftp_server_bytes_received(endpoint);
        started = now_ms();
        back = ncl_client_read(client, remote);
        if (back == NULL) {
            ncl_log_error("续传失败");
            return 1;
        }
        report("resume (half)", size / 2, now_ms() - started,
               ncl_ftp_server_bytes_received(endpoint) - wire_before);
        printf("  verify resume:   %s\n",
               file_matches(back, expected) ? "byte-identical" : "MISMATCH");
        ncl_free_safe(back);
    }
    ncl_free_safe(expected);
    ncl_client_close_file_channel(client);
    ncl_client_holder_shutdown();
    return 0;
}

int main(int argc, char **argv)
{
    ncl_log_init(NULL);
    ncl_log_set_level(NCL_LOG_WARN);

    if (argc < 4) {
        printf("usage: %s device <broker> <sn> [seconds]\n", argv[0]);
        printf("       %s client <broker> <sn> <size_mb> [host] [port] "
               "[passive]\n",
               argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "device") == 0) {
        int seconds = argc > 4 ? atoi(argv[4]) : 60;
        return run_device(argv[2], argv[3], seconds > 0 ? seconds : 60);
    }
    if (strcmp(argv[1], "client") == 0) {
        int size_mb = argc > 4 ? atoi(argv[4]) : 16;
        const char *host = argc > 5 ? argv[5] : NULL;
        unsigned port = argc > 6 ? (unsigned)atoi(argv[6]) : 0;
        bool passive = argc > 7 && strcmp(argv[7], "passive") == 0;

        return run_client(argv[2], argv[3], size_mb > 0 ? size_mb : 16, host,
                          port, passive);
    }
    printf("unknown role: %s\n", argv[1]);
    return 2;
}
