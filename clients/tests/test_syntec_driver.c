/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * The SYNTEC driver against a mock OCAPIServer: the mock reads a packet the way
 * the real server does (12 byte header, then `Length` more), answers with the
 * same shape and echoes the request's uSerial.
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink/ncl_host.h"
#include "nclink/ncl_module.h"
#include "test_point_map.h"
#include "nclink/clients/syntec.h"
#include "syntec/ncl_syntec_driver.h"

#ifndef NCL_SYNTEC_PLUGIN_DIR
#  define NCL_SYNTEC_PLUGIN_DIR "" /* the adapter test is skipped without it */
#endif

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)(in[0] | ((in[1]) << 8));
}

static uint32_t get_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

/** The mock controller: it knows one command (200 = KrnlAPI) and the codes the
 *  test uses, which is exactly how a real one behaves for a known code. */
typedef struct {
    ncl_socket *listener;
    unsigned    port;
    ncl_thread *thread;
    bool        stop;
    int         requests;
    int         fails_to_skip;
    uint16_t    last_cmd;
    uint16_t    last_func;
    uint8_t     last_serial;
    int32_t     last_code;
    int32_t     last_size_out;
    uint8_t     answer[64];
    size_t      answer_len;
    bool        answer_as_is;
    /* §3.1/§3.2: the item service. The mock plays the controller: it answers
     * each item request with the request's 20 byte header echoed plus the
     * value, which is exactly what the probe found (tools/site-probe/
     * syntec_reply_probe.sh). */
    struct {
        uint32_t key;   /**< parameter B: the register / state number */
        uint16_t value; /**< the u16 it answers with                 */
    } items[16];
    size_t   item_count;
    char     program[64];      /**< the PROGRAM answer's text */
    bool     warning_body;     /**< WARNING with a body (never captured) */
    uint32_t item_log[8];      /**< parameter B of every item request, in order */
    size_t   item_log_count;
    uint16_t last_item_code;
    uint32_t last_item_b;
    /* 状态区：区号 -> 一串 int16（判 int16 + 10^-dec 缩放用） */
    struct {
        uint16_t zone;
        int16_t  values[8];
        size_t   count;
    } zones[4];
    size_t zone_count;
    /* 参数区（§11.4）：参数号 -> 一个 i32（请求号 0x0404） */
    struct {
        uint32_t number;
        int32_t  value;
    } params[48];
    size_t param_count;
    uint32_t last_param;
    /* 参数写入（§11.6）：答什么 hr、上一次收到的号与值 */
    int32_t  put_hr;
    uint32_t last_put_param;
    uint32_t last_put_value;
    /* 参数表（§11.4）：0x0401 答条数，0x0402 答这么多条 268 字节的记录 */
    struct {
        int32_t no;
        char    title[64];
        int32_t flags;
        int32_t fallback;
    } schema[4];
    size_t schema_count;
    /* 刀具表（§11.7）：0x04C2 答条数、0x043F 答一条 224 字节 */
    size_t tool_count;
    struct {
        int32_t tool_nose;
        double  radius_geometry;
        double  radius_wear;
        double  length_geometry[12];
        double  length_wear[12];
        double  tool_angle;
    } tools[4]; /**< 刀号就是下标（刀号从 1 起，tools[0] 不用） */
    /* 写刀补：记下刀号与桩头里报的 dwSizeIn，用来验 228 字节那条 In */
    uint32_t last_tool_put;
    size_t   tool_put_in_len;
    int      tool_put_count;
    /* §11.9 PLC / 变量：容量 + 一个号上的值 */
    uint32_t plc_registers;
    uint32_t plc_bits[5];  /**< I/O/C/S/A 的个数 */
    uint32_t plc_register[8]; /**< 号 -> 值（mock 只摆 8 个） */
    uint8_t  plc_bit[5];   /**< 号 0 上的位值，按族 */
    uint32_t var_count;
    int32_t  var_int[8];   /**< 号 -> 值（mock 只摆 8 个） */
    int16_t  var_type[8];  /**< 0 空 / 1 整数 / 2 浮点 */
    double   var_double[8];
    uint32_t last_plc_bit_code;
    uint32_t last_var_no;
} syntec_mock;

/** 小端写一个 IEEE754 double（mock 侧造 224 字节记录用）。 */
static void put_f64(uint8_t *out, double value)
{
    union {
        double   d;
        uint64_t u;
    } v;

    v.d = value;
    put_u32(out, (uint32_t)(v.u & 0xFFFFFFFFu));
    put_u32(out + 4u, (uint32_t)(v.u >> 32));
}

/** The value the mock answers for one register / state number (0 when unset). */
/** 读一个小端 IEEE754 double（mock 验 224 字节记录用）。 */
static double get_f64(const uint8_t *in)
{
    union {
        double   d;
        uint64_t u;
    } v;

    v.u = (uint64_t)get_u32(in) | ((uint64_t)get_u32(in + 4u) << 32);
    return v.d;
}

static uint16_t mock_item_value(const syntec_mock *mock, uint32_t key)
{
    size_t i;

    for (i = 0; i < mock->item_count; i++) {
        if (mock->items[i].key == key) {
            return mock->items[i].value;
        }
    }
    return 0;
}

/** Script one answer: "when the request asks for @p key, answer @p value". */
static void mock_set_value(syntec_mock *mock, uint32_t key, uint16_t value)
{
    size_t i;

    for (i = 0; i < mock->item_count; i++) {
        if (mock->items[i].key == key) {
            mock->items[i].value = value;
            return;
        }
    }
    if (mock->item_count < sizeof(mock->items) / sizeof(mock->items[0])) {
        mock->items[mock->item_count].key = key;
        mock->items[mock->item_count].value = value;
        mock->item_count++;
    }
}

/** Script one state zone: "this zone answers these int16 values". */
static void mock_set_zone(syntec_mock *mock, uint16_t zone,
                          const int16_t *values, size_t count)
{
    size_t i;

    if (mock->zone_count >= sizeof(mock->zones) / sizeof(mock->zones[0]) ||
        count > 8) {
        return;
    }
    mock->zones[mock->zone_count].zone = zone;
    mock->zones[mock->zone_count].count = count;
    for (i = 0; i < count; i++) {
        mock->zones[mock->zone_count].values[i] = values[i];
    }
    mock->zone_count++;
}

    /* 一台车床：X 在第 1 槽（端口 1），Z 在第 3 槽（端口 3），其余槽都是 0。 */
static void mock_set_param(syntec_mock *mock, uint32_t number, int32_t value)
{
    size_t i;

    for (i = 0; i < mock->param_count; i++) {
        if (mock->params[i].number == number) {
            mock->params[i].value = value;
            return;
        }
    }
    if (mock->param_count < sizeof(mock->params) / sizeof(mock->params[0])) {
        mock->params[mock->param_count].number = number;
        mock->params[mock->param_count].value = value;
        mock->param_count++;
    }
}

/** Script one parameter table row: the number, the title and the two u32s. */
static void mock_set_schema(syntec_mock *mock, size_t index, int32_t no,
                            const char *title, int32_t flags, int32_t fallback)
{
    if (index >= sizeof(mock->schema) / sizeof(mock->schema[0])) {
        return;
    }
    mock->schema[index].no = no;
    mock->schema[index].flags = flags;
    mock->schema[index].fallback = fallback;
    snprintf(mock->schema[index].title, sizeof(mock->schema[index].title), "%s",
             title);
    if (index + 1u > mock->schema_count) {
        mock->schema_count = index + 1u;
    }
}

