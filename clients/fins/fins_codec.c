/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Omron FINS byte level (see ncl_fins.h). Big endian
 * throughout (12-OMRON-FINS.md §8.3).
 */

#include "nclink/clients/fins.h"

#include <stdio.h>
#include <string.h>

/* ================================================================ areas == */

static const struct {
    const char *name;
    uint8_t     code;
    bool        bits;
} kAreas[] = {
    {"CIO", 0xB0, true},  {"W", 0xB1, true},   {"H", 0xB2, true},
    {"A", 0xB3, true},    {"D", 0x82, false},  {"P", 0x87, false},
    {"C", 0x89, false},   {"T", 0x83, false},  {"TS", 0x85, true},
    {"CS", 0x84, true},   {"CF", 0xA5, false}, {"IR", 0xDC, false},
    {"DR", 0xBC, false},  {"TK", 0xB4, true},
};

bool ncl_fins_area_lookup(const char *name, uint8_t *code)
{
    size_t i;

    if (ncl_str_is_blank(name)) {
        return false;
    }
    /* EM banks are written "E<bank>_<library>": E0_0..E0_15 -> 0x20..0x2F,
     * E1_0..E1_15 -> 0x50..0x5F (§4). */
    if ((name[0] == 'E' || name[0] == 'e') && name[1] >= '0' && name[1] <= '9') {
        const char *underscore = strchr(name, '_');
        unsigned bank = (unsigned)(name[1] - '0');
        unsigned library = 0;

        if (underscore == NULL || bank > 1) {
            return false;
        }
        if (underscore[1] < '0' || underscore[1] > '9') {
            return false;
        }
        library = (unsigned)(underscore[1] - '0');
        if (underscore[2] != '\0') {
            if (underscore[2] < '0' || underscore[2] > '9') {
                return false;
            }
            library = library * 10u + (unsigned)(underscore[2] - '0');
        }
        if (library > 15) {
            return false;
        }
        if (code != NULL) {
            *code = (uint8_t)(0x20u + bank * 0x30u + library);
        }
        return true;
    }
    for (i = 0; i < sizeof(kAreas) / sizeof(kAreas[0]); i++) {
        if (ncl_streq_ignore_case(name, kAreas[i].name)) {
            if (code != NULL) {
                *code = kAreas[i].code;
            }
            return true;
        }
    }
    return false;
}

const char *ncl_fins_area_name(uint8_t code)
{
    size_t i;

    if (code >= 0x20 && code <= 0x2F) {
        return "E0_x";
    }
    if (code >= 0x50 && code <= 0x5F) {
        return "E1_x";
    }
    for (i = 0; i < sizeof(kAreas) / sizeof(kAreas[0]); i++) {
        if (kAreas[i].code == code) {
            return kAreas[i].name;
        }
    }
    return NULL;
}

bool ncl_fins_area_is_bit(uint8_t code)
{
    size_t i;

    for (i = 0; i < sizeof(kAreas) / sizeof(kAreas[0]); i++) {
        if (kAreas[i].code == code) {
            return kAreas[i].bits;
        }
    }
    return false;
}

/* =============================================================== frames == */

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8); /* big endian */
    out[1] = (uint8_t)value;
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8) | in[1]);
}

static uint32_t get_u32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | in[3];
}

size_t ncl_fins_tcp_frame(uint8_t *out, size_t cap, uint32_t command,
                          const uint8_t *payload, size_t payload_len)
{
    size_t total = 16u + payload_len;

    if (out == NULL || cap < total || (payload_len > 0 && payload == NULL)) {
        return 0;
    }
    memcpy(out, "FINS", 4);
    put_u32(out + 4, (uint32_t)(8u + payload_len)); /* command + error + payload */
    put_u32(out + 8, command);
    put_u32(out + 12, 0); /* error code: 0 on the way out */
    if (payload_len > 0) {
        memcpy(out + 16, payload, payload_len);
    }
    return total;
}

ncl_err ncl_fins_tcp_split(const uint8_t *frame, size_t len, uint32_t *command,
                           uint32_t *error, const uint8_t **payload,
                           size_t *payload_len, size_t *frame_len)
{
    size_t length;
    size_t total;

    if (frame == NULL || len < 16) {
        return NCL_ERR_RANGE; /* the header has not fully arrived */
    }
    if (memcmp(frame, "FINS", 4) != 0) {
        return NCL_DRV_ERR_PROTOCOL(1); /* not a FINS/TCP frame */
    }
    length = get_u32(frame + 4);
    if (length < 8 || length > 0x10000u) {
        return NCL_DRV_ERR_PROTOCOL(2);
    }
    total = 8u + length; /* magic + length field + what the length covers */
    if (len < total) {
        return NCL_ERR_RANGE;
    }
    if (command != NULL) {
        *command = get_u32(frame + 8);
    }
    if (error != NULL) {
        *error = get_u32(frame + 12);
    }
    if (payload != NULL) {
        *payload = frame + 16;
    }
    if (payload_len != NULL) {
        *payload_len = total - 16u;
    }
    if (frame_len != NULL) {
        *frame_len = total;
    }
    return NCL_OK;
}

