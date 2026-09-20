/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Modbus byte level (see ncl_modbus.h).
 *
 * Everything here is a pure function over byte buffers, which is what lets the
 * golden frames in the test suite stand in for a device.
 */

#include "nclink/clients/modbus.h"

#include <stdio.h>
#include <string.h>

/* ================================================================= areas == */

static const struct {
    const char *text;
    ncl_modbus_area area;
} kAreaNames[] = {
    {"coil", NCL_MB_COIL},
    {"coils", NCL_MB_COIL},
    {"0x", NCL_MB_COIL},
    {"discrete", NCL_MB_DISCRETE},
    {"discrete_input", NCL_MB_DISCRETE},
    {"1x", NCL_MB_DISCRETE},
    {"input", NCL_MB_INPUT},
    {"input_register", NCL_MB_INPUT},
    {"3x", NCL_MB_INPUT},
    {"holding", NCL_MB_HOLDING},
    {"holding_register", NCL_MB_HOLDING},
    {"register", NCL_MB_HOLDING},
    {"4x", NCL_MB_HOLDING},
};

bool ncl_modbus_area_parse(const char *text, ncl_modbus_area *out)
{
    size_t i;

    if (ncl_str_is_blank(text)) {
        return false;
    }
    for (i = 0; i < sizeof(kAreaNames) / sizeof(kAreaNames[0]); i++) {
        if (ncl_streq_ignore_case(text, kAreaNames[i].text)) {
            if (out != NULL) {
                *out = kAreaNames[i].area;
            }
            return true;
        }
    }
    return false;
}

const char *ncl_modbus_area_name(ncl_modbus_area area)
{
    switch (area) {
    case NCL_MB_COIL: return "coil";
    case NCL_MB_DISCRETE: return "discrete";
    case NCL_MB_INPUT: return "input";
    case NCL_MB_HOLDING: return "holding";
    default: return "?";
    }
}

bool ncl_modbus_area_is_writable(ncl_modbus_area area)
{
    return area == NCL_MB_COIL || area == NCL_MB_HOLDING;
}

uint8_t ncl_modbus_area_read_code(ncl_modbus_area area)
{
    switch (area) {
    case NCL_MB_COIL: return NCL_MB_FC_READ_COILS;
    case NCL_MB_DISCRETE: return NCL_MB_FC_READ_DISCRETE;
    case NCL_MB_INPUT: return NCL_MB_FC_READ_INPUT;
    default: return NCL_MB_FC_READ_HOLDING;
    }
}

unsigned ncl_modbus_area_max_read(ncl_modbus_area area)
{
    return (area == NCL_MB_COIL || area == NCL_MB_DISCRETE) ? 2000u : 125u;
}

unsigned ncl_modbus_area_max_write(ncl_modbus_area area)
{
    return (area == NCL_MB_COIL || area == NCL_MB_DISCRETE) ? 1968u : 123u;
}

/* ============================================================ byte order == */

bool ncl_modbus_word_order_parse(const char *text, ncl_modbus_word_order *out)
{
    static const char *const kNames[] = {"ABCD", "CDAB", "BADC", "DCBA"};
    size_t i;

    if (ncl_str_is_blank(text)) {
        return false;
    }
    for (i = 0; i < 4; i++) {
        if (ncl_streq_ignore_case(text, kNames[i])) {
            if (out != NULL) {
                *out = (ncl_modbus_word_order)i;
            }
            return true;
        }
    }
    return false;
}

const char *ncl_modbus_word_order_name(ncl_modbus_word_order order)
{
    static const char *const kNames[] = {"ABCD", "CDAB", "BADC", "DCBA"};

    return (unsigned)order < 4 ? kNames[order] : "?";
}

unsigned ncl_modbus_registers_per_element(ncl_dtype dtype)
{
    switch (dtype) {
    case NCL_DTYPE_BIT:
        return 0;
    case NCL_DTYPE_BYTE:
    case NCL_DTYPE_INT16:
        return 1;
    case NCL_DTYPE_INT32:
    case NCL_DTYPE_FLOAT32:
        return 2;
    case NCL_DTYPE_FLOAT64:
        return 4;
    default:
        return 1; /* strings are measured in registers by the caller */
    }
}