static void mock_main(void *arg)
{
    syntec_mock *mock = (syntec_mock *)arg;

    while (!mock->stop) {
        ncl_socket *peer = ncl_socket_accept(mock->listener, 200);

        if (peer == NULL) {
            continue;
        }
        for (;;) {
            uint8_t header[NCL_SYNTEC_PACKET_HEADER];
            uint8_t frame[256];
            /* 参数表一页最多 4 条 × 268 字节（§11.4），加 20 字节头。 */
            uint8_t reply[NCL_SYNTEC_REPLY_BODY + 4 * NCL_SYNTEC_PARAM_SPEC_SIZE];
            ncl_syntec_view view;
            size_t content;
            size_t total;
            size_t reply_len;

            if (ncl_socket_recv_exact(peer, header, sizeof(header), 2000) != NCL_OK) {
                break;
            }
            content = get_u32(header);
            if (content < NCL_SYNTEC_FUNCTION_HEADER ||
                NCL_SYNTEC_PACKET_HEADER + content > sizeof(frame)) {
                break;
            }
            memcpy(frame, header, sizeof(header));
            if (ncl_socket_recv_exact(peer, frame + sizeof(header), content,
                                      2000) != NCL_OK) {
                break;
            }
            total = NCL_SYNTEC_PACKET_HEADER + content;
            if (mock->fails_to_skip > 0) {
                mock->fails_to_skip--;
                ncl_sleep_millis(200);
                break; /* no answer at all */
            }
            mock->requests++;
            if (ncl_syntec_split(frame, total, &view, NULL) != NCL_OK) {
                break;
            }
            mock->last_cmd = view.packet.cmd_id;
            mock->last_func = view.function.func_id;
            mock->last_serial = view.function.serial;
            if (view.packet.cmd_id == NCL_SYNTEC_CMD_ITEM &&
                view.body_len >= NCL_SYNTEC_ITEM_BODY) {
                /* §3.1: type | param A | param B | flag at [20..35]. */
                uint32_t param_b = get_u32(view.body + 8);
                uint32_t param_a = get_u32(view.body + 4);
                uint32_t request = get_u32(frame + 16);
                uint16_t code = get_u16(frame + 10);
                size_t item_body = 0;
                size_t z;
                bool zone_handled = false;
                bool send_failed = false;

                /* 状态区读（位置）：A = 4 + 2*count、B = 区号；答案是 count 个 int16。 */
                for (z = 0; z < mock->zone_count; z++) {
                    size_t want;
                    size_t n;
                    size_t k;

                    if (request != 0x0407u ||
                        mock->zones[z].zone != (uint16_t)param_b) {
                        continue;
                    }
                    want = param_a >= 4u ? (size_t)(param_a - 4u) / 2u : 0u;
                    n = want < mock->zones[z].count ? want : mock->zones[z].count;
                    memset(reply, 0, NCL_SYNTEC_REPLY_BODY);
                    memcpy(reply, frame, NCL_SYNTEC_PACKET_HEADER);
                    for (k = 0; k < n; k++) {
                        reply[NCL_SYNTEC_REPLY_BODY + k * 2u] =
                            (uint8_t)((uint16_t)mock->zones[z].values[k] & 0xFF);
                        reply[NCL_SYNTEC_REPLY_BODY + k * 2u + 1u] =
                            (uint8_t)(((uint16_t)mock->zones[z].values[k] >> 8) & 0xFF);
                    }
                    put_u32(reply, (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER + n * 2u));
                    if (ncl_socket_send(peer, reply,
                                        NCL_SYNTEC_REPLY_BODY + n * 2u) != NCL_OK) {
                        send_failed = true;
                    }
                    zone_handled = true;
                    break;
                }
                if (zone_handled) {
                    if (send_failed) {
                        break;
                    }
                    continue;
                }

                /* 参数区（§11.4）：A = 4 + 4，B = 参数号，答一个 i32。 */
                if (request == NCL_SYNTEC_CODE_PARAM_GET) {
                    uint32_t raw = 0;
                    size_t p;

                    mock->last_param = param_b;
                    for (p = 0; p < mock->param_count; p++) {
                        if (mock->params[p].number == param_b) {
                            raw = (uint32_t)mock->params[p].value;
                            break;
                        }
                    }
                    memset(reply, 0, NCL_SYNTEC_REPLY_BODY);
                    memcpy(reply, frame, NCL_SYNTEC_PACKET_HEADER);
                    reply[NCL_SYNTEC_REPLY_BODY + 0] = (uint8_t)(raw & 0xFFu);
                    reply[NCL_SYNTEC_REPLY_BODY + 1] = (uint8_t)((raw >> 8) & 0xFFu);
                    reply[NCL_SYNTEC_REPLY_BODY + 2] = (uint8_t)((raw >> 16) & 0xFFu);
                    reply[NCL_SYNTEC_REPLY_BODY + 3] = (uint8_t)((raw >> 24) & 0xFFu);
                    put_u32(reply, (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER + 4u));
                    if (ncl_socket_send(peer, reply,
                                        NCL_SYNTEC_REPLY_BODY + 4u) != NCL_OK) {
                        break;
                    }
                    continue;
                }

                /* 参数写入（§11.6）：A = 4、B = 参数号、flag = 新值；Out = { hr }。 */
                if (request == NCL_SYNTEC_CODE_PARAM_PUT) {
                    uint32_t new_value = get_u32(view.body + 12);
                    uint32_t raw = (uint32_t)mock->put_hr;

                    mock->last_put_param = param_b;
                    mock->last_put_value = new_value;
                    if (mock->put_hr == 0) {
                        mock_set_param(mock, param_b, (int32_t)new_value);
                    }
                    memset(reply, 0, NCL_SYNTEC_REPLY_BODY);
                    memcpy(reply, frame, NCL_SYNTEC_PACKET_HEADER);
                    put_u32(reply + NCL_SYNTEC_REPLY_HR, raw);
                    put_u32(reply, (uint32_t)(4u + sizeof(int32_t)));
                    if (ncl_socket_send(peer, reply,
                                        NCL_SYNTEC_REPLY_HR + 4u) != NCL_OK) {
                        break;
                    }
                    continue;
                }

                /* §11.9 PLC 容量（0x041E）：In 空，答 8 个 u32。 */
                if (request == NCL_SYNTEC_CODE_PLC_GET_CAPACITY) {
                    uint8_t *at = reply + NCL_SYNTEC_REPLY_BODY;

                    put_u32(reply, (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER +
                                              8u * sizeof(uint32_t)));
                    put_u32(at + 0u, mock->plc_bits[0]);
                    put_u32(at + 4u, mock->plc_bits[1]);
                    put_u32(at + 8u, mock->plc_bits[2]);
                    put_u32(at + 12u, mock->plc_bits[3]);
                    put_u32(at + 16u, mock->plc_bits[4]);
                    put_u32(at + 20u, mock->plc_registers);
                    put_u32(at + 24u, 256u);
                    put_u32(at + 28u, 256u);
                    if (ncl_socket_send(peer, reply, NCL_SYNTEC_REPLY_BODY +
                                        8u * sizeof(uint32_t)) != NCL_OK) {
                        break;
                    }
                    continue;
                }

                /* §11.9 位（0x0412/14/15/17/19）：B = 号，答 { hr, u8 }。 */
                if (request == NCL_SYNTEC_CODE_PLC_GET_BIT ||
                    (request >= NCL_SYNTEC_CODE_PLC_GET_BIT + 2u &&
                     request <= NCL_SYNTEC_CODE_PLC_GET_ABIT &&
                     (request - NCL_SYNTEC_CODE_PLC_GET_BIT) % 2u == 0u)) {
                    size_t k = (size_t)((request - NCL_SYNTEC_CODE_PLC_GET_BIT) / 2u);
                    uint8_t bit = (param_b == 0u && k < 5u) ? mock->plc_bit[k] : 0u;

                    mock->last_plc_bit_code = request;
                    memset(reply, 0, NCL_SYNTEC_REPLY_BODY);
                    memcpy(reply, frame, NCL_SYNTEC_PACKET_HEADER);
                    reply[NCL_SYNTEC_REPLY_BODY] = bit;
                    put_u32(reply, (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER +
                                              2u * sizeof(uint32_t)));
                    if (ncl_socket_send(peer, reply,
                                        NCL_SYNTEC_REPLY_BODY +
                                            2u * sizeof(uint32_t)) != NCL_OK) {
                        break;
                    }
                    continue;
                }

                /* §11.9 变量容量（0x0423）：In 空，答 { hr, u32 }。 */
                if (request == NCL_SYNTEC_CODE_GLOBAL_GET_CAPACITY) {
                    memset(reply, 0, NCL_SYNTEC_REPLY_BODY);
                    memcpy(reply, frame, NCL_SYNTEC_PACKET_HEADER);
                    put_u32(reply + NCL_SYNTEC_REPLY_BODY, mock->var_count);
                    put_u32(reply, (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER +
                                              sizeof(uint32_t)));
                    if (ncl_socket_send(peer, reply, NCL_SYNTEC_REPLY_BODY +
                                        sizeof(uint32_t)) != NCL_OK) {
                        break;
                    }
                    continue;
                }

                /* §11.9 变量（0x0421）：B = 号，答 { hr, TOcVariant 16 }。 */
                if (request == NCL_SYNTEC_CODE_GLOBAL_GET_VALUE) {
                    uint8_t *at = reply + NCL_SYNTEC_REPLY_BODY;
                    int16_t type = param_b < 8u ? mock->var_type[param_b] : 0;

                    mock->last_var_no = param_b;
                    memset(reply, 0, NCL_SYNTEC_REPLY_BODY);
                    memcpy(reply, frame, NCL_SYNTEC_PACKET_HEADER);
                    /* 线上是 i16 类型在 [20]，值在 [28]（int 或 double） */
                    at[0] = (uint8_t)((uint16_t)type & 0xFFu);
                    at[1] = (uint8_t)(((uint16_t)type >> 8) & 0xFFu);
                    if (type == 1) {
                        put_u32(at + 8u, (uint32_t)mock->var_int[param_b]);
                    } else if (type == 2) {
                        put_f64(at + 8u, mock->var_double[param_b]);
                    }
                    /* 应答 = 12 包头 + 传输层 hr + { hr, TOcVariant } */
                    put_u32(reply, (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER + 16u));
                    if (ncl_socket_send(peer, reply,
                                        NCL_SYNTEC_REPLY_BODY + 16u) != NCL_OK) {
                        break;
                    }
                    continue;
                }

                /*
                 * 写刀补（§11.7，0x0440）：In 是 { nToolNo, TToolOffset } 228 字节，
                 * 跟在 16 字节桩头后面（整帧 256 字节）；Out 是 { hr }。
                 */
                if (request == NCL_SYNTEC_CODE_TOOL_PUT) {
                    uint32_t index = get_u32(frame + NCL_SYNTEC_PACKET_HEADER +
                                             NCL_SYNTEC_KRML_HEAD);
                    const uint8_t *record = frame + NCL_SYNTEC_PACKET_HEADER +
                                            NCL_SYNTEC_KRML_HEAD + 4u;
                    const size_t tool_slots =
                        sizeof(mock->tools) / sizeof(mock->tools[0]);
                    uint32_t raw = (uint32_t)mock->put_hr;
                    size_t k;

                    mock->last_tool_put = index;
                    mock->tool_put_in_len =
                        (size_t)get_u32(frame + NCL_SYNTEC_PACKET_HEADER + 8u);
                    mock->tool_put_count++;
                    if (mock->put_hr == 0 && index < tool_slots) {
                        mock->tools[index].tool_nose = (int32_t)get_u32(record);
                        mock->tools[index].radius_geometry = get_f64(record + 8u);
                        mock->tools[index].radius_wear = get_f64(record + 16u);
                        for (k = 0; k < NCL_SYNTEC_TOOL_LENGTHS; k++) {
                            mock->tools[index].length_geometry[k] =
                                get_f64(record + 24u + k * 8u);
                            mock->tools[index].length_wear[k] =
                                get_f64(record + 120u + k * 8u);
                        }
                        mock->tools[index].tool_angle = get_f64(record + 216u);
                    }
                    memset(reply, 0, NCL_SYNTEC_REPLY_BODY);
                    memcpy(reply, frame, NCL_SYNTEC_PACKET_HEADER);
                    put_u32(reply + NCL_SYNTEC_REPLY_HR, raw);
                    put_u32(reply, (uint32_t)(4u + sizeof(int32_t)));
                    if (ncl_socket_send(peer, reply,
                                        NCL_SYNTEC_REPLY_HR + 4u) != NCL_OK) {
                        break;
                    }
                    continue;
                }

                /* 参数表容量（§11.4）：答条数，正文 4 字节。 */
                if (request == NCL_SYNTEC_CODE_PARAM_CAPACITY) {
                    uint32_t raw = (uint32_t)mock->schema_count;

                    memset(reply, 0, NCL_SYNTEC_REPLY_BODY);
                    memcpy(reply, frame, NCL_SYNTEC_PACKET_HEADER);
                    reply[NCL_SYNTEC_REPLY_BODY + 0] = (uint8_t)(raw & 0xFFu);
                    reply[NCL_SYNTEC_REPLY_BODY + 1] = (uint8_t)((raw >> 8) & 0xFFu);
                    reply[NCL_SYNTEC_REPLY_BODY + 2] = (uint8_t)((raw >> 16) & 0xFFu);
                    reply[NCL_SYNTEC_REPLY_BODY + 3] = (uint8_t)((raw >> 24) & 0xFFu);
                    put_u32(reply, (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER + 4u));
                    if (ncl_socket_send(peer, reply,
                                        NCL_SYNTEC_REPLY_BODY + 4u) != NCL_OK) {
                        break;
                    }
                    continue;
                }

                /* 参数表 dump（§11.4）：B = 要几条，正文是那么多条 268 字节。 */
                if (request == NCL_SYNTEC_CODE_PARAM_SCHEMA) {
                    size_t want = param_b;
                    size_t n = want < mock->schema_count ? want
                                                         : mock->schema_count;
                    size_t k;

                    memset(reply, 0, NCL_SYNTEC_REPLY_BODY);
                    memcpy(reply, frame, NCL_SYNTEC_PACKET_HEADER);
                    for (k = 0; k < n; k++) {
                        uint8_t *record =
                            reply + NCL_SYNTEC_REPLY_BODY +
                            k * NCL_SYNTEC_PARAM_SPEC_SIZE;
                        uint16_t no = (uint16_t)mock->schema[k].no;
                        const char *title = mock->schema[k].title;
                        size_t t;

                        memset(record, 0, NCL_SYNTEC_PARAM_SPEC_SIZE);
                        record[0] = (uint8_t)(no & 0xFFu);
                        record[1] = (uint8_t)(no >> 8);
                        /* titles travel as UTF-16LE, NUL padded */
                        for (t = 0; t < strlen(title) &&
                                    t + 1u < NCL_SYNTEC_PARAM_TITLE_BYTES / 2u;
                             t++) {
                            record[4u + t * 2u] = (uint8_t)title[t];
                            record[5u + t * 2u] = 0;
                        }
                        put_u32(record + 260u, (uint32_t)mock->schema[k].flags);
                        put_u32(record + 264u,
                                (uint32_t)mock->schema[k].fallback);
                    }
                    put_u32(reply, (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER +
                                              n * NCL_SYNTEC_PARAM_SPEC_SIZE));
                    if (ncl_socket_send(peer, reply,
                                        NCL_SYNTEC_REPLY_BODY +
                                            n * NCL_SYNTEC_PARAM_SPEC_SIZE) !=
                        NCL_OK) {
                        break;
                    }
                    continue;
                }

                /* 刀具表条数（§11.7）：同一个形状，答一个 i32。 */
                if (request == NCL_SYNTEC_CODE_TOOL_COUNT) {
                    uint32_t raw = (uint32_t)mock->tool_count;

                    memset(reply, 0, NCL_SYNTEC_REPLY_BODY);
                    memcpy(reply, frame, NCL_SYNTEC_PACKET_HEADER);
                    reply[NCL_SYNTEC_REPLY_BODY + 0] = (uint8_t)(raw & 0xFFu);
                    reply[NCL_SYNTEC_REPLY_BODY + 1] = (uint8_t)((raw >> 8) & 0xFFu);
                    reply[NCL_SYNTEC_REPLY_BODY + 2] = (uint8_t)((raw >> 16) & 0xFFu);
                    reply[NCL_SYNTEC_REPLY_BODY + 3] = (uint8_t)((raw >> 24) & 0xFFu);
                    put_u32(reply, (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER + 4u));
                    if (ncl_socket_send(peer, reply,
                                        NCL_SYNTEC_REPLY_BODY + 4u) != NCL_OK) {
                        break;
                    }
                    continue;
                }

                /* 一条刀补（§11.7）：B = 刀号（从 1 起），正文 224 字节。 */
                if (request == NCL_SYNTEC_CODE_TOOL_GET && param_b >= 1u &&
                    param_b < mock->tool_count + 1u) {
                    uint8_t *record = reply + NCL_SYNTEC_REPLY_BODY;
                    size_t k;

                    memset(reply, 0, NCL_SYNTEC_REPLY_BODY);
                    memcpy(reply, frame, NCL_SYNTEC_PACKET_HEADER);
                    memset(record, 0, NCL_SYNTEC_TOOL_SIZE);
                    put_u32(record, (uint32_t)mock->tools[param_b].tool_nose);
                    put_f64(record + 8u, mock->tools[param_b].radius_geometry);
                    put_f64(record + 16u, mock->tools[param_b].radius_wear);
                    for (k = 0; k < NCL_SYNTEC_TOOL_LENGTHS; k++) {
                        put_f64(record + 24u + k * 8u,
                                mock->tools[param_b].length_geometry[k]);
                        put_f64(record + 120u + k * 8u,
                                mock->tools[param_b].length_wear[k]);
                    }
                    put_f64(record + 216u, mock->tools[param_b].tool_angle);
                    put_u32(reply, (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER +
                                              NCL_SYNTEC_TOOL_SIZE));
                    if (ncl_socket_send(peer, reply,
                                        NCL_SYNTEC_REPLY_BODY +
                                            NCL_SYNTEC_TOOL_SIZE) != NCL_OK) {
                        break;
                    }
                    continue;
                }

                mock->last_item_b = param_b;
                mock->last_item_code = code;
                if (mock->item_log_count <
                    sizeof(mock->item_log) / sizeof(mock->item_log[0])) {
                    mock->item_log[mock->item_log_count++] = param_b;
                }
                /* The answer repeats the request's 20 byte header and carries
                 * the value after it (§3.2). */
                memset(reply, 0, NCL_SYNTEC_REPLY_BODY);
                memcpy(reply, frame, NCL_SYNTEC_PACKET_HEADER);
                if (code == 0x071eu) {          /* PROGRAM: the body is text  */
                    item_body = strlen(mock->program);
                    if (item_body > 0) {
                        memcpy(reply + NCL_SYNTEC_REPLY_BODY, mock->program,
                               item_body);
                    }
                } else if (code == 0x0701u) {   /* WARNING: usually no body   */
                    if (mock->warning_body) {
                        reply[NCL_SYNTEC_REPLY_BODY] = 0x07;
                        reply[NCL_SYNTEC_REPLY_BODY + 1] = 0x00;
                        item_body = 2;
                    }
                } else if (request == NCL_SYNTEC_CODE_PLC_GET_REGISTER) {
                    /*
                     * §11.9：PART_COUNT/SPDL_SPEED 用的就是这个码 —— 它本来就是
                     * **R 寄存器**读，应答是 u32；条目的 u16 只是取低半。
                     */
                    uint16_t value = mock_item_value(mock, param_b);

                    reply[NCL_SYNTEC_REPLY_BODY] = (uint8_t)(value & 0xFF);
                    reply[NCL_SYNTEC_REPLY_BODY + 1] = (uint8_t)(value >> 8);
                    item_body = 4;
                } else {                        /* a u16 at [20..21]          */
                    uint16_t value = mock_item_value(mock, param_b);
                    reply[NCL_SYNTEC_REPLY_BODY] = (uint8_t)(value & 0xFF);
                    reply[NCL_SYNTEC_REPLY_BODY + 1] = (uint8_t)(value >> 8);
                    item_body = 2;
                }
                /* Length counts what follows the 12 byte header. */
                put_u32(reply,
                        (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER + item_body));
                if (ncl_socket_send(peer, reply,
                                    NCL_SYNTEC_REPLY_BODY + item_body) != NCL_OK) {
                    break;
                }
                continue;
            }
            /* The body is MMI_Request_KrnlAPI: funcID u2, code i4, sizeIn i4,
             * sizeOut i4, then the input bytes (§10.2). */
            if (view.body_len >= 14) {
                mock->last_code = (int32_t)get_u32(view.body + 2);
                mock->last_size_out = (int32_t)get_u32(view.body + 10);
            }
            {
                uint8_t body[160];
                size_t body_len = 0;
                ncl_syntec_function function;

                memset(&function, 0, sizeof(function));
                function.func_id = view.function.func_id;
                function.serial = view.function.serial; /* echo it back */
                put_u32(body + body_len, (uint32_t)view.function.func_id);
                body_len += 2;
                put_u32(body + body_len, (uint32_t)mock->last_code);
                body_len += 4;
                put_u32(body + body_len, 0);
                body_len += 4;
                put_u32(body + body_len, (uint32_t)mock->answer_len);
                body_len += 4;
                memcpy(body + body_len, mock->answer, mock->answer_len);
                body_len += mock->answer_len;
                reply_len = ncl_syntec_build(reply, sizeof(reply),
                                             view.packet.cmd_id, &function, body,
                                             body_len);
            }
            if (reply_len == 0 || ncl_socket_send(peer, reply, reply_len) != NCL_OK) {
                break;
            }
            (void)mock->answer_as_is;
        }
        ncl_socket_close(peer);
    }
}

