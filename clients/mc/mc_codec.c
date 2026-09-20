/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Mitsubishi MC / SLMP byte level (see ncl_mc.h).
 *
 * 3E and 4E binary frames, little endian from the first header byte to the last
 * data word (06-MITSUBISHI-PLC-MC-SLMP.md §3, §6).
 */

#include "nclink/clients/mc.h"

#include <stdio.h>
#include <string.h>

/* =============================================================== devices == */

static const struct {
    const char *name;
    uint8_t     code;
    ncl_mc_unit unit;
} kDevices[] = {
    {"SM", 0x91, NCL_MC_BIT},  /* special relay            */
    {"SD", 0xA9, NCL_MC_WORD}, /* special register         */
    {"X", 0x9C, NCL_MC_BIT},   /* input                    */
    {"Y", 0x9D, NCL_MC_BIT},   /* output                   */
    {"M", 0x90, NCL_MC_BIT},   /* internal relay           */
    {"L", 0x92, NCL_MC_BIT},   /* latch relay              */
    {"F", 0x93, NCL_MC_BIT},   /* annunciator              */
    {"V", 0x94, NCL_MC_BIT},   /* edge relay               */
    {"B", 0xA0, NCL_MC_BIT},   /* link relay               */
    {"D", 0xA8, NCL_MC_WORD},  /* data register            */
    {"R", 0xAF, NCL_MC_WORD},  /* file register            */
    {"W", 0xB4, NCL_MC_WORD},  /* link register            */
    {"ZR", 0xB0, NCL_MC_WORD}, /* file register (extended) */
    {"TN", 0xC2, NCL_MC_WORD}, /* timer, current value     */
    {"TS", 0xC1, NCL_MC_BIT},  /* timer contact            */
    {"CN", 0xC5, NCL_MC_WORD}, /* counter, current value   */
    {"CS", 0xC4, NCL_MC_BIT},  /* counter contact          */
};

bool ncl_mc_device_lookup(const char *name, uint8_t *code, ncl_mc_unit *unit)
{
    size_t i;

    if (ncl_str_is_blank(name)) {
        return false;
    }
    for (i = 0; i < sizeof(kDevices) / sizeof(kDevices[0]); i++) {
        if (ncl_streq_ignore_case(name, kDevices[i].name)) {
            if (code != NULL) {
                *code = kDevices[i].code;
            }
            if (unit != NULL) {
                *unit = kDevices[i].unit;
            }
            return true;
        }
    }
    return false;
}

const char *ncl_mc_device_name(uint8_t code)
{
    size_t i;

    for (i = 0; i < sizeof(kDevices) / sizeof(kDevices[0]); i++) {
        if (kDevices[i].code == code) {
            return kDevices[i].name;
        }
    }
    return NULL;
}

/* =============================================================== headers == */

void ncl_mc_header_default(ncl_mc_header *header, ncl_mc_frame_type frame)
{
    if (header == NULL) {
        return;
    }
    memset(header, 0, sizeof(*header));
    header->frame = frame;
    header->plc = 0xFF;
    header->module = 0x03FF;
    header->timer = 0x0010; /* 4 s, the usual value  */
}

/* ================================================================ frames == */

/*
 * A word device with a bit index is addressed the traditional way: the wire
 * address is number * 16 + bit. A bit device keeps its plain number, because
 * the bit sub command already says "in bits" (§8.2).
 */
uint32_t ncl_mc_wire_address(uint32_t number, int bit, bool word_device)
{
    if (bit >= 0 && word_device) {
        return number * 16u + (uint32_t)bit;
    }
    return number;
}

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFF); /* little endian */
    out[1] = (uint8_t)(value >> 8);
}

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
}

size_t ncl_mc_device_spec(uint8_t *out, size_t cap, uint8_t device_code,
                          uint32_t address, uint16_t points)
{
    if (out == NULL || cap < 6 || address > 0xFFFFFFu) {
        return 0;
    }
    out[0] = device_code;
    out[1] = (uint8_t)(address & 0xFF);
    out[2] = (uint8_t)((address >> 8) & 0xFF);
    out[3] = (uint8_t)((address >> 16) & 0xFF);
    put_u16(out + 4, points);
    return 6;
}

size_t ncl_mc_request_size(ncl_mc_frame_type frame, size_t data_len)
{
    /* subheader + network + plc + module + station + length(2) + timer(2) +
     * command(2) + subcommand(2) + data; 4E adds its 2 byte serial number. */
    return 15u + (frame == NCL_MC_FRAME_4E ? 2u : 0u) + data_len;
}

