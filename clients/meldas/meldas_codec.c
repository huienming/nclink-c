/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - MELDAS / GIOP byte level (see ncl_meldas.h).
 */

#include "nclink/clients/meldas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint32_t get_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

size_t ncl_meldas_build_request(uint8_t *out, size_t cap, uint32_t request_id,
                                const char *operation,
                                const ncl_meldas_request *fields)
{
    size_t name_len;

    if (out == NULL || operation == NULL || fields == NULL ||
        cap < NCL_MELDAS_REQUEST_BYTES) {
        return 0;
    }
    name_len = strlen(operation);
    /* "mochaGetData" is 12 characters; the length field counts the NUL, and the
     * fixed 00 00 00 03 follows (§8.2). */
    if (name_len == 0 || name_len > 12) {
        return 0;
    }
    memset(out, 0, NCL_MELDAS_REQUEST_BYTES);
    memcpy(out, "GIOP", 4);
    out[4] = 0x01; /* version 1.0   */
    out[5] = 0x00;
    out[6] = 0x01; /* flags: little endian */
    out[7] = 0x00; /* message type: Request */
    put_u32(out + 8, NCL_MELDAS_REQUEST_BYTES - 12u); /* §8.1: total - 12 */
    put_u32(out + 16, request_id);
    out[20] = 0x01; /* response expected */
    out[21] = 0xFF;
    out[22] = 0xFF;
    out[23] = 0xFF;
    put_u32(out + 24, 4);  /* object key length */
    put_u32(out + 28, 1);  /* object key        */
    put_u32(out + 32, (uint32_t)(name_len + 1u));
    memcpy(out + 36, operation, name_len);
    out[36 + name_len] = '\0';
    /* 48..55 is the fixed 00 00 00 03 / 00 00 00 00 pair the samples show. */
    out[51] = 0x03;
    put_u32(out + 56, fields->command);
    put_u32(out + 60, fields->subcode);
    put_u32(out + 64, fields->count);
    put_u32(out + 68, fields->address);
    put_u32(out + 76, fields->want);
    return NCL_MELDAS_REQUEST_BYTES;
}

double ncl_meldas_double10(const uint8_t *bytes)
{
    /*
     * §3.3 lists a ten byte double without giving its layout; the delivered
     * tooling uses the CString form for coordinates, so this is the x87
     * extended precision reading (exponent in the last two bytes, 64 bit
     * mantissa with an explicit integer bit) and it needs a real machine to be
     * confirmed. A caller that wants certainty asks for the CString form.
     */
    uint64_t mantissa = 0;
    unsigned exponent;
    int sign;
    double value;
    int i;

    if (bytes == NULL) {
        return 0;
    }
    for (i = 7; i >= 0; i--) {
        mantissa = (mantissa << 8) | bytes[i];
    }
    exponent = (unsigned)(((bytes[9] & 0x7Fu) << 8) | bytes[8]);
    sign = (bytes[9] & 0x80u) != 0;
    if (exponent == 0 && mantissa == 0) {
        return 0;
    }
    /* mantissa * 2^(exponent - 16383 - 63) */
    value = (double)mantissa;
    {
        int shift = (int)exponent - 16383 - 63;

        while (shift > 0) {
            value *= 2.0;
            shift--;
        }
        while (shift < 0) {
            value /= 2.0;
            shift++;
        }
    }
    return sign ? -value : value;
}