size_t ncl_fins_node_request(uint8_t *out, size_t cap, uint32_t client_node)
{
    uint8_t payload[4];

    if (out == NULL || cap < 20) {
        return 0;
    }
    put_u32(payload, client_node);
    return ncl_fins_tcp_frame(out, cap, NCL_FINS_TCP_NODE_ADDRESS, payload,
                              sizeof(payload));
}

ncl_err ncl_fins_node_response(const uint8_t *payload, size_t len,
                               uint8_t *client_node, uint8_t *server_node)
{
    if (payload == NULL || len < 8) {
        return NCL_ERR_RANGE;
    }
    /* Both are 32 bit fields; only the low byte carries the node number. */
    if (client_node != NULL) {
        *client_node = payload[3];
    }
    if (server_node != NULL) {
        *server_node = payload[7];
    }
    return NCL_OK;
}

void ncl_fins_header_default(ncl_fins_header *header)
{
    if (header == NULL) {
        return;
    }
    memset(header, 0, sizeof(*header));
    header->icf = 0x80; /* a response is expected */
    header->gct = 0x02;
}

size_t ncl_fins_frame(uint8_t *out, size_t cap, const ncl_fins_header *header,
                      uint16_t command, const uint8_t *data, size_t data_len)
{
    size_t total = 12u + data_len;

    if (out == NULL || header == NULL || cap < total ||
        (data_len > 0 && data == NULL)) {
        return 0;
    }
    out[0] = header->icf;
    out[1] = header->rsv;
    out[2] = header->gct;
    out[3] = header->dna;
    out[4] = header->da1;
    out[5] = header->da2;
    out[6] = header->sna;
    out[7] = header->sa1;
    out[8] = header->sa2;
    out[9] = header->sid;
    put_u16(out + 10, command); /* MRC + SRC */
    if (data_len > 0) {
        memcpy(out + 12, data, data_len);
    }
    return total;
}

ncl_err ncl_fins_split(const uint8_t *frame, size_t len, ncl_fins_header *header,
                       uint16_t *command, const uint8_t **data,
                       size_t *data_len, size_t *frame_len)
{
    if (frame == NULL || len < 12) {
        return NCL_ERR_RANGE;
    }
    if (header != NULL) {
        header->icf = frame[0];
        header->rsv = frame[1];
        header->gct = frame[2];
        header->dna = frame[3];
        header->da1 = frame[4];
        header->da2 = frame[5];
        header->sna = frame[6];
        header->sa1 = frame[7];
        header->sa2 = frame[8];
        header->sid = frame[9];
    }
    if (command != NULL) {
        *command = get_u16(frame + 10);
    }
    if (data != NULL) {
        *data = frame + 12;
    }
    if (data_len != NULL) {
        *data_len = len - 12u;
    }
    if (frame_len != NULL) {
        *frame_len = len;
    }
    return NCL_OK;
}

/* ============================================================= commands == */

size_t ncl_fins_read_body(uint8_t *out, size_t cap, uint8_t area,
                          uint16_t address, uint8_t bit, uint16_t count)
{
    if (out == NULL || cap < 6) {
        return 0;
    }
    out[0] = area;
    put_u16(out + 1, address);
    out[3] = bit; /* 0x00 for a word access */
    put_u16(out + 4, count);
    return 6;
}

size_t ncl_fins_write_body(uint8_t *out, size_t cap, uint8_t area,
                           uint16_t address, uint8_t bit, uint16_t count,
                           const uint8_t *data, size_t data_len)
{
    size_t total = 6u + data_len;

    if (out == NULL || cap < total || (data_len > 0 && data == NULL)) {
        return 0;
    }
    if (ncl_fins_read_body(out, cap, area, address, bit, count) == 0) {
        return 0;
    }
    if (data_len > 0) {
        memcpy(out + 6, data, data_len);
    }
    return total;
}