size_t ncl_mc_request(uint8_t *out, size_t cap, const ncl_mc_header *header,
                      uint16_t command, uint16_t subcommand, const uint8_t *data,
                      size_t data_len)
{
    size_t total;
    size_t at = 2;
    uint16_t length;

    if (out == NULL || header == NULL || cap < 16) {
        return 0;
    }
    total = ncl_mc_request_size(header->frame, data_len);
    if (total > cap) {
        return 0;
    }
    /* The length field covers timer + command + sub command + data. */
    length = (uint16_t)(4u + 2u + data_len);
    out[0] = 0xD0;
    out[1] = 0x00;
    /* 4E carries its serial number right after the sub header (§3.3). */
    if (header->frame == NCL_MC_FRAME_4E) {
        put_u16(out + at, header->serial);
        at += 2;
    }
    out[at++] = header->network;
    out[at++] = header->plc;
    put_u16(out + at, header->module);
    at += 2;
    out[at++] = header->station;
    put_u16(out + at, length);
    at += 2;
    put_u16(out + at, header->timer);
    at += 2;
    put_u16(out + at, command);
    at += 2;
    put_u16(out + at, subcommand);
    at += 2;
    if (data_len > 0) {
        memcpy(out + at, data, data_len);
    }
    at += data_len;
    return at == total ? total : 0;
}