ncl_err ncl_meldas_parse_reply(const uint8_t *reply, size_t len,
                               uint32_t want_id, ncl_meldas_value *out,
                               char *err, size_t err_len)
{
    static const uint8_t kHeader[8] = {0x47, 0x49, 0x4F, 0x50,
                                       0x01, 0x00, 0x01, 0x01};
    uint32_t id;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (reply == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (len < 33) {
        if (err != NULL) {
            snprintf(err, err_len, "应答不足 %u 字节", (unsigned)len);
        }
        return NCL_DRV_ERR_PROTOCOL(0x50); /* -1 in the delivered check frame */
    }
    if (memcmp(reply, kHeader, sizeof(kHeader)) != 0) {
        if (err != NULL) {
            snprintf(err, err_len, "GIOP 报文头异常");
        }
        return NCL_DRV_ERR_PROTOCOL(0x51);
    }
    id = get_u32(reply + 16);
    if (id != want_id) {
        if (err != NULL) {
            snprintf(err, err_len, "请求 ID 不匹配：期望 %u，收到 %u", want_id, id);
        }
        return NCL_DRV_ERR_PROTOCOL(0x52);
    }
    if (len >= 31 && memcmp(reply + 28, "IDL", 3) == 0) {
        out->present = false; /* the operation answered "no data" (§3.2) */
        return NCL_OK;
    }
    out->present = true;
    out->type = reply[28];
    switch (out->type) {
    case NCL_MELDAS_TYPE_BYTE:
        if (len < 37) {
            return NCL_ERR_RANGE;
        }
        out->integer = reply[36];
        break;
    case NCL_MELDAS_TYPE_INT16:
        if (len < 38) {
            return NCL_ERR_RANGE;
        }
        out->integer = (int16_t)(reply[36] | (reply[37] << 8));
        break;
    case NCL_MELDAS_TYPE_INT32:
        if (len < 40) {
            return NCL_ERR_RANGE;
        }
        out->integer = (int32_t)get_u32(reply + 36);
        break;
    case NCL_MELDAS_TYPE_DOUBLE: {
        uint64_t raw = 0;
        double value;
        int i;

        if (len < 44) {
            return NCL_ERR_RANGE;
        }
        for (i = 7; i >= 0; i--) {
            raw = (raw << 8) | reply[36 + i];
        }
        memcpy(&value, &raw, sizeof(value));
        out->real = value;
        break;
    }
    case NCL_MELDAS_TYPE_DOUBLE10:
        if (len < 46) {
            return NCL_ERR_RANGE;
        }
        out->real = ncl_meldas_double10(reply + 36);
        break;
    case NCL_MELDAS_TYPE_STRING:
    case NCL_MELDAS_TYPE_STRING188: {
        size_t chars;

        if (len < 40) {
            return NCL_ERR_RANGE;
        }
        /* §8.5: the length is at [36] and the text starts at [40]. */
        chars = reply[36];
        if ((size_t)chars > sizeof(out->text) - 1u) {
            chars = sizeof(out->text) - 1u;
        }
        if (40u + chars > len) {
            chars = len - 40u;
        }
        memcpy(out->text, reply + 40, chars);
        out->text[chars] = '\0';
        break;
    }
    default:
        if (err != NULL) {
            snprintf(err, err_len, "未知的返回类型标记 0x%02X", out->type);
        }
        return NCL_DRV_ERR_PROTOCOL(0x53);
    }
    return NCL_OK;
}

/** A numeric text ("123.456") out of a CString reply. */
static bool text_to_number(const char *text, double *out)
{
    char *end = NULL;
    double value;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    value = strtod(text, &end);
    if (end == text) {
        return false;
    }
    while (*end == ' ') {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

ncl_json *ncl_meldas_value_to_json(const ncl_meldas_value *value,
                                   ncl_dtype dtype)
{
    if (value == NULL || !value->present) {
        return ncl_json_new_null(); /* the operation had no data for us */
    }
    switch (value->type) {
    case NCL_MELDAS_TYPE_STRING:
    case NCL_MELDAS_TYPE_STRING188: {
        double number = 0;

        if (dtype == NCL_DTYPE_STRING || !text_to_number(value->text, &number)) {
            return ncl_json_new_string(value->text);
        }
        /* A coordinate comes back as text but is a number to the model. */
        if (dtype == NCL_DTYPE_FLOAT32 || dtype == NCL_DTYPE_FLOAT64) {
            return ncl_json_new_double(number);
        }
        return ncl_json_new_int((long long)number);
    }
    case NCL_MELDAS_TYPE_DOUBLE:
    case NCL_MELDAS_TYPE_DOUBLE10:
        return dtype == NCL_DTYPE_INT16 || dtype == NCL_DTYPE_INT32 ||
                       dtype == NCL_DTYPE_BYTE
                   ? ncl_json_new_int((long long)value->real)
                   : ncl_json_new_double(value->real);
    case NCL_MELDAS_TYPE_BYTE:
        if (dtype == NCL_DTYPE_BIT) {
            return ncl_json_new_bool(value->integer != 0);
        }
        return ncl_json_new_int(value->integer);
    default:
        return ncl_json_new_int(value->integer);
    }
}

/* ================================================================ 语义 ==== */

static const struct {
    const char *name;
    uint32_t    command;
    uint32_t    subcode;
    uint32_t    want;
    const char *canonical;
} kCommands[] = {
    /* 读坐标：CString 返回（§6.1 的样例用 0x10），子码即坐标类型 */
    {"work_position", 0x25, 1, NCL_MELDAS_TYPE_STRING, "work_position"},
    {"machine_position", 0x25, 2, NCL_MELDAS_TYPE_STRING, "machine_position"},
    {"current_position", 0x25, 3, NCL_MELDAS_TYPE_STRING, "current_position"},
    {"remaining", 0x25, 6, NCL_MELDAS_TYPE_STRING, "remaining"},
    /* 同一批数据的 DOUBLE 形式（0x03 命令族的子码） */
    {"machine_position_d", 0x03, 0x02, NCL_MELDAS_TYPE_DOUBLE,
     "machine_position_d"},
    {"current_position_d", 0x03, 0x03, NCL_MELDAS_TYPE_DOUBLE,
     "current_position_d"},
    {"remaining_d", 0x03, 0x06, NCL_MELDAS_TYPE_DOUBLE, "remaining_d"},
    {"zero_offset", 0x03, 0x08, NCL_MELDAS_TYPE_DOUBLE, "zero_offset"},
    {"tool_offset", 0x03, 0x0e, NCL_MELDAS_TYPE_DOUBLE, "tool_offset"},
    {"tool_life", 0x03, 0x12, NCL_MELDAS_TYPE_INT32, "tool_life"},
    {"g_code_group", 0x03, 0x29, NCL_MELDAS_TYPE_STRING, "g_code_group"},
    {"servo_monitor", 0x03, 0x3b, NCL_MELDAS_TYPE_STRING, "servo_monitor"},
    {"machining_param", 0x03, 0x7e, NCL_MELDAS_TYPE_STRING, "machining_param"},
    /* 状态与工况 */
    {"feedrate", 0x21, 1, NCL_MELDAS_TYPE_STRING, "feedrate"},
    {"run_status", 0x23, 0x0a, NCL_MELDAS_TYPE_BYTE, "run_status"},
    {"work_mode", 0x23, 0x0b, NCL_MELDAS_TYPE_BYTE, "work_mode"},
    {"power_on_time", 0x28, 1, NCL_MELDAS_TYPE_INT32, "power_on_time"},
    {"run_time", 0x28, 2, NCL_MELDAS_TYPE_INT32, "run_time"},
    {"code_group", 0x29, 0, NCL_MELDAS_TYPE_STRING, "code_group"},
    {"feedrate_set", 0x2a, 1, NCL_MELDAS_TYPE_STRING, "feedrate_set"},
    {"spindle_speed_set", 0x2b, 1, NCL_MELDAS_TYPE_INT32, "spindle_speed_set"},
    {"tool_number", 0x2b, 101, NCL_MELDAS_TYPE_INT32, "tool_number"},
    {"program", 0x2d, 101, NCL_MELDAS_TYPE_STRING, "program"},
    {"io_bit", 0x35, 0, NCL_MELDAS_TYPE_BYTE, "io_bit"},
    {"io_byte", 0x36, 0, NCL_MELDAS_TYPE_BYTE, "io_byte"},
    {"io_word", 0x37, 0, NCL_MELDAS_TYPE_INT16, "io_word"},
    {"servo_monitor_2", 0x3b, 0, NCL_MELDAS_TYPE_STRING, "servo_monitor_2"},
    {"spindle_speed", 0x3f, 3, NCL_MELDAS_TYPE_INT32, "spindle_speed"},
    {"spindle_load", 0x3f, 4, NCL_MELDAS_TYPE_INT32, "spindle_load"},
    {"spindle_current", 0x3f, 201, NCL_MELDAS_TYPE_INT32, "spindle_current"},
    {"spindle_temp", 0x3f, 221, NCL_MELDAS_TYPE_INT32, "spindle_temp"},
    {"part_count", 0x7e, 8002, NCL_MELDAS_TYPE_INT32, "part_count"},
    /* 系统信息 */
    {"cnc_model", 0x01, 0, NCL_MELDAS_TYPE_STRING, "cnc_model"},
    {"system_type", 0x02, 0, NCL_MELDAS_TYPE_BYTE, "system_type"},
};

bool ncl_meldas_command_lookup(const char *name, uint32_t *command,
                               uint32_t *subcode, uint32_t *want,
                               const char **canonical)
{
    size_t i;

    if (ncl_str_is_blank(name)) {
        return false;
    }
    for (i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
        if (ncl_streq_ignore_case(name, kCommands[i].name)) {
            if (command != NULL) {
                *command = kCommands[i].command;
            }
            if (subcode != NULL) {
                *subcode = kCommands[i].subcode;
            }
            if (want != NULL) {
                *want = kCommands[i].want;
            }
            if (canonical != NULL) {
                *canonical = kCommands[i].canonical;
            }
            return true;
        }
    }
    /* The raw form "0x25/2" keeps a command the table does not name usable,
     * which is how an unknown command is probed on a real machine. */
    if ((name[0] == '0' && (name[1] == 'x' || name[1] == 'X'))) {
        unsigned long cmd = 0;
        const char *slash = strchr(name, '/');
        char *end = NULL;

        cmd = strtoul(name, &end, 0);
        if (end == name) {
            return false;
        }
        if (command != NULL) {
            *command = (uint32_t)cmd;
        }
        if (subcode != NULL) {
            *subcode = slash != NULL ? (uint32_t)strtoul(slash + 1, NULL, 0) : 0;
        }
        if (want != NULL) {
            *want = NCL_MELDAS_TYPE_STRING; /* the safe default: text */
        }
        if (canonical != NULL) {
            *canonical = name;
        }
        return true;
    }
    return false;
}

bool ncl_meldas_command_uses_axis(uint32_t command, uint32_t subcode)
{
    /* §4: the coordinate family addresses an axis; everything else addresses an
     * I/O point, a variable number or nothing at all. */
    if (command == 0x25) {
        return true; /* 工件/机械/当前/剩余坐标 */
    }
    if (command == 0x03) {
        return subcode == 0x02 || subcode == 0x03 || subcode == 0x06 ||
               subcode == 0x08 || subcode == 0x0e;
    }
    return false;
}

uint32_t ncl_meldas_axis(unsigned axis, bool bit_mode)
{
    if (!bit_mode) {
        return axis; /* the C++ samples number the axes 1..n */
    }
    /* §5: the C# delivery bit codes them, X=1 Y=2 Z=4 (the 4th axis 8, ...). */
    if (axis == 0 || axis > 8) {
        return 0;
    }
    return (uint32_t)1 << (axis - 1u);
}

uint32_t ncl_meldas_want_for(ncl_dtype dtype)
{
    switch (dtype) {
    case NCL_DTYPE_BIT: return NCL_MELDAS_TYPE_BYTE;
    case NCL_DTYPE_BYTE: return NCL_MELDAS_TYPE_BYTE;
    case NCL_DTYPE_INT16: return NCL_MELDAS_TYPE_INT16;
    case NCL_DTYPE_INT32: return NCL_MELDAS_TYPE_INT32;
    case NCL_DTYPE_FLOAT32:
    case NCL_DTYPE_FLOAT64: return NCL_MELDAS_TYPE_DOUBLE;
    case NCL_DTYPE_STRING: return NCL_MELDAS_TYPE_STRING;
    default: return NCL_MELDAS_TYPE_STRING;
    }
}