ncl_err ncl_fins_reply_begin(const uint8_t *data, size_t data_len,
                             const uint8_t **payload, size_t *payload_len,
                             char *message, size_t message_len)
{
    ncl_err err;

    if (payload != NULL) {
        *payload = NULL;
    }
    if (payload_len != NULL) {
        *payload_len = 0;
    }
    if (data == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (data_len < 3) {
        return NCL_ERR_RANGE; /* end code, MRES and SRES are still arriving */
    }
    err = ncl_fins_check_end_code(data[0], data[1], message, message_len);
    if (err != NCL_OK) {
        return err;
    }
    if (payload != NULL) {
        *payload = data + 3;
    }
    if (payload_len != NULL) {
        *payload_len = data_len - 3u;
    }
    return NCL_OK;
}

ncl_err ncl_fins_check_end_code(uint8_t code, uint8_t detail, char *message,
                                size_t message_len)
{
    static const struct {
        uint8_t     code;
        const char *text;
    } kCodes[] = {
        {0x01, "未找到本地节点"},
        {0x02, "令牌超时"},
        {0x03, "重发超限"},
        {0x04, "超时"},
        {0x05, "节点地址范围错"},
        {0x10, "命令格式错"},
        {0x11, "参数错"},
        {0x20, "读取不可能（地址越界或区不存在）"},
        {0x21, "写入不可能"},
        {0x22, "当前运行模式不允许"},
        {0x23, "单元不存在"},
        {0x24, "起始地址 + 数量越界"},
        {0x25, "数据长度不符"},
        {0xA3, "数据被中止"},
        {0xA4, "服务被中止"},
        {0xA5, "服务被拒绝（CPU 忙）"},
    };
    size_t i;

    if (code == 0) {
        return NCL_OK;
    }
    for (i = 0; i < sizeof(kCodes) / sizeof(kCodes[0]); i++) {
        if (kCodes[i].code == code) {
            if (message != NULL) {
                snprintf(message, message_len, "FINS 结束码 0x%02X：%s", code,
                         kCodes[i].text);
            }
            break;
        }
    }
    if (i == sizeof(kCodes) / sizeof(kCodes[0])) {
        if (message != NULL) {
            snprintf(message, message_len,
                     "FINS 未知结束码 0x%02X（明细 0x%02X）", code, detail);
        }
        return NCL_DRV_ERR_PROTOCOL(0x2FF);
    }
    /* A network that lost a token or a reply is a link problem: reconnect and
     * re-send. "Cannot read/write there" is a point map problem: the frame was
     * fine and the answer is definitive. */
    switch (code) {
    case 0x02:
    case 0x03:
    case 0x04:
        return NCL_DRV_ERR_TRANSPORT(0x200 | code);
    case 0x22: /* the PLC is in the wrong mode */
    case 0xA3:
    case 0xA4:
    case 0xA5: /* busy or refused */
        return NCL_DRV_ERR_BUSINESS(0x200 | code);
    default:
        return NCL_DRV_ERR_PROTOCOL(0x200 | code);
    }
}

/* ================================================================= data === */

size_t ncl_fins_element_bytes(ncl_dtype dtype, size_t text_chars)
{
    switch (dtype) {
    case NCL_DTYPE_BIT:
        return 1;
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

size_t ncl_fins_wire_bytes(const ncl_address *address, bool bit_access)
{
    if (address == NULL) {
        return 0;
    }
    if (bit_access) {
        return (size_t)address->length; /* one byte per bit */
    }
    return ncl_fins_element_bytes(address->dtype, (size_t)address->length) *
           (size_t)address->length;
}

ncl_err ncl_fins_decode(const uint8_t *data, size_t data_len, size_t offset,
                        ncl_dtype dtype, size_t text_chars, ncl_json **out)
{
    size_t width = ncl_fins_element_bytes(dtype, text_chars);

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
        *out = ncl_json_new_int(data[1]); /* the low byte of the word */
        break;
    case NCL_DTYPE_INT16:
        *out = ncl_json_new_int((int16_t)get_u16(data));
        break;
    case NCL_DTYPE_INT32:
        *out = ncl_json_new_int((int32_t)get_u32(data));
        break;
    case NCL_DTYPE_FLOAT32: {
        uint32_t raw = get_u32(data);
        float value;

        memcpy(&value, &raw, sizeof(value));
        *out = ncl_json_new_double((double)value);
        break;
    }
    case NCL_DTYPE_FLOAT64: {
        uint64_t raw = 0;
        double value;
        size_t i;

        for (i = 0; i < 8; i++) {
            raw = (raw << 8) | data[i];
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

ncl_err ncl_fins_encode(const ncl_json *value, ncl_dtype dtype, size_t text_chars,
                        uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t width = ncl_fins_element_bytes(dtype, text_chars);

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

        if (!ncl_json_as_int(value, &number)) {
            return NCL_ERR_INVALID_VALUE;
        }
        put_u32(out, (uint32_t)number);
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
        put_u32(out, raw);
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
        for (i = 0; i < 8; i++) {
            out[i] = (uint8_t)(raw >> (56 - 8 * i));
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