ncl_err ncl_mc_split_reply(const uint8_t *frame, size_t len,
                           ncl_mc_frame_type type, uint16_t want_serial,
                           ncl_mc_reply *out, size_t *frame_len)
{
    size_t header = type == NCL_MC_FRAME_4E ? 11u : 9u;
    size_t length;
    size_t total;

    if (frame == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (frame[0] != 0xD0 || frame[1] != 0x00) {
        return NCL_DRV_ERR_PROTOCOL(1); /* not a 3E/4E binary reply */
    }
    if (len < header) {
        return NCL_ERR_RANGE; /* the header has not fully arrived */
    }
    /* 4E puts its serial number right after the sub header, request and
     * reply alike (§3.3). */
    if (type == NCL_MC_FRAME_4E && get_u16(frame + 2) != want_serial) {
        return NCL_DRV_ERR_PROTOCOL(2); /* a reply to an older request */
    }
    /* length covers the end code and the data. */
    length = get_u16(frame + header - 2);
    if (length < 2) {
        return NCL_DRV_ERR_PROTOCOL(3);
    }
    total = header + length;
    if (len < total) {
        return NCL_ERR_RANGE;
    }
    out->end_code = get_u16(frame + header);
    out->data = frame + header + 2;
    out->data_len = length - 2u;
    if (frame_len != NULL) {
        *frame_len = total;
    }
    return NCL_OK;
}

ncl_err ncl_mc_check_end_code(uint16_t code, char *message, size_t message_len)
{
    static const struct {
        uint16_t    code;
        const char *text;
    } kCodes[] = {
        {0x0005, "请求数据长度不符"},
        {0x0006, "请求数据量超限"},
        {0xC050, "数据超出范围（地址越界）"},
        {0xC051, "命令/子命令错误"},
        {0xC052, "请求数据区错误"},
        {0xC056, "不能对指定设备执行"},
        {0xC058, "请求数据的指定方式错误"},
        {0xC059, "命令未找到（帧型或编码不符）"},
        {0xC05B, "不能对指定设备执行（保护）"},
        {0xC05C, "命令未找到（软元件指定错）"},
        {0xC05F, "请求数据重复"},
        {0xC060, "请求数据非法（ASCII 转换错）"},
        {0xC061, "请求数据长度不符"},
        {0xC0B5, "CPU 错误"},
        {0xC200, "远程 RUN/STOP 未受理"},
        {0xC201, "远程 RUN/STOP 状态错"},
    };
    size_t i;

    if (code == 0) {
        return NCL_OK;
    }
    for (i = 0; i < sizeof(kCodes) / sizeof(kCodes[0]); i++) {
        if (kCodes[i].code == code) {
            if (message != NULL) {
                snprintf(message, message_len, "MC 结束代码 0x%04X：%s", code,
                         kCodes[i].text);
            }
            /* A CPU in a fault state or a remote control that was refused is
             * about the device, not about the frame: business level, so the
             * caller does not tear the link down over it. */
            return (code == 0xC0B5 || code == 0xC200 || code == 0xC201)
                       ? NCL_DRV_ERR_BUSINESS(0x100 | (int)(code & 0xFF))
                       : NCL_DRV_ERR_PROTOCOL(0x100 | (int)(code & 0xFF));
        }
    }
    if (message != NULL) {
        snprintf(message, message_len, "MC 未知结束代码 0x%04X", code);
    }
    return NCL_DRV_ERR_PROTOCOL(0x1FF);
}

/* ================================================================= data === */

size_t ncl_mc_element_bytes(ncl_dtype dtype, size_t text_chars)
{
    switch (dtype) {
    case NCL_DTYPE_BIT:
        return 1; /* one point per byte in a bit unit read */
    case NCL_DTYPE_BYTE:
    case NCL_DTYPE_INT16:
        return 2;
    case NCL_DTYPE_INT32:
    case NCL_DTYPE_FLOAT32:
        return 4;
    case NCL_DTYPE_FLOAT64:
        return 8;
    case NCL_DTYPE_STRING:
        return text_chars;
    default:
        return 0;
    }
}

ncl_err ncl_mc_decode(const uint8_t *data, size_t data_len, size_t offset,
                      ncl_dtype dtype, size_t text_chars, ncl_json **out)
{
    size_t width = ncl_mc_element_bytes(dtype, text_chars);

    if (out != NULL) {
        *out = NULL;
    }
    if (data == NULL || out == NULL || width == 0) {
        return NCL_ERR_INVALID_ARG;
    }
    if (offset + width > data_len) {
        return NCL_ERR_RANGE;
    }
    data += offset;
    switch (dtype) {
    case NCL_DTYPE_BIT:
        *out = ncl_json_new_bool(data[0] != 0);
        break;
    case NCL_DTYPE_BYTE:
        *out = ncl_json_new_int(data[0]); /* the low byte of the word */
        break;
    case NCL_DTYPE_INT16:
        *out = ncl_json_new_int((int16_t)get_u16(data));
        break;
    case NCL_DTYPE_INT32: {
        uint32_t raw = (uint32_t)get_u16(data) |
                       ((uint32_t)get_u16(data + 2) << 16);

        *out = ncl_json_new_int((int32_t)raw);
        break;
    }
    case NCL_DTYPE_FLOAT32: {
        uint32_t raw = (uint32_t)get_u16(data) |
                       ((uint32_t)get_u16(data + 2) << 16);
        float value;

        memcpy(&value, &raw, sizeof(value));
        *out = ncl_json_new_double((double)value);
        break;
    }
    case NCL_DTYPE_FLOAT64: {
        uint64_t raw = 0;
        double value;
        size_t i;

        for (i = 0; i < 4; i++) {
            raw |= (uint64_t)get_u16(data + i * 2) << (16 * i);
        }
        memcpy(&value, &raw, sizeof(value));
        *out = ncl_json_new_double(value);
        break;
    }
    case NCL_DTYPE_STRING: {
        char text[512];
        size_t chars = text_chars < sizeof(text) - 1 ? text_chars
                                                     : sizeof(text) - 1;
        size_t i;

        /* Two characters per word, low byte first; a NUL ends the text. */
        for (i = 0; i < chars; i++) {
            text[i] = (char)data[i];
        }
        text[chars] = '\0';
        for (i = 0; i < chars; i++) {
            if (text[i] == '\0') {
                text[i] = ' ';
            }
        }
        *out = ncl_json_new_string(text);
        break;
    }
    default:
        return NCL_ERR_INVALID_DATA_TYPE;
    }
    return *out != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

ncl_err ncl_mc_encode(const ncl_json *value, ncl_dtype dtype, size_t text_chars,
                      uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t width = ncl_mc_element_bytes(dtype, text_chars);

    if (value == NULL || out == NULL || width == 0 || out_cap < width) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(out, 0, width);
    switch (dtype) {
    case NCL_DTYPE_BIT: {
        bool set = false;
        double number = 0;

        if (!ncl_json_as_bool(value, &set)) {
            if (!ncl_json_as_double(value, &number)) {
                return NCL_ERR_INVALID_VALUE;
            }
            set = number != 0;
        }
        out[0] = set ? 1 : 0;
        break;
    }
    case NCL_DTYPE_BYTE:
    case NCL_DTYPE_INT16: {
        long long number = 0;

        if (!ncl_json_as_int(value, &number)) {
            return NCL_ERR_INVALID_VALUE;
        }
        put_u16(out, (uint16_t)number);
        break;
    }
    case NCL_DTYPE_INT32: {
        long long number = 0;
        uint32_t raw;

        if (!ncl_json_as_int(value, &number)) {
            return NCL_ERR_INVALID_VALUE;
        }
        raw = (uint32_t)number;
        put_u16(out, (uint16_t)(raw & 0xFFFF));
        put_u16(out + 2, (uint16_t)(raw >> 16));
        break;
    }
    case NCL_DTYPE_FLOAT32: {
        double number = 0;
        float single;
        uint32_t raw;

        if (!ncl_json_as_double(value, &number)) {
            return NCL_ERR_INVALID_VALUE;
        }
        single = (float)number;
        memcpy(&raw, &single, sizeof(raw));
        put_u16(out, (uint16_t)(raw & 0xFFFF));
        put_u16(out + 2, (uint16_t)(raw >> 16));
        break;
    }
    case NCL_DTYPE_FLOAT64: {
        double number = 0;
        uint64_t raw;
        size_t i;

        if (!ncl_json_as_double(value, &number)) {
            return NCL_ERR_INVALID_VALUE;
        }
        memcpy(&raw, &number, sizeof(raw));
        for (i = 0; i < 4; i++) {
            put_u16(out + i * 2, (uint16_t)((raw >> (16 * i)) & 0xFFFF));
        }
        break;
    }
    case NCL_DTYPE_STRING: {
        const char *text = ncl_json_as_string(value);
        size_t i;

        if (text == NULL) {
            return NCL_ERR_INVALID_VALUE;
        }
        for (i = 0; i < width && text[i] != '\0'; i++) {
            out[i] = (uint8_t)text[i];
        }
        break;
    }
    default:
        return NCL_ERR_INVALID_DATA_TYPE;
    }
    if (out_len != NULL) {
        *out_len = width;
    }
    return NCL_OK;
}