static syntec_mock *mock_start(void)
{
    syntec_mock *mock = (syntec_mock *)ncl_mem_calloc(1, sizeof(*mock));

    if (mock == NULL) {
        return NULL;
    }
    mock->listener = ncl_socket_listen(0, NULL, 0);
    if (mock->listener == NULL) {
        ncl_free_safe(mock);
        return NULL;
    }
    mock->port = ncl_socket_local_port(mock->listener);
    mock->thread = ncl_thread_start(mock_main, mock);
    if (mock->thread == NULL) {
        ncl_socket_close(mock->listener);
        ncl_free_safe(mock);
        return NULL;
    }
    return mock;
}

static void mock_stop(syntec_mock *mock)
{
    if (mock == NULL) {
        return;
    }
    mock->stop = true;
    ncl_thread_join(mock->thread);
    ncl_socket_close(mock->listener);
    ncl_free_safe(mock);
}

static ncl_driver *syntec_driver(syntec_mock *mock, const char *extra)
{
    ncl_driver *driver = ncl_syntec_create();
    ncl_strbuf json;
    ncl_json *params;

    if (driver == NULL) {
        return NULL;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"host\":\"127.0.0.1\",\"port\":%u,"
                            "\"timeoutMs\":800,\"retries\":0%s}",
                            mock->port, extra != NULL ? extra : "");
    params = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    if (params == NULL || driver->ops->create(driver, params) != NCL_OK) {
        ncl_json_free(params);
        driver->ops->destroy(driver);
        return NULL;
    }
    ncl_json_free(params);
    return driver;
}

static ncl_err read_point(ncl_driver *driver, const char *area, long long code,
                          int length, const char *dtype, ncl_json **value)
{
    ncl_strbuf json;
    ncl_json *node;
    ncl_address address;
    ncl_err err;

    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"area\":\"%s\",\"offset\":%lld,\"length\":%d,"
                            "\"dtype\":\"%s\"}",
                            area, code, length, dtype);
    node = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    if (node == NULL) {
        return NCL_ERR_PARSE;
    }
    err = ncl_address_from_json(node, &address);
    ncl_json_free(node);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_driver_read_one(driver, &address, value);
    ncl_address_clear(&address);
    return err;
}

static void test_read(void)
{
    syntec_mock *mock = mock_start();
    ncl_driver *driver;
    ncl_json *value = NULL;
    ncl_err err;

    NCL_TEST_CASE("a KrnlAPI read goes out as CmdID 200 and the code the point says");
    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    /* The mock answers 0x0000002A (42) as an int32. */
    mock->answer[0] = 0x00;
    mock->answer[1] = 0x00;
    mock->answer[2] = 0x00;
    mock->answer[3] = 0x2A;
    mock->answer_len = 4;
    driver = syntec_driver(mock, NULL);
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        mock_stop(mock);
        return;
    }
    NCL_CHECK_EQ_INT(read_point(driver, "KrnlAPI", 1, 4, "int32", &value), NCL_OK);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 42);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(mock->last_cmd, NCL_SYNTEC_CMD_KRML_API);
    /* §10.9: the function header carries the same number as the packet's CmdID */
    NCL_CHECK_EQ_INT(mock->last_func, NCL_SYNTEC_CMD_KRML_API);
    NCL_CHECK_EQ_INT(mock->last_code, 1); /* the point's offset is the dwCode */
    NCL_CHECK_EQ_INT(mock->last_size_out, 4);
    NCL_CHECK_EQ_INT(mock->last_serial, 1);

    NCL_TEST_CASE("§6: the driver hands the frames of the exchange to the audit");
    {
        ncl_driver_raw raw;
        uint32_t length;

        ncl_driver_last_raw(driver, &raw);
        /* 12 byte packet header + 8 byte function header + the 14 byte body */
        NCL_CHECK_EQ_INT(raw.request_len, 34);
        NCL_CHECK(raw.request != NULL);
        if (raw.request != NULL && raw.request_len == 34) {
            length = get_u32(raw.request);
            NCL_CHECK_EQ_INT(length, 22); /* §10.3: everything after the header */
            NCL_CHECK_EQ_INT(get_u16(raw.request + 4), NCL_SYNTEC_CMD_KRML_API);
            NCL_CHECK_EQ_INT(get_u16(raw.request + 12), NCL_SYNTEC_CMD_KRML_API);
            NCL_CHECK_EQ_INT(raw.request[14], 1); /* the serial that went out */
            /* §10.9 again: the body's uFuncID repeats the command number */
            NCL_CHECK_EQ_INT(get_u16(raw.request + 20), NCL_SYNTEC_CMD_KRML_API);
            NCL_CHECK_EQ_INT(get_u32(raw.request + 22), 1); /* dwCode */
            NCL_CHECK_EQ_INT(get_u32(raw.request + 30), 4); /* dwSizeOut */
        }
        NCL_CHECK(raw.reply != NULL);
        NCL_CHECK(raw.reply_len >= NCL_SYNTEC_PACKET_HEADER);
        if (raw.reply != NULL && raw.reply_len >= NCL_SYNTEC_PACKET_HEADER) {
            NCL_CHECK_EQ_INT(get_u16(raw.reply + 4), NCL_SYNTEC_CMD_KRML_API);
        }
    }

    NCL_TEST_CASE("the serial advances, and a bare command number is accepted");
    /* 1.5 as a big endian double, which is what this point asks for */
    mock->answer[0] = 0x3F;
    mock->answer[1] = 0xF8;
    memset(mock->answer + 2, 0, 6);
    mock->answer_len = 8;
    NCL_CHECK_EQ_INT(read_point(driver, "200", 5, 8, "float64", &value), NCL_OK);
    {
        double real = 0;

        NCL_CHECK(ncl_json_as_double(value, &real));
        NCL_CHECK(real == 1.5);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(mock->last_serial, 2);
    NCL_CHECK_EQ_INT(mock->last_code, 5);

    NCL_TEST_CASE("an unknown command name is a point map error");
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(read_point(driver, "NoSuch", 1, 4,
                                                     "int32", &value)),
                     3);

    NCL_TEST_CASE("writing is refused: the box lists no SYNTEC write endpoint");
    NCL_CHECK_EQ_INT(ncl_driver_write_one(driver, NULL, NULL),
                     NCL_ERR_NOT_SUPPORTED);

    NCL_TEST_CASE("the session reports itself");
    {
        ncl_json *result = NULL;

        NCL_CHECK_EQ_INT(driver->ops->call(driver, "serial", NULL, &result),
                         NCL_OK);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(result, "serial", -1), 2);
        ncl_json_free(result);
        NCL_CHECK(ncl_driver_error_tier(driver->ops->call(driver, "nonsense", NULL,
                                                          NULL)) == 2);
    }

    NCL_TEST_CASE("the raw hatch takes CmdID + funcId + body");
    {
        ncl_driver_result out;
        /* CmdID 200, funcId 1, then the four KrnlAPI fields */
        const uint8_t raw[] = {0xC8, 0x00, 0x01, 0x00,
                               0x01, 0x00, 0x07, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00,
                               0x04, 0x00, 0x00, 0x00};

        mock->answer_len = 4; /* the answer the raw reply should carry */
        ncl_driver_result_init(&out);
        NCL_CHECK_EQ_INT(driver->ops->read_raw(driver, raw, sizeof(raw), &out),
                         NCL_OK);
        NCL_CHECK(out.success);
        NCL_CHECK_EQ_INT(out.raw_len, 18); /* 14 fields + the 4 answer bytes */
        ncl_driver_result_free(&out);
    }

    NCL_TEST_CASE("a controller that stops answering is a transport failure");
    mock->fails_to_skip = 1;
    err = read_point(driver, "KrnlAPI", 1, 4, "int32", &value);
    NCL_CHECK_EQ_INT(ncl_driver_error_tier(err), 1);

    NCL_TEST_CASE("closing and reopening works");
    driver->ops->close(driver);
    NCL_CHECK(!driver->ops->is_connected(driver));
    NCL_CHECK_EQ_INT(read_point(driver, "KrnlAPI", 1, 4, "int32", &value), NCL_OK);
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK(driver->ops->is_connected(driver));

    NCL_TEST_CASE("§10.12: a named reading asks KrnlAPI for its own code");
    mock->answer[0] = 0x00;
    mock->answer[1] = 0x00;
    mock->answer[2] = 0x03;
    mock->answer[3] = 0xE8; /* 1000 parts */
    mock->answer_len = 4;
    /* The client writes it "READ_part_count"; the point map is lenient about it,
     * and a reading carries its own code, so the offset is not used. */
    NCL_CHECK_EQ_INT(read_point(driver, "READ_part_count", 7, 4, "int32", &value),
                     NCL_OK);
    {
        long long number = 0;

        NCL_CHECK(ncl_json_as_int(value, &number));
        NCL_CHECK_EQ_INT(number, 1000);
    }
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(mock->last_cmd, NCL_SYNTEC_CMD_KRML_API);
    NCL_CHECK_EQ_INT(mock->last_code, 1000); /* from the table, not the offset 7 */
    NCL_CHECK_EQ_INT(mock->last_size_out, 4);

    driver->ops->destroy(driver);
    mock_stop(mock);
}

