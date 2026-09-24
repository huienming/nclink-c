/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * ncl_server - the one device program of this library.
 *
 * It is a single ncl_server device plus everything around it (MQTT, HTTP/REST,
 * sampling, audit, configuration). The vendor side is not compiled in: the
 * program loads the adapter modules in <root>/plugins, each of which *declares*
 * the points it serves (see nclink/ncl_tool.h), and hands the configuration to
 * the host layer. Nothing here knows a protocol, a point or an address.
 *
 *   ncl_server -c conf/device.json
 *   ncl_server -c conf/device.json --plugins        # what is loaded
 *   ncl_server -c conf/device.json -b tcp://10.0.0.9:1883
 *   ncl_server -c conf/device.json --once           # read every point, exit
 *   ncl_server -c conf/device.json --stats --raw    # ... then the §6 counters
 *
 * A site that would rather compile its adapter in (one .c file, no module
 * directory) writes its own main() the same way: build the declaration, hand it
 * to ncl_host_register_tool(), run the host. This file is the generic version
 * of exactly that program.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_env.h"
#include "nclink/ncl_http.h"
#include "nclink/ncl_library.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_rest.h"
#include "nclink/ncl_audit.h"
#include "nclink/ncl_host.h"
#include "nclink/ncl_module.h"

#include "tool/text.h"

typedef struct {
    const char *root;          /**< --root: conf/ bin/ plugins/ log/ live here */
    const char *config;
    const char *broker;        /**< --broker: a URL, or "-" for offline */
    const char *plugin_dir;    /**< --plugin-dir: default <root>/plugins */
    const char *plugins[8];    /**< --plugin: modules (name or file) */
    size_t      plugin_count;
    bool        plugins_list;  /**< --plugins: print what is loaded, exit */
    bool        model_dump;    /**< --model: print the model, then exit */
    bool        offline;
    bool        once;
    const char *probe;         /**< --probe: read one point, print it, exit */
    bool        stats;         /**< --stats: the §6 counters, then exit */
    bool        raw;           /**< --raw: log the frames of each request */
    const char *operator_name; /**< who is driving, for the write audit */
    unsigned    port;
    unsigned    interval_ms;
} host_args;

static void usage(const char *program)
{
    printf("用法: %s [选项]\n", program);
    printf("  -r, --root <目录>     安装根目录（conf/ bin/ plugins/ log/ 都在它下面；默认当前目录）\n");
    printf("  -c, --config <文件>   设备配置（默认 <root>/conf/device.json）\n");
    printf("  -b, --broker <URL>    MQTT broker；\"-\" 表示不接（省略 = conf/mqtt.cfg）\n");
    printf("  -P, --plugin-dir <目录>  适配器模块目录（默认 <root>/plugins）\n");
    printf("      --plugin <名字|文件> 再加载一个模块（可重复；名字= <dir>/ncl_driver_<名字>.*）\n");
    printf("      --plugins         列出已装载的适配器模块与协议，然后退出\n");
    printf("      --model           打印这份声明生成的设备模型（JSON），然后退出\n");
    printf("                        （这就是设备对外发布的模型；要现场调采样周期就\n");
    printf("                         把它存成文件、在配置里用 \"model\" 指过去）\n");
    printf("      --offline         不连 MQTT，只跑 REST 与轮询\n");
    printf("      --once            轮询一次并打印，然后退出（自检）\n");
    printf("      --probe <路径>    读一个点位并打印（走的是和客户端一样的绑定），然后退出\n");
    printf("      --stats           跑完 --once 再打印审计计数（§6），然后退出\n");
    printf("      --raw             审计里带上每次请求的原始报文 hex（§6）\n");
    printf("      --operator <名字> 写审计里的操作者（默认不写）\n");
    printf("      --port <端口>     REST 端口，0 表示随机（默认 8080）\n");
    printf("      --interval <毫秒> 轮询周期（默认 1000）\n");
}

