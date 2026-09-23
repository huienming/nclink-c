/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 huienming
 *
 * 把 client 的公开接口**挨个问一遍**，逐条打 `rc` + `last_error` —— 盘"FANUC 这套还有
 * 哪些没通"用（01 册 §11.11.1 那张现状表就是它跑出来的）。
 *
 * 用法：
 *     focas_sweep.exe <host> [port]
 *
 * 读法：**只看 `rc != 0` 的行**（`last_error` 是黏的，`rc=0` 的行里带的是上一次的错话，
 * 看着像错其实不是）。表/数组那种会连"拿到几条"一起打出来，空表也是信息。
 *
 * 需要"逐条问某个 item 的原始应答"时用同目录的 `focas_item`，试连接/帧走哪条用
 * `focas_channel_probe.py`。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/clients/focas.h"
#include "nclink/ncl_platform.h"

static ncl_focas *F;
static const char *const kAxes[] = { "X", "Y", "Z", "A", "C" };

static void r(const char *what, ncl_err rc)
{
    const char *e = ncl_focas_last_error(F);

    printf("%-34s rc=%-10d %s\n", what, rc, (e != NULL && e[0] != '\0') ? e : "");
}

/** 表/数组那种：除了 rc，再把"拿到几条"打出来（空表也是信息）。 */
static void rj(const char *what, ncl_err rc, const ncl_json *json)
{
    const char *e = ncl_focas_last_error(F);
    long long len = -1;

    if (json != NULL) {
        len = ncl_json_type_of(json) == NCL_JSON_ARRAY ? ncl_json_arr_len(json)
                                                       : ncl_json_obj_len(json);
    }
    printf("%-34s rc=%-10d 条数=%-5lld %s\n", what, rc, len,
           (rc != NCL_OK && e != NULL && e[0] != '\0') ? e : "");
}

#define SHOW(expr) r(#expr, (expr))
/** 表/数组：先算值再打（rj 的实参求值顺序不保证，别把调用和 json 一起传） */
#define SHOWJ(expr) do { ncl_err _rc = (expr); rj(#expr, _rc, json); } while (0)

