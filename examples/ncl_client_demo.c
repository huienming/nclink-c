/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * 客户端示例（NC-Link Client）
 *
 * 演示上位机/网关访问一台 NC-Link 设备的完整流程：
 *   1. 初始化客户端管理器（内部建立 MQTT 连接与 FTP 端点）
 *   2. 按 SN 取得设备客户端，probe 拉取设备模型
 *   3. 读值、写值、区间读值
 *   4. 参数校验（check）、方法调用
 *   5. 订阅采样上报（Sample/<sn>/<通道id>）与设备事件，按通道逐项打印数值
 *   6. 文件上传/下载与目录列举
 *
 * 用法：ncl_client_demo [broker 地址] [设备 SN] [运行秒数]
 *       broker 省略时读 conf/mqtt.cfg；运行秒数建议大于设备的 uploadInterval
 *       （示例设备为 2 s），否则可能收不到采样上报。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_client.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_model.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_topic.h"

/* ------------------------------------------------------------- 事件回调 -- */

static int g_events;
static int g_samples;
static int g_bad_samples;

/** 事件回调：msg 在返回后立即释放，不能保存指针。 */
static void on_event(ncl_client *client, const char *topic,
                     const ncl_message *msg, void *user)
{
    const char *key = ncl_json_obj_get_string(msg->as.event.event, "key");
    long long value = ncl_json_obj_get_int(msg->as.event.event, "value", 0);

    (void)client;
    (void)user;
    g_events++;
    ncl_log_info("收到事件 [%s] id=%s key=%s value=%lld", topic,
                 msg->as.event.id != NULL ? msg->as.event.id : "?",
                 key != NULL ? key : "?", value);
}

/* ------------------------------------------------------------- 采样回调 -- */

/**
 * 把模型里声明的采集通道与采样项打出来。
 *
 * 采样报文的表头（paths）就是照这里声明的顺序一项项对下来的；设备没带表头
 * 时，客户端会拿报文里的通道 id 到模型里找同名 SAMPLE_CHANNEL 来补，所以先
 * 确认模型里到底有哪些通道、每个通道有几个采样项。
 */
static void log_sample_channels(const ncl_node *node)
{
    size_t i;

    if (node == NULL) {
        return;
    }
    if (ncl_node_is_sample_node(node)) {
        size_t count = ncl_node_sample_count(node);

        ncl_log_info("模型里的采集通道 %s: %u 个采样项",
                     node->id != NULL ? node->id : "?", (unsigned)count);
        for (i = 0; i < count; i++) {
            ncl_sample_ref *ref = ncl_node_sample_at(node, i);
            char *path = ref != NULL ? ncl_sample_ref_path(ref) : NULL;

            ncl_log_info("    [%u] %s", (unsigned)i,
                         path != NULL ? path : "(未解析)");
            free(path);
        }
    }
    for (i = 0; ncl_node_device_at(node, i) != NULL; i++) {
        log_sample_channels(ncl_node_device_at(node, i));
    }
    for (i = 0; ncl_node_component_at(node, i) != NULL; i++) {
        log_sample_channels(ncl_node_component_at(node, i));
    }
    for (i = 0; ncl_node_config_at(node, i) != NULL; i++) {
        log_sample_channels(ncl_node_config_at(node, i));
    }
}

/**
 * 采样回调：设备按 uploadInterval 聚合后上报一个 Sample 报文。
 *   topic                         Sample/<sn>/<通道id>
 *   msg->as.sample.id             通道 id（与主题末段一致）
 *   interval / upload_interval    采样周期 / 上报周期（毫秒）
 *   paths[i]                      第 i 个采样项的路径
 *   data[i]                       第 i 个采样项的数据（values 数组，按时间先后）
 */
