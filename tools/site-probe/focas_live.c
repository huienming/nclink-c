/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * focas_live —— 拿**这一份 client** 去接一台**真的 FOCAS 服务端**（机床或仿真器），
 * 逐条把语义接口读一遍，把"读到了什么 / 读不到的理由"打出来。
 *
 * 为什么要它：clients/tests/test_focas.c 里那台假机床是**我们自己写的**，它认的帧格式
 * 就是我们自己发的 —— 过了只说明自洽。真服务端过了，才说明 01 册 §2.2/§2.3 那套
 * （握手四条、块布局、item 码表）是照真机抄来的。
 *
 *   focas_live 192.168.110.192 8193            # 全部条目跑一遍
 *   focas_live 192.168.110.192 8193 --raw      # 每条再打一次"发了什么/回了什么"
 *   focas_live 192.168.110.192 8193 --session  # 只看握手（hello 的 43 条记录）
 *
 * 退出码：全部读得到 = 0，有读不到的 = 1（"读不到"分两种：NCL_ERR_UNAVAILABLE 是
 * 这一份 client 还没核完帧的点位，别的错误码就是对着这台服务端出的问题）。
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/clients/focas.h"
#include "nclink/ncl_common.h"
#include "nclink/ncl_json.h"

static int g_raw = 0;

static void dump_hex(const char *tag, const uint8_t *data, size_t len)
{
    size_t i;

    if (data == NULL || len == 0) {
        printf("    %-6s (empty)\n", tag);
        return;
    }
    printf("    %-6s %zu B:", tag, len);
    for (i = 0; i < len; i++) {
        if (i % 16u == 0u) {
            printf("\n      %04zu  ", i);
        }
        printf("%02x ", data[i]);
    }
    printf("\n");
}

/** Print the exchange a call just made (the driver keeps the last one). */
static void show_raw(ncl_focas *focas)
{
    ncl_driver_raw raw;

    if (!g_raw) {
        return;
    }
    memset(&raw, 0, sizeof(raw));
    ncl_focas_last_raw(focas, &raw);
    dump_hex("tx", raw.request, raw.request_len);
    dump_hex("rx", raw.reply, raw.reply_len);
}

/* ------------------------------------------------------------- formatters -- */

typedef ncl_err (*live_fn)(ncl_focas *focas, char *out, size_t cap);

static ncl_err fmt_text(ncl_err err, const char *text, char *out, size_t cap)
{
    if (err == NCL_OK) {
        snprintf(out, cap, "%s", text != NULL ? text : "");
    }
    return err;
}

static ncl_err fmt_ll(ncl_err err, long long value, char *out, size_t cap)
{
    if (err == NCL_OK) {
        snprintf(out, cap, "%lld", value);
    }
    return err;
}

static ncl_err fmt_double(ncl_err err, double value, char *out, size_t cap)
{
    if (err == NCL_OK) {
        snprintf(out, cap, "%.6g", value);
    }
    return err;
}

static ncl_err fmt_bool(ncl_err err, bool value, char *out, size_t cap)
{
    if (err == NCL_OK) {
        snprintf(out, cap, "%s", value ? "true" : "false");
    }
    return err;
}

static ncl_err fmt_json(ncl_err err, ncl_json *value, char *out, size_t cap)
{
    char *text = NULL;

    if (err == NCL_OK && value != NULL) {
        text = ncl_json_write_string(value);
        snprintf(out, cap, "%s", text != NULL ? text : "?");
        ncl_free_safe(text);
    }
    ncl_json_free(value);
    return err;
}

/* ------------------------------------------------------------- the calls -- */

static ncl_err live_status(ncl_focas *f, char *out, size_t cap)
{
    char text[64] = "";
    ncl_err err = ncl_focas_status(f, text, sizeof(text));
    return fmt_text(err, text, out, cap);
}