static bool parse_args(int argc, char **argv, host_args *args)
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
        } else if (strcmp(argv[i], "-r") == 0 ||
                   strcmp(argv[i], "--root") == 0) {
            if (++i >= argc) {
                return false;
            }
            args->root = argv[i];
        } else if (strcmp(argv[i], "-b") == 0 ||
                   strcmp(argv[i], "--broker") == 0) {
            if (++i >= argc) {
                return false;
            }
            args->broker = argv[i];
            args->offline = strcmp(argv[i], "-") == 0;
        } else if (strcmp(argv[i], "-P") == 0 ||
                   strcmp(argv[i], "--plugin-dir") == 0) {
            if (++i >= argc) {
                return false;
            }
            args->plugin_dir = argv[i];
        } else if (strcmp(argv[i], "--plugin") == 0) {
            if (++i >= argc) {
                return false;
            }
            if (args->plugin_count >=
                sizeof(args->plugins) / sizeof(args->plugins[0])) {
                printf("--plugin 最多 %u 个\n",
                       (unsigned)(sizeof(args->plugins) /
                                  sizeof(args->plugins[0])));
                return false;
            }
            args->plugins[args->plugin_count++] = argv[i];
        } else if (strcmp(argv[i], "--plugins") == 0) {
            args->plugins_list = true;
        } else if (strcmp(argv[i], "--model") == 0) {
            args->model_dump = true;
            args->offline = true; /* showing the model is not going on the bus */
        } else if (strcmp(argv[i], "--offline") == 0) {
            args->offline = true;
        } else if (strcmp(argv[i], "--once") == 0) {
            args->once = true;
            args->offline = true;
        } else if (strcmp(argv[i], "--probe") == 0) {
            if (++i >= argc) {
                return false;
            }
            args->probe = argv[i];
            args->offline = true; /* a one shot probe stays off the bus */
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

/**
 * Put the broker into the configuration, the way the host wants it:
 *
 *   -b <URL>          that URL, anonymous
 *   -b - / --offline  no broker
 *   --once / --stats  no broker (a one-shot self check stays off the bus)
 *   (nothing)         <conf>/mqtt.cfg, the same file the device demos read
 */
static ncl_err config_apply_broker(ncl_json *config, const host_args *args,
                                   ncl_strbuf *err)
{
    return ncl_host_config_set_broker(config, args->broker, args->offline,
                                         err);
}

/* ---------------------------------------------------------------- modules -- */

/** One line per loaded module: the tool it declares and what it serves. */
static void log_modules(const ncl_module_set *set)
{
    size_t i;

    if (ncl_module_count(set) == 0) {
        ncl_log_info("未装载适配器模块：<root>/plugins 为空，这台设备没有点位"
                     "（只会回答文件工具）");
        return;
    }
    for (i = 0; i < ncl_module_count(set); i++) {
        const ncl_tool_decl *tool = ncl_module_tool(set, i);

        if (tool != NULL) {
            /* A declared tool is not a protocol waiting for a driver: it serves
             * its own points, so it is reported as a tool. Values and methods
             * are counted apart, the way the model sees them. */
            size_t values = 0;
            size_t methods = 0;
            size_t p;

            for (p = 0; p < tool->point_count; p++) {
                if (!ncl_tool_point_is_method(&tool->points[p])) {
                    values++;
                } else {
                    methods++;
                }
            }
            ncl_log_info("适配器模块 %s：工具 \"%s\"%s%s（%u 个点位 + %u 个方法，%s）",
                         ncl_module_path(set, i), ncl_module_name(set, i),
                         ncl_module_version(set, i) != NULL ? " " : "",
                         ncl_module_version(set, i) != NULL
                             ? ncl_module_version(set, i)
                             : "",
                         (unsigned)values, (unsigned)methods,
                         ncl_module_description(set, i) != NULL
                             ? ncl_module_description(set, i)
                             : "声明式适配器");
            continue;
        }

    }
}

/**
 * Load the host modules the configuration (and the command line) asks for.
 *
 * The default is <root>/plugins, loaded whole: a site "adds a brand" by
 * copying a module in, and the configuration's drivers section then names the
 * protocol. A "plugins" section may narrow that down (see ncl_module.h).
 */
static ncl_module_set *load_modules(const host_args *args,
                                    const ncl_json *config, ncl_strbuf *err)
{
    ncl_module_set *set = ncl_modules_create();
    const char *default_dir = args->plugin_dir != NULL
                                  ? args->plugin_dir
                                  : ncl_env_plugin_path();
    char *dir;
    size_t i;

    if (set == NULL) {
        return NULL;
    }
    (void)ncl_modules_add_config(set, config, default_dir, err);
    dir = ncl_modules_dir_from_config(config, default_dir);
    for (i = 0; i < args->plugin_count; i++) {
        if (ncl_modules_add(set, args->plugins[i], dir, err) != NCL_OK) {
            ncl_log_warn("模块 %s 未能装载: %s", args->plugins[i],
                         ncl_strbuf_cstr(err));
        }
    }
    ncl_free_safe(dir);
    return set;
}

/**
 * Refuse to start when the configuration names a tool no loaded module
 * declares: the useful sentence is "that module is not in the plugin
 * directory", not "the configuration describes no driver" three layers down.
 */
static ncl_err check_tools(const ncl_json *config, const char *plugin_dir,
                           const ncl_module_set *modules, ncl_strbuf *err)
{
    const ncl_json *tools = ncl_json_obj_get(config, "tools");
    size_t i;

    if (ncl_json_type_of(tools) != NCL_JSON_ARRAY) {
        return NCL_OK;
    }
    for (i = 0; i < ncl_json_arr_len(tools); i++) {
        const char *name =
            ncl_json_obj_get_string(ncl_json_arr_get(tools, i), "name");
        bool found = false;
        size_t m;
        char *file;

        if (ncl_str_is_blank(name)) {
            continue;
        }
        for (m = 0; m < ncl_module_count(modules); m++) {
            const ncl_tool_decl *decl = ncl_module_tool(modules, m);

            if (decl != NULL && strcmp(decl->name, name) == 0) {
                found = true;
                break;
            }
        }
        if (found) {
            continue;
        }
        file = ncl_library_file_name(name);
        (void)ncl_strbuf_printf(err,
                                "工具 \"%s\" 未装载：%s 里没有 %s"
                                "（--plugin-dir 换目录，--plugins 看已装载的模块）",
                                name, plugin_dir,
                                file != NULL ? file : "对应模块");
        ncl_free_safe(file);
        return NCL_ERR_NOT_FOUND;
    }
    return NCL_OK;
}
/**
 * Print the value of every point, for --once and for a first bring-up.
 *
 * 读不了的点位（client 那边那个协议调用还没实现，回来的码是 NCL_ERR_UNAVAILABLE）
 * 单独列出来，不算失败 —— 它们是"还没有"，不是"机床坏了"。@p pending 收到它们的
 * 个数。
 *
 * @return how many points could not be read - --once turns that into the
 *         process exit code, so a site script can tell "自检全过" from "有机床
 *         点位没读到" without reading the log.
 */
static size_t dump_points(ncl_host *host, size_t *pending)
{
    size_t failed = 0;
    size_t waiting = 0;
    size_t i;

    for (i = 0; i < ncl_host_point_count(host); i++) {
        const char *path = ncl_host_point_path(host, i);
        const ncl_json *value;
        char *text = NULL;
        ncl_strbuf note;
        ncl_err rc;

        /* The host knows how this point is served - a driver or a declared
         * module - so the self check goes through it rather than the manager. */
        ncl_strbuf_init(&note);
        rc = ncl_host_poll_one(host, path, &note);
        if (rc == NCL_ERR_UNAVAILABLE) {
            ncl_log_warn("%s = <待抓包>（%s）", path, ncl_strbuf_cstr(&note));
            ncl_strbuf_free(&note);
            waiting++;
            continue;
        }
        if (rc != NCL_OK) {
            ncl_log_warn("%s = <读取失败>（%s）", path, ncl_strbuf_cstr(&note));
            ncl_strbuf_free(&note);
            failed++;
            continue;
        }
        ncl_strbuf_free(&note);
        value = ncl_host_point_value(host, i);
        text = ncl_json_write_string(value);
        ncl_log_info("%s = %s", path, text != NULL ? text : "?");
        ncl_free_safe(text);
    }
    if (pending != NULL) {
        *pending = waiting;
    }
    return failed;
}

/**
 * --probe "<路径>": read one point and print what came back.
 *
 * It is the first thing to run when a point does not look right: the same path,
 * the same binding and the same code a client would get, without a broker, a
 * REST port or a poll round in the way. Exit code 0 when the read answered OK.
 */
static int probe_point(ncl_host *host, const char *path)
{
    ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
    ncl_query_request_item *item;
    ncl_message *response;
    ncl_query_response_item *answer;
    int exit_code = 1;

    if (request == NULL) {
        ncl_log_error("探测失败: 内存不足");
        return 1;
    }
    item = ncl_query_request_item_new(path);
    if (item == NULL) {
        ncl_message_free(request);
        return 1;
    }
    (void)ncl_params_set_string(&item->params, "operation", "get_value");
    (void)ncl_message_set_message_id(request, "probe");
    (void)ncl_message_add_query_request_item(request, item);
    response = ncl_server_invoke_query(ncl_host_server(host), request);
    ncl_message_free(request);
    if (response == NULL) {
        ncl_log_error("探测失败: 服务器没有应答");
        return 1;
    }
    answer = (ncl_query_response_item *)ncl_message_item_at(response, 0);
    if (answer != NULL && answer->code != NULL &&
        strcmp(answer->code, NCL_KW_CODE_OK) == 0) {
        char *text = ncl_json_as_text(ncl_query_response_item_data(answer));

        printf("%s = %s\n", path, text != NULL ? text : "(无值)");
        ncl_free_safe(text);
        exit_code = 0;
    } else {
        printf("%s: %s%s%s\n", path,
               answer != NULL && answer->code != NULL ? answer->code : "NG",
               answer != NULL && answer->reason != NULL ? " —— " : "",
               answer != NULL && answer->reason != NULL ? answer->reason : "");
    }
    ncl_message_free(response);
    return exit_code;
}

int main(int argc, char **argv)
{
    host_args args;
    ncl_host *host;
    ncl_json *config = NULL;
    ncl_module_set *modules = NULL;
    ncl_strbuf err;
    ncl_http_server *http = NULL;
    const char *plugin_dir;
    char default_config[NCL_PATH_MAX_BUF];
    int exit_code = 0;

    if (!parse_args(argc, argv, &args)) {
        usage(argv[0]);
        return 2;
    }
    /* The root comes first: everything below (the default config path, the
     * plugin directory, bin/sn.txt, log/) is resolved against it. */
    ncl_env_set_root(args.root);
    if (args.config == NULL) {
        snprintf(default_config, sizeof(default_config), "%s/device.json",
                 ncl_env_conf_path());
        args.config = default_config;
    }
    {
        ncl_audit_options audit;

        ncl_audit_options_default(&audit);
        audit.raw = args.raw;                   /* §6: off unless asked for */
        audit.operator_name = args.operator_name;
        ncl_audit_init(&audit);
        if (args.raw) {
            /* The frames go to the trail's request lines, which are DEBUG: the
             * hex is only interesting when it was asked for, so asking for it
             * raises the level too - otherwise `--raw` would log nothing. */
            ncl_log_set_level(NCL_LOG_DEBUG);
        }
    }
    ncl_strbuf_init(&err);
    config = ncl_tool_json_from_file(args.config, &err);
    if (config == NULL && args.plugins_list) {
        /* `--plugins` answers "what can this program talk to", which must not
         * depend on a readable device configuration. */
        ncl_log_warn("%s；只列模块", ncl_strbuf_cstr(&err));
        ncl_strbuf_reset(&err);
        config = ncl_json_new_object();
    }
    if (config == NULL) {
        ncl_log_error("适配器启动失败: %s", ncl_strbuf_cstr(&err));
        ncl_strbuf_free(&err);
        return 1;
    }
    plugin_dir = args.plugin_dir != NULL ? args.plugin_dir
                                         : ncl_env_plugin_path();

    /* The vendor adapters come in first: without them the drivers section of
     * the configuration names protocols nobody can build. */
    ncl_strbuf_reset(&err);
    modules = load_modules(&args, config, &err);
    if (modules == NULL) {
        ncl_log_error("适配器模块装载失败: %s", ncl_strbuf_cstr(&err));
        ncl_json_free(config);
        ncl_strbuf_free(&err);
        return 1;
    }
    log_modules(modules);
    if (err.len > 0) {
        ncl_log_warn("模块装载有告警: %s", ncl_strbuf_cstr(&err));
    }
    if (args.plugins_list) {
        ncl_modules_free(modules);
        ncl_json_free(config);
        ncl_strbuf_free(&err);
        return 0;
    }
    ncl_strbuf_reset(&err);
    if (check_tools(config, plugin_dir, modules, &err) != NCL_OK) {
        ncl_log_error("%s", ncl_strbuf_cstr(&err));
        ncl_modules_free(modules);
        ncl_json_free(config);
        ncl_strbuf_free(&err);
        return 1;
    }

    if (config_apply_broker(config, &args, &err) == NCL_OK) {
        host = ncl_host_create_with_modules(config, modules, &err);
    } else {
        host = NULL;
    }
    ncl_json_free(config);
    if (host == NULL) {
        ncl_log_error("适配器启动失败: %s", ncl_strbuf_cstr(&err));
        ncl_strbuf_free(&err);
        return 1;
    }
    ncl_log_info("设备 %s：声明式适配器 \"%s\" 提供 %u 个点位（模型与绑定来自模块）",
                 ncl_host_sn(host), ncl_host_tool(host)->name,
                 (unsigned)ncl_host_point_count(host));
    if (ncl_host_broker_url(host) != NULL) {
        ncl_log_info("MQTT: %s（%s）", ncl_host_broker_url(host),
                     ncl_host_online(host) ? "已连接" : "待连接");
    } else {
        ncl_log_info("离线运行：不接 broker，REST 与轮询照常");
    }


    if (args.probe != NULL) {
        exit_code = probe_point(host, args.probe);
    } else if (args.model_dump) {
        char *text = ncl_json_write_pretty_string(ncl_host_model(host));

        if (text == NULL) {
            ncl_log_error("模型打印失败: 内存不足");
            exit_code = 1;
        } else {
            printf("%s\n", text);
            ncl_free_safe(text);
        }
    } else if (args.once) {
        size_t pending = 0;
        size_t failed = dump_points(host, &pending);

        printf("自检：%u 个点位（%u 个可读，%u 个待抓包），%u 个读取失败\n",
               (unsigned)ncl_host_point_count(host),
               (unsigned)(ncl_host_point_count(host) - pending),
               (unsigned)pending, (unsigned)failed);
        if (failed > 0) {
            exit_code = 1; /* a self check that could not read is a failure */
        }
    } else {
        unsigned round = 0;
        bool machine_reported = false;
        bool broker_reported = false;

        http = ncl_http_server_create(args.port);
        if (http != NULL && ncl_http_server_start(http) == NCL_OK) {
            ncl_rest_attach(http, ncl_host_server(host));
            ncl_rest_attach_config(http);
            ncl_log_info("HTTP: http://localhost:%u/swagger-ui",
                         ncl_http_server_port(http));
        } else {
            ncl_log_warn("REST 未能启动，只跑 MQTT 与轮询");
        }
        ncl_server_register_builtin_tool(ncl_host_server(host));
        ncl_server_init_samples(ncl_host_server(host));

        for (;;) {
            size_t failed = 0;
            ncl_err result;

            ncl_strbuf_reset(&err);
            result = ncl_host_broker_poll(host, &err);
            if (result == NCL_ERR_CLOSED) {
                /* Once when it breaks, then about once a minute: a broker that
                 * is not up yet must not fill the log at the poll rate. */
                if (!broker_reported || round % 60u == 0u) {
                    ncl_log_warn("broker 未就绪: %s", ncl_strbuf_cstr(&err));
                    broker_reported = true;
                }
            } else if (broker_reported) {
                ncl_log_info("broker 恢复了");
                broker_reported = false;
            }
            ncl_strbuf_reset(&err);
            if (ncl_host_poll_round(host, &failed, &err) != NCL_OK) {
                if (!machine_reported || round % 60u == 0u) {
                    ncl_log_warn("轮询有失败（%u 个点位未应答）: %s",
                                 (unsigned)failed, ncl_strbuf_cstr(&err));
                    machine_reported = true;
                }
            } else if (machine_reported) {
                ncl_log_info("机床读取恢复正常");
                machine_reported = false;
            }
            round++;
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
    ncl_host_free(host);
    /* The modules outlive the device (their factories are still registered in
     * the process-wide registry), so they go last. */
    ncl_modules_free(modules);
    return exit_code;
}