static void on_sample(ncl_client *client, const char *topic,
                      const ncl_message *msg, void *user)
{
    size_t items = ncl_message_item_count(msg);
    size_t paths = ncl_strvec_len(&msg->as.sample.paths);
    size_t i;

    (void)client;
    (void)user;
    /* 设备端保证只发完整报文，这里再兜一层：表头与数据块对不上就忽略。 */
    if (!ncl_message_sample_is_complete(msg)) {
        ncl_node *root = ncl_client_root_node(client);
        ncl_node *channel = (root != NULL && msg->as.sample.id != NULL)
                                ? ncl_node_find_by_id(root, msg->as.sample.id)
                                : NULL;

        g_bad_samples++;
        ncl_log_warn("采样报文不完整，已忽略: %s", topic);
        /* 头几条把原因说清楚：补表头要求"模型里有同名 SAMPLE_CHANNEL，且它的
         * 采样项数 == 报文 data 列数"，两条缺一条就补不出来。 */
        if (g_bad_samples <= 3) {
            ncl_log_warn("    通道 id=%s；模型里%s；模型采样项=%u，报文 data 列数=%u",
                         msg->as.sample.id != NULL ? msg->as.sample.id
                                                   : "(报文没带 id)",
                         channel != NULL ? "有同名 SAMPLE_CHANNEL"
                                         : "没有同名 SAMPLE_CHANNEL",
                         (unsigned)(channel != NULL
                                        ? ncl_node_sample_count(channel)
                                        : 0),
                         (unsigned)items);
        }
        if (g_bad_samples == 1) {
            char *raw = ncl_message_write_string(msg);

            if (raw != NULL) {
                ncl_log_warn("    报文前 300 字符: %.300s", raw);
                free(raw);
            }
        }
        return;
    }
    g_samples++;
    ncl_log_info("收到采样 [%s] 通道=%s 采样周期=%lldms 上报周期=%lldms 采样项=%u",
                 topic,
                 msg->as.sample.id != NULL ? msg->as.sample.id : "?",
                 msg->as.sample.interval, msg->as.sample.upload_interval,
                 (unsigned)items);

    /* 表头：本次采集了哪些数据项。报文字段是数组 "paths":[...]，
     * 每一项与下面 data 里同下标的采样项一一对应。 */
    {
        ncl_json *header = ncl_strvec_to_json(&msg->as.sample.paths);
        char *text = header != NULL ? ncl_json_write_string(header) : NULL;
        ncl_log_info("    表头 paths(%u 项) = %s", (unsigned)paths,
                     text != NULL ? text : "[]");
        free(text);
        ncl_json_free(header);
    }

    /* 首次收到时把原始报文打出来，方便对接方逐字段核对。 */
    if (g_samples == 1) {
        char *raw = ncl_message_write_string(msg);
        ncl_log_info("    原始报文: %s", raw != NULL ? raw : "");
        free(raw);
    }

    for (i = 0; i < items; i++) {
        const ncl_sample_item *item =
            (const ncl_sample_item *)ncl_message_item_at(msg, i);
        const char *path = i < paths ? ncl_strvec_at(&msg->as.sample.paths, i)
                                     : "?";
        size_t slots = item != NULL ? ncl_json_arr_len(item->data) : 0;
        size_t points = ncl_sample_item_value_count(item);

        /* 亚毫秒采样：一个槽位里是"一批值"，元素本身是数组。
         * 用助手读就不必自己判断两层结构。 */
        if (ncl_sample_item_is_nested(item)) {
            const ncl_json *first = ncl_sample_item_value_at(item, 0);
            char *text = first != NULL ? ncl_json_as_text(first) : NULL;
            ncl_log_info("    %-16s 批量采样: %u 个槽位 × 每槽约 %u 点 = %u 点，首个=%s",
                         path, (unsigned)slots,
                         (unsigned)(slots > 0 ? points / slots : 0),
                         (unsigned)points, text != NULL ? text : "null");
            free(text);
        } else {
            ncl_strbuf line;
            size_t v;

            ncl_strbuf_init(&line);
            for (v = 0; v < slots; v++) {
                char *text = ncl_json_as_text(ncl_json_arr_get(item->data, v));
                ncl_strbuf_printf(&line, "%s%s", v == 0 ? "" : ", ",
                                  text != NULL ? text : "null");
                free(text);
            }
            ncl_log_info("    %-16s 编码=%s 本轮 %u 个值: [%s]", path,
                         /* 未设置 encoding 表示按原始 JSON 值传输 */
                         item != NULL && item->encoding != NULL ? item->encoding
                                                                : "raw",
                         (unsigned)slots, ncl_strbuf_cstr(&line));
            ncl_strbuf_free(&line);
        }
    }
}

