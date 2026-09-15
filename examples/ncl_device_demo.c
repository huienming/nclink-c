/*
 * 设备端示例（NC-Link Server）
 *
 * 演示一台设备接入 NC-Link 的完整流程：
 *   1. 指定安装根目录、初始化日志与配置（conf/bin 目录、SN）
 *   2. 从 conf/mqtt.cfg 读取 broker 参数并建立 MQTT 连接
 *   3. 装载数据模型，注册工具方法与模型路径绑定（可带参数 JSON Schema）
 *   4. 注册文件工具并启动 FTP 端点
 *   5. 启动模型里声明的采样通道
 *   6. 开放 HTTP 接口（OpenAPI 3.0 文档 + /swagger-ui + 配置接口）
 *   7. 运行期间发布一次事件，退出时按相反顺序释放资源
 *
 * 用法：ncl_device_demo [安装根目录] [运行秒数]
 *       两个参数都可省略（默认 "." 与 5 秒；真实设备应一直运行到 Ctrl+C）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_client.h"
#include "nclink/ncl_config.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_http.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_rest.h"
#include "nclink/ncl_server.h"

/* ------------------------------------------------------------------ 模型 -- */

/*
 * 一台 PLC：
 *   - 两个数据项，挂在设备下，路径即 "/STATUS" 与 "/PART_COUNT"
 *   - 再加一个 TRACE 数据项：它的工具**一次返回一批 10 个值**，用来演示
 *     亚毫秒采样（1ms 槽位里放 10 个点，报文里就是"数组套数组"）
 *   - 两个采样通道，都是 1 s 采样 / 2 s 上报：
 *       ch1 = /STATUS + 030002   → 标量列，报文是扁平数组（老格式）
 *       ch2 = /TRACE             → 批量列，报文是"槽位数组套批次数组"
 *     注意同一通道里不要混用标量列与批量列：那属于"列与列没对齐"，
 *     设备端会判为不完整丢掉（见 4.5 的完整性校验）。
 */
static const char *kModelJson =
    "{\"name\":\"nclink\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\",\"devices\":["
    "{\"id\":\"02\",\"type\":\"PLC\",\"configs\":["
    "{\"id\":\"ch1\",\"type\":\"SAMPLE_CHANNEL\","
    "\"sampleInterval\":1000,\"uploadInterval\":2000,"
    "\"ids\":[{\"id\":\"/STATUS\"},{\"id\":\"030002\"}]},"
    "{\"id\":\"ch2\",\"type\":\"SAMPLE_CHANNEL\","
    "\"sampleInterval\":1000,\"uploadInterval\":2000,"
    "\"ids\":[{\"id\":\"/TRACE\"}]}],"
    "\"dataItems\":["
    "{\"id\":\"030001\",\"type\":\"STATUS\"},"
    "{\"id\":\"030002\",\"type\":\"PART_COUNT\"},"
    "{\"id\":\"030003\",\"type\":\"TRACE\"}],\"version\":\"2.0\"}]}";

/* ------------------------------------------------------------------ 工具 -- */

/** 工具实例：真实设备这里放句柄（串口、PLC 连接等）。 */
typedef struct {
    int status;      /**< /STATUS 的当前值 */
    int part_count;  /**< /PART_COUNT 的当前值 */
    int trace_seq;   /**< /TRACE 的批次序号 */
} demo_device;

static ncl_err tool_get_status(void *instance, const ncl_json *params,
                               ncl_json **result, char **reason)
{
    demo_device *device = (demo_device *)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_int(device->status);
    return NCL_OK;
}

static ncl_err tool_set_status(void *instance, const ncl_json *params,
                               ncl_json **result, char **reason)
{
    demo_device *device = (demo_device *)instance;
    long long value = 0;

    if (!ncl_json_as_int(ncl_json_obj_get(params, "value"), &value)) {
        if (reason != NULL) {
            *reason = ncl_strdup("value 必须是整数");
        }
        return NCL_ERR_INVALID_VALUE;
    }
    device->status = (int)value;
    ncl_log_info("STATUS 被设置为 %d", device->status);
    /* 返回 NULL 会让应答 code=NG；这里返回 true 表示写入成功。 */
    *result = ncl_json_new_bool(true);
    return NCL_OK;
}

static ncl_err tool_get_part_count(void *instance, const ncl_json *params,
                                   ncl_json **result, char **reason)
{
    demo_device *device = (demo_device *)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_int(device->part_count);
    return NCL_OK;
}