int main(int argc, char **argv)
{
    ncl_focas_config config;
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    unsigned port = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 8193u;
    char buf[256];
    char *err = NULL;
    char *text = NULL;
    size_t len = 0;
    double d = 0;
    long long n = 0;
    ncl_json *json = NULL;
    ncl_json *fields = NULL;
    bool on = false;
    size_t i;

    ncl_focas_config_default(&config);
    config.host = host;
    config.port = (unsigned short)port;
    config.negotiate = true;
    config.timeout_ms = 15000;
    F = ncl_focas_open(&config, &err);
    if (F == NULL) {
        printf("open 失败: %s\n", err != NULL ? err : "?");
        return 1;
    }
    printf("== %s:%u connected=%d\n", host, port, (int)ncl_focas_connected(F));

    memset(buf, 0, sizeof(buf));
    SHOW(ncl_focas_status(F, buf, sizeof(buf)));
    memset(buf, 0, sizeof(buf));
    SHOW(ncl_focas_mode(F, buf, sizeof(buf)));
    SHOW(ncl_focas_emergency(F, &on));
    SHOW(ncl_focas_alarm_status(F, &n));
    json = NULL;
    SHOWJ(ncl_focas_alarm(F, &json));
    ncl_json_free(json);
    for (i = 0; i < (size_t)NCL_FOCAS_AXIS_COUNT; i++) {
        char tag[64];
        ncl_focas_axis ax = (ncl_focas_axis)i;

#define AXW(fmt, call)                                                         \
    do {                                                                       \
        snprintf(tag, sizeof(tag), fmt, kAxes[i]);                             \
        r(tag, (call));                                                        \
    } while (0)
        AXW("axis_position %s", ncl_focas_axis_position(F, ax, &d));
        AXW("axis_position_machine %s", ncl_focas_axis_position_machine(F, ax, &d));
        AXW("axis_position_relative %s", ncl_focas_axis_position_relative(F, ax, &d));
        AXW("axis_position_cmd %s", ncl_focas_axis_position_cmd(F, ax, &d));
        AXW("axis_distance %s", ncl_focas_axis_distance(F, ax, &d));
        AXW("axis_srv_delay %s", ncl_focas_axis_srv_delay(F, ax, &d));
        AXW("axis_feedrate %s", ncl_focas_axis_feedrate(F, ax, &d));
        AXW("axis_load %s", ncl_focas_axis_load(F, ax, &d));
        AXW("axis_torque %s", ncl_focas_axis_torque(F, ax, &d));
        AXW("axis_current %s", ncl_focas_axis_current(F, ax, &d));
        AXW("axis_temperature %s", ncl_focas_axis_temperature(F, ax, &d));
#undef AXW
    }
    memset(buf, 0, sizeof(buf));
    SHOW(ncl_focas_axis_type(F, NCL_FOCAS_AXIS_X, buf, sizeof(buf)));
    SHOW(ncl_focas_spindle_speed(F, 0, &d));
    SHOW(ncl_focas_spindle_load(F, 0, &d));
    SHOW(ncl_focas_feed_speed(F, &d));
    SHOW(ncl_focas_feed_override(F, &d));
    SHOW(ncl_focas_spindle_override(F, &d));

    memset(buf, 0, sizeof(buf));
    SHOW(ncl_focas_program_name(F, buf, sizeof(buf)));
    SHOW(ncl_focas_program_number(F, &n));
    SHOW(ncl_focas_main_program_number(F, &n));
    SHOW(ncl_focas_subprogram_number(F, &n));
    memset(buf, 0, sizeof(buf));
    SHOW(ncl_focas_line_number(F, buf, sizeof(buf)));
    memset(buf, 0, sizeof(buf));
    SHOW(ncl_focas_executed_block(F, buf, sizeof(buf)));
    json = NULL;
    SHOWJ(ncl_focas_program_directory(F, &json));
    ncl_json_free(json);
    json = NULL;
    SHOWJ(ncl_focas_modal(F, &json));
    ncl_json_free(json);

    SHOW(ncl_focas_tool_number(F, &n));
    SHOW(ncl_focas_part_count(F, &n));
    SHOW(ncl_focas_tool_group_count(F, &n));
    SHOW(ncl_focas_tool_offset_count(F, &n));
    SHOW(ncl_focas_timer(F, NCL_FOCAS_TIMER_POWER_ON, &n));
    json = NULL;
    SHOW(ncl_focas_tool_offset(F, 1, &json));
    ncl_json_free(json);
    json = NULL;
    SHOW(ncl_focas_tool_offset_typed(F, 1, 1, &json)); /* 1 = 半径 */
    ncl_json_free(json);
    json = NULL;
    SHOWJ(ncl_focas_tool_list(F, &json));
    ncl_json_free(json);
    json = NULL;
    SHOW(ncl_focas_tool_param(F, 1, &json));
    ncl_json_free(json);
    json = NULL;
    SHOWJ(ncl_focas_tool_param_table(F, &json));
    ncl_json_free(json);
    SHOW(ncl_focas_tool_life(F, 1, &n));

    json = NULL;
    SHOW(ncl_focas_parameter(F, 6711, &json));
    ncl_json_free(json);
    json = NULL;
    SHOWJ(ncl_focas_parameter_table(F, &json));
    ncl_json_free(json);
    json = NULL;
    SHOW(ncl_focas_macro_variable(F, 100, &json));
    ncl_json_free(json);
    json = NULL;
    SHOW(ncl_focas_macro_variables(F, 100, 5, &json));
    ncl_json_free(json);
    json = NULL;
    SHOWJ(ncl_focas_variable_table(F, &json));
    ncl_json_free(json);
    json = NULL;
    SHOW(ncl_focas_work_offset(F, "G54", &json));
    ncl_json_free(json);
    json = NULL;
    SHOWJ(ncl_focas_work_offsets(F, &json));
    ncl_json_free(json);

    json = NULL;
    SHOWJ(ncl_focas_system(F, &json));
    ncl_json_free(json);
    memset(buf, 0, sizeof(buf));
    SHOW(ncl_focas_model(F, buf, sizeof(buf)));
    memset(buf, 0, sizeof(buf));
    SHOW(ncl_focas_version(F, buf, sizeof(buf)));
    memset(buf, 0, sizeof(buf));
    SHOW(ncl_focas_manufacturer(F, buf, sizeof(buf)));
    json = NULL;
    SHOW(ncl_focas_read_item(F, "ODBSYS", 0, 18, NCL_DTYPE_BYTE, &json));
    ncl_json_free(json);

    /* 写这一侧（值给的是"跟当前值不同"的那种，才看得出是不是真写进去了） */
    SHOW(ncl_focas_tool_offset_write(F, 1, "0.008"));
    SHOW(ncl_focas_tool_offset_write_typed(F, 1, 1, 0.009));
    fields = ncl_json_new_object();
    (void)ncl_json_obj_set_string(fields, "radius", "0.010");
    SHOW(ncl_focas_tool_param_write(F, 1, fields));
    ncl_json_free(fields);
    SHOW(ncl_focas_parameter_write(F, 6711, "0"));
    SHOW(ncl_focas_macro_write(F, 100, 1.0));

    /* 程序那一家族 */
    SHOW(ncl_focas_program_create(F, "//CNC_MEM/USER/PATH1/O0600", false));
    text = NULL;
    len = 0;
    SHOW(ncl_focas_program_upload(F, 0, "O0600", &text, &len));
    ncl_free_safe(text);
    SHOW(ncl_focas_program_select_main(F, "O0600"));
    SHOW(ncl_focas_program_delete(F, "//CNC_MEM/USER/PATH1/O0600"));
    SHOW(ncl_focas_program_download(F, 0, "//CNC_MEM/USER/PATH1/",
                                    "\nO0600\nG01 X1 Y2\nM30\n%"));
    SHOW(ncl_focas_program_delete(F, "//CNC_MEM/USER/PATH1/O0600"));

    SHOW(ncl_focas_call(F, "session", NULL, NULL));
    ncl_focas_close(F);
    return 0;
}