/* ------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    const char *url = argc > 1 ? argv[1] : NULL;
    const char *sn = argc > 2 ? argv[2] : "HNC_VDEV_00000";
    int seconds = argc > 3 ? atoi(argv[3]) : 8;
    ncl_mqtt_config config;
    ncl_client *client;
    ncl_json *value = NULL;
    ncl_message *response = NULL;
    int i;

    memset(&config, 0, sizeof(config));
    ncl_log_init(NULL);

    /* 1. 初始化：宿主进程只需调用一次；内部建立 MQTT 连接并启动 FTP 端点。 */
    if (url == NULL) {
        if (ncl_mqtt_config_read(&config) != NCL_OK) {
            ncl_log_error("读取 conf/mqtt.cfg 失败");
            return 1;
        }
        url = config.url;
    }
    ncl_log_info("连接 %s ...", url);
    if (ncl_client_holder_init(url, config.username, config.password) != NCL_OK) {
        ncl_log_error("客户端管理器初始化失败");
        ncl_mqtt_config_free(&config);
        return 1;
    }

    /* 2. 取得设备客户端（按 SN 缓存，30 分钟空闲过期）。 */
    client = ncl_client_holder_get(sn);
    if (client == NULL) {
        ncl_log_error("无法取得设备客户端: %s", sn);
        ncl_client_holder_shutdown();
        ncl_mqtt_config_free(&config);
        return 1;
    }

    /* 3. probe：让设备回传完整模型。
     *    模型的**所有权**要显式交给客户端：先从响应里脱开，再装进客户端，
     *    之后就能用路径与节点 id 互查（getValue 也可以直接用 id）。 */
    if (ncl_client_probe(client, 5000, &response) == NCL_OK &&
        response != NULL) {
        ncl_node *model = ncl_message_take_model(response);
        if (model != NULL) {
            ncl_client_set_root_node(client, model);
        }
        ncl_message_free(response);
        response = NULL;
        {
            ncl_node *root = ncl_client_root_node(client);
            char *id = ncl_client_get_id(client, "/STATUS");
            ncl_log_info("设备模型已装载: %s，/STATUS 的节点 id = %s",
                         root != NULL && ncl_node_path(root) != NULL
                             ? ncl_node_path(root)
                             : "(空)",
                         id != NULL ? id : "?");
            free(id);
            log_sample_channels(root);
        }
    } else {
        ncl_log_warn("probe 失败，继续尝试直接读值");
    }

    /* 4. 读值 / 区间读值。 */
    if (ncl_client_get_value(client, "/STATUS", 5000, &value) == NCL_OK) {
        long long number = 0;
        ncl_json_as_int(value, &number);
        ncl_log_info("GET /STATUS = %lld", number);
        ncl_json_free(value);
        value = NULL;
    }
    if (ncl_client_get_value_range(client, "/PART_COUNT", 0, 9, 5000,
                                   &value) == NCL_OK) {
        ncl_json_free(value);
        value = NULL;
    }

    /* 5. 写值：返回 NCL_OK 表示设备接受了这次写入。 */
    if (ncl_client_set_value(client, "/STATUS", ncl_json_new_int(42), 5000) ==
        NCL_OK) {
        ncl_log_info("SET /STATUS = 42 成功");
    } else {
        ncl_log_warn("SET /STATUS 失败或被拒绝");
    }

    /* 6. 参数校验：check=true 时设备只校验参数、不执行动作。 */
    {
        ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
        ncl_json *params = ncl_json_new_object();

        ncl_json_obj_set_int(params, "value", 99999); /* 超出 schema 上限 */
        ncl_message_set_method(request, "/plc/setValue");
        ncl_message_set_params(request, params);
        ncl_message_set_check(request, true);
        if (ncl_client_method_call(client, request, 5000, &response) == NCL_OK &&
            response != NULL) {
            ncl_log_info("check 结果: code=%s reason=%s",
                         response->as.method_call_response.code != NULL
                             ? response->as.method_call_response.code
                             : "?",
                         response->as.method_call_response.reason != NULL
                             ? response->as.method_call_response.reason
                             : "(无)");
            ncl_message_free(response);
            response = NULL;
        }
    }

    /* 7. 先订阅采样与事件：这两个是设备主动推的，越早订越好。
     *    采样主题用通配符 "Sample/<sn>/#"，一次覆盖所有采样通道。 */
    ncl_client_set_sample_handler(client, on_sample, NULL);
    if (ncl_client_subscribe_samples(client, 0) == NCL_OK) {
        ncl_log_info("已订阅采样主题: %s", ncl_client_sample_topic(client));
    } else {
        ncl_log_error("订阅采样主题失败");
    }
    ncl_client_set_event_handler(client, on_event, NULL);
    ncl_client_subscribe_events(client, 2);

    /* 8. 文件传输：写一个本地文件到设备，再读回来并列举目录。 */
    {
        const char *text = "hello NC-Link\n";
        char staged[NCL_PATH_MAX_BUF];
        ncl_err rc;

        /* 约定：文件通道里的路径是相对于 <cwd>/<sn>/ 的，所以先把本地文件
         * 放到那个目录，再用同样的相对路径调用 write()。 */
        ncl_mkdir_p(sn);
        snprintf(staged, sizeof(staged), "%s%cdemo.txt", sn, NCL_PATH_SEP);
        rc = ncl_file_write_all(staged, text, strlen(text));
        if (rc != NCL_OK) {
            ncl_log_error("写本地文件 %s 失败: %s", staged, ncl_err_name(rc));
        }
        /* 上传：文件先落到 <cwd>/<sn>/demo.txt，再经 /CONTROLLER/FILE 与 FTP
         * 送到设备（默认收在设备的 uploadFile/ 下）。 */
        rc = ncl_client_write(client, "/demo.txt");
        if (rc != NCL_OK) {
            ncl_log_error("上传 /demo.txt 失败: %s", ncl_err_name(rc));
        }
        {
            /* 读回同一个相对路径：设备侧它相对 uploadFile/，
             * 客户端侧相对 <cwd>/<sn>/。 */
            char *local = ncl_client_read(client, "/demo.txt");
            ncl_ptrvec files;
            size_t n;

            ncl_log_info("文件回传路径: %s", local != NULL ? local : "(失败)");
            free(local);

            ncl_ptrvec_init(&files, ncl_file_attribute_release);
            if (ncl_client_ll(client, "/", &files) == NCL_OK) {
                for (n = 0; n < ncl_ptrvec_len(&files); n++) {
                    const ncl_file_attribute *attribute =
                        (const ncl_file_attribute *)ncl_ptrvec_at(&files, n);
                    ncl_log_info("  远端文件 %s (%lld 字节)",
                                 attribute->file_name, attribute->file_size);
                }
            }
            ncl_ptrvec_free(&files);
        }
    }

    /* 9. 停在订阅窗口里：设备的采样上报（Sample）与事件（Event）会陆续到达。 */
    for (i = 0; i < seconds * 10; i++) {
        ncl_sleep_millis(100);
    }
    ncl_client_unsubscribe_events(client);
    ncl_client_unsubscribe_samples(client);
    ncl_log_info("共收到 %d 条事件、%d 条采样上报", g_events, g_samples);

    /* 10. 释放：先撤掉文件通道与事件回调，再关掉整个 holder。 */
    ncl_client_holder_shutdown();
    ncl_mqtt_config_free(&config);
    ncl_log_info("客户端已退出");
    ncl_log_shutdown();
    return 0;
}