/*
 * 亚毫秒采样：工具一次返回一批 10 个值 —— 相当于在 1 ms 的采样槽位里采了 10 个点
 * （0.1 ms 等效分辨率）。库不需要任何配置：返回值是数组，报文里这一列的元素
 * 就是一个内层数组；消费端用 ncl_sample_item_value_count() 等助手读它。
 */
static ncl_err tool_get_trace(void *instance, const ncl_json *params,
                              ncl_json **result, char **reason)
{
    demo_device *device = (demo_device *)instance;
    ncl_json *batch = ncl_json_new_array();
    int i;

    (void)params;
    (void)reason;
    device->trace_seq++;
    for (i = 0; i < 10; i++) {
        /* 一批示例数据：批次序号 × 100 + 批内序号 */
        ncl_json_arr_push(batch, ncl_json_new_int(device->trace_seq * 100 + i));
    }
    *result = batch;
    return NCL_OK;
}

/*
 * 参数 JSON Schema：只在收到 check=true 的 MethodCall 时使用，
 * 用于在不执行任何动作的前提下校验参数。
 */
#define SET_STATUS_SCHEMA                                                      \
    "{\"type\":\"object\",\"properties\":{"                                    \
    "\"value\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":65535}},"       \
    "\"required\":[\"value\"]}"

static const ncl_tool_method kMethods[] = {
    {"getValue", tool_get_status, NULL},
    {"setValue", tool_set_status, SET_STATUS_SCHEMA},
    {"getCount", tool_get_part_count, NULL},
    {"getTrace", tool_get_trace, NULL}};

static const ncl_tool_binding kBindings[] = {
    {"/STATUS", NCL_OP_GET_VALUE, "getValue", "plc"},
    {"/STATUS", NCL_OP_SET_VALUE, "setValue", "plc"},
    {"/PART_COUNT", NCL_OP_GET_VALUE, "getCount", "plc"},
    {"/TRACE", NCL_OP_GET_VALUE, "getTrace", "plc"}};

/* ------------------------------------------------------------- MQTT 回调 -- */

typedef struct {
    ncl_server *server;
} device_link;

/** 每收到一条请求：按主题推断类型解析，再交给服务端处理。 */
static void on_mqtt_message(void *user, const ncl_mqtt_publish *publish)
{
    device_link *link = (device_link *)user;
    ncl_message *request;

    if (publish->topic == NULL) {
        return;
    }
    request = ncl_message_parse(publish->topic,
                                (const char *)publish->payload,
                                publish->payload_len);
    if (request == NULL) {
        ncl_log_warn("无法解析来自 %s 的报文", publish->topic);
        return;
    }
    /* 内部会转交线程池处理，因此不要在收包线程上逗留。 */
    ncl_server_on_message(link->server, publish->topic, request);
}

