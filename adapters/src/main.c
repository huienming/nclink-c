/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * ncl_adapter - the adapter host: one NC-Link device program.
 *
 * It is a single ncl_server plus everything around it (MQTT, REST, sampling,
 * audit). The vendor side is not compiled in: the host loads adapter modules
 * (plugins/ncl_driver_<protocol>.dll / .so) at start-up, registers the driver
 * factories they hand over, and then builds the device from the configuration
 * file - which is also where the point map lives.
 *
 *   ncl_adapter -c conf/fanuc.json
 *   ncl_adapter -c conf/fanuc.json --plugins        # what is loaded
 *   ncl_adapter -c conf/fanuc.json -b tcp://10.0.0.9:1883
 *   ncl_adapter -c conf/fanuc.json --once           # read every point, exit
 *   ncl_adapter -c conf/fanuc.json --stats --raw    # ... then the §6 counters
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
#include "nclink_adapter/ncl_audit.h"
#include "nclink_adapter/ncl_adapter.h"
#include "nclink_adapter/ncl_module.h"

#include "core/adapter_text.h"

typedef struct {
    const char *root;          /**< --root: conf/ bin/ plugins/ log/ live here */
    const char *config;
    const char *broker;        /**< --broker: a URL, or "-" for offline */
    const char *plugin_dir;    /**< --plugin-dir: default <root>/plugins */
    const char *plugins[8];    /**< --plugin: modules (name or file) */
    size_t      plugin_count;
    bool        plugins_list;  /**< --plugins: print what is loaded, exit */
    bool        offline;
    bool        once;
    const char *probe;         /**< --probe: read one point, print it, exit */
    bool        stats;         /**< --stats: the §6 counters, then exit */
    bool        raw;           /**< --raw: log the frames of each request */
    const char *operator_name; /**< who is driving, for the write audit */
    unsigned    port;
    unsigned    interval_ms;
} adapter_args;