/** Apply the word/byte permutation of @p order to @p bytes (in place). */
static void order_apply(uint8_t *bytes, size_t len, ncl_modbus_word_order order)
{
    size_t i;
    bool swap_words = order == NCL_MB_ORDER_CDAB || order == NCL_MB_ORDER_DCBA;
    bool swap_bytes = order == NCL_MB_ORDER_BADC || order == NCL_MB_ORDER_DCBA;

    if (swap_bytes) {
        for (i = 0; i + 1 < len; i += 2) {
            uint8_t tmp = bytes[i];

            bytes[i] = bytes[i + 1];
            bytes[i + 1] = tmp;
        }
    }
    if (swap_words) {
        for (i = 0; i + 3 < len; i += 4) {
            uint8_t tmp[2];

            memcpy(tmp, bytes + i, 2);
            memcpy(bytes + i, bytes + i + 2, 2);
            memcpy(bytes + i + 2, tmp, 2);
        }
    }
}

/** Undo order_apply(): the same permutation (each is its own inverse). */
#define order_undo order_apply

static int64_t sign_extend(uint64_t value, unsigned bits)
{
    uint64_t sign_bit = (uint64_t)1 << (bits - 1);

    if ((value & sign_bit) != 0) {
        return (int64_t)(value | ~(((uint64_t)1 << bits) - 1));
    }
    return (int64_t)value;
}