/* ------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : ".";
    int seconds = argc > 2 ? atoi(argv[2]) : 5;
    demo_device device;
    ncl_mqtt_config config;
    ncl_mqtt_client_options mqtt_options;
    ncl_mqtt_client *mqtt = NULL;
    ncl_server_options server_options;
    ncl_server *server = NULL;
    ncl_http_server *http = NULL;
    device_link link;
    char *sn = NULL;
    int i;

    memset(&device, 0, sizeof(device));
    memset(&config, 0, sizeof(config));
    memset(&link, 0, sizeof(link));

    /* 1. 安装根目录：conf/、bin/、log/、uploadFile/ 等都在它下面。 */
    ncl_env_set_root(root);
    ncl_log_init(NULL);
    ncl_log_info("NC-Link 设备端启动，根目录 %s", ncl_env_root());

    /* 2. 准备目录并取得设备 SN。
     *
     *    运行时身份走 ncl_sn_read()：bin/sn.txt 存在就沿用，不存在才生成并
     *    持久化。
     *
     *    注意不要在这里调用 ncl_config_init(NULL, &sn)：那是 REST 的
     *    /api/cfg/init（ncl_config_init），它会**无条件覆盖** bin/sn.txt，
     *    每调用一次设备身份就变一次。 */
    ncl_mkdir_p(ncl_env_run_path());
    ncl_mkdir_p(ncl_env_conf_path());
    ncl_mkdir_p(ncl_env_log_path());
    sn = ncl_sn_read();
    if (sn == NULL) {
        ncl_log_error("无法取得设备 SN");
        return 1;
    }
    ncl_log_info("设备 SN: %s", sn);

    /* 3. 读取 conf/mqtt.cfg，连接 broker。 */
    if (ncl_mqtt_config_read(&config) != NCL_OK) {
        ncl_log_error("读取 mqtt.cfg 失败");
        free(sn);
        return 1;
    }
    ncl_log_info("MQTT 服务器: %s", config.url);
    ncl_mqtt_client_options_default(&mqtt_options);
    mqtt_options.url = config.url;
    mqtt_options.client_id = sn;              /* 设备端用 SN 做 clientId */
    mqtt_options.username = config.username;
    mqtt_options.password = config.password;
    mqtt_options.keep_alive_seconds = 60;
    mqtt_options.automatic_reconnect = true;  /* 断线自动重连并恢复订阅 */
    mqtt_options.on_message = on_mqtt_message;
    mqtt_options.user = &link;
    mqtt = ncl_mqtt_client_create(&mqtt_options);
    if (mqtt == NULL || ncl_mqtt_client_connect(mqtt) != NCL_OK) {
        ncl_log_error("MQTT 连接失败: %s",
                      mqtt != NULL ? ncl_mqtt_client_last_error(mqtt) : "?");
        ncl_mqtt_client_destroy(mqtt);
        ncl_mqtt_config_free(&config);
        free(sn);
        return 1;
    }

    /* 4. 服务端：装载模型、注册工具、订阅请求主题。 */
    memset(&server_options, 0, sizeof(server_options));
    server_options.sn = sn;
    server_options.mqtt = mqtt;
    server_options.model_json = kModelJson;
    server = ncl_server_create(&server_options);
    if (server == NULL) {
        ncl_log_error("服务端创建失败");
        goto cleanup;
    }
    link.server = server;
    ncl_server_register_tool(server, "plc", &device, kMethods,
                             sizeof(kMethods) / sizeof(kMethods[0]), kBindings,
                             sizeof(kBindings) / sizeof(kBindings[0]));
    ncl_server_register_builtin_tool(server);   /* /nclinkServer/addSample 等 */
    ncl_server_register_file_tool(server);      /* /CONTROLLER/FILE */
    ncl_server_subscribe(server);

    /* 5. FTP 端点：端口与账号来自 bin/ftp.txt（缺省 admin/123456/2121）。 */
    ncl_server_start_ftp(server);

    /* 6. 采样：启动模型里声明的 SAMPLE_CHANNEL。 */
    ncl_server_init_samples(server);

    /* 7. HTTP：OpenAPI 3.0 文档、Swagger UI 与配置接口。 */
    http = ncl_http_server_create(9008);
    if (http != NULL && ncl_http_server_start(http) == NCL_OK) {
        ncl_rest_attach(http, server);
        ncl_rest_attach_config(http);
        ncl_log_info("HTTP 接口: http://localhost:9008/swagger-ui");
    }

    /* 8. 运行：真实设备这里一直循环，示例只在指定秒数内跑一会儿。 */
    ncl_log_info("运行 %d 秒（Ctrl+C 可随时退出）", seconds);
    for (i = 0; i < seconds * 10; i++) {
        ncl_sleep_millis(100);
        device.part_count++;                   /* 模拟产量累加 */

        /* 每秒发布一条事件到 Event/<sn>：time 与 @id 会自动补齐。 */
        if (i % 10 == 9) {
            ncl_json *event = ncl_json_new_object();
            ncl_json_obj_set_string(event, "key", "PART_COUNT");
            ncl_json_obj_set_int(event, "value", device.part_count);
            ncl_json_obj_set_int(event, "oldValue", device.part_count - 1);
            ncl_server_push_event(server, "030002", event);
            ncl_json_free(event);
        }
    }

    ncl_log_info("采样上报次数: %u", (unsigned)ncl_server_sample_upload_count(server));
    ncl_log_info("已发布事件数: %u", (unsigned)ncl_server_event_count(server));

cleanup:
    /* 释放顺序：先停服务（采样线程、FTP），再拆连接，最后是配置与 SN。 */
    if (http != NULL) {
        ncl_http_server_stop(http);
        ncl_http_server_free(http);
    }
    if (server != NULL) {
        ncl_server_free(server);   /* 内部会停采样、FTP 与文件工具 */
    }
    ncl_mqtt_client_disconnect(mqtt);
    ncl_mqtt_client_destroy(mqtt);
    ncl_mqtt_config_free(&config);
    ncl_log_info("设备端已退出");
    ncl_log_shutdown();
    ncl_env_shutdown();
    free(sn);
    return 0;
}