static void test_through_the_manager(void)
{
    syntec_mock *mock = mock_start();
    test_point_map *manager = test_point_map_create(ncl_syntec_create);
    ncl_strbuf json;
    ncl_strbuf err;
    ncl_json *config;
    ncl_json *value = NULL;
    long long number = 0;

    NCL_TEST_CASE("a configured SYNTEC link reads through the point map");
    NCL_CHECK(mock != NULL && manager != NULL);
    if (mock == NULL || manager == NULL) {
        mock_stop(mock);
        test_point_map_free(manager);
        return;
    }
    mock->answer[0] = 0x00;
    mock->answer[1] = 0x00;
    mock->answer[2] = 0x01;
    mock->answer[3] = 0x2C; /* 300 */
    mock->answer_len = 4;
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"id\":\"cnc\",\"path\":\"/CNC\","
                            "\"type\":\"syntec\","
                            "\"parameters\":{\"host\":\"127.0.0.1\","
                            "\"port\":%u,\"timeoutMs\":800,\"funcId\":1},"
                            "\"points\":["
                            "{\"path\":\"/CNC/PART\",\"addr\":"
                            "{\"area\":\"KrnlAPI\",\"offset\":3,\"length\":4,"
                            "\"dtype\":\"int32\"}},"
                            "{\"path\":\"/CNC/COUNT\",\"addr\":"
                            "{\"area\":\"part_count\",\"length\":4,"
                            "\"dtype\":\"int32\"}}]}",
                            mock->port);
    config = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    NCL_CHECK(config != NULL);
    ncl_strbuf_init(&err);
    if (config != NULL) {
        NCL_CHECK_EQ_INT(test_point_map_add_json(manager, config, &err),
                         NCL_OK);
    }
    if (err.len > 0) {
        printf("    %s\n", ncl_strbuf_cstr(&err));
    }
    ncl_strbuf_free(&err);
    ncl_json_free(config);

    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/CNC/PART", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 300);
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(mock->last_code, 3);

    NCL_TEST_CASE("a point may name a §10.12 reading instead of its code");
    mock->answer[0] = 0x00;
    mock->answer[1] = 0x00;
    mock->answer[2] = 0x00;
    mock->answer[3] = 0xFA; /* 250 */
    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/CNC/COUNT", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 250);
    ncl_json_free(value);
    NCL_CHECK_EQ_INT(mock->last_cmd, NCL_SYNTEC_CMD_KRML_API);
    NCL_CHECK_EQ_INT(mock->last_code, 1000); /* the total part counter */

    NCL_CHECK_EQ_INT(test_point_map_read(manager, "/CNC/NOPE", &value),
                     NCL_ERR_NOT_FOUND);
    test_point_map_free(manager);
    mock_stop(mock);
}

/*
 * §3.1/§3.2: the nine items the delivery closed the loop on. The item table is
 * checked against the captured STATUS frame byte for byte, and the nine getters
 * run against the mock controller: it answers every item request the way the
 * probe found a real one does - request echoed, value after the 20 byte header.
 */
