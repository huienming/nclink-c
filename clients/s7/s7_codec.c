/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Siemens S7comm byte level (see ncl_s7.h).
 * TPKT over COTP over S7comm, big endian, three length fields (§7.4).
 */

#include "nclink/clients/s7.h"

#include <stdio.h>
#include <string.h>

/* ================================================================ TPKT === */

size_t ncl_s7_tpkt(uint8_t *out, size_t cap, const uint8_t *payload,
                   size_t payload_len)
{
    size_t total = 4u + payload_len;

    if (out == NULL || cap < total || (payload_len > 0 && payload == NULL) ||
        total > 0xFFFFu) {
        return 0;
    }
    out[0] = 0x03; /* version   */
    out[1] = 0x00; /* reserved  */
    out[2] = (uint8_t)(total >> 8);
    out[3] = (uint8_t)total;
    if (payload_len > 0) {
        memcpy(out + 4, payload, payload_len);
    }
    return total;
}

ncl_err ncl_s7_tpkt_split(const uint8_t *frame, size_t len,
                          const uint8_t **payload, size_t *payload_len,
                          size_t *frame_len)
{
    size_t total;

    if (frame == NULL || len < 4) {
        return NCL_ERR_RANGE;
    }
    if (frame[0] != 0x03) {
        return NCL_DRV_ERR_PROTOCOL(1); /* not ISO-on-TCP */
    }
    total = ((size_t)frame[2] << 8) | frame[3];
    if (total < 4 || total > 0xFFFFu) {
        return NCL_DRV_ERR_PROTOCOL(2);
    }
    if (len < total) {
        return NCL_ERR_RANGE; /* the frame is still arriving */
    }
    if (payload != NULL) {
        *payload = frame + 4;
    }
    if (payload_len != NULL) {
        *payload_len = total - 4u;
    }
    if (frame_len != NULL) {
        *frame_len = total;
    }
    return NCL_OK;
}

/* ================================================================ COTP === */

uint16_t ncl_s7_tsap(unsigned rack, unsigned slot)
{
    /* 0x03 is the "S7 basic communication" connection type; the rack and slot
     * sit in the two low bytes: rack 0 slot 2 -> 0x0302 (§2). */
    return (uint16_t)(0x0300u + (rack & 0x07u) * 0x20u + (slot & 0x1Fu));
}

size_t ncl_s7_cotp_cr(uint8_t *out, size_t cap, uint16_t calling_tsap,
                      uint16_t called_tsap, uint8_t pdu_size_code)
{
    uint8_t cotp[32];
    size_t at = 0;
    size_t total;

    if (out == NULL || cap < 32) {
        return 0;
    }
    /* The length byte counts everything from the PDU type onward. */
    cotp[at++] = 0x11;
    cotp[at++] = NCL_S7_COTP_CR;
    cotp[at++] = 0x00; /* destination reference (unused in a request) */
    cotp[at++] = 0x00;
    cotp[at++] = 0x00; /* source reference      */
    cotp[at++] = 0x01;
    cotp[at++] = 0x00; /* class and options     */
    cotp[at++] = 0xC1; /* calling TSAP          */
    cotp[at++] = 0x02;
    cotp[at++] = (uint8_t)(calling_tsap >> 8);
    cotp[at++] = (uint8_t)calling_tsap;
    cotp[at++] = 0xC2; /* called TSAP           */
    cotp[at++] = 0x02;
    cotp[at++] = (uint8_t)(called_tsap >> 8);
    cotp[at++] = (uint8_t)called_tsap;
    cotp[at++] = 0xC0; /* PDU size negotiation  */
    cotp[at++] = 0x01;
    cotp[at++] = pdu_size_code;
    total = ncl_s7_tpkt(out, cap, cotp, at);
    return total;
}