static ncl_err live_mode(ncl_focas *f, char *out, size_t cap)
{
    char text[64] = "";
    ncl_err err = ncl_focas_mode(f, text, sizeof(text));
    return fmt_text(err, text, out, cap);
}

static ncl_err live_emergency(ncl_focas *f, char *out, size_t cap)
{
    bool on = false;
    ncl_err err = ncl_focas_emergency(f, &on);
    return fmt_bool(err, on, out, cap);
}

static ncl_err live_alarm_status(ncl_focas *f, char *out, size_t cap)
{
    long long bits = 0;
    ncl_err err = ncl_focas_alarm_status(f, &bits);
    if (err == NCL_OK) {
        snprintf(out, cap, "0x%llx", bits);
    }
    return err;
}

static ncl_err live_alarm(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_alarm(f, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_program_name(ncl_focas *f, char *out, size_t cap)
{
    char text[128] = "";
    ncl_err err = ncl_focas_program_name(f, text, sizeof(text));
    return fmt_text(err, text, out, cap);
}

static ncl_err live_program_number(ncl_focas *f, char *out, size_t cap)
{
    long long value = 0;
    ncl_err err = ncl_focas_program_number(f, &value);
    return fmt_ll(err, value, out, cap);
}

static ncl_err live_main_program_number(ncl_focas *f, char *out, size_t cap)
{
    long long value = 0;
    ncl_err err = ncl_focas_main_program_number(f, &value);
    return fmt_ll(err, value, out, cap);
}

static ncl_err live_subprogram_number(ncl_focas *f, char *out, size_t cap)
{
    long long value = 0;
    ncl_err err = ncl_focas_subprogram_number(f, &value);
    return fmt_ll(err, value, out, cap);
}

static ncl_err live_line_number(ncl_focas *f, char *out, size_t cap)
{
    char text[64] = "";
    ncl_err err = ncl_focas_line_number(f, text, sizeof(text));
    return fmt_text(err, text, out, cap);
}

static ncl_err live_executed_block(ncl_focas *f, char *out, size_t cap)
{
    char text[128] = "";
    ncl_err err = ncl_focas_executed_block(f, text, sizeof(text));
    return fmt_text(err, text, out, cap);
}

static ncl_err live_program_directory(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_program_directory(f, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_tool_number(ncl_focas *f, char *out, size_t cap)
{
    long long value = 0;
    ncl_err err = ncl_focas_tool_number(f, &value);
    return fmt_ll(err, value, out, cap);
}

static ncl_err live_part_count(ncl_focas *f, char *out, size_t cap)
{
    long long value = 0;
    ncl_err err = ncl_focas_part_count(f, &value);
    return fmt_ll(err, value, out, cap);
}

static ncl_err live_tool_group_count(ncl_focas *f, char *out, size_t cap)
{
    long long value = 0;
    ncl_err err = ncl_focas_tool_group_count(f, &value);
    return fmt_ll(err, value, out, cap);
}

static ncl_err live_timer_power(ncl_focas *f, char *out, size_t cap)
{
    long long seconds = 0;
    ncl_err err = ncl_focas_timer(f, NCL_FOCAS_TIMER_POWER_ON, &seconds);
    return fmt_ll(err, seconds,
                  out, cap);
}

static ncl_err live_timer_operating(ncl_focas *f, char *out, size_t cap)
{
    long long seconds = 0;
    ncl_err err = ncl_focas_timer(f, NCL_FOCAS_TIMER_OPERATING, &seconds);
    return fmt_ll(err, seconds,
                  out, cap);
}

static ncl_err live_timer_cutting(ncl_focas *f, char *out, size_t cap)
{
    long long seconds = 0;
    ncl_err err = ncl_focas_timer(f, NCL_FOCAS_TIMER_CUTTING, &seconds);
    return fmt_ll(err, seconds,
                  out, cap);
}

static ncl_err live_tool_list(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_tool_list(f, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_tool_offset(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_tool_offset(f, 1, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_tool_life(ncl_focas *f, char *out, size_t cap)
{
    long long value = 0;
    ncl_err err = ncl_focas_tool_life(f, 1, &value);
    return fmt_ll(err, value, out, cap);
}

static ncl_err live_macro_variable(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_macro_variable(f, 100, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_macro_variables(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_macro_variables(f, 1, 5, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_parameter(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_parameter(f, 1, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_tool_param(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_tool_param(f, 1, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_tool_param_table(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_tool_param_table(f, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_parameter_table(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_parameter_table(f, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_variable_table(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_variable_table(f, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_work_offset(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_work_offset(f, "G54", &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_work_offsets(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_work_offsets(f, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_modal(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_modal(f, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_system(ncl_focas *f, char *out, size_t cap)
{
    ncl_json *value = NULL;
    ncl_err err = ncl_focas_system(f, &value);
    return fmt_json(err, value, out, cap);
}

static ncl_err live_model(ncl_focas *f, char *out, size_t cap)
{
    char text[64] = "";
    ncl_err err = ncl_focas_model(f, text, sizeof(text));
    return fmt_text(err, text, out, cap);
}

static ncl_err live_version(ncl_focas *f, char *out, size_t cap)
{
    char text[64] = "";
    ncl_err err = ncl_focas_version(f, text, sizeof(text));
    return fmt_text(err, text, out, cap);
}

static ncl_err live_manufacturer(ncl_focas *f, char *out, size_t cap)
{
    char text[64] = "";
    ncl_err err = ncl_focas_manufacturer(f, text, sizeof(text));
    return fmt_text(err, text, out, cap);
}

static ncl_err live_feed_speed(ncl_focas *f, char *out, size_t cap)
{
    double value = 0.0;
    ncl_err err = ncl_focas_feed_speed(f, &value);
    return fmt_double(err, value, out, cap);
}

static ncl_err live_feed_override(ncl_focas *f, char *out, size_t cap)
{
    double value = 0.0;
    ncl_err err = ncl_focas_feed_override(f, &value);
    return fmt_double(err, value, out, cap);
}

static ncl_err live_spindle_override(ncl_focas *f, char *out, size_t cap)
{
    double value = 0.0;
    ncl_err err = ncl_focas_spindle_override(f, &value);
    return fmt_double(err, value, out, cap);
}

/* 轴那一族：X/Y/Z 各读一遍 ------------------------------------------------- */

/** One axis of an axis family, through the semantic function that reads it. */
static ncl_err axis_read(ncl_focas *f, ncl_focas_axis axis, char *out, size_t cap,
                         ncl_err (*fn)(ncl_focas *, ncl_focas_axis, double *))
{
    double value = 0.0;
    ncl_err err = fn(f, axis, &value);
    return fmt_double(err, value, out, cap);
}

static ncl_err live_pos_x(ncl_focas *f, char *out, size_t cap)
{
    return axis_read(f, NCL_FOCAS_AXIS_X, out, cap, ncl_focas_axis_position);
}

static ncl_err live_pos_machine_x(ncl_focas *f, char *out, size_t cap)
{
    return axis_read(f, NCL_FOCAS_AXIS_X, out, cap, ncl_focas_axis_position_machine);
}

static ncl_err live_pos_relative_x(ncl_focas *f, char *out, size_t cap)
{
    return axis_read(f, NCL_FOCAS_AXIS_X, out, cap, ncl_focas_axis_position_relative);
}

static ncl_err live_pos_distance_x(ncl_focas *f, char *out, size_t cap)
{
    return axis_read(f, NCL_FOCAS_AXIS_X, out, cap, ncl_focas_axis_distance);
}

static ncl_err live_pos_cmd_x(ncl_focas *f, char *out, size_t cap)
{
    return axis_read(f, NCL_FOCAS_AXIS_X, out, cap, ncl_focas_axis_position_cmd);
}

static ncl_err live_srv_delay_x(ncl_focas *f, char *out, size_t cap)
{
    return axis_read(f, NCL_FOCAS_AXIS_X, out, cap, ncl_focas_axis_srv_delay);
}

static ncl_err live_feedrate_x(ncl_focas *f, char *out, size_t cap)
{
    return axis_read(f, NCL_FOCAS_AXIS_X, out, cap, ncl_focas_axis_feedrate);
}

static ncl_err live_load_x(ncl_focas *f, char *out, size_t cap)
{
    return axis_read(f, NCL_FOCAS_AXIS_X, out, cap, ncl_focas_axis_load);
}

static ncl_err live_torque_x(ncl_focas *f, char *out, size_t cap)
{
    return axis_read(f, NCL_FOCAS_AXIS_X, out, cap, ncl_focas_axis_torque);
}

static ncl_err live_current_x(ncl_focas *f, char *out, size_t cap)
{
    return axis_read(f, NCL_FOCAS_AXIS_X, out, cap, ncl_focas_axis_current);
}

static ncl_err live_temperature_x(ncl_focas *f, char *out, size_t cap)
{
    return axis_read(f, NCL_FOCAS_AXIS_X, out, cap, ncl_focas_axis_temperature);
}

static ncl_err live_axis_type_x(ncl_focas *f, char *out, size_t cap)
{
    char text[64] = "";
    ncl_err err = ncl_focas_axis_type(f, NCL_FOCAS_AXIS_X, text, sizeof(text));
    return fmt_text(err, text, out, cap);
}

static ncl_err live_spindle_speed(ncl_focas *f, char *out, size_t cap)
{
    double value = 0.0;
    ncl_err err = ncl_focas_spindle_speed(f, 0, &value);
    return fmt_double(err, value, out, cap);
}

static ncl_err live_spindle_load(ncl_focas *f, char *out, size_t cap)
{
    double value = 0.0;
    ncl_err err = ncl_focas_spindle_load(f, 0, &value);
    return fmt_double(err, value, out, cap);
}

/* ------------------------------------------------------------------ table -- */

typedef struct {
    const char *label;
    live_fn     call;
} live_item;

static const live_item kItems[] = {
    { "status",                live_status },
    { "mode",                  live_mode },
    { "emergency",             live_emergency },
    { "alarm_status",          live_alarm_status },
    { "alarm",                 live_alarm },
    { "program_name",          live_program_name },
    { "program_number",        live_program_number },
    { "main_program_number",   live_main_program_number },
    { "subprogram_number",     live_subprogram_number },
    { "line_number",           live_line_number },
    { "executed_block",        live_executed_block },
    { "program_directory",     live_program_directory },
    { "tool_number",           live_tool_number },
    { "part_count",            live_part_count },
    { "tool_group_count",      live_tool_group_count },
    { "timer.power_on",        live_timer_power },
    { "timer.operating",       live_timer_operating },
    { "timer.cutting",         live_timer_cutting },
    { "tool_list",             live_tool_list },
    { "tool_offset[1]",        live_tool_offset },
    { "tool_life[1]",          live_tool_life },
    { "macro_variable[100]",   live_macro_variable },
    { "macro_variables[1..5]", live_macro_variables },
    { "parameter[1]",          live_parameter },
    { "tool_param[1]",         live_tool_param },
    { "tool_param_table",      live_tool_param_table },
    { "parameter_table",       live_parameter_table },
    { "variable_table",        live_variable_table },
    { "work_offset.G54",       live_work_offset },
    { "work_offsets",          live_work_offsets },
    { "modal",                 live_modal },
    { "system",                live_system },
    { "model",                 live_model },
    { "version",               live_version },
    { "manufacturer",          live_manufacturer },
    { "feed_speed",            live_feed_speed },
    { "feed_override",         live_feed_override },
    { "spindle_override",      live_spindle_override },
    { "spindle_speed[0]",      live_spindle_speed },
    { "spindle_load[0]",       live_spindle_load },
    { "axis_position.X",       live_pos_x },
    { "axis_position_machine.X", live_pos_machine_x },
    { "axis_position_relative.X", live_pos_relative_x },
    { "axis_distance.X",       live_pos_distance_x },
    { "axis_position_cmd.X",   live_pos_cmd_x },
    { "axis_srv_delay.X",      live_srv_delay_x },
    { "axis_feedrate.X",       live_feedrate_x },
    { "axis_load.X",           live_load_x },
    { "axis_torque.X",         live_torque_x },
    { "axis_current.X",        live_current_x },
    { "axis_temperature.X",    live_temperature_x },
    { "axis_type.X",           live_axis_type_x },
};

static void show_session(ncl_focas *focas)
{
    ncl_json *session = NULL;
    char *text;

    if (ncl_focas_call(focas, "session", NULL, &session) != NCL_OK ||
        session == NULL) {
        printf("session: (no answer)\n");
        return;
    }
    text = ncl_json_write_string(session);
    printf("session: %s\n", text != NULL ? text : "?");
    ncl_free_safe(text);
    ncl_json_free(session);
}

int main(int argc, char **argv)
{
    const char *host = NULL;
    unsigned port = 8193u;
    int session_only = 0;
    ncl_focas_config config;
    ncl_focas *focas;
    char *err = NULL;
    size_t i;
    int failures = 0;
    int unavailable = 0;
    int usable = 0;

    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--raw") == 0) {
            g_raw = 1;
        } else if (strcmp(argv[i], "--session") == 0) {
            session_only = 1;
        } else if (host == NULL) {
            host = argv[i];
        } else {
            port = (unsigned)strtoul(argv[i], NULL, 10);
        }
    }
    if (host == NULL) {
        host = "192.168.110.192";
    }

    printf("FOCAS live probe: %s:%u (this build's client, not the SDK)\n", host,
           port);
    ncl_focas_config_default(&config);
    config.host = host;
    config.port = port;
    config.negotiate = true;
    focas = ncl_focas_open(&config, &err);
    if (focas == NULL) {
        printf("open: %s\n", err != NULL ? err : "?");
        ncl_free_safe(err);
        return 2;
    }
    ncl_free_safe(err);

    printf("connected: %s\n", ncl_focas_connected(focas) ? "yes" : "not yet");
    show_session(focas);
    if (g_raw) {
        show_raw(focas);
    }
    if (session_only) {
        ncl_focas_close(focas);
        return 0;
    }

    printf("\n%-28s %-14s %s\n", "point", "result", "value / reason");
    printf("%-28s %-14s %s\n", "----------------------------",
           "--------------", "---------------------------------------");
    for (i = 0; i < sizeof(kItems) / sizeof(kItems[0]); i++) {
        char value[1024];
        ncl_err code;

        value[0] = '\0';
        code = kItems[i].call(focas, value, sizeof(value));
        if (code == NCL_OK) {
            usable++;
            printf("%-28s %-14s %s\n", kItems[i].label, "ok", value);
        } else {
            failures++;
            if (code == NCL_ERR_UNAVAILABLE) {
                unavailable++;
            }
            printf("%-28s %-14s %s: %s\n", kItems[i].label, ncl_err_name(code),
                   ncl_focas_last_error(focas),
                   code == NCL_ERR_UNAVAILABLE ? "(frame not captured yet)"
                                               : "(against this server)");
            printf("%-28s %-14s code=0x%08x (tier %d)\n", "", "",
                   (unsigned)code, ncl_driver_error_tier(code));
        }
        if (g_raw) {
            show_raw(focas);
        }
    }

    printf("\n%d read(s) ok, %d unavailable (point table), %d failed\n", usable,
           unavailable, failures - unavailable);
    ncl_focas_close(focas);
    return failures == 0 ? 0 : 1;
}