static void test_items(void)
{
    /* The full STATUS frame from the doc (§3.1), with uSerial 0. */
    static const uint8_t kStatusFrame[NCL_SYNTEC_ITEM_FRAME] = {
        0x18, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x07,
        0xc8, 0x00, 0x00, 0x00, 0x07, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    uint8_t frame[NCL_SYNTEC_ITEM_FRAME];
    const ncl_syntec_item *item;
    syntec_mock *mock;
    ncl_syntec_config config;
    ncl_syntec *session;
    char text[64];
    char *err = NULL;
    long long number = 0;
    double speed = 0.0;
    ncl_json *warnings = NULL;

    NCL_TEST_CASE("§3.1: the item table holds the nine closed loop items");
    NCL_CHECK_EQ_INT(ncl_syntec_item_count(), 9);
    NCL_CHECK(ncl_syntec_item_lookup("STATUS") != NULL);
    NCL_CHECK(ncl_syntec_item_lookup("read_status") != NULL);
    item = ncl_syntec_item_lookup("partCount");
    NCL_CHECK(item != NULL && item->param_b == 1000u);
    NCL_CHECK(ncl_syntec_item_lookup("no-such-item") == NULL);
    NCL_CHECK(ncl_syntec_item_at(ncl_syntec_item_count()) == NULL);

    NCL_TEST_CASE("§3.1: the STATUS request is the captured 36 bytes");
    item = ncl_syntec_item_lookup("STATUS");
    NCL_CHECK(item != NULL);
    NCL_CHECK_EQ_INT(
        ncl_syntec_item_frame(frame, sizeof(frame), item, item->param_b, 0),
        NCL_SYNTEC_ITEM_FRAME);
    NCL_CHECK(memcmp(frame, kStatusFrame, sizeof(kStatusFrame)) == 0);

    NCL_TEST_CASE("§3.1: param B is the register, uSerial the only moving byte");
    item = ncl_syntec_item_lookup("FEED_SPEED");
    NCL_CHECK(item != NULL);
    NCL_CHECK_EQ_INT(
        ncl_syntec_item_frame(frame, sizeof(frame), item, 12, 7),
        NCL_SYNTEC_ITEM_FRAME);
    NCL_CHECK_EQ_INT(get_u16(frame + 4), NCL_SYNTEC_CMD_ITEM);
    NCL_CHECK_EQ_INT(get_u16(frame + 12), NCL_SYNTEC_CMD_KRML_API);
    NCL_CHECK_EQ_INT(get_u32(frame + 28), 12u);
    NCL_CHECK_EQ_INT(frame[14], 7);
    NCL_CHECK_EQ_INT(frame[6], 0); /* FEED_SPEED's own flags */

    mock = mock_start();
    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    mock_set_value(mock, 1000u, 1234u); /* PART_COUNT */
    mock_set_value(mock, 10u, 4321u);   /* LINE_NUMBER */
    mock_set_value(mock, 19u, 80u);     /* FEED_OVERRIDE */
    mock_set_value(mock, 771u, 9000u);  /* SPDL_SPEED */
    mock_set_value(mock, 21u, 90u);     /* SPDL_OVERRIDE */
    mock_set_value(mock, 700u, 4321u);  /* FEED_SPEED's register */
    mock_set_value(mock, 12u, 0u);      /* its unit state */
    mock_set_value(mock, 76u, 70u);     /* its mode state */
    snprintf(mock->program, sizeof(mock->program), "O1000");

    ncl_syntec_config_default(&config);
    NCL_CHECK_EQ_INT(config.port, 8000);
    config.host = "127.0.0.1";
    config.port = mock->port;
    config.timeout_ms = 800;
    NCL_CHECK(ncl_syntec_open(&config, &err) != NULL || err != NULL);
    session = ncl_syntec_open(&config, &err);
    NCL_CHECK(session != NULL);
    if (session == NULL) {
        mock_stop(mock);
        return;
    }

    NCL_TEST_CASE("§3.2: STATUS maps the state enum");
    mock_set_value(mock, 4u, 2u);
    NCL_CHECK_EQ_INT(ncl_syntec_status(session, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "running");
    mock_set_value(mock, 4u, 3u);
    NCL_CHECK_EQ_INT(ncl_syntec_status(session, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "holding");
    mock_set_value(mock, 4u, 4u);
    NCL_CHECK_EQ_INT(ncl_syntec_status(session, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "free");
    mock_set_value(mock, 4u, 9u);
    NCL_CHECK_EQ_INT(ncl_syntec_status(session, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "unknown");

    NCL_TEST_CASE("§3.2: the numeric items read their u16");
    NCL_CHECK_EQ_INT(ncl_syntec_part_count(session, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 1234);
    NCL_CHECK_EQ_INT(mock->last_item_b, 1000u);
    NCL_CHECK_EQ_INT(ncl_syntec_line_number(session, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 4321);
    NCL_CHECK_EQ_INT(ncl_syntec_feed_override(session, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 80);
    NCL_CHECK_EQ_INT(ncl_syntec_spindle_speed(session, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 9000);
    NCL_CHECK_EQ_INT(ncl_syntec_spindle_override(session, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 90);

    NCL_TEST_CASE("§3.2: PROGRAM comes back as the body's text");
    NCL_CHECK_EQ_INT(ncl_syntec_program(session, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "O1000");

    NCL_TEST_CASE("§3.2: FEED_SPEED asks 700, then the states 12 and 76");
    mock->item_log_count = 0;
    NCL_CHECK_EQ_INT(ncl_syntec_feed_speed(session, &speed), NCL_OK);
    NCL_CHECK_EQ_INT((long long)speed, 4321);
    NCL_CHECK_EQ_INT(mock->item_log_count, 3);
    NCL_CHECK_EQ_INT(mock->item_log[0], 700u);
    NCL_CHECK_EQ_INT(mock->item_log[1], 12u);
    NCL_CHECK_EQ_INT(mock->item_log[2], 76u);

    NCL_TEST_CASE("§3.2: a unit state of (0,0) is the factor 1.0");
    mock_set_value(mock, 76u, 1u);
    NCL_CHECK_EQ_INT(ncl_syntec_feed_speed(session, &speed), NCL_OK);
    NCL_CHECK_EQ_INT((long long)speed, 4321);

    NCL_TEST_CASE("§3.2: an uncaptured unit step says 还读不了, not a guess");
    mock_set_value(mock, 12u, 32u);
    NCL_CHECK_EQ_INT(ncl_syntec_feed_speed(session, &speed),
                     NCL_ERR_UNAVAILABLE);
    NCL_CHECK(strstr(ncl_syntec_last_error(session), "单位换算表") != NULL);

    NCL_TEST_CASE("§3.2: WARNING with no body is an empty list");
    NCL_CHECK_EQ_INT(ncl_syntec_warning(session, &warnings), NCL_OK);
    NCL_CHECK(warnings != NULL);
    NCL_CHECK_EQ_INT(ncl_json_arr_len(warnings), 0);
    ncl_json_free(warnings);
    warnings = NULL;

    NCL_TEST_CASE("§3.2: a populated WARNING was never captured");
    mock->warning_body = true;
    NCL_CHECK_EQ_INT(ncl_syntec_warning(session, &warnings),
                     NCL_ERR_UNAVAILABLE);
    NCL_CHECK(strstr(ncl_syntec_last_error(session), "待抓包") != NULL);
    mock->warning_body = false;

    ncl_syntec_close(session);
    mock_stop(mock);
}

/**
 * §11.4：参数区（请求号 0x0404）与轴名。轴名不是猜出来的：控制器自己说的，321 + 槽 = 轴名代号（100 = X、300 = Z），21 + 槽 = 端口号（0 = 这一槽没接轴）。
 */
static void test_params(void)
{
    syntec_mock *mock = mock_start();
    ncl_syntec_axis axes[NCL_SYNTEC_AXIS_SLOTS];
    ncl_syntec_config config;
    ncl_syntec *session;
    char name[16];
    char *err = NULL;
    int32_t value = 0;
    unsigned slot = 99;
    size_t count = 0;

    NCL_TEST_CASE("§11.4: an axis name code decodes the client's way");
    NCL_CHECK(ncl_syntec_axis_name_decode(100, name, sizeof(name)));
    NCL_CHECK_EQ_STR(name, "X");
    NCL_CHECK(ncl_syntec_axis_name_decode(102, name, sizeof(name)));
    NCL_CHECK_EQ_STR(name, "X2");
    NCL_CHECK(ncl_syntec_axis_name_decode(300, name, sizeof(name)));
    NCL_CHECK_EQ_STR(name, "Z");
    NCL_CHECK(ncl_syntec_axis_name_decode(901, name, sizeof(name)));
    NCL_CHECK_EQ_STR(name, "W1");
    /* 0 与 >= 10000 都是“这一槽没有名字”：空串是答案，不是错 */
    NCL_CHECK(ncl_syntec_axis_name_decode(0, name, sizeof(name)));
    NCL_CHECK_EQ_STR(name, "");
    NCL_CHECK(ncl_syntec_axis_name_decode(10999, name, sizeof(name)));
    NCL_CHECK_EQ_STR(name, "");

    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    /* 一台车床：X 在第 1 槽（端口 1），Z 在第 3 槽（端口 3），其余槽都是 0。 */
    mock_set_param(mock, NCL_SYNTEC_PARAM_AXIS_PORT + 0u, 1);
    mock_set_param(mock, NCL_SYNTEC_PARAM_AXIS_NAME + 0u, 100); /* X */
    mock_set_param(mock, NCL_SYNTEC_PARAM_AXIS_PORT + 2u, 3);
    mock_set_param(mock, NCL_SYNTEC_PARAM_AXIS_NAME + 2u, 300); /* Z */

    ncl_syntec_config_default(&config);
    config.host = "127.0.0.1";
    config.port = mock->port;
    config.timeout_ms = 800;
    session = ncl_syntec_open(&config, &err);
    NCL_CHECK(session != NULL);
    if (session == NULL) {
        mock_stop(mock);
        return;
    }

    NCL_TEST_CASE("§11.4: a parameter is one i32 behind request 0x0404");
    NCL_CHECK_EQ_INT(ncl_syntec_param(session, NCL_SYNTEC_PARAM_AXIS_NAME, &value),
                     NCL_OK);
    NCL_CHECK_EQ_INT(value, 100);
    NCL_CHECK_EQ_INT(mock->last_param, NCL_SYNTEC_PARAM_AXIS_NAME);
    NCL_CHECK_EQ_INT(ncl_syntec_param(session, 0, &value), NCL_ERR_INVALID_ARG);

    NCL_TEST_CASE("§11.4: the axis table is what the controller says");
    NCL_CHECK_EQ_INT(ncl_syntec_axes(session, axes, NCL_SYNTEC_AXIS_SLOTS,
                                     &count),
                     NCL_OK);
    NCL_CHECK_EQ_INT(count, 2);
    NCL_CHECK_EQ_INT(axes[0].slot, 0);
    NCL_CHECK_EQ_INT(axes[0].port, 1);
    NCL_CHECK_EQ_STR(axes[0].name, "X");
    NCL_CHECK_EQ_INT(axes[1].slot, 2);
    NCL_CHECK_EQ_INT(axes[1].port, 3);
    NCL_CHECK_EQ_STR(axes[1].name, "Z");

    NCL_TEST_CASE("11.4: a name resolves to the slot at read time");
    NCL_CHECK_EQ_INT(ncl_syntec_axis_index(session, "Z", &slot, &count), NCL_OK);
    NCL_CHECK_EQ_INT(slot, 2);   /* 控制器说 Z 是第 3 个槽 ... */
    NCL_CHECK_EQ_INT(count, 3);  /* ... 而状态区一项有 3 个（最后槽号 + 1） */
    NCL_CHECK_EQ_INT(ncl_syntec_axis_index(session, "x", &slot, &count), NCL_OK);
    NCL_CHECK_EQ_INT(slot, 0); /* 名字不挑大小写 */
    NCL_CHECK_EQ_INT(ncl_syntec_axis_index(session, "Y", &slot, &count),
                     NCL_ERR_NOT_FOUND); /* 没启用的槽不算数 */
    NCL_CHECK(strstr(ncl_syntec_last_error(session), "X/Z") != NULL);
    NCL_CHECK_EQ_INT(ncl_syntec_axis_index(session, "", &slot, &count),
                     NCL_ERR_INVALID_ARG);

    NCL_TEST_CASE("§11.4: one axis name on its own, and an unused slot");
    NCL_CHECK_EQ_INT(ncl_syntec_axis_name(session, 2, name, sizeof(name)),
                     NCL_OK);
    NCL_CHECK_EQ_STR(name, "Z");
    NCL_CHECK_EQ_INT(ncl_syntec_axis_name(session, 1, name, sizeof(name)),
                     NCL_OK);
    NCL_CHECK_EQ_STR(name, ""); /* 没接轴 */
    NCL_CHECK_EQ_INT(ncl_syntec_axis_name(session, NCL_SYNTEC_AXIS_SLOTS, name,
                                          sizeof(name)),
                     NCL_ERR_INVALID_ARG);

    NCL_TEST_CASE("11.4: the table is cached - a read does not ask 32 params again");
    mock->param_count = 0; /* 这会儿控制器“什么都不知道”了 */
    NCL_CHECK_EQ_INT(ncl_syntec_axes(session, axes, NCL_SYNTEC_AXIS_SLOTS,
                                     &count),
                     NCL_OK);
    NCL_CHECK_EQ_INT(count, 2); /* TTL 内不重问，还是刚才那张表 */

    NCL_TEST_CASE("11.4: a controller that says nothing is a clear 读不了");
    ncl_syntec_close(session);
    session = ncl_syntec_open(&config, &err);
    NCL_CHECK(session != NULL);
    if (session != NULL) {
        count = 123;
        NCL_CHECK_EQ_INT(ncl_syntec_axes(session, axes, NCL_SYNTEC_AXIS_SLOTS,
                                         &count),
                         NCL_ERR_UNAVAILABLE);
        NCL_CHECK_EQ_INT(count, 0);
        NCL_CHECK(strstr(ncl_syntec_last_error(session), "轴表") != NULL);
        ncl_syntec_close(session);
    }

    mock_stop(mock);
}

/**
 * 11.4: the parameter table. Numbers and titles come off the wire as 268 byte
 * records (title = UTF-16LE); paging happens here because the command has no
 * offset, so the whole table is read once and kept in the session.
 */
static void test_param_table(void)
{
    syntec_mock *mock = mock_start();
    ncl_syntec_param_spec page[4];
    ncl_syntec_tool tool;
    ncl_syntec_config config;
    ncl_syntec *session;
    char *err = NULL;
    int32_t value = 0;
    size_t total = 0;
    size_t got = 0;
    size_t index = 0;

    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    mock_set_schema(mock, 0, 321, "*X axis axis name", 10999, 100);
    mock_set_schema(mock, 1, 323, "*Z axis axis name", 10999, 100);
    mock_set_schema(mock, 2, 4761, "*Some other parameter", 4, 1);

    ncl_syntec_config_default(&config);
    config.host = "127.0.0.1";
    config.port = mock->port;
    config.timeout_ms = 800;
    session = ncl_syntec_open(&config, &err);
    NCL_CHECK(session != NULL);
    if (session == NULL) {
        mock_stop(mock);
        return;
    }

    NCL_TEST_CASE("11.4: the table's capacity is request 0x0401");
    NCL_CHECK_EQ_INT(ncl_syntec_param_capacity(session, &total), NCL_OK);
    NCL_CHECK_EQ_INT(total, 3);

    NCL_TEST_CASE("11.4: the table gives number, title, default");
    NCL_CHECK_EQ_INT(ncl_syntec_param_table(session, 0, 3, page, &got, &total),
                     NCL_OK);
    NCL_CHECK_EQ_INT(got, 3);
    NCL_CHECK_EQ_INT(total, 3);
    NCL_CHECK_EQ_INT(page[0].no, 321);
    NCL_CHECK_EQ_STR(page[0].title, "*X axis axis name");
    NCL_CHECK_EQ_INT(page[0].flags, 10999);
    NCL_CHECK_EQ_INT(page[0].fallback, 100); /* 100 = 'X' (§11.4's decoding) */
    NCL_CHECK_EQ_INT(page[1].no, 323);
    NCL_CHECK_EQ_STR(page[1].title, "*Z axis axis name");
    NCL_CHECK_EQ_INT(page[2].no, 4761);
    NCL_CHECK_EQ_STR(page[2].title, "*Some other parameter");

    NCL_TEST_CASE("11.4: paging clamps at the end of the table");
    got = 0;
    NCL_CHECK_EQ_INT(ncl_syntec_param_table(session, 2, 4, page, &got, &total),
                     NCL_OK);
    NCL_CHECK_EQ_INT(got, 1); /* only the last one is left */
    NCL_CHECK_EQ_INT(page[0].no, 4761);

    NCL_TEST_CASE("11.4: lookup is by parameter number, not by position");
    NCL_CHECK_EQ_INT(ncl_syntec_param_find(session, 323, &index), NCL_OK);
    NCL_CHECK_EQ_INT(index, 1);
    NCL_CHECK_EQ_INT(ncl_syntec_param_find(session, 999, &index),
                     NCL_ERR_NOT_FOUND);
    NCL_CHECK(strstr(ncl_syntec_last_error(session), "999") != NULL);

    NCL_TEST_CASE("11.4: a parameter value is one i32 (request 0x0404)");
    mock_set_param(mock, 321u, 100);
    NCL_CHECK_EQ_INT(ncl_syntec_param(session, 321u, &value), NCL_OK);
    NCL_CHECK_EQ_INT(value, 100);

    NCL_TEST_CASE("11.7: a tool is 224 bytes (nose, radius, 12 lengths, angle)");
    mock->tool_count = 2;
    mock->tools[1].tool_nose = 3; /* 刀号 1 = tools[1]：线上刀号从 1 起 */
    mock->tools[1].radius_geometry = 0.8;
    mock->tools[1].radius_wear = 0.01;
    mock->tools[1].length_geometry[0] = 12.5;
    mock->tools[1].length_geometry[11] = -1.25;
    mock->tools[1].length_wear[11] = -0.02;
    mock->tools[1].tool_angle = 60.0;
    NCL_CHECK_EQ_INT(ncl_syntec_tool_count(session, &total), NCL_OK);
    NCL_CHECK_EQ_INT(total, 2);
    NCL_CHECK_EQ_INT(ncl_syntec_tool_get(session, 1, &tool), NCL_OK);
    NCL_CHECK_EQ_INT(tool.tool_nose, 3);
    NCL_CHECK(tool.radius_geometry == 0.8);
    NCL_CHECK(tool.radius_wear == 0.01);
    NCL_CHECK(tool.length_geometry[0] == 12.5);
    NCL_CHECK(tool.length_geometry[11] == -1.25);
    NCL_CHECK(tool.length_wear[11] == -0.02);
    NCL_CHECK(tool.tool_angle == 60.0); /* 它在最后，而且是 double */

    NCL_TEST_CASE("11.6: a write lands (A=4, B=the number, flag=the value)");
    mock->put_hr = 0;
    NCL_CHECK_EQ_INT(ncl_syntec_param_put(session, 321u, 111), NCL_OK);
    NCL_CHECK_EQ_INT(mock->last_put_param, 321u);
    NCL_CHECK_EQ_INT(mock->last_put_value, 111u); /* flag 那一位装的是新值 */
    NCL_CHECK_EQ_INT(ncl_syntec_param(session, 321u, &value), NCL_OK);
    NCL_CHECK_EQ_INT(value, 111);

    NCL_TEST_CASE("11.6: a refused write is NCL_ERR_IO with the hr in last_error");
    mock->put_hr = 0x1234;
    NCL_CHECK_EQ_INT(ncl_syntec_param_put(session, 321u, 222), NCL_ERR_IO);
    NCL_CHECK(strstr(ncl_syntec_last_error(session), "0x00001234") != NULL);
    mock->put_hr = 0;

    /*
     * §11.7 写刀：帧是 256 字节，In 是 228 字节的 { nToolNo, TToolOffset }，
     * 跟在 16 字节桩头后面（桩头里塞不下），A = dwSizeOut = 4（Out 只有 hr）。
     */
    NCL_TEST_CASE("11.7: the write frame is 16 + 228 bytes, the In behind the head");
    {
        uint8_t frame[NCL_SYNTEC_TOOL_FRAME];
        ncl_syntec_tool want;

        memset(&want, 0, sizeof(want));
        want.tool_nose = 3;
        want.radius_geometry = 0.75;
        want.length_geometry[0] = 1.5;
        want.tool_angle = 60.0;
        NCL_CHECK_EQ_INT(NCL_SYNTEC_TOOL_FRAME, 256);
        NCL_CHECK_EQ_INT(NCL_SYNTEC_TOOL_IN, 228);
        NCL_CHECK_EQ_INT(ncl_syntec_tool_put_frame(frame, sizeof(frame), &want,
                                                   5u, 0u),
                         NCL_SYNTEC_TOOL_FRAME);
        NCL_CHECK_EQ_INT(get_u32(frame), 16u + 228u);        /* Length */
        NCL_CHECK_EQ_INT(get_u16(frame + 4), 16);            /* CmdID */
        NCL_CHECK_EQ_INT(get_u32(frame + 8), 0x070000C8u);   /* Reserved */
        NCL_CHECK_EQ_INT(get_u16(frame + 12), 200);          /* uFuncID */
        NCL_CHECK_EQ_INT(get_u32(frame + 16), NCL_SYNTEC_CODE_TOOL_PUT);
        NCL_CHECK_EQ_INT(get_u32(frame + 20), 228u);         /* dwSizeIn */
        NCL_CHECK_EQ_INT(get_u32(frame + 24), 4u);           /* dwSizeOut */
        NCL_CHECK_EQ_INT(get_u32(frame + 28), 5u);           /* nToolNo */
        NCL_CHECK_EQ_INT(get_u32(frame + 32), 3u);           /* ToolNose */
        NCL_CHECK(get_f64(frame + 40) == 0.75);              /* RadiusGeometry */
        NCL_CHECK(get_f64(frame + 56) == 1.5);               /* LengthGeometry[0] */
        NCL_CHECK(get_f64(frame + 248) == 60.0);             /* ToolAngle */
    }

    NCL_TEST_CASE("11.7: a tool write lands and the read gives it back");
    {
        ncl_syntec_tool want;

        memset(&want, 0, sizeof(want));
        want.tool_nose = 7;
        want.radius_geometry = 1.25;
        want.tool_angle = 80.0;
        mock->put_hr = 0;
        NCL_CHECK_EQ_INT(ncl_syntec_tool_put(session, 1u, &want), NCL_OK);
        NCL_CHECK_EQ_INT(mock->last_tool_put, 1u);
        NCL_CHECK_EQ_INT(mock->tool_put_in_len, NCL_SYNTEC_TOOL_IN);
        NCL_CHECK_EQ_INT(ncl_syntec_tool_get(session, 1u, &tool), NCL_OK);
        NCL_CHECK_EQ_INT(tool.tool_nose, 7);
        NCL_CHECK(tool.radius_geometry == 1.25);
        NCL_CHECK(tool.tool_angle == 80.0);
    }

    NCL_TEST_CASE("11.9: PLC capacity, one register and one bit off the mock");
    {
        ncl_syntec_plc_slots slots;
        uint32_t value = 0;
        bool bit = false;
        uint8_t frame[NCL_SYNTEC_ITEM_FRAME];

        mock->plc_bits[0] = 512;
        mock->plc_bits[4] = 512;
        mock->plc_registers = 65536;
        mock->plc_bit[NCL_SYNTEC_PLC_I] = 1;
        /* R 寄存器读的就是 0x041A，和条目那条路同源：值放在条目表里 */
        mock_set_value(mock, 771u, 1000u);
        NCL_CHECK_EQ_INT(ncl_syntec_plc_capacity(session, &slots), NCL_OK);
        NCL_CHECK_EQ_INT(slots.ibits, 512);
        NCL_CHECK_EQ_INT(slots.abits, 512);
        NCL_CHECK_EQ_INT(slots.registers, 65536);
        NCL_CHECK_EQ_INT(slots.timers, 256);
        NCL_CHECK_EQ_INT(ncl_syntec_plc_register(session, 771u, &value), NCL_OK);
        NCL_CHECK_EQ_INT(value, 1000); /* 21A 上 R771 就是主轴转速 */
        NCL_CHECK_EQ_INT(ncl_syntec_plc_bit(session, NCL_SYNTEC_PLC_I, 0u, &bit),
                         NCL_OK);
        NCL_CHECK(bit);
        NCL_CHECK_EQ_INT(mock->last_plc_bit_code, NCL_SYNTEC_CODE_PLC_GET_BIT);
        /* 位族之间只差两个数：I=0x0412、A=0x0419 */
        NCL_CHECK_EQ_INT(ncl_syntec_plc_bit_frame(frame, sizeof(frame),
                                                 NCL_SYNTEC_PLC_A, 0u, 0u),
                         NCL_SYNTEC_ITEM_FRAME);
        NCL_CHECK_EQ_INT(get_u32(frame + 16), 0x0419u);
        NCL_CHECK_EQ_INT(get_u32(frame + 24), 8u); /* Out = { hr, Value u8 } */
    }

    NCL_TEST_CASE("11.9: a variable comes back as int or double");
    {
        ncl_syntec_variant value;
        size_t count = 0;
        uint8_t frame[NCL_SYNTEC_ITEM_FRAME];

        mock->var_count = 14096; /* 21A 的真实容量 */
        /* mock 只摆 8 个号（0..7）：5 号是整数、6 号是浮点 */
        mock->var_type[5] = 1;
        mock->var_int[5] = 1;
        mock->var_type[6] = 2;
        mock->var_double[6] = 2.5;
        NCL_CHECK_EQ_INT(ncl_syntec_variable_capacity(session, &count), NCL_OK);
        NCL_CHECK_EQ_INT(count, 14096);
        NCL_CHECK_EQ_INT(ncl_syntec_variable(session, 5u, &value), NCL_OK);
        NCL_CHECK_EQ_INT(value.type, 1);
        NCL_CHECK_EQ_INT(value.int_value, 1);
        NCL_CHECK_EQ_INT(ncl_syntec_variable(session, 6u, &value), NCL_OK);
        NCL_CHECK_EQ_INT(value.type, 2);
        NCL_CHECK(value.double_value == 2.5);
        NCL_CHECK_EQ_INT(mock->last_var_no, 6u);
        /* In = { nNo }（4），Out = { hr, TOcVariant 16 } -> A = 20 */
        NCL_CHECK_EQ_INT(ncl_syntec_variable_frame(frame, sizeof(frame), 500u, 0u),
                         NCL_SYNTEC_ITEM_FRAME);
        NCL_CHECK_EQ_INT(get_u32(frame + 16), NCL_SYNTEC_CODE_GLOBAL_GET_VALUE);
        NCL_CHECK_EQ_INT(get_u32(frame + 20), 4u);  /* dwSizeIn */
        NCL_CHECK_EQ_INT(get_u32(frame + 24), 20u); /* dwSizeOut */
        NCL_CHECK_EQ_INT(get_u32(frame + 28), 500u);
    }

    NCL_TEST_CASE("11.7: a refused tool write is NCL_ERR_IO, tool 0 is invalid");
    {
        ncl_syntec_tool want;

        memset(&want, 0, sizeof(want));
        want.tool_nose = 9;
        mock->put_hr = 0x1234;
        NCL_CHECK_EQ_INT(ncl_syntec_tool_put(session, 1u, &want), NCL_ERR_IO);
        NCL_CHECK(strstr(ncl_syntec_last_error(session), "0x00001234") != NULL);
        mock->put_hr = 0;
        NCL_CHECK_EQ_INT(ncl_syntec_tool_put(session, 0u, &want),
                         NCL_ERR_INVALID_ARG); /* 刀号从 1 起 */
    }

    ncl_syntec_close(session);
    mock_stop(mock);
}

/** Index of a point by its model path, or (size_t)-1. */
static size_t host_point_index(const ncl_host *host, const char *path)
{
    size_t i;

    for (i = 0; i < ncl_host_point_count(host); i++) {
        const char *candidate = ncl_host_point_path(host, i);

        if (candidate != NULL && strcmp(candidate, path) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

/*
 * The adapter itself: plugins/syntec.c is loaded as a module by protocol name
 * (exactly the path the device program takes), the host turns its declaration
 * into the model, and the nine points are read over the same mock controller.
 * That is the "整机仿真" 10 册 §3.2 promised: adapter + client + captured frames.
 */
static void test_adapter(void)
{
    static const char *kPaths[] = {
        "/MACHINE/STATUS",           "/MACHINE/PART_COUNT",
        "/MACHINE/CONTROLLER/PROGRAM", "/MACHINE/CONTROLLER/WARNING",
        "/MACHINE/CONTROLLER/LINE_NUMBER", "/MACHINE/FEED_OVERRIDE",
        "/MACHINE/SPINDLE_OVERRIDE", "/MACHINE/FEED_SPEED",
        "/MACHINE/SPINDLE_SPEED",
        /* 九个轴字母 × 六格（实际/指令/机械 + 绝对/相对/剩余），模型一次配全 */
        "/MACHINE/AXIS@X/SCREW/POSITION",
        "/MACHINE/AXIS@Y/SCREW/POSITION",
        "/MACHINE/AXIS@Z/SCREW/POSITION",
        "/MACHINE/AXIS@A/SCREW/POSITION",
        "/MACHINE/AXIS@B/SCREW/POSITION",
        "/MACHINE/AXIS@C/SCREW/POSITION",
        "/MACHINE/AXIS@U/SCREW/POSITION",
        "/MACHINE/AXIS@V/SCREW/POSITION",
        "/MACHINE/AXIS@W/SCREW/POSITION",
        "/MACHINE/AXIS@X/SERVO_DRIVER/POSITION",
        "/MACHINE/AXIS@Y/SERVO_DRIVER/POSITION",
        "/MACHINE/AXIS@Z/SERVO_DRIVER/POSITION",
        "/MACHINE/AXIS@A/SERVO_DRIVER/POSITION",
        "/MACHINE/AXIS@B/SERVO_DRIVER/POSITION",
        "/MACHINE/AXIS@C/SERVO_DRIVER/POSITION",
        "/MACHINE/AXIS@U/SERVO_DRIVER/POSITION",
        "/MACHINE/AXIS@V/SERVO_DRIVER/POSITION",
        "/MACHINE/AXIS@W/SERVO_DRIVER/POSITION",
        "/MACHINE/AXIS@X/MOTOR/POSITION",
        "/MACHINE/AXIS@Y/MOTOR/POSITION",
        "/MACHINE/AXIS@Z/MOTOR/POSITION",
        "/MACHINE/AXIS@A/MOTOR/POSITION",
        "/MACHINE/AXIS@B/MOTOR/POSITION",
        "/MACHINE/AXIS@C/MOTOR/POSITION",
        "/MACHINE/AXIS@U/MOTOR/POSITION",
        "/MACHINE/AXIS@V/MOTOR/POSITION",
        "/MACHINE/AXIS@W/MOTOR/POSITION",
        "/MACHINE/AXIS@X/MOTOR/VARIABLE@ABSOLUTE",
        "/MACHINE/AXIS@Y/MOTOR/VARIABLE@ABSOLUTE",
        "/MACHINE/AXIS@Z/MOTOR/VARIABLE@ABSOLUTE",
        "/MACHINE/AXIS@A/MOTOR/VARIABLE@ABSOLUTE",
        "/MACHINE/AXIS@B/MOTOR/VARIABLE@ABSOLUTE",
        "/MACHINE/AXIS@C/MOTOR/VARIABLE@ABSOLUTE",
        "/MACHINE/AXIS@U/MOTOR/VARIABLE@ABSOLUTE",
        "/MACHINE/AXIS@V/MOTOR/VARIABLE@ABSOLUTE",
        "/MACHINE/AXIS@W/MOTOR/VARIABLE@ABSOLUTE",
        "/MACHINE/AXIS@X/MOTOR/VARIABLE@RELATIVE",
        "/MACHINE/AXIS@Y/MOTOR/VARIABLE@RELATIVE",
        "/MACHINE/AXIS@Z/MOTOR/VARIABLE@RELATIVE",
        "/MACHINE/AXIS@A/MOTOR/VARIABLE@RELATIVE",
        "/MACHINE/AXIS@B/MOTOR/VARIABLE@RELATIVE",
        "/MACHINE/AXIS@C/MOTOR/VARIABLE@RELATIVE",
        "/MACHINE/AXIS@U/MOTOR/VARIABLE@RELATIVE",
        "/MACHINE/AXIS@V/MOTOR/VARIABLE@RELATIVE",
        "/MACHINE/AXIS@W/MOTOR/VARIABLE@RELATIVE",
        "/MACHINE/AXIS@X/MOTOR/VARIABLE@DISTANCE",
        "/MACHINE/AXIS@Y/MOTOR/VARIABLE@DISTANCE",
        "/MACHINE/AXIS@Z/MOTOR/VARIABLE@DISTANCE",
        "/MACHINE/AXIS@A/MOTOR/VARIABLE@DISTANCE",
        "/MACHINE/AXIS@B/MOTOR/VARIABLE@DISTANCE",
        "/MACHINE/AXIS@C/MOTOR/VARIABLE@DISTANCE",
        "/MACHINE/AXIS@U/MOTOR/VARIABLE@DISTANCE",
        "/MACHINE/AXIS@V/MOTOR/VARIABLE@DISTANCE",
        "/MACHINE/AXIS@W/MOTOR/VARIABLE@DISTANCE"};
    syntec_mock *mock;
    ncl_module_set *modules;
    ncl_strbuf err;
    ncl_strbuf json;
    ncl_json *config;
    ncl_host *host = NULL;
    const ncl_json *value;
    long long number = 0;
    double real = 0.0;
    size_t i;

    if (NCL_SYNTEC_PLUGIN_DIR[0] == '\0') {
        return; /* built without the adapter modules (NCLINK_BUILD_PLUGINS=OFF) */
    }
    mock = mock_start();
    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    mock_set_value(mock, 4u, 2u);      /* STATUS = running */
    mock_set_value(mock, 1000u, 1234u);
    mock_set_value(mock, 10u, 4321u);
    mock_set_value(mock, 19u, 80u);
    mock_set_value(mock, 771u, 9000u);
    mock_set_value(mock, 21u, 90u);
    mock_set_value(mock, 700u, 4321u);
    mock_set_value(mock, 12u, 0u);
    mock_set_value(mock, 76u, 70u);
    /* 轴表（§11.4）：X 在第 1 槽（端口 1），Z 在第 3 槽（端口 3）。 */
    mock_set_param(mock, NCL_SYNTEC_PARAM_AXIS_PORT + 0u, 1);
    mock_set_param(mock, NCL_SYNTEC_PARAM_AXIS_NAME + 0u, 100); /* X */
    mock_set_param(mock, NCL_SYNTEC_PARAM_AXIS_PORT + 2u, 3);
    mock_set_param(mock, NCL_SYNTEC_PARAM_AXIS_NAME + 2u, 300); /* Z */
    /* 参数表（§11.4）：一条轴名，值也脚本化好。 */
    mock_set_schema(mock, 0, 321, "*X axis axis name", 10999, 100);
    mock_set_param(mock, 321u, 100);
    /* 刀具表（§11.7）：两把刀，第一把给点非零值。 */
    mock->tool_count = 2;
    mock->tools[1].tool_nose = 3; /* 刀号 1 = tools[1] */
    mock->tools[1].radius_geometry = 0.8;
    mock->tools[1].tool_angle = 60.0;
    snprintf(mock->program, sizeof(mock->program), "O1000");

    modules = ncl_modules_create();
    ncl_strbuf_init(&err);
    NCL_CHECK(modules != NULL);
    NCL_TEST_CASE("the syntec adapter loads as a tool module");
    if (modules == NULL ||
        ncl_modules_add(modules, "syntec", NCL_SYNTEC_PLUGIN_DIR, &err) != NCL_OK) {
        NCL_CHECK_EQ_INT((int)err.len, 0);
        ncl_strbuf_free(&err);
        mock_stop(mock);
        return;
    }
    NCL_CHECK_EQ_INT((int)err.len, 0);
    NCL_CHECK_EQ_STR(ncl_module_name(modules, 0), "syntec");

    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(
        &json,
        "{ \"sn\": \"V000000001\","
        "  \"tools\": [ { \"name\": \"syntec\", \"parameters\": {"
        "     \"host\": \"127.0.0.1\", \"port\": %u, \"timeoutMs\": 800 } } ],"
        "  \"device\": { \"type\": \"MACHINE\", \"id\": \"01\","
        "                \"name\": \"新代机床\" },"
        "  \"sample\": { \"intervalMs\": 250, \"uploadMs\": 250 } }",
        mock->port);
    config = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), &err);
    ncl_strbuf_free(&json);
    NCL_CHECK(config != NULL);
    if (config != NULL) {
        host = ncl_host_create_with_modules(config, modules, &err);
        ncl_json_free(config);
    }
    NCL_CHECK(host != NULL);
    if (host == NULL) {
        ncl_strbuf_free(&err);
        ncl_modules_free(modules);
        mock_stop(mock);
        return;
    }

    NCL_TEST_CASE("the nine points are the model the device publishes");
    /* 9 项 + 9 轴 × 6 格(54) + 参数表 + 刀具表 + 寄存器 6 族 + 变量表 = 72 */
    NCL_CHECK_EQ_INT(ncl_host_point_count(host), 72);
    for (i = 0; i < sizeof(kPaths) / sizeof(kPaths[0]); i++) {
        NCL_CHECK(host_point_index(host, kPaths[i]) != (size_t)-1);
    }
    /* 模型里就是这台机床的能力面：路径是按树推出来的（模型文档里没有 path 字段），
     * 采样通道只引用四样（状态、计件、程序名、报警）。 */
    {
        ncl_node *part = ncl_node_find_by_id(ncl_server_model(ncl_host_server(host)),
                                            "p1");
        NCL_CHECK(part != NULL);
        if (part != NULL) {
            NCL_CHECK_EQ_STR(ncl_node_path(part), "/MACHINE/PART_COUNT");
        }
        NCL_CHECK(ncl_node_find_by_type(ncl_server_model(ncl_host_server(host)),
                                        NCL_NODE_TYPE_SAMPLE_CHANNEL) != NULL);
    }

    NCL_TEST_CASE("the adapter reads the nine items off the mock controller");
    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/STATUS", &err), NCL_OK);
    value = ncl_host_point_value(host, host_point_index(host, "/MACHINE/STATUS"));
    NCL_CHECK(value != NULL);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "running");

    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/PART_COUNT", &err), NCL_OK);
    value = ncl_host_point_value(host,
                                 host_point_index(host, "/MACHINE/PART_COUNT"));
    NCL_CHECK(value != NULL && ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 1234);

    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/CONTROLLER/PROGRAM", &err),
                     NCL_OK);
    value = ncl_host_point_value(
        host, host_point_index(host, "/MACHINE/CONTROLLER/PROGRAM"));
    NCL_CHECK(value != NULL);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "O1000");

    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/CONTROLLER/WARNING", &err),
                     NCL_OK);
    value = ncl_host_point_value(
        host, host_point_index(host, "/MACHINE/CONTROLLER/WARNING"));
    NCL_CHECK(value != NULL);
    NCL_CHECK_EQ_INT(ncl_json_arr_len(value), 0); /* no alarm = empty list */

    NCL_CHECK_EQ_INT(
        ncl_host_poll_one(host, "/MACHINE/CONTROLLER/LINE_NUMBER", &err), NCL_OK);
    value = ncl_host_point_value(
        host, host_point_index(host, "/MACHINE/CONTROLLER/LINE_NUMBER"));
    NCL_CHECK(value != NULL);
    NCL_CHECK_EQ_STR(ncl_json_as_string(value), "4321"); /* 表 7 是 string */

    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/FEED_OVERRIDE", &err),
                     NCL_OK);
    value = ncl_host_point_value(host,
                                 host_point_index(host, "/MACHINE/FEED_OVERRIDE"));
    NCL_CHECK(value != NULL && ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 80);

    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/SPINDLE_OVERRIDE", &err),
                     NCL_OK);
    value = ncl_host_point_value(
        host, host_point_index(host, "/MACHINE/SPINDLE_OVERRIDE"));
    NCL_CHECK(value != NULL && ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 90);

    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/SPINDLE_SPEED", &err),
                     NCL_OK);
    value = ncl_host_point_value(host,
                                 host_point_index(host, "/MACHINE/SPINDLE_SPEED"));
    NCL_CHECK(value != NULL && ncl_json_as_double(value, &real));
    NCL_CHECK_EQ_INT((long long)real, 9000); /* 主轴转速，rpm */

    NCL_TEST_CASE("FEED_SPEED goes through its three frames here too");
    mock->item_log_count = 0;
    NCL_CHECK_EQ_INT(ncl_host_poll_one(host, "/MACHINE/FEED_SPEED", &err), NCL_OK);
    value = ncl_host_point_value(
        host, host_point_index(host, "/MACHINE/FEED_SPEED"));
    NCL_CHECK(value != NULL && ncl_json_as_double(value, &real));
    NCL_CHECK_EQ_INT((long long)real, 4321);
    NCL_CHECK_EQ_INT(mock->item_log_count, 3);

    NCL_TEST_CASE("位置走状态区：int16 + 10^-小数位");
    {
        /* 房把 X 摆到 1.234、Z 摆到 -0.5（int16 原值），小数位 3。 */
        /*
         * 状态区一项按**槽号**排：槽 0 = X、槽 1 = 另一个轴（这里塞 777 当陷阱）、
         * 槽 2 = Z。轴的顺序由控制器说了算，所以读 Z 必须落在下标 2 上。
         */
        static const int16_t kMachine[3] = {1234, 777, -500};
        static const int16_t kDecimals[1] = {3};
        static const int16_t kAbsolute[3] = {2000, 999, -1};

        mock_set_zone(mock, NCL_SYNTEC_ZONE_MACHINE, kMachine, 3);
        mock_set_zone(mock, NCL_SYNTEC_ZONE_DECIMALS, kDecimals, 1);
        mock_set_zone(mock, NCL_SYNTEC_ZONE_ABSOLUTE, kAbsolute, 3);

        value = NULL;
        NCL_CHECK_EQ_INT(
            ncl_host_poll_one(host, "/MACHINE/AXIS@X/MOTOR/POSITION", &err),
            NCL_OK);
        value = ncl_host_point_value(
            host, host_point_index(host, "/MACHINE/AXIS@X/MOTOR/POSITION"));
        NCL_CHECK(value != NULL && ncl_json_as_double(value, &real));
        NCL_CHECK_EQ_INT((long long)(real * 1000.0 + 0.5), 1234);

        NCL_CHECK_EQ_INT(
            ncl_host_poll_one(host, "/MACHINE/AXIS@Z/MOTOR/POSITION", &err),
            NCL_OK);
        value = ncl_host_point_value(
            host, host_point_index(host, "/MACHINE/AXIS@Z/MOTOR/POSITION"));
        NCL_CHECK(value != NULL && ncl_json_as_double(value, &real));
        NCL_CHECK_EQ_INT((long long)(real * 1000.0 - 0.5), (long long)-500);

        NCL_CHECK_EQ_INT(ncl_host_poll_one(
                             host, "/MACHINE/AXIS@X/MOTOR/VARIABLE@ABSOLUTE", &err),
                         NCL_OK);
        value = ncl_host_point_value(
            host,
            host_point_index(host, "/MACHINE/AXIS@X/MOTOR/VARIABLE@ABSOLUTE"));
        NCL_CHECK(value != NULL && ncl_json_as_double(value, &real));
        NCL_CHECK_EQ_INT((long long)(real * 1000.0 + 0.5), 2000);
    }

    NCL_TEST_CASE("11.3.3: the demo's 实际位置 is the screw side, 指令位置 a gap");
    /* 实际位置（丝杠侧）与 MOTOR/POSITION 同源：都读区 101 的 X。 */
    NCL_CHECK_EQ_INT(
        ncl_host_poll_one(host, "/MACHINE/AXIS@X/SCREW/POSITION", &err), NCL_OK);
    value = ncl_host_point_value(
        host, host_point_index(host, "/MACHINE/AXIS@X/SCREW/POSITION"));
    NCL_CHECK(value != NULL && ncl_json_as_double(value, &real));
    NCL_CHECK_EQ_INT((long long)(real * 1000.0 + 0.5), 1234);
    /* 指令位置（驱动侧）：控制器里还没有这一项 → 照实报"读不了"，不给个数。 */
    NCL_CHECK(ncl_host_poll_one(host, "/MACHINE/AXIS@X/SERVO_DRIVER/POSITION",
                                &err) != NCL_OK);
    NCL_CHECK(ncl_host_poll_one(host, "/MACHINE/AXIS@W/SERVO_DRIVER/POSITION",
                                &err) != NCL_OK);

    NCL_TEST_CASE("11.9: one REGISTER per family, and the variable table");
    NCL_CHECK(host_point_index(host, "/MACHINE/CONTROLLER/REGISTER@R") != (size_t)-1);
    NCL_CHECK(host_point_index(host, "/MACHINE/CONTROLLER/REGISTER@I") != (size_t)-1);
    NCL_CHECK(host_point_index(host, "/MACHINE/CONTROLLER/REGISTER@A") != (size_t)-1);
    NCL_CHECK(host_point_index(host, "/MACHINE/CONTROLLER/VARIABLE") != (size_t)-1);

    NCL_TEST_CASE("11.4: parameters are a config object (dict), not a method");
    {
        ncl_json *model = ncl_node_to_json(ncl_server_model(ncl_host_server(host)));
        char *text = model != NULL ? ncl_json_write_string(model) : NULL;

        NCL_CHECK(text != NULL);
        if (text != NULL) {
        NCL_CHECK(strstr(text, "/MACHINE/CONTROLLER/PARAMETER") != NULL);
        NCL_CHECK(strstr(text, "HASH") != NULL); /* dataType：册 4 说 PARAMETER 是 dict */
        NCL_CHECK(strstr(text, "/MACHINE/CONTROLLER/TOOL") != NULL);
        NCL_CHECK(strstr(text, "LIST") != NULL); /* 刀具列表是 list（册 4） */
            ncl_mem_free(text);
        }
        ncl_json_free(model);
    }

    NCL_TEST_CASE("11.9: a Query reads REGISTER@R by number");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        ncl_query_request_item *item =
            ncl_query_request_item_new("/MACHINE/CONTROLLER/REGISTER@R");
        ncl_message *response;

        /* mock：容量 65536（REGISTER 是 LIST，长度走 get_length），R771 = 1000 */
        mock->plc_registers = 65536;
        mock_set_value(mock, 771u, 1000u);

        NCL_CHECK(request != NULL && item != NULL);
        (void)ncl_params_set_string(&item->params, "operation", "get_length");
        (void)ncl_message_set_message_id(request, "q6");
        (void)ncl_message_add_query_request_item(request, item);
        response = ncl_server_invoke_query(ncl_host_server(host), request);
        ncl_message_free(request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *row =
                ncl_ptrvec_at(&response->as.query_response.items, 0);
            long long length = -1;

            NCL_CHECK(row != NULL && ncl_check_is_code_ok(row->code));
            NCL_CHECK(row->values != NULL &&
                      ncl_json_as_int(ncl_json_arr_get(row->values, 0), &length));
            NCL_CHECK_EQ_INT(length, 65536);
            ncl_message_free(response);
        }

        request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        item = ncl_query_request_item_new("/MACHINE/CONTROLLER/REGISTER@R");
        NCL_CHECK(request != NULL && item != NULL);
        (void)ncl_params_set_string(&item->params, "operation", "get_value");
        (void)ncl_params_set_string(&item->params, "keys", "771");
        (void)ncl_message_set_message_id(request, "q6b");
        (void)ncl_message_add_query_request_item(request, item);
        response = ncl_server_invoke_query(ncl_host_server(host), request);
        ncl_message_free(request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *row =
                ncl_ptrvec_at(&response->as.query_response.items, 0);
            const ncl_json *values = row != NULL ? row->values : NULL;

            NCL_CHECK(row != NULL && ncl_check_is_code_ok(row->code));
            if (values != NULL && ncl_json_arr_len(values) == 1) {
                NCL_CHECK_EQ_INT(
                    ncl_json_obj_get_int(ncl_json_arr_get(values, 0), "771", -1),
                    1000);
            }

            NCL_TEST_CASE("11.9: REGISTER@R refuses a number past its length");
            if (response != NULL) {
                ncl_message_free(response);
            }
            request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
            item = ncl_query_request_item_new("/MACHINE/CONTROLLER/REGISTER@R");
            NCL_CHECK(request != NULL && item != NULL);
            (void)ncl_params_set_string(&item->params, "operation", "get_value");
            (void)ncl_params_set_string(&item->params, "keys", "99999");
            (void)ncl_message_set_message_id(request, "q6c");
            (void)ncl_message_add_query_request_item(request, item);
            response = ncl_server_invoke_query(ncl_host_server(host), request);
            ncl_message_free(request);
            NCL_CHECK(response != NULL);
            if (response != NULL) {
                ncl_query_response_item *bad =
                    ncl_ptrvec_at(&response->as.query_response.items, 0);

                NCL_CHECK(bad != NULL && !ncl_check_is_code_ok(bad->code));
                ncl_message_free(response);
            }
        }
    }

    NCL_TEST_CASE("11.9: a Query reads a bit out of REGISTER@I");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        ncl_query_request_item *item =
            ncl_query_request_item_new("/MACHINE/CONTROLLER/REGISTER@I");
        ncl_message *response;

        mock->plc_bits[0] = 512;
        mock->plc_bit[NCL_SYNTEC_PLC_I] = 1;
        NCL_CHECK(request != NULL && item != NULL);
        (void)ncl_params_set_string(&item->params, "operation", "get_value");
        (void)ncl_params_set_string(&item->params, "keys", "0");
        (void)ncl_message_set_message_id(request, "q7");
        (void)ncl_message_add_query_request_item(request, item);
        response = ncl_server_invoke_query(ncl_host_server(host), request);
        ncl_message_free(request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *row =
                ncl_ptrvec_at(&response->as.query_response.items, 0);
            const ncl_json *values = row != NULL ? row->values : NULL;

            NCL_CHECK(row != NULL && ncl_check_is_code_ok(row->code));
            if (values != NULL && ncl_json_arr_len(values) == 1) {
                NCL_CHECK(ncl_json_obj_get_bool(ncl_json_arr_get(values, 0), "0",
                                                false));
            }
            ncl_message_free(response);
        }
    }

    NCL_TEST_CASE("11.4: a Query reads a parameter by key");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        ncl_query_request_item *item =
            ncl_query_request_item_new("/MACHINE/CONTROLLER/PARAMETER");
        ncl_message *response;

        NCL_CHECK(request != NULL && item != NULL);
        (void)ncl_params_set_string(&item->params, "operation", "get_value");
        (void)ncl_params_set_string(&item->params, "keys", "321");
        (void)ncl_message_set_message_id(request, "q1");
        (void)ncl_message_add_query_request_item(request, item);
        response = ncl_server_invoke_query(ncl_host_server(host), request);
        ncl_message_free(request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *row = ncl_ptrvec_at(&response->as.query_response.items, 0);
            ncl_json *values = row != NULL ? row->values : NULL;

            NCL_CHECK(row != NULL && ncl_check_is_code_ok(row->code));
            NCL_CHECK(values != NULL && ncl_json_arr_len(values) == 1);
            if (values != NULL && ncl_json_arr_len(values) == 1) {
                const ncl_json *v = ncl_json_arr_get(values, 0);

                NCL_CHECK_EQ_INT(ncl_json_obj_get_int(v, "321", -1), 100);
            }
            ncl_message_free(response);
        }
    }

    NCL_TEST_CASE("11.7: a Query reads the tool table by tool number");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        ncl_query_request_item *item =
            ncl_query_request_item_new("/MACHINE/CONTROLLER/TOOL");
        ncl_message *response;

        NCL_CHECK(request != NULL && item != NULL);
        (void)ncl_params_set_string(&item->params, "operation", "get_value");
        (void)ncl_params_set_string(&item->params, "keys", "1");
        (void)ncl_message_set_message_id(request, "q4");
        (void)ncl_message_add_query_request_item(request, item);
        response = ncl_server_invoke_query(ncl_host_server(host), request);
        ncl_message_free(request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *row =
                ncl_ptrvec_at(&response->as.query_response.items, 0);
            const ncl_json *values = row != NULL ? row->values : NULL;

            NCL_CHECK(row != NULL && ncl_check_is_code_ok(row->code));
            if (values != NULL && ncl_json_arr_len(values) == 1) {
                const ncl_json *tool = ncl_json_obj_get(
                    ncl_json_arr_get(values, 0), "1");

                NCL_CHECK(tool != NULL);
                if (tool != NULL) {
                    /* 册 4 的四个名字在前：id/kind/radius/length */
                    NCL_CHECK_EQ_INT(ncl_json_obj_get_int(tool, "id", -1), 1);
                    NCL_CHECK_EQ_INT(ncl_json_obj_get_int(tool, "kind", -1), 3);
                    NCL_CHECK_EQ_INT(
                        (long long)(ncl_json_obj_get_double(tool, "radius", 0.0) *
                                        1000.0 +
                                    0.5),
                        800);
                    NCL_CHECK_EQ_INT(
                        ncl_json_arr_len(ncl_json_obj_get(tool, "length_geometry")),
                        12);
                }
            }
            ncl_message_free(response);
        }
    }

    NCL_TEST_CASE("11.6: a Set writes the parameter (permissions live outside)");
    {
        ncl_message *write = ncl_message_new(NCL_MSG_SET_REQUEST);
        ncl_set_request_item *item =
            ncl_set_request_item_new("/MACHINE/CONTROLLER/PARAMETER");
        ncl_message *response;

        NCL_CHECK(write != NULL && item != NULL);
        (void)ncl_params_set_string(&item->params, "operation", "set_value");
        (void)ncl_params_set_string(&item->params, "keys", "321");
        (void)ncl_params_set_int(&item->params, "value", 111);
        (void)ncl_message_set_message_id(write, "s1");
        (void)ncl_message_add_set_request_item(write, item);
        mock->put_hr = 0;
        response = ncl_server_invoke_set(ncl_host_server(host), write);
        ncl_message_free(write);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_set_response_item *row =
                ncl_ptrvec_at(&response->as.set_response.items, 0);

            NCL_CHECK(row != NULL && ncl_check_is_code_ok(row->code));
            ncl_message_free(response);
        }
        NCL_CHECK_EQ_INT(mock->last_put_param, 321u);
        NCL_CHECK_EQ_INT(mock->last_put_value, 111u);
        {
            size_t k;
            long long stored = -1;

            for (k = 0; k < mock->param_count; k++) {
                if (mock->params[k].number == 321u) {
                    stored = mock->params[k].value;
                }
            }
            NCL_CHECK_EQ_INT(stored, 111); /* 值落到了 mock 的表里 */
        }
    }

    /* 册 4：get_keys 是 HASH（dict）的操作，get_length 是 LIST 的 —— 两边都验，
     * 并且都要能看出"另一个操作没声明"。 */
    NCL_TEST_CASE("11.4: a HASH answers get_keys (and not get_length)");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        ncl_query_request_item *item =
            ncl_query_request_item_new("/MACHINE/CONTROLLER/PARAMETER");
        ncl_message *response;

        NCL_CHECK(request != NULL && item != NULL);
        (void)ncl_params_set_string(&item->params, "operation", "get_keys");
        (void)ncl_message_set_message_id(request, "q2");
        (void)ncl_message_add_query_request_item(request, item);
        response = ncl_server_invoke_query(ncl_host_server(host), request);
        ncl_message_free(request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *row = ncl_ptrvec_at(&response->as.query_response.items, 0);

            NCL_CHECK(row != NULL && ncl_check_is_code_ok(row->code));
            /* mock 的参数表里只有 321 一条 */
            NCL_CHECK(row->values != NULL && ncl_json_arr_len(row->values) == 1);
            ncl_message_free(response);
        }

        NCL_TEST_CASE("11.4: the same HASH does not answer get_length");
        request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        item = ncl_query_request_item_new("/MACHINE/CONTROLLER/PARAMETER");
        NCL_CHECK(request != NULL && item != NULL);
        (void)ncl_params_set_string(&item->params, "operation", "get_length");
        (void)ncl_message_set_message_id(request, "q2b");
        (void)ncl_message_add_query_request_item(request, item);
        response = ncl_server_invoke_query(ncl_host_server(host), request);
        ncl_message_free(request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *row = ncl_ptrvec_at(&response->as.query_response.items, 0);

            NCL_CHECK(row != NULL && !ncl_check_is_code_ok(row->code));
            ncl_message_free(response);
        }
    }

    NCL_TEST_CASE("11.7: a LIST answers get_length (and not get_keys)");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        ncl_query_request_item *item =
            ncl_query_request_item_new("/MACHINE/CONTROLLER/TOOL");
        ncl_message *response;

        NCL_CHECK(request != NULL && item != NULL);
        (void)ncl_params_set_string(&item->params, "operation", "get_length");
        (void)ncl_message_set_message_id(request, "q5");
        (void)ncl_message_add_query_request_item(request, item);
        response = ncl_server_invoke_query(ncl_host_server(host), request);
        ncl_message_free(request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *row = ncl_ptrvec_at(&response->as.query_response.items, 0);
            long long length = -1;

            NCL_CHECK(row != NULL && ncl_check_is_code_ok(row->code));
            NCL_CHECK(row->values != NULL &&
                      ncl_json_as_int(ncl_json_arr_get(row->values, 0), &length));
            NCL_CHECK_EQ_INT(length, 2); /* mock 摆了两把刀 */
            ncl_message_free(response);
        }

        NCL_TEST_CASE("11.7: the same LIST does not answer get_keys");
        request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        item = ncl_query_request_item_new("/MACHINE/CONTROLLER/TOOL");
        NCL_CHECK(request != NULL && item != NULL);
        (void)ncl_params_set_string(&item->params, "operation", "get_keys");
        (void)ncl_message_set_message_id(request, "q5b");
        (void)ncl_message_add_query_request_item(request, item);
        response = ncl_server_invoke_query(ncl_host_server(host), request);
        ncl_message_free(request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *row = ncl_ptrvec_at(&response->as.query_response.items, 0);

            NCL_CHECK(row != NULL && !ncl_check_is_code_ok(row->code));
            ncl_message_free(response);
        }
    }

    NCL_TEST_CASE("11.7: a Set writes the tool (permissions live outside)");
    {
        ncl_message *write = ncl_message_new(NCL_MSG_SET_REQUEST);
        ncl_set_request_item *item =
            ncl_set_request_item_new("/MACHINE/CONTROLLER/TOOL");
        ncl_message *response;
        ncl_json *value;

        NCL_CHECK(write != NULL && item != NULL);
        /* 只给要改的字段：适配器先读回整条打底，别的字段不会被动。 */
        value = ncl_json_new_object();
        NCL_CHECK(value != NULL);
        (void)ncl_json_obj_set_double(value, "radius_wear", 0.02);
        (void)ncl_json_obj_set_int(value, "kind", 5);
        (void)ncl_params_set_string(&item->params, "operation", "set_value");
        (void)ncl_params_set_string(&item->params, "keys", "1");
        (void)ncl_json_obj_set(item->params, "value", value);
        (void)ncl_message_set_message_id(write, "s2");
        (void)ncl_message_add_set_request_item(write, item);
        mock->put_hr = 0;
        response = ncl_server_invoke_set(ncl_host_server(host), write);
        ncl_message_free(write);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_set_response_item *row =
                ncl_ptrvec_at(&response->as.set_response.items, 0);

            NCL_CHECK(row != NULL && ncl_check_is_code_ok(row->code));
            ncl_message_free(response);
        }
        NCL_CHECK_EQ_INT(mock->last_tool_put, 1u);
        NCL_CHECK_EQ_INT(mock->tool_put_in_len, NCL_SYNTEC_TOOL_IN);
        /* 改的两个字段进去了，没提的半径还是 mock 里原来那个 0.8 */
        NCL_CHECK_EQ_INT(mock->tools[1].tool_nose, 5);
        NCL_CHECK(mock->tools[1].radius_wear == 0.02);
        NCL_CHECK(mock->tools[1].radius_geometry == 0.8);
    }

    NCL_TEST_CASE("11.7: a Set refuses a field the tool does not have");
    {
        ncl_message *write = ncl_message_new(NCL_MSG_SET_REQUEST);
        ncl_set_request_item *item =
            ncl_set_request_item_new("/MACHINE/CONTROLLER/TOOL");
        ncl_message *response;
        ncl_json *value;

        NCL_CHECK(write != NULL && item != NULL);
        value = ncl_json_new_object();
        NCL_CHECK(value != NULL);
        (void)ncl_json_obj_set_double(value, "radiuswear", 0.02); /* 少个下划线 */
        (void)ncl_params_set_string(&item->params, "operation", "set_value");
        (void)ncl_params_set_string(&item->params, "keys", "1");
        (void)ncl_json_obj_set(item->params, "value", value);
        (void)ncl_message_set_message_id(write, "s3");
        (void)ncl_message_add_set_request_item(write, item);
        response = ncl_server_invoke_set(ncl_host_server(host), write);
        ncl_message_free(write);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_set_response_item *row =
                ncl_ptrvec_at(&response->as.set_response.items, 0);

            NCL_CHECK(row != NULL && !ncl_check_is_code_ok(row->code));
            ncl_message_free(response);
        }
    }

    NCL_TEST_CASE("11.4: a key that is not in the table is refused");
    {
        ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
        ncl_query_request_item *item =
            ncl_query_request_item_new("/MACHINE/CONTROLLER/PARAMETER");
        ncl_message *response;

        NCL_CHECK(request != NULL && item != NULL);
        (void)ncl_params_set_string(&item->params, "operation", "get_attributes");
        (void)ncl_params_set_string(&item->params, "keys", "999");
        (void)ncl_message_set_message_id(request, "q3");
        (void)ncl_message_add_query_request_item(request, item);
        response = ncl_server_invoke_query(ncl_host_server(host), request);
        ncl_message_free(request);
        NCL_CHECK(response != NULL);
        if (response != NULL) {
            ncl_query_response_item *row = ncl_ptrvec_at(&response->as.query_response.items, 0);

            NCL_CHECK(row != NULL && !ncl_check_is_code_ok(row->code));
            ncl_message_free(response);
        }
    }


    ncl_host_free(host);
    ncl_strbuf_free(&err);
    ncl_modules_free(modules);
    mock_stop(mock);
}

NCL_TEST_MAIN_BEGIN()
    test_read();
    test_through_the_manager();
    test_items();
    test_params();
    test_param_table();
    test_adapter();
NCL_TEST_MAIN_END()
