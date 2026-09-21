/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - FANUC FOCAS PDU layer (see ncl_focas.h).
 *
 * Every field on this wire is a big endian u16 (or a pair of them), which is
 * what `Pdu::receive` byte swaps before comparing against its literals.
 */

#include "focas/ncl_focas_pdu.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void put_u16be(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void put_u32be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint16_t get_u16be(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8) | in[1]);
}

static uint32_t get_u32be(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | in[3];
}

/* ============================================================== framing == */

size_t ncl_focas_build(uint8_t *out, size_t cap, uint8_t func, uint8_t dir,
                       const void *body, size_t body_len)
{
    size_t total = NCL_FOCAS_HEADER + body_len;

    if (out == NULL || cap < total || body_len > 0xFFFFu ||
        (body_len > 0 && body == NULL)) {
        return 0;
    }
    out[0] = NCL_FOCAS_MAGIC0;
    out[1] = NCL_FOCAS_MAGIC0;
    out[2] = NCL_FOCAS_MAGIC0;
    out[3] = NCL_FOCAS_MAGIC0;
    put_u16be(out + 4, NCL_FOCAS_TYPE_V1); /* "request always 0001" (§2.2) */
    out[6] = func;
    out[7] = dir;
    put_u16be(out + 8, (uint16_t)body_len);
    if (body_len > 0) {
        memcpy(out + NCL_FOCAS_HEADER, body, body_len);
    }
    return total;
}