static void usage(const char *program)
{
    printf("用法: %s [选项]\n", program);
    printf("  -r, --root <目录>     安装根目录（conf/ bin/ plugins/ log/ 都在它下面；默认当前目录）\n");
    printf("  -c, --config <文件>   设备配置（默认 <root>/conf/adapter.json）\n");
    printf("  -b, --broker <URL>    MQTT broker；\"-\" 表示不接（省略 = conf/mqtt.cfg）\n");
    printf("  -P, --plugin-dir <目录>  适配器模块目录（默认 <root>/plugins）\n");
    printf("      --plugin <名字|文件> 再加载一个模块（可重复；名字= <dir>/ncl_driver_<名字>.*）\n");
    printf("      --plugins         列出已装载的适配器模块与协议，然后退出\n");
    printf("      --offline         不连 MQTT，只跑 REST 与轮询\n");
    printf("      --once            轮询一次并打印，然后退出（自检）\n");
    printf("      --probe <路径>    读一个点位并打印（走的是和客户端一样的绑定），然后退出\n");
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
static ncl_err config_apply_broker(ncl_json *config, const adapter_args *args,
                                   ncl_strbuf *err)
{
    return ncl_adapter_config_set_broker(config, args->broker, args->offline,
                                         err);
}

/* ---------------------------------------------------------------- modules -- */

/** One line per loaded module, plus what the built-in registry holds. */
static void log_modules(const ncl_module_set *set)
{
    size_t i;

    if (ncl_module_count(set) == 0) {
        ncl_log_info("未装载适配器模块（内置协议 %u 个）",
                     (unsigned)ncl_driver_protocol_count());
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
                if (tool->points[p].readable || tool->points[p].writable) {
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
        ncl_log_info("适配器模块 %s：协议 \"%s\"%s%s%s（%s）",
                     ncl_module_path(set, i), ncl_module_name(set, i),
                     ncl_module_registered(set, i) ? "" : "（未注册）",
                     ncl_module_version(set, i) != NULL ? " " : "",
                     ncl_module_version(set, i) != NULL
                         ? ncl_module_version(set, i)
                         : "",
                     ncl_module_description(set, i) != NULL
                         ? ncl_module_description(set, i)
                         : "");
    }
}

/**
 * Load the adapter modules the configuration (and the command line) asks for.
 *
 * The default is <root>/plugins, loaded whole: a site "adds a brand" by
 * copying a module in, and the configuration's drivers section then names the
 * protocol. A "plugins" section may narrow that down (see ncl_module.h).
 */
static ncl_module_set *load_modules(const adapter_args *args,
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
    if (ncl_modules_register(set, err) != NCL_OK) {
        ncl_log_warn("部分模块未能注册: %s", ncl_strbuf_cstr(err));
    }
    return set;
}

/**
 * Refuse to start when a configured link names a protocol nobody registered:
 * the useful sentence is "that module is not in the plugin directory", not
 * "unknown protocol" three layers down.
 */
/**
 * A configuration that names a protocol nobody can build is the most common
 * bring-up mistake, so it is reported before the device starts. A protocol a
 * loaded module *declares* counts as buildable too: such a module serves its
 * points itself (the "drivers" entry it still matches only carries the
 * connection parameters).
 */
static const char *tool_serving(const ncl_module_set *modules,
                                const char *protocol)
{
    size_t i;

    for (i = 0; modules != NULL && i < ncl_module_count(modules); i++) {
        if (ncl_module_tool(modules, i) == NULL) {
            continue;
        }
        if (ncl_module_name(modules, i) != NULL &&
            strcmp(ncl_module_name(modules, i), protocol) == 0) {
            return ncl_module_name(modules, i);
        }
    }
    return NULL;
}

/**
 * The other half of the same check: a "tools" entry names a module that has to
 * be loaded, because that module is what declares the device's points. A tool
 * that is not there is reported by the file name the loader would have looked
 * for, which is what a site needs to see.
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

static ncl_err check_protocols(const ncl_json *config, const char *plugin_dir,
                               const ncl_module_set *modules, ncl_strbuf *err)
{
    const ncl_json *drivers = ncl_json_obj_get(config, "drivers");
    size_t i;

    if (ncl_json_type_of(drivers) != NCL_JSON_ARRAY) {
        return NCL_OK;
    }
    for (i = 0; i < ncl_json_arr_len(drivers); i++) {
        const char *protocol =
            ncl_json_obj_get_string(ncl_json_arr_get(drivers, i), "type");

        if (ncl_str_is_blank(protocol) || ncl_driver_protocol_known(protocol) ||
            tool_serving(modules, protocol) != NULL) {
            continue;
        }
        {
            char *file = ncl_library_file_name(protocol);

            (void)ncl_strbuf_printf(err,
                                    "协议 \"%s\" 未注册：%s 里没有 %s"
                                    "（--plugin-dir 换目录，--plugins 看已装载的模块）",
                                    protocol, plugin_dir,
                                    file != NULL ? file : "对应模块");
            ncl_free_safe(file);
        }
        return NCL_ERR_NOT_FOUND;
    }
    return NCL_OK;
}

/**
 * Print the value of every point, for --once and for a first bring-up.
 *
 * @return how many points could not be read - --once turns that into the
 *         process exit code, so a site script can tell "自检全过" from "有机床
 *         点位没读到" without reading the log.
 */
static size_t dump_points(ncl_adapter *adapter)
{
    size_t failed = 0;
    size_t i;

    for (i = 0; i < ncl_adapter_point_count(adapter); i++) {
        const char *path = ncl_adapter_point_path(adapter, i);
        const ncl_json *value;
        char *text = NULL;
        ncl_strbuf note;

        /* The adapter knows how this point is served - a driver or a declared
         * module - so the self check goes through it rather than the manager. */
        ncl_strbuf_init(&note);
        if (ncl_adapter_poll_one(adapter, path, &note) != NCL_OK) {
            ncl_log_warn("%s = <读取失败>（%s）", path, ncl_strbuf_cstr(&note));
            ncl_strbuf_free(&note);
            failed++;
            continue;
        }
        ncl_strbuf_free(&note);
        value = ncl_adapter_point_value(adapter, i);
        text = ncl_json_write_string(value);
        ncl_log_info("%s = %s", path, text != NULL ? text : "?");
        ncl_free_safe(text);
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
static int probe_point(ncl_adapter *adapter, const char *path)
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
    response = ncl_server_invoke_query(ncl_adapter_server(adapter), request);
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
    adapter_args args;
    ncl_adapter *adapter;
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
    config = ncl_adapter_json_from_file(args.config, &err);
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
    if (check_protocols(config, plugin_dir, modules, &err) != NCL_OK ||
        check_tools(config, plugin_dir, modules, &err) != NCL_OK) {
        ncl_log_error("%s", ncl_strbuf_cstr(&err));
        ncl_modules_free(modules);
        ncl_json_free(config);
        ncl_strbuf_free(&err);
        return 1;
    }

    if (config_apply_broker(config, &args, &err) == NCL_OK) {
        adapter = ncl_adapter_create_with_modules(config, modules, &err);
    } else {
        adapter = NULL;
    }
    ncl_json_free(config);
    if (adapter == NULL) {
        ncl_log_error("适配器启动失败: %s", ncl_strbuf_cstr(&err));
        ncl_strbuf_free(&err);
        return 1;
    }
    if (ncl_adapter_tool(adapter) != NULL) {
        ncl_log_info("设备 %s：声明式适配器 \"%s\" 提供 %u 个点位（模型与绑定来自模块）",
                     ncl_adapter_sn(adapter), ncl_adapter_tool(adapter)->name,
                     (unsigned)ncl_adapter_point_count(adapter));
    } else {
        ncl_log_info("设备 %s：%u 个点位、%u 个方法（%u 条驱动链路）",
                     ncl_adapter_sn(adapter),
                     (unsigned)ncl_adapter_point_count(adapter),
                     (unsigned)ncl_adapter_method_count(adapter),
                     (unsigned)ncl_driver_manager_count(
                         ncl_adapter_drivers(adapter)));
    }
    if (ncl_adapter_broker_url(adapter) != NULL) {
        ncl_log_info("MQTT: %s（%s）", ncl_adapter_broker_url(adapter),
                     ncl_adapter_online(adapter) ? "已连接" : "待连接");
    } else {
        ncl_log_info("离线运行：不接 broker，REST 与轮询照常");
    }

    /* Opening every session up front makes an offline device visible at
     * start-up; a failure is not fatal, reads open on demand anyway. */
    ncl_strbuf_reset(&err);
    if (ncl_driver_manager_open_all(ncl_adapter_drivers(adapter), &err) !=
        NCL_OK) {
        ncl_log_warn("部分链路未连通: %s", ncl_strbuf_cstr(&err));
    }
    if (args.probe != NULL) {
        exit_code = probe_point(adapter, args.probe);
    } else if (args.once) {
        size_t failed = dump_points(adapter);

        printf("自检：%u 个点位，%u 个读取失败\n",
               (unsigned)ncl_adapter_point_count(adapter), (unsigned)failed);
        if (failed > 0) {
            exit_code = 1; /* a self check that could not read is a failure */
        }
    } else {
        unsigned round = 0;
        bool machine_reported = false;
        bool broker_reported = false;

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
            size_t failed = 0;
            ncl_err result;

            ncl_strbuf_reset(&err);
            result = ncl_adapter_broker_poll(adapter, &err);
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
            if (ncl_adapter_poll_round(adapter, &failed, &err) != NCL_OK) {
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
    ncl_adapter_free(adapter);
    /* The modules outlive the device (their factories are still registered in
     * the process-wide registry), so they go last. */
    ncl_modules_free(modules);
    return exit_code;
}
