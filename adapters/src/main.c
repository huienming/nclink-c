/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * ncl_adapter - the adapter daemon.
 *
 * Reads a driver configuration, exposes every point as an operation of an
 * NC-Link device, keeps the model's values current and (unless asked not to)
 * brings up the MQTT transport and the REST endpoints.
 *
 *   ncl_adapter -c conf/adapter.json
 *   ncl_adapter -c conf/adapter.json --once      # read every point and exit
 *   ncl_adapter -c conf/adapter.json --stats --raw  # ... then the §6 counters
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_http.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_rest.h"
#include "nclink_adapter/ncl_audit.h"
#include "nclink_adapter/ncl_adapter.h"

typedef struct {
    const char *config;
    bool        offline;
    bool        once;
    bool        stats;         /**< --stats: the §6 counters, then exit */
    bool        raw;           /**< --raw: log the frames of each request */
    const char *operator_name; /**< who is driving, for the write audit */
    unsigned    port;
    unsigned    interval_ms;
} adapter_args;

static void usage(const char *program)
{
    printf("用法: %s [选项]\n", program);
    printf("  -c, --config <文件>   适配器配置（默认 conf/adapter.json）\n");
    printf("      --offline         不连 MQTT，只跑 REST 与轮询\n");
    printf("      --once            轮询一次并打印，然后退出（自检）\n");
    printf("      --stats           跑完 --once 再打印审计计数（§6），然后退出\n");
    printf("      --raw             审计里带上每次请求的原始报文 hex（§6）\n");
    printf("      --operator <名字> 写审计里的操作者（默认不写）\n");
    printf("      --port <端口>     REST 端口，0 表示随机（默认 8080）\n");
    printf("      --interval <毫秒> 轮询周期（默认 1000）\n");
}

static bool parse_args(int argc, char **argv, adapter_args *args)
{
    int i;

    memset(args, 0, sizeof(*args));
    args->port = 8080;
    args->interval_ms = 1000;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (++i >= argc) {
                return false;
            }
            args->config = argv[i];
        } else if (strcmp(argv[i], "--offline") == 0) {
            args->offline = true;
        } else if (strcmp(argv[i], "--once") == 0) {
            args->once = true;
            args->offline = true;
        } else if (strcmp(argv[i], "--stats") == 0) {
            args->stats = true;
            args->once = true; /* the counters are only worth reading after a run */
            args->offline = true;
        } else if (strcmp(argv[i], "--raw") == 0) {
            args->raw = true;
        } else if (strcmp(argv[i], "--operator") == 0) {
            if (++i >= argc) {
                return false;
            }
            args->operator_name = argv[i];
        } else if (strcmp(argv[i], "--port") == 0) {
            if (++i >= argc) {
                return false;
            }
            args->port = (unsigned)atoi(argv[i]);
        } else if (strcmp(argv[i], "--interval") == 0) {
            if (++i >= argc) {
                return false;
            }
            args->interval_ms = (unsigned)atoi(argv[i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            return false;
        } else {
            printf("无法识别的参数: %s\n", argv[i]);
            return false;
        }
    }
    return true;
}

/** Print the value of every point, for --once and for a first bring-up. */
static void dump_points(ncl_adapter *adapter)
{
    ncl_driver_manager *manager = ncl_adapter_drivers(adapter);
    size_t i;

    for (i = 0; i < ncl_adapter_point_count(adapter); i++) {
        const char *path = ncl_adapter_point_path(adapter, i);
        ncl_json *value = NULL;
        char *text = NULL;

        if (ncl_driver_manager_read(manager, path, &value) != NCL_OK) {
            ncl_log_warn("%s = <读取失败>", path);
            continue;
        }
        text = ncl_json_write_string(value);
        ncl_log_info("%s = %s", path, text != NULL ? text : "?");
        ncl_free_safe(text);
        ncl_json_free(value);
    }
}

int main(int argc, char **argv)
{
    adapter_args args;
    ncl_adapter *adapter;
    ncl_strbuf err;
    ncl_http_server *http = NULL;
    char default_config[NCL_PATH_MAX_BUF];
    int exit_code = 0;

    if (!parse_args(argc, argv, &args)) {
        usage(argv[0]);
        return 2;
    }
    if (args.config == NULL) {
        snprintf(default_config, sizeof(default_config), "%s/adapter.json",
                 ncl_env_conf_path());
        args.config = default_config;
    }
    {
        ncl_audit_options audit;

        ncl_audit_options_default(&audit);
        audit.raw = args.raw;                   /* §6: off unless asked for */
        audit.operator_name = args.operator_name;
        ncl_audit_init(&audit);
    }
    ncl_strbuf_init(&err);
    adapter = ncl_adapter_create_from_file(args.config, &err);
    if (adapter == NULL) {
        ncl_log_error("适配器启动失败: %s", ncl_strbuf_cstr(&err));
        ncl_strbuf_free(&err);
        return 1;
    }
    ncl_log_info("设备 %s：%u 个点位、%u 个方法（%u 条驱动链路）",
                 ncl_adapter_sn(adapter),
                 (unsigned)ncl_adapter_point_count(adapter),
                 (unsigned)ncl_adapter_method_count(adapter),
                 (unsigned)ncl_driver_manager_count(ncl_adapter_drivers(adapter)));

    /* Opening every session up front makes an offline device visible at
     * start-up; a failure is not fatal, reads open on demand anyway. */
    ncl_strbuf_reset(&err);
    if (ncl_driver_manager_open_all(ncl_adapter_drivers(adapter), &err) !=
        NCL_OK) {
        ncl_log_warn("部分链路未连通: %s", ncl_strbuf_cstr(&err));
    }
    if (args.once) {
        dump_points(adapter);
    } else {
        if (!args.offline && ncl_server_subscribe(ncl_adapter_server(adapter)) !=
                                 NCL_OK) {
            ncl_log_warn("MQTT 订阅失败，继续以离线模式运行");
        }
        http = ncl_http_server_create(args.port);
        if (http != NULL && ncl_http_server_start(http) == NCL_OK) {
            ncl_rest_attach(http, ncl_adapter_server(adapter));
            ncl_rest_attach_config(http);
            ncl_log_info("HTTP: http://localhost:%u/swagger-ui",
                         ncl_http_server_port(http));
        } else {
            ncl_log_warn("REST 未能启动，只跑 MQTT 与轮询");
        }
        ncl_server_register_builtin_tool(ncl_adapter_server(adapter));
        ncl_server_init_samples(ncl_adapter_server(adapter));

        for (;;) {
            ncl_strbuf_reset(&err);
            if (ncl_adapter_poll(adapter, &err) != NCL_OK) {
                ncl_log_warn("轮询有失败: %s", ncl_strbuf_cstr(&err));
            }
            ncl_sleep_millis(args.interval_ms);
        }
    }
    ncl_strbuf_free(&err);
    if (args.stats) {
        ncl_json *stats = ncl_audit_stats();
        char *text = stats != NULL ? ncl_json_write_string(stats) : NULL;

        if (text != NULL) {
            printf("%s\n", text);
            ncl_free_safe(text);
        } else {
            ncl_log_warn("审计计数取不到");
            exit_code = 1;
        }
        ncl_json_free(stats);
    }
    if (http != NULL) {
        ncl_http_server_stop(http);
    }
    ncl_adapter_free(adapter);
    return exit_code;
}