ncl_err ncl_focas_split(const uint8_t *frame, size_t len, ncl_focas_pdu *out,
                        size_t *frame_len)
{
    size_t total;

    if (frame == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (len < NCL_FOCAS_HEADER) {
        return NCL_ERR_RANGE; /* the header is still arriving */
    }
    /* `Pdu::receive` #23fe0 swaps and compares against a0 a0 a0 a0. */
    if (frame[0] != NCL_FOCAS_MAGIC0 || frame[1] != NCL_FOCAS_MAGIC0 ||
        frame[2] != NCL_FOCAS_MAGIC0 || frame[3] != NCL_FOCAS_MAGIC0) {
        return NCL_FOCAS_ERR_MAGIC;
    }
    /* #25230: the direction must be 1..4. */
    if (frame[7] < 1u || frame[7] > 4u) {
        return NCL_FOCAS_ERR_HEADER;
    }
    memset(out, 0, sizeof(*out));
    out->type = get_u16be(frame + 4);
    out->func = frame[6];
    out->dir = frame[7];
    out->length = get_u16be(frame + 8);
    total = NCL_FOCAS_HEADER + out->length;
    if (frame_len != NULL) {
        *frame_len = total; /* the caller reads this many bytes in total */
    }
    if (len < total) {
        return NCL_ERR_RANGE;
    }
    return NCL_OK;
}

ncl_err ncl_focas_hello_reply(const uint8_t *body, size_t body_len,
                              size_t *records)
{
    size_t n;

    if (body == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (body_len < 16u) {
        return NCL_FOCAS_ERR_LENGTH;
    }
    n = get_u16be(body + 8);
    /* §2.2 rule 5: the body length must be exactly 16 + 8n. */
    if (body_len != 16u + 8u * n) {
        return NCL_FOCAS_ERR_LENGTH;
    }
    if (records != NULL) {
        *records = n;
    }
    return NCL_OK;
}

uint16_t ncl_focas_hello_field(const uint8_t *body, size_t body_len,
                               size_t offset)
{
    if (body == NULL || offset + 2u > body_len) {
        return 0;
    }
    return get_u16be(body + offset);
}

/* ========================================================= command block == */

void ncl_focas_cb_init(ncl_focas_cb *cb, uint16_t code)
{
    if (cb == NULL) {
        return;
    }
    memset(cb, 0, sizeof(*cb));
    cb->first = 1;
    cb->index = 1;
    cb->code = code;
}

size_t ncl_focas_cb_write(uint8_t *out, size_t cap, const ncl_focas_cb *cb)
{
    if (out == NULL || cb == NULL || cap < NCL_FOCAS_CB_SIZE) {
        return 0;
    }
    put_u16be(out + 0, NCL_FOCAS_CB_SIZE); /* the block's own size first */
    put_u16be(out + 2, cb->first);
    put_u16be(out + 4, cb->index);
    put_u16be(out + 6, cb->code);
    put_u32be(out + 8, cb->arg0);
    put_u32be(out + 12, cb->arg1);
    put_u32be(out + 16, cb->arg2);
    put_u32be(out + 20, cb->arg3);
    put_u16be(out + 24, cb->tag0);
    put_u16be(out + 26, cb->tag1);
    return NCL_FOCAS_CB_SIZE;
}

size_t ncl_focas_body_begin(uint8_t *out, size_t cap)
{
    if (out == NULL || cap < 2u) {
        return 0;
    }
    put_u16be(out, 0); /* the count, rewritten by every add() */

    return 2u;
}

size_t ncl_focas_body_add(uint8_t *out, size_t cap, size_t used,
                          const ncl_focas_cb *cb)
{
    size_t next = used + NCL_FOCAS_CB_SIZE;

    if (out == NULL || cb == NULL || used < 2u || cap < next) {
        return 0;
    }
    (void)ncl_focas_cb_write(out + used, cap - used, cb);
    put_u16be(out, (uint16_t)((next - 2u) / NCL_FOCAS_CB_SIZE));
    return next;
}

/* ========================================================= reply blocks == */

size_t ncl_focas_block_count(const uint8_t *body, size_t body_len)
{
    if (body == NULL || body_len < 2u) {
        return 0;
    }
    return get_u16be(body);
}

ncl_err ncl_focas_block_at(const uint8_t *body, size_t body_len, size_t index,
                           const uint8_t **block, size_t *block_len)
{
    size_t count;
    size_t offset;
    size_t i;
    size_t size;

    if (body == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (body_len < 2u) {
        return NCL_FOCAS_ERR_LENGTH;
    }
    count = get_u16be(body);
    /* `Pdu::getRbPos`: `count <= index` throws, and that is the error a
     * one-block-short reply produced (§2.3 rule 1). */
    if (index >= count) {
        return count == 0 ? NCL_FOCAS_ERR_RB_COUNT : NCL_FOCAS_ERR_RB_MISSING;
    }
    offset = 2u;
    for (i = 0; i < index; i++) {
        if (offset + 2u > body_len) {
            return NCL_FOCAS_ERR_LENGTH;
        }
        offset += get_u16be(body + offset); /* the block's own size field */
        if (offset > body_len) {
            return NCL_FOCAS_ERR_LENGTH;
        }
    }
    if (offset + 2u > body_len) {
        return NCL_FOCAS_ERR_LENGTH;
    }
    size = get_u16be(body + offset);
    if (size < 10u || offset + size > body_len) {
        return NCL_FOCAS_ERR_LENGTH;
    }
    if (block != NULL) {
        *block = body + offset;
    }
    if (block_len != NULL) {
        *block_len = size;
    }
    return NCL_OK;
}

int ncl_focas_block_code(const uint8_t *block, size_t block_len)
{
    if (block == NULL || block_len < 10u) {
        return 0;
    }
    return (int)(int16_t)get_u16be(block + 8);
}

uint16_t ncl_focas_block_payload_len(const uint8_t *block, size_t block_len)
{
    if (block == NULL || block_len < 16u) {
        return 0;
    }
    return get_u16be(block + 14);
}

const uint8_t *ncl_focas_block_payload(const uint8_t *block, size_t block_len,
                                       size_t *len)
{
    if (block == NULL || block_len < 16u) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    if (len != NULL) {
        *len = block_len - 16u;
    }
    return block + 16;
}

ncl_err ncl_focas_check_blocks(const uint8_t *body, size_t body_len,
                               size_t *index_out)
{
    size_t count = ncl_focas_block_count(body, body_len);
    size_t i;

    if (count == 0) {
        return NCL_FOCAS_ERR_RB_COUNT;
    }
    for (i = 0; i < count; i++) {
        const uint8_t *block = NULL;
        size_t block_len = 0;
        ncl_err err = ncl_focas_block_at(body, body_len, i, &block, &block_len);

        if (err != NCL_OK) {
            return err;
        }
        if (ncl_focas_block_code(block, block_len) != 0) {
            if (index_out != NULL) {
                *index_out = i;
            }
            return NCL_FOCAS_ERR_RB_CODE; /* `Pdu::getRb` throws here */
        }
    }
    return NCL_OK;
}

/* ================================================================ items == */

/*
 * FOCAS 数据项表：`code` 是命令块的 `c` 字段（item 码），`arg0`/`arg1` 是随它
 * 一起发的 `d`/`e` 两个 long，`scalar` 说每个块是"载荷偏移 0 处一个值"还是
 * "块 0 的载荷就是整个数组"。
 *
 * 码从哪来：§2.3 那批（假机床 + 报文对照），以及 2026-09 用 FANUC 官方 SDK
 * （Fwlib64.dll + 官方手册/文档包）**一条调用一条调用地问出来**的（工具见
 * tools/site-probe/focas_sdk_probe.*：既印 SDK 发出的 Cb 码，也用"可辨识载荷"
 * 反推应答布局）。新增的几条：
 *
 *   RDPRG     0x1c  cnc_rdprgnum   载荷 @2 = 运行程序号、@6 = 主程序号（都是 BE16）
 *   RDSEQ     0x1d  cnc_rdseqnum   载荷 @0 = 顺序号（BE32）
 *   RDALM     0x1a  cnc_alarm2     载荷 @0 = 报警状态位（BE32，0 = 无报警）
 *   RDNGROUP  0x4a  cnc_rdngrp     载荷 @0 = 刀具组数（BE32）
 *   RDTIMER   0x120 cnc_rdtimer    载荷 @0 = 分钟、@4 = 毫秒（都是 BE32）
 *
 * `cnc_statinfo` 是特例：三个块，块 1 / 块 2 各是结构体的一个 short，块 0 的
 * 载荷是剩下的数组（§2.3 的 ODBST 切法）。
 *
 * 修正（同一轮核出来的）：`cnc_rdcount` 的 d/e 是 **0/0**、`cnc_rdlife` 才是 1/1
 * —— 原来两条都写 1/1，件数读的其实是寿命那一支；`cnc_rdblkcount` 的码是 **0x35**
 * （不是 0x06，0x06 是程序目录那一族）。
 */
static const ncl_focas_item kItems[] = {
    /* name          cbs                  arg0            arg1        n scalar */
    { "STATINFO",   { 25, 225, 152 },    { 0, 0, 0 },      { 0, 0, 0 },      3, true },
    { "ACTF",       { 0x24, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    { "ACTS",       { 0x25, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    { "RDCOUNT",    { 0x8b, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    { "RDLIFE",     { 0x8b, 0, 0 },      { 1, 0, 0 },      { 1, 0, 0 },      1, false },
    { "RDPRG",      { 0x1c, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    { "RDSEQ",      { 0x1d, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    { "RDALM",      { 0x1a, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    { "RDNGROUP",   { 0x4a, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    /* cnc_rdtimer 的 type 走 Cb 的 d：0 通电 / 1 运行 / 2 切削 / 3 循环 / 4 自由 */
    { "RDTIMER",    { 0x120, 0, 0 },     { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    { "RDTIMER1",   { 0x120, 0, 0 },     { 1, 0, 0 },      { 0, 0, 0 },      1, false },
    { "RDTIMER2",   { 0x120, 0, 0 },     { 2, 0, 0 },      { 0, 0, 0 },      1, false },
    { "RDTIMER3",   { 0x120, 0, 0 },     { 3, 0, 0 },      { 0, 0, 0 },      1, false },
    { "RDTIMER4",   { 0x120, 0, 0 },     { 4, 0, 0 },      { 0, 0, 0 },      1, false },
    /*
     * 坐标：官方 SDK 的 `cnc_rdposition` 一条请求带 **9 个块**（§2.5 实测）——
     * `0x19` 框住两头、中间四个 `0x26` 就是四种位置（d = 0 绝对 / 1 机械 / 2 相对 /
     * 3 剩余），再跟 `0x89`/`0x0e`/`0x88` 三条轴信息。**应答块与 Cb 一一对应**
     * （§2.3 的约定，statinfo 就是这么对的），所以第 2 个块（下标 1）就是绝对位置
     * 那个数组，每个轴一个 `POSELM`（12 字节：int32 data + dec/unit/disp + 轴名）。
     * 位置值 = `data / 10^dec`（NCGuide 上实测到 `dec=3`、轴名 'X'）。
     */
    { "RDPOSITION", { 0x19, 0x26, 0x26, 0x26, 0x26, 0x89, 0x0e, 0x88, 0x19, 0, 0, 0 },
                    { 0, 0, 1, 2, 3, 0xffffffff, 0xc2b, 2, 0, 0, 0, 0 },
                    { 0, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0, 0xc2b, 0, 0, 0, 0, 0 },
                    9, false },
    { "RDMACRO",    { 0x15, 0, 0 },      { 1, 0, 0 },      { 1, 0, 0 },      1, false },
    { "RDPARAM",    { 0x0e, 0, 0 },      { 1, 0, 0 },      { 1, 0, 0 },      1, false },
    { "RDTOFS",     { 0x08, 0, 0 },      { 1, 0, 0 },      { 1, 0, 0 },      1, false },
    { "RDPROGDIR3", { 0x06, 0, 0 },      { 0x13, 0, 0 },   { 1, 0, 0 },      1, false },
    { "EXEPRGNAME2",{ 0xfc, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    /* the capability block the session negotiation sends (§2.3) */
    { "VERSION",    { 0x0e, 0, 0 },      { 0x26f0, 0, 0 }, { 0x26f0, 0, 0 }, 1, false },
    { "RDBLKCOUNT", { 0x35, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    /* 下面这些**码已核、应答布局还没核**（要么值不在载荷 0 处，要么是结构体数组）：
     * 表里先记着码，语义层暂时按 NCL_ERR_UNAVAILABLE 回，等真机抓一次再启用。
     *   ABSOLUTE/MACHINE/RELATIVE/DISTANCE  0x26，d = 位置类型，e = ALL_AXES
     *   RDPOSITION                          0x26 的四种类型各发一条（d = 0..3）
     *   RDSVLOAD                            0x56 + 0x89，主轴/伺服负载
     *   RDSPLOAD                            0x40（d=4 负载 / d=5 转速）+ 0x8a
     *   RDALMMSG2                           0x23，报警消息（d = 类型，e = 条数）
     *   RDOPMODE                            0x57，主轴调整模式
     *   RDaxisdata / rdexecprog / rdgcode / rdwkcdshft 也都在这一档
     */
};

#define NCL_FOCAS_ITEM_COUNT (sizeof(kItems) / sizeof(kItems[0]))

/** Parse "36", "0x24" or "CB:0x24" into a code. */
bool ncl_focas_parse_code(const char *text, uint16_t *code)
{
    unsigned long value = 0;

    if (text == NULL || code == NULL) {
        return false;
    }
    if (strncmp(text, "CB:", 3) == 0 || strncmp(text, "cb:", 3) == 0) {
        text += 3;
    }
    if (text[0] == '\0') {
        return false;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        if (sscanf(text + 2, "%lx", &value) != 1) {
            return false;
        }
    } else if (sscanf(text, "%lu", &value) != 1) {
        return false;
    }
    if (value > 0xFFFFu) {
        return false;
    }
    *code = (uint16_t)value;
    return true;
}

const ncl_focas_item *ncl_focas_item_lookup(const char *name)
{
    size_t i;

    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (i = 0; i < NCL_FOCAS_ITEM_COUNT; i++) {
        if (ncl_streq_ignore_case(name, kItems[i].name)) {
            return &kItems[i];
        }
    }
    return NULL;
}

const char *ncl_focas_code_name(uint16_t code)
{
    size_t i;
    size_t k;

    for (i = 0; i < NCL_FOCAS_ITEM_COUNT; i++) {
        for (k = 0; k < kItems[i].cb_count; k++) {
            if (kItems[i].cbs[k] == code) {
                return kItems[i].name;
            }
        }
    }
    return NULL;
}

/* =============================================================== values == */

ncl_err ncl_focas_decode(const uint8_t *data, size_t len, ncl_dtype dtype,
                         size_t count, ncl_json **values)
{
    size_t width;
    size_t i;
    ncl_json *array;

    if (values == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    switch (dtype) {
    case NCL_DTYPE_BIT:
    case NCL_DTYPE_BYTE:
        width = 1;
        break;
    case NCL_DTYPE_INT16:
        width = 2;
        break;
    case NCL_DTYPE_INT32:
    case NCL_DTYPE_FLOAT32:
        width = 4;
        break;
    case NCL_DTYPE_FLOAT64:
        width = 8;
        break;
    case NCL_DTYPE_STRING:
        width = 1;
        break;
    default:
        return NCL_ERR_INVALID_DATA_TYPE;
    }
    if (count == 0 || data == NULL || len < width * count) {
        return NCL_ERR_RANGE;
    }
    if (dtype == NCL_DTYPE_STRING) {
        /* count is the point's own length - the reply may carry more (the
         * cnc_exeprgname2 reply is name[36] plus two longs). A FOCAS character
         * array is NUL terminated and space padded, so the value ends at the
         * first NUL and the trailing spaces after it are padding. */
        const char *nul;
        size_t used = count;

        nul = (const char *)memchr(data, '\0', used);
        if (nul != NULL) {
            used = (size_t)(nul - (const char *)data);
        }
        while (used > 0 && data[used - 1] == ' ') {
            used--;
        }
        *values = ncl_json_new_string_len((const char *)data, used);
        return *values != NULL ? NCL_OK : NCL_ERR_NOMEM;
    }
    if (count == 1) {
        ncl_json *value = NULL;

        switch (dtype) {
        case NCL_DTYPE_BIT:
            value = ncl_json_new_bool(data[0] != 0);
            break;
        case NCL_DTYPE_BYTE:
            value = ncl_json_new_int((long long)data[0]);
            break;
        case NCL_DTYPE_INT16:
            value = ncl_json_new_int((long long)(int16_t)get_u16be(data));
            break;
        case NCL_DTYPE_INT32:
            value = ncl_json_new_int((long long)(int32_t)get_u32be(data));
            break;
        case NCL_DTYPE_FLOAT32: {
            uint32_t bits = get_u32be(data);
            float number = 0.0f;

            memcpy(&number, &bits, sizeof(number));
            value = ncl_json_new_double((double)number);
            break;
        }
        case NCL_DTYPE_FLOAT64: {
            uint64_t bits;
            double number = 0.0;

            bits = ((uint64_t)get_u32be(data) << 32) | get_u32be(data + 4);
            memcpy(&number, &bits, sizeof(number));
            value = ncl_json_new_double(number);
            break;
        }
        default:
            return NCL_ERR_INVALID_DATA_TYPE;
        }
        if (value == NULL) {
            return NCL_ERR_NOMEM;
        }
        *values = value;
        return NCL_OK;
    }
    array = ncl_json_new_array();
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < count; i++) {
        ncl_json *scalar = NULL;

        if (ncl_focas_decode(data + i * width, width, dtype, 1, &scalar) != NCL_OK) {
            ncl_json_free(array);
            return NCL_ERR_NOMEM;
        }
        if (ncl_json_arr_push(array, scalar) != NCL_OK) {
            ncl_json_free(array);
            return NCL_ERR_NOMEM;
        }
    }
    *values = array;
    return NCL_OK;
}