ncl_err ncl_modbus_decode(const uint8_t *data, size_t data_len, size_t offset,
                          ncl_dtype dtype, size_t text_chars,
                          ncl_modbus_word_order order, ncl_json **out)
{
    uint8_t buffer[8];
    size_t width;
    size_t i;

    if (out != NULL) {
        *out = NULL;
    }
    if (data == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    width = ncl_modbus_registers_per_element(dtype) * 2u;
    if (dtype == NCL_DTYPE_STRING) {
        uint8_t text[256];
        size_t chars = text_chars;

        if (chars > sizeof(text) - 1) {
            chars = sizeof(text) - 1;
        }
        if (offset + chars > data_len) {
            return NCL_ERR_RANGE;
        }
        memcpy(text, data + offset, chars);
        if (order == NCL_MB_ORDER_BADC || order == NCL_MB_ORDER_DCBA) {
            order_apply(text, chars, NCL_MB_ORDER_BADC);
        }
        text[chars] = '\0';
        /* A NUL inside the field ends the string, as a device would. */
        for (i = 0; i < chars; i++) {
            if (text[i] == 0) {
                text[i] = ' ';
            }
        }
        *out = ncl_json_new_string((const char *)text);
        return *out != NULL ? NCL_OK : NCL_ERR_NOMEM;
    }
    if (width == 0 || width > sizeof(buffer)) {
        return NCL_ERR_INVALID_DATA_TYPE;
    }
    if (offset + width > data_len) {
        return NCL_ERR_RANGE;
    }
    memcpy(buffer, data + offset, width);
    order_undo(buffer, width, order);

    switch (dtype) {
    case NCL_DTYPE_BYTE:
        *out = ncl_json_new_int(buffer[1]); /* the low byte of the register */
        break;
    case NCL_DTYPE_INT16:
        *out = ncl_json_new_int(sign_extend((uint64_t)((buffer[0] << 8) | buffer[1]),
                                            16));
        break;
    case NCL_DTYPE_INT32: {
        uint32_t raw = ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
                       ((uint32_t)buffer[2] << 8) | buffer[3];

        *out = ncl_json_new_int(sign_extend(raw, 32));
        break;
    }
    case NCL_DTYPE_FLOAT32: {
        uint32_t raw = ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
                       ((uint32_t)buffer[2] << 8) | buffer[3];
        float value;

        memcpy(&value, &raw, sizeof(value));
        *out = ncl_json_new_double((double)value);
        break;
    }
    case NCL_DTYPE_FLOAT64: {
        uint64_t raw = 0;
        double value;

        for (i = 0; i < 8; i++) {
            raw = (raw << 8) | buffer[i];
        }
        memcpy(&value, &raw, sizeof(value));
        *out = ncl_json_new_double(value);
        break;
    }
    default:
        return NCL_ERR_INVALID_DATA_TYPE;
    }
    return *out != NULL ? NCL_OK : NCL_ERR_NOMEM;
}

ncl_err ncl_modbus_encode(const ncl_json *value, ncl_dtype dtype,
                          size_t text_chars, ncl_modbus_word_order order,
                          uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t buffer[8];
    size_t width;
    size_t i;

    if (value == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (dtype == NCL_DTYPE_STRING) {
        const char *text = ncl_json_as_string(value);
        size_t chars = text_chars;

        if (text == NULL) {
            return NCL_ERR_INVALID_VALUE;
        }
        if (order == NCL_MB_ORDER_CDAB || order == NCL_MB_ORDER_DCBA) {
            return NCL_ERR_NOT_SUPPORTED; /* strings are written as they read */
        }
        if (out_cap < chars) {
            return NCL_ERR_RANGE;
        }
        memset(out, 0, chars);
        for (i = 0; i < chars && text[i] != '\0'; i++) {
            out[i] = (uint8_t)text[i];
        }
        if (order == NCL_MB_ORDER_BADC) {
            order_apply(out, chars, NCL_MB_ORDER_BADC);
        }
        *out_len = chars;
        return NCL_OK;
    }
    width = ncl_modbus_registers_per_element(dtype) * 2u;
    if (width == 0 || width > sizeof(buffer) || out_cap < width) {
        return NCL_ERR_INVALID_DATA_TYPE;
    }
    switch (dtype) {
    case NCL_DTYPE_BYTE:
    case NCL_DTYPE_INT16: {
        long long number = 0;

        if (!ncl_json_as_int(value, &number)) {
            return NCL_ERR_INVALID_VALUE;
        }
        buffer[0] = (uint8_t)((number >> 8) & 0xFF);
        buffer[1] = (uint8_t)(number & 0xFF);
        break;
    }
    case NCL_DTYPE_INT32: {
        long long number = 0;
        uint32_t raw;

        if (!ncl_json_as_int(value, &number)) {
            return NCL_ERR_INVALID_VALUE;
        }
        raw = (uint32_t)(number & 0xFFFFFFFFLL);
        buffer[0] = (uint8_t)(raw >> 24);
        buffer[1] = (uint8_t)(raw >> 16);
        buffer[2] = (uint8_t)(raw >> 8);
        buffer[3] = (uint8_t)raw;
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
        buffer[0] = (uint8_t)(raw >> 24);
        buffer[1] = (uint8_t)(raw >> 16);
        buffer[2] = (uint8_t)(raw >> 8);
        buffer[3] = (uint8_t)raw;
        break;
    }
    case NCL_DTYPE_FLOAT64: {
        double number = 0;
        uint64_t raw;

        if (!ncl_json_as_double(value, &number)) {
            return NCL_ERR_INVALID_VALUE;
        }
        memcpy(&raw, &number, sizeof(raw));
        for (i = 0; i < 8; i++) {
            buffer[i] = (uint8_t)(raw >> (56 - 8 * i));
        }
        break;
    }
    default:
        return NCL_ERR_INVALID_DATA_TYPE;
    }
    memcpy(out, buffer, width);
    order_apply(out, width, order);
    *out_len = width;
    return NCL_OK;
}

/* ================================================================== CRC === */

uint16_t ncl_modbus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t i;

    if (data == NULL) {
        return crc;
    }
    for (i = 0; i < len; i++) {
        int bit;

        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            if ((crc & 1) != 0) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* ================================================================== PDU === */

size_t ncl_modbus_read_pdu(uint8_t *out, size_t cap, uint8_t function,
                           uint16_t address, uint16_t count)
{
    if (out == NULL || cap < 5) {
        return 0;
    }
    out[0] = function;
    out[1] = (uint8_t)(address >> 8);
    out[2] = (uint8_t)address;
    out[3] = (uint8_t)(count >> 8);
    out[4] = (uint8_t)count;
    return 5;
}

size_t ncl_modbus_write_single_pdu(uint8_t *out, size_t cap, uint8_t function,
                                   uint16_t address, uint16_t value)
{
    if (out == NULL || cap < 5) {
        return 0;
    }
    out[0] = function;
    out[1] = (uint8_t)(address >> 8);
    out[2] = (uint8_t)address;
    out[3] = (uint8_t)(value >> 8);
    out[4] = (uint8_t)value;
    return 5;
}

size_t ncl_modbus_write_multi_pdu(uint8_t *out, size_t cap, uint8_t function,
                                  uint16_t address, uint16_t count,
                                  const uint8_t *data, size_t data_len)
{
    if (out == NULL || data == NULL || data_len > 255 || cap < 6 + data_len) {
        return 0;
    }
    out[0] = function;
    out[1] = (uint8_t)(address >> 8);
    out[2] = (uint8_t)address;
    out[3] = (uint8_t)(count >> 8);
    out[4] = (uint8_t)count;
    out[5] = (uint8_t)data_len;
    memcpy(out + 6, data, data_len);
    return 6 + data_len;
}

ncl_err ncl_modbus_check_exception(const uint8_t *pdu, size_t pdu_len,
                                   uint8_t function, char *message,
                                   size_t message_len)
{
    static const struct {
        uint8_t     code;
        const char *text;
    } kExceptions[] = {
        {0x01, "设备不支持该功能码"},
        {0x02, "地址非法（越界或区不存在）"},
        {0x03, "数据值非法（数量或取值超限）"},
        {0x04, "从站设备故障"},
        {0x05, "从站已确认，处理中"},
        {0x06, "从站忙"},
        {0x08, "存储奇偶校验错"},
        {0x0A, "网关路径不可用"},
        {0x0B, "网关目标无响应"},
    };
    size_t i;

    if (pdu == NULL || pdu_len < 2) {
        return NCL_ERR_PARSE;
    }
    if (pdu[0] != (uint8_t)(function | 0x80)) {
        return NCL_OK;
    }
    for (i = 0; i < sizeof(kExceptions) / sizeof(kExceptions[0]); i++) {
        if (kExceptions[i].code == pdu[1]) {
            if (message != NULL) {
                snprintf(message, message_len, "Modbus 异常 0x%02X：%s", pdu[1],
                         kExceptions[i].text);
            }
            /* 0x05/0x06/0x08 are "later": the link is fine, the device is
             * busy, so these are business level, not protocol level. */
            return (pdu[1] == 0x05 || pdu[1] == 0x06 || pdu[1] == 0x08)
                       ? NCL_DRV_ERR_BUSINESS(0x100 | pdu[1])
                       : NCL_DRV_ERR_PROTOCOL(0x100 | pdu[1]);
        }
    }
    if (message != NULL) {
        snprintf(message, message_len, "Modbus 未知异常码 0x%02X", pdu[1]);
    }
    return NCL_DRV_ERR_PROTOCOL(0x1FF);
}

ncl_err ncl_modbus_check_read_reply(const uint8_t *pdu, size_t pdu_len,
                                    uint8_t function, size_t expect_bytes,
                                    const uint8_t **data, char *message,
                                    size_t message_len)
{
    ncl_err err = ncl_modbus_check_exception(pdu, pdu_len, function, message,
                                             message_len);

    if (err != NCL_OK) {
        return err;
    }
    if (pdu[0] != function || pdu_len < 3) {
        if (message != NULL) {
            snprintf(message, message_len, "功能码不符：期望 0x%02X，收到 0x%02X",
                     function, pdu[0]);
        }
        return NCL_DRV_ERR_PROTOCOL(1);
    }
    if (pdu[1] != expect_bytes || pdu_len < (size_t)pdu[1] + 2) {
        if (message != NULL) {
            snprintf(message, message_len,
                     "字节数不符：期望 %u，收到 %u（报文 %u 字节）",
                     (unsigned)expect_bytes, (unsigned)pdu[1],
                     (unsigned)pdu_len);
        }
        return NCL_DRV_ERR_PROTOCOL(2);
    }
    if (data != NULL) {
        *data = pdu + 2;
    }
    return NCL_OK;
}

ncl_err ncl_modbus_check_write_reply(const uint8_t *pdu, size_t pdu_len,
                                     uint8_t function, uint16_t address,
                                     uint16_t count, char *message,
                                     size_t message_len)
{
    uint16_t echo_address;
    uint16_t echo_count;
    ncl_err err = ncl_modbus_check_exception(pdu, pdu_len, function, message,
                                             message_len);

    if (err != NCL_OK) {
        return err;
    }
    if (pdu[0] != function || pdu_len < 5) {
        if (message != NULL) {
            snprintf(message, message_len, "写响应不完整（%u 字节）",
                     (unsigned)pdu_len);
        }
        return NCL_DRV_ERR_PROTOCOL(3);
    }
    echo_address = (uint16_t)((pdu[1] << 8) | pdu[2]);
    echo_count = (uint16_t)((pdu[3] << 8) | pdu[4]);
    if (echo_address != address || echo_count != count) {
        if (message != NULL) {
            snprintf(message, message_len,
                     "写响应回显不符：期望 %u/%u，收到 %u/%u",
                     (unsigned)address, (unsigned)count, (unsigned)echo_address,
                     (unsigned)echo_count);
        }
        return NCL_DRV_ERR_PROTOCOL(4);
    }
    return NCL_OK;
}

/* ============================================================ transport == */

size_t ncl_modbus_tcp_frame(uint8_t *out, size_t cap, uint16_t transaction,
                            uint8_t unit, const uint8_t *pdu, size_t pdu_len)
{
    size_t total = pdu_len + 7u;

    if (out == NULL || pdu == NULL || cap < total || pdu_len > 253) {
        return 0;
    }
    out[0] = (uint8_t)(transaction >> 8);
    out[1] = (uint8_t)transaction;
    out[2] = 0x00; /* protocol id */
    out[3] = 0x00;
    out[4] = (uint8_t)((pdu_len + 1u) >> 8);
    out[5] = (uint8_t)(pdu_len + 1u);
    out[6] = unit;
    memcpy(out + 7, pdu, pdu_len);
    return total;
}

ncl_err ncl_modbus_tcp_split(const uint8_t *frame, size_t len,
                             uint16_t want_transaction, const uint8_t **pdu,
                             size_t *pdu_len, size_t *frame_len)
{
    size_t length;
    size_t total;

    if (frame == NULL || len < 8) {
        return NCL_ERR_PARSE;
    }
    if (frame[2] != 0x00 || frame[3] != 0x00) {
        return NCL_DRV_ERR_PROTOCOL(5); /* not Modbus: protocol id is 0 */
    }
    length = ((size_t)frame[4] << 8) | frame[5];
    if (length < 2 || length > 254) {
        return NCL_DRV_ERR_PROTOCOL(6);
    }
    total = length + 6u;
    if (len < total) {
        return NCL_ERR_RANGE;
    }
    if (want_transaction != 0xFFFFu) {
        uint16_t transaction = (uint16_t)((frame[0] << 8) | frame[1]);

        if (transaction != want_transaction) {
            return NCL_DRV_ERR_PROTOCOL(7); /* a stale reply from an earlier call */
        }
    }
    if (pdu != NULL) {
        *pdu = frame + 7;
    }
    if (pdu_len != NULL) {
        *pdu_len = length - 1u;
    }
    if (frame_len != NULL) {
        *frame_len = total;
    }
    return NCL_OK;
}

size_t ncl_modbus_rtu_frame(uint8_t *out, size_t cap, uint8_t unit,
                            const uint8_t *pdu, size_t pdu_len)
{
    uint16_t crc;

    if (out == NULL || pdu == NULL || pdu_len > 252 || cap < pdu_len + 3u) {
        return 0;
    }
    out[0] = unit;
    memcpy(out + 1, pdu, pdu_len);
    crc = ncl_modbus_crc16(out, pdu_len + 1u);
    out[pdu_len + 1] = (uint8_t)(crc & 0xFF); /* low byte first */
    out[pdu_len + 2] = (uint8_t)(crc >> 8);
    return pdu_len + 3u;
}

ncl_err ncl_modbus_rtu_split(const uint8_t *frame, size_t len, uint8_t want_unit,
                             const uint8_t **pdu, size_t *pdu_len,
                             size_t *frame_len)
{
    uint16_t crc;
    uint16_t expected;

    if (frame == NULL || len < 4) {
        return NCL_ERR_PARSE;
    }
    if (frame[0] != want_unit) {
        return NCL_DRV_ERR_PROTOCOL(8); /* another slave on the bus */
    }
    crc = ncl_modbus_crc16(frame, len - 2u);
    expected = (uint16_t)(frame[len - 2] | ((uint16_t)frame[len - 1] << 8));
    if (crc != expected) {
        return NCL_DRV_ERR_TRANSPORT(2); /* corrupted: re-send, do not trust it */
    }
    if (pdu != NULL) {
        *pdu = frame + 1;
    }
    if (pdu_len != NULL) {
        *pdu_len = len - 3u;
    }
    if (frame_len != NULL) {
        *frame_len = len;
    }
    return NCL_OK;
}

ncl_err ncl_modbus_rtu_reply_size(const uint8_t *head, size_t head_len,
                                  size_t *total)
{
    uint8_t function;

    if (head == NULL || total == NULL || head_len < 2) {
        return NCL_ERR_INVALID_ARG;
    }
    *total = 0;
    function = head[1];
    if ((function & 0x80) != 0) {
        *total = 5; /* address + function + exception + CRC */
        return NCL_OK;
    }
    switch (function) {
    case NCL_MB_FC_READ_COILS:
    case NCL_MB_FC_READ_DISCRETE:
    case NCL_MB_FC_READ_HOLDING:
    case NCL_MB_FC_READ_INPUT:
        if (head_len < 3) {
            return NCL_ERR_RANGE; /* the byte count has not arrived yet */
        }
        *total = (size_t)head[2] + 5u;
        return NCL_OK;
    case NCL_MB_FC_WRITE_COIL:
    case NCL_MB_FC_WRITE_REGISTER:
    case NCL_MB_FC_WRITE_COILS:
    case NCL_MB_FC_WRITE_REGISTERS:
        *total = 8;
        return NCL_OK;
    default:
        return NCL_DRV_ERR_PROTOCOL(9); /* not a reply we asked for */
    }
}