ncl_err ncl_s7_cotp_split(const uint8_t *frame, size_t len, uint8_t *pdu_type,
                          uint8_t *pdu_size_code, const uint8_t **payload,
                          size_t *payload_len, size_t *frame_len)
{
    /* @p frame is the COTP section, i.e. what ncl_s7_tpkt_split() hands back:
     * TPKT framing is TPKT's business. */
    const uint8_t *cotp = frame;
    size_t cotp_len = len;
    size_t header_len;
    size_t at;

    if (frame == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (pdu_size_code != NULL) {
        *pdu_size_code = 0;
    }
    if (payload != NULL) {
        *payload = NULL;
    }
    if (payload_len != NULL) {
        *payload_len = 0;
    }
    if (cotp_len < 2) {
        return NCL_ERR_RANGE; /* the header is still arriving */
    }
    if (pdu_type != NULL) {
        *pdu_type = cotp[1];
    }
    /* The header length depends on the PDU type. */
    switch (cotp[1]) {
    case NCL_S7_COTP_CR:
    case NCL_S7_COTP_CC:
        header_len = 6; /* length, type, dst ref (2), src ref (2) */
        break;
    case NCL_S7_COTP_DT:
        header_len = 3; /* length, type, TPDU number/EOT          */
        break;
    default:
        header_len = 2;
        break;
    }
    if (cotp_len < header_len) {
        return NCL_ERR_RANGE;
    }
    /* Walk the parameters of a CR/CC looking for the PDU size (0xC0). */
    if (cotp[1] == NCL_S7_COTP_CR || cotp[1] == NCL_S7_COTP_CC) {
        at = header_len + 1u; /* skip the class/options byte */
        while (at + 1u < cotp_len) {
            uint8_t code = cotp[at];
            uint8_t plen = cotp[at + 1];

            if (code == 0xC0 && plen >= 1 && at + 2u < cotp_len &&
                pdu_size_code != NULL) {
                *pdu_size_code = cotp[at + 2];
            }
            if (code == 0x00) {
                break; /* user data follows */
            }
            at += 2u + plen;
        }
    }
    if (payload != NULL) {
        *payload = cotp + header_len;
    }
    if (payload_len != NULL) {
        *payload_len = cotp_len - header_len;
    }
    if (frame_len != NULL) {
        *frame_len = cotp_len;
    }
    return NCL_OK;
}

size_t ncl_s7_cotp_dt(uint8_t *out, size_t cap, const uint8_t *s7, size_t s7_len)
{
    if (out == NULL || s7 == NULL || cap < s7_len + 7u) {
        return 0;
    }
    /* TPKT + the three COTP bytes + the S7 PDU, built directly so the two
     * blocks do not have to be concatenated twice. */
    {
        size_t total = 7u + s7_len;

        out[0] = 0x03;
        out[1] = 0x00;
        out[2] = (uint8_t)(total >> 8);
        out[3] = (uint8_t)total;
        out[4] = 0x02;
        out[5] = NCL_S7_COTP_DT;
        out[6] = 0x80;
        memcpy(out + 7, s7, s7_len);
        return total;
    }
}

/* ================================================================ S7comm == */

size_t ncl_s7_pdu(uint8_t *out, size_t cap, uint8_t rosctr, uint16_t pdu_ref,
                  const uint8_t *params, size_t params_len, const uint8_t *data,
                  size_t data_len)
{
    size_t total = 10u + params_len + data_len;

    if (out == NULL || cap < total || params_len > 0xFFFFu ||
        data_len > 0xFFFFu) {
        return 0;
    }
    out[0] = NCL_S7_PROTOCOL_ID;
    out[1] = rosctr;
    out[2] = 0x00; /* redundancy identification */
    out[3] = 0x00;
    out[4] = (uint8_t)(pdu_ref >> 8);
    out[5] = (uint8_t)pdu_ref;
    out[6] = (uint8_t)(params_len >> 8);
    out[7] = (uint8_t)params_len;
    out[8] = (uint8_t)(data_len >> 8);
    out[9] = (uint8_t)data_len;
    if (params_len > 0) {
        memcpy(out + 10, params, params_len);
    }
    if (data_len > 0) {
        memcpy(out + 10 + params_len, data, data_len);
    }
    return total;
}

ncl_err ncl_s7_pdu_split(const uint8_t *pdu, size_t len, ncl_s7_view *out,
                         size_t *pdu_len)
{
    size_t params_len;
    size_t data_len;
    size_t total;

    if (pdu == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (len < 10) {
        return NCL_ERR_RANGE;
    }
    if (pdu[0] != NCL_S7_PROTOCOL_ID) {
        return NCL_DRV_ERR_PROTOCOL(4); /* not an S7 PDU */
    }
    params_len = ((size_t)pdu[6] << 8) | pdu[7];
    data_len = ((size_t)pdu[8] << 8) | pdu[9];
    total = 10u + params_len + data_len;
    if (len < total) {
        return NCL_ERR_RANGE;
    }
    memset(out, 0, sizeof(*out));
    out->rosctr = pdu[1];
    out->pdu_ref = (uint16_t)((pdu[4] << 8) | pdu[5]);
    /* The two error fields live where the parameter length would be for a
     * plain Ack. */
    if (out->rosctr == NCL_S7_ROSCTR_ACK) {
        out->error_class = (uint16_t)((pdu[6] << 8) | pdu[7]);
        out->error_code = (uint16_t)((pdu[8] << 8) | pdu[9]);
        out->params = NULL;
        out->params_len = 0;
        out->data = NULL;
        out->data_len = 0;
    } else {
        out->params = pdu + 10;
        out->params_len = params_len;
        out->data = pdu + 10 + params_len;
        out->data_len = data_len;
    }
    if (pdu_len != NULL) {
        *pdu_len = total;
    }
    return NCL_OK;
}

size_t ncl_s7_setup_params(uint8_t *out, size_t cap, uint16_t max_amq_calling,
                           uint16_t max_amq_called, uint16_t pdu_size)
{
    if (out == NULL || cap < 8) {
        return 0;
    }
    out[0] = NCL_S7_FUNC_SETUP_COMM;
    out[1] = 0x00; /* reserved */
    out[2] = (uint8_t)(max_amq_calling >> 8);
    out[3] = (uint8_t)max_amq_calling;
    out[4] = (uint8_t)(max_amq_called >> 8);
    out[5] = (uint8_t)max_amq_called;
    out[6] = (uint8_t)(pdu_size >> 8);
    out[7] = (uint8_t)pdu_size;
    return 8;
}

ncl_err ncl_s7_setup_reply(const uint8_t *params, size_t params_len,
                           uint16_t *pdu_size)
{
    if (params == NULL || params_len < 8) {
        return NCL_ERR_RANGE;
    }
    if (params[0] != NCL_S7_FUNC_SETUP_COMM) {
        return NCL_DRV_ERR_PROTOCOL(5);
    }
    if (pdu_size != NULL) {
        *pdu_size = (uint16_t)((params[6] << 8) | params[7]);
    }
    return NCL_OK;
}

/* ------------------------------------------------------------ variables -- */

size_t ncl_s7_var_params(uint8_t *out, size_t cap, uint8_t function,
                         const ncl_s7_item *items, size_t count)
{
    size_t total = 2u + count * NCL_S7_ITEM_BYTES;
    size_t at = 2;
    size_t i;

    if (out == NULL || cap < total || (count > 0 && items == NULL) ||
        count > 0xFFu) {
        return 0;
    }
    out[0] = function;
    out[1] = (uint8_t)count;
    for (i = 0; i < count; i++) {
        out[at] = 0x12;      /* variable specification  */
        out[at + 1] = 0x0A;  /* the length that follows */
        out[at + 2] = 0x10;  /* syntax id: S7ANY        */
        out[at + 3] = items[i].tsize;
        out[at + 4] = (uint8_t)(items[i].elements >> 8);
        out[at + 5] = (uint8_t)items[i].elements;
        out[at + 6] = (uint8_t)(items[i].db >> 8);
        out[at + 7] = (uint8_t)items[i].db;
        out[at + 8] = items[i].area;
        out[at + 9] = (uint8_t)(items[i].bit >> 16);
        out[at + 10] = (uint8_t)(items[i].bit >> 8);
        out[at + 11] = (uint8_t)items[i].bit;
        at += NCL_S7_ITEM_BYTES;
    }
    return total;
}

size_t ncl_s7_write_data(uint8_t *out, size_t cap, uint8_t tsize, uint16_t bits,
                         const uint8_t *bytes, size_t byte_len)
{
    size_t padded = byte_len % 2u != 0 ? byte_len + 1u : byte_len;
    size_t total = 4u + padded;

    if (out == NULL || cap < total || (byte_len > 0 && bytes == NULL)) {
        return 0;
    }
    out[0] = 0x00; /* reserved */
    out[1] = tsize;
    out[2] = (uint8_t)(bits >> 8);
    out[3] = (uint8_t)bits;
    if (byte_len > 0) {
        memcpy(out + 4, bytes, byte_len);
    }
    if (padded > byte_len) {
        out[4 + byte_len] = 0; /* an odd byte count is padded */
    }
    return total;
}

ncl_err ncl_s7_read_reply(const uint8_t *data, size_t data_len, size_t count,
                          ncl_s7_reply_item *items, size_t *items_filled)
{
    size_t at = 0;
    size_t i;

    if (items_filled != NULL) {
        *items_filled = 0;
    }
    if (data == NULL || items == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < count; i++) {
        uint8_t code;
        uint16_t bits;
        size_t bytes;

        if (at + 4u > data_len) {
            return NCL_ERR_RANGE; /* the reply is short of items */
        }
        code = data[at];
        items[i].return_code = code;
        items[i].tsize = data[at + 1];
        bits = (uint16_t)((data[at + 2] << 8) | data[at + 3]);
        items[i].bits = bits;
        items[i].bytes = NULL;
        items[i].byte_len = 0;
        at += 4;
        if (code != 0xFF) {
            /* A failed item carries no data; 0x04 means "no data" too. */
            if (items_filled != NULL) {
                *items_filled = i + 1;
            }
            continue;
        }
        /* Bit units answer one bit per bit, everything else one byte per
         * eight bits, padded to an even count. */
        bytes = items[i].tsize == NCL_S7_TS_REPLY_BIT ? (size_t)((bits + 7u) / 8u)
                                                      : (size_t)(bits / 8u);
        if (at + bytes > data_len) {
            return NCL_ERR_RANGE;
        }
        items[i].bytes = data + at;
        items[i].byte_len = bytes;
        at += bytes;
        if (bytes % 2u != 0) {
            at++; /* the pads are there to keep items aligned */
        }
        if (items_filled != NULL) {
            *items_filled = i + 1;
        }
    }
    return NCL_OK;
}

ncl_err ncl_s7_check_return_code(uint8_t code, char *message, size_t message_len)
{
    static const struct {
        uint8_t     code;
        const char *text;
    } kCodes[] = {
        {0x01, "应用关系错误"},
        {0x03, "资源不足"},
        {0x05, "地址越界"},
        {0x06, "数据类型不支持"},
        {0x07, "数据类型不一致"},
        {0x0A, "对象不存在（DB 号或地址错）"},
        {0x0B, "对象已存在"},
    };
    size_t i;

    if (code == 0xFF || code == 0x00) {
        return NCL_OK;
    }
    for (i = 0; i < sizeof(kCodes) / sizeof(kCodes[0]); i++) {
        if (kCodes[i].code == code) {
            if (message != NULL) {
                snprintf(message, message_len, "S7 数据项错误 0x%02X：%s", code,
                         kCodes[i].text);
            }
            return (code == 0x05 || code == 0x06 || code == 0x07)
                       ? NCL_DRV_ERR_PROTOCOL(0x300 | code)
                       : NCL_DRV_ERR_BUSINESS(0x300 | code);
        }
    }
    if (message != NULL) {
        snprintf(message, message_len, "S7 未知数据项错误 0x%02X", code);
    }
    return NCL_DRV_ERR_PROTOCOL(0x3FF);
}

ncl_err ncl_s7_check_pdu_error(uint16_t error_class, uint16_t error_code,
                               char *message, size_t message_len)
{
    if (error_class == 0 && error_code == 0) {
        return NCL_OK;
    }
    if (message != NULL) {
        snprintf(message, message_len, "S7 协议错误 0x%02X/0x%02X", error_class,
                 error_code);
    }
    /* Class 0x84 is "error on service processing", which is where address
     * problems surface; 0x81 is the application relationship, i.e. the link. */
    return error_class == 0x81 ? NCL_DRV_ERR_TRANSPORT(0x301)
                               : NCL_DRV_ERR_PROTOCOL(0x300 | (error_class & 0xFF));
}

/* ================================================================= data === */

/** Area code, DB number and the type implied by a suffixed name. */
bool ncl_s7_area_lookup(const char *name, uint8_t *area, uint16_t *db,
                        ncl_dtype *implied)
{
    char base[8];
    size_t len;

    if (implied != NULL) {
        /* -1: the name says nothing about the access width, so the point map's
         * own type stands. "MB"/"MW"/"MD"/"MX" do say it (§4). */
        *implied = (ncl_dtype)-1;
    }
    if (db != NULL) {
        *db = 0;
    }
    if (ncl_str_is_blank(name) || area == NULL) {
        return false;
    }
    len = strlen(name);
    if (len >= sizeof(base)) {
        return false;
    }
    memcpy(base, name, len + 1);
    /* "MB"/"MW"/"MD" and the I/Q twins name the area plus the access width,
     * which is what the point map usually means (§4). */
    if (len >= 2) {
        char last = base[len - 1];
        char first = base[0];

        if ((first == 'M' || first == 'I' || first == 'E' || first == 'Q' ||
             first == 'A') &&
            (last == 'B' || last == 'W' || last == 'D' || last == 'X')) {
            base[len - 1] = '\0';
            if (implied != NULL) {
                *implied = last == 'B'   ? NCL_DTYPE_BYTE
                           : last == 'W' ? NCL_DTYPE_INT16
                           : last == 'D' ? NCL_DTYPE_INT32
                                         : NCL_DTYPE_BIT;
            }
        }
    }
    if (ncl_streq_ignore_case(base, "I") || ncl_streq_ignore_case(base, "E")) {
        *area = NCL_S7_AREA_I;
        return true;
    }
    if (ncl_streq_ignore_case(base, "Q") || ncl_streq_ignore_case(base, "A")) {
        *area = NCL_S7_AREA_Q;
        return true;
    }
    if (ncl_streq_ignore_case(base, "M")) {
        *area = NCL_S7_AREA_M;
        return true;
    }
    if (ncl_streq_ignore_case(base, "T")) {
        *area = NCL_S7_AREA_T;
        return true;
    }
    if (ncl_streq_ignore_case(base, "C")) {
        *area = NCL_S7_AREA_C;
        return true;
    }
    /* "DB1", "DB100": the data block number is part of the name. */
    if ((base[0] == 'D' || base[0] == 'd') && (base[1] == 'B' || base[1] == 'b')) {
        long number = 0;
        const char *digits = base + 2;
        size_t i;

        if (*digits == '\0') {
            return false;
        }
        for (i = 0; digits[i] != '\0'; i++) {
            if (digits[i] < '0' || digits[i] > '9') {
                return false;
            }
            number = number * 10 + (digits[i] - '0');
            if (number > 65535) {
                return false;
            }
        }
        *area = NCL_S7_AREA_DB;
        if (db != NULL) {
            *db = (uint16_t)number;
        }
        return true;
    }
    return false;
}

static uint8_t s7_tsize_for(ncl_dtype dtype)
{
    switch (dtype) {
    case NCL_DTYPE_BIT: return NCL_S7_TS_BIT;
    case NCL_DTYPE_BYTE: return NCL_S7_TS_BYTE;
    case NCL_DTYPE_INT16: return NCL_S7_TS_INT;
    case NCL_DTYPE_INT32: return NCL_S7_TS_DINT;
    case NCL_DTYPE_FLOAT32: return NCL_S7_TS_REAL;
    case NCL_DTYPE_FLOAT64: return 0; /* S7 has no 64 bit float */
    case NCL_DTYPE_STRING: return NCL_S7_TS_BYTE;
    default: return 0;
    }
}

ncl_dtype ncl_s7_effective_dtype(const ncl_address *address)
{
    uint8_t area = 0;
    uint16_t db = 0;
    ncl_dtype implied;

    if (address == NULL) {
        return NCL_DTYPE_INT16;
    }
    if (address->bit >= 0 || address->dtype == NCL_DTYPE_BIT) {
        return NCL_DTYPE_BIT;
    }
    if (ncl_s7_area_lookup(address->area, &area, &db, &implied) &&
        (int)implied >= 0) {
        return implied;
    }
    return address->dtype;
}

bool ncl_s7_item_for(const ncl_address *address, ncl_s7_item *item,
                     size_t *byte_len)
{
    uint8_t area = 0;
    uint16_t db = 0;
    ncl_dtype implied;
    ncl_dtype dtype;
    size_t bytes;

    if (address == NULL || item == NULL || byte_len == NULL) {
        return false;
    }
    if (!ncl_s7_area_lookup(address->area, &area, &db, &implied)) {
        return false;
    }
    dtype = ncl_s7_effective_dtype(address);
    if (address->bit >= 0 || dtype == NCL_DTYPE_BIT) {
        item->tsize = NCL_S7_TS_BIT;
        item->elements = (uint16_t)address->length;
        bytes = (size_t)address->length;
        item->bit = (uint32_t)address->offset * 8u +
                    (uint32_t)(address->bit >= 0 ? address->bit : 0);
    } else {
        if (dtype == NCL_DTYPE_STRING) {
            /* S7 strings carry a two byte header (max, current) before the
             * characters (§4). */
            bytes = (size_t)address->length + 2u;
            item->tsize = NCL_S7_TS_BYTE;
            /* The length field counts elements of the transport size, so a
             * byte oriented access counts bytes. */
            item->elements = (uint16_t)bytes;
        } else {
            item->tsize = s7_tsize_for(dtype);
            if (item->tsize == 0) {
                return false;
            }
            bytes = ncl_s7_image_bytes(dtype, (size_t)address->length);
            item->elements = (uint16_t)address->length;
        }
        item->bit = (uint32_t)address->offset * 8u;
    }
    item->area = area;
    item->db = db;
    *byte_len = bytes;
    return true;
}

size_t ncl_s7_image_bytes(ncl_dtype dtype, size_t elements)
{
    switch (dtype) {
    case NCL_DTYPE_BIT: return elements;
    case NCL_DTYPE_BYTE: return elements;
    case NCL_DTYPE_INT16: return elements * 2u;
    case NCL_DTYPE_INT32:
    case NCL_DTYPE_FLOAT32: return elements * 4u;
    case NCL_DTYPE_FLOAT64: return elements * 8u;
    default: return elements;
    }
}

ncl_err ncl_s7_decode(const uint8_t *data, size_t data_len, size_t offset,
                      ncl_dtype dtype, size_t text_chars, ncl_json **out)
{
    size_t width;

    if (out != NULL) {
        *out = NULL;
    }
    if (data == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    width = dtype == NCL_DTYPE_STRING ? text_chars + 2u
                                      : ncl_s7_image_bytes(dtype, 1);
    if (offset + width > data_len) {
        return NCL_ERR_RANGE;
    }
    data += offset;
    switch (dtype) {
    case NCL_DTYPE_BIT:
        *out = ncl_json_new_bool((data[0] & 0x01) != 0);
        break;
    case NCL_DTYPE_BYTE:
        *out = ncl_json_new_int(data[0]);
        break;
    case NCL_DTYPE_INT16:
        *out = ncl_json_new_int((int16_t)((data[0] << 8) | data[1]));
        break;
    case NCL_DTYPE_INT32: {
        uint32_t raw = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                       ((uint32_t)data[2] << 8) | data[3];

        *out = ncl_json_new_int((int32_t)raw);
        break;
    }
    case NCL_DTYPE_FLOAT32: {
        uint32_t raw = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                       ((uint32_t)data[2] << 8) | data[3];
        float value;

        memcpy(&value, &raw, sizeof(value));
        *out = ncl_json_new_double((double)value);
        break;
    }
    case NCL_DTYPE_STRING: {
        char text[512];
        size_t chars = text_chars < sizeof(text) - 1 ? text_chars
                                                     : sizeof(text) - 1;
        size_t i;

        memcpy(text, data + 2, chars); /* the two header bytes are skipped */
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

ncl_err ncl_s7_encode(const ncl_json *value, ncl_dtype dtype, size_t text_chars,
                      uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t width = dtype == NCL_DTYPE_STRING ? text_chars + 2u
                                             : ncl_s7_image_bytes(dtype, 1);

    if (value == NULL || out == NULL || out_cap < width) {
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
        out[0] = set ? 0x01 : 0x00;
        break;
    }
    case NCL_DTYPE_BYTE: {
        long long number = 0;

        if (!ncl_json_as_int(value, &number)) {
            return NCL_ERR_INVALID_VALUE;
        }
        out[0] = (uint8_t)number;
        break;
    }
    case NCL_DTYPE_INT16: {
        long long number = 0;

        if (!ncl_json_as_int(value, &number)) {
            return NCL_ERR_INVALID_VALUE;
        }
        out[0] = (uint8_t)((uint16_t)number >> 8);
        out[1] = (uint8_t)number;
        break;
    }
    case NCL_DTYPE_INT32: {
        long long number = 0;
        uint32_t raw;

        if (!ncl_json_as_int(value, &number)) {
            return NCL_ERR_INVALID_VALUE;
        }
        raw = (uint32_t)number;
        out[0] = (uint8_t)(raw >> 24);
        out[1] = (uint8_t)(raw >> 16);
        out[2] = (uint8_t)(raw >> 8);
        out[3] = (uint8_t)raw;
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
        out[0] = (uint8_t)(raw >> 24);
        out[1] = (uint8_t)(raw >> 16);
        out[2] = (uint8_t)(raw >> 8);
        out[3] = (uint8_t)raw;
        break;
    }
    case NCL_DTYPE_STRING: {
        const char *text = ncl_json_as_string(value);
        size_t chars = text_chars > 254u ? 254u : text_chars;
        size_t i;

        if (text == NULL) {
            return NCL_ERR_INVALID_VALUE;
        }
        out[0] = (uint8_t)chars; /* maximum length  */
        out[1] = 0;              /* current length  */
        for (i = 0; i < chars && text[i] != '\0'; i++) {
            out[2 + i] = (uint8_t)text[i];
        }
        out[1] = (uint8_t)i;
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
