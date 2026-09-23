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
    /*
     * 2026-09 真机（0i-MD，01 册 §2.3）：应答体 360 字节，而 [8..10) 写的是 8 ——
     * "体长必须是 16 + 8n" 那条等式对不上，可官方 SDK 收下它并且 rc=0。所以这里
     * 只要求"像一条握手应答"（至少 16 字节的块头），n 只当"机床自己怎么数"上报。
     */
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

size_t ncl_focas_body_add_payload(uint8_t *out, size_t cap, size_t used,
                                 const uint8_t *data, size_t len)
{
    size_t block_at;

    if (out == NULL || data == NULL || len == 0u ||
        used < 2u + NCL_FOCAS_CB_SIZE) {
        return 0; /* 至少得有一个块在前面 */
    }
    if (cap < used + len || used + len > 0xFFFFu) {
        return 0;
    }
    block_at = used - NCL_FOCAS_CB_SIZE; /* 载荷是挂在**最后一个块**后面的 */
    if (get_u16be(out + block_at) != NCL_FOCAS_CB_SIZE) {
        return 0; /* 那个块已经带过载荷了，或者根本不是我们写的块 */
    }
    memcpy(out + used, data, len);
    put_u16be(out + block_at, (uint16_t)(NCL_FOCAS_CB_SIZE + len));
    /* **块尾那两格（tag1，[26..28)）就是载荷长度**（字节数）。
     *
     * 2026-09 逐字节对过官方 SDK 的帧：写刀补那条块长 0x24、`tag0` = 0、
     * **`tag1` = 0x0008**（载荷 8 字节）。上一轮把长度写进 `tag0` 正是被机床回
     * EW_LENGTH=2 的原因（01 册 §11.13）—— 对，差的就是这一格。应答块这一格
     * 的位置也对得上（应答块头 16 字节、长度在 [14..16)，同样是"块尾"）。
     */
    put_u16be(out + block_at + 26u, (uint16_t)len);
    return used + len;
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
            int code = ncl_focas_block_code(block, block_len);

            if (index_out != NULL) {
                *index_out = i;
            }
            /*
             * 机床自己的返回码里有两个**不是"调用出错"，而是"这台机器没有"**：
             *
             *   EW_FUNC  = 1  这条功能这个机型不支持（`cnc_rdtooldata` 在这台
             *                 0i-MF 上就是 1）
             *   EW_NOOPT = 6  这个选件没开（刀具寿命管理 `cnc_rdlife`、用户宏变量
             *                 `cnc_rdmacro` 在这台模拟器上都是 6）
             *
             * 2026-09 实测（01 册 §11.13）：这两条以前一路冒到上层成了"模块错"
             * （`NCL_FOCAS_ERR_RB_CODE`），点位表里看着像 client 坏了。这里直接翻成
             * `NCL_ERR_UNAVAILABLE`（"声明了、但这台机床读不到"），站点一眼就知道
             * 该关掉这一格。
             */
            if (code == 1 || code == 6) {
                return NCL_ERR_UNAVAILABLE;
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
     * 坐标：`cnc_rdposition` 一条请求带 **8 个块**（§2.8 真机实测）——
     * `0x19` 框住两头、中间四个 `0x26` 就是四种位置（d = 0 绝对 / 1 机械 / 2 相对 /
     * 3 剩余），再跟 `0x89`（轴表）/`0x88` 两条轴信息。**应答块与 Cb 一一对应**
     * （§2.3 的约定），所以下标 1..4 就是那四种位置，每个轴一个 `POSELM`
     * （12 字节：int32 data + dec/unit/disp + 轴名）。位置值 = `data / 10^dec`。
     *
     * **原来这里是 9 个块**，中间多一条 `0x0e` + `d=e=0x26f0`（"能力块"）—— 那是
     * 照假机床/官方库对假机床的行为定的：真机（0i-MD）**只拒这一条**（块返回码 1，
     * 上层看到 `NCL_FOCAS_ERR_RB_CODE`），去掉之后八个块全 rc=0（同轮实测，见 §2.8）。
     * 官方库对着这台机器发的是另一套 7 块帧（`0xa4`/`0x89`/`0x88`×2/`0xa3`/`0x26`/
     * `0xa4`），同样不带 `0x0e` —— 两套帧都说明"这条别发"。
     */
    { "RDPOSITION", { 0x19, 0x26, 0x26, 0x26, 0x26, 0x89, 0x88, 0x19, 0, 0, 0, 0 },
                    { 0, 0, 1, 2, 3, 0xffffffff, 2, 0, 0, 0, 0, 0 },
                    { 0, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0, 0, 0, 0, 0, 0, 0 },
                    8, false },
    /*
     * 宏变量（`cnc_rdmacro`，0x15）：d = 变量号、e = 1（官方 SDK 对这台机器就是
     * `d = 变量号, e = 1`）。应答 8 字节 = 上面那种记录形状里的前 8 字节
     * （data@0、dec@6），真机实测：`00 00 00 00 00 0a ff ff` → 值 0、dec 那格是
     * 0xffff（=> 按 0 算）。
     */
    { "RDMACRO",    { 0x15, 0, 0 },      { 1, 0, 0 },      { 1, 0, 0 },      1, false },
    /*
     * CNC 参数（`cnc_rdparam`）：**码是 0x8d，不是 0x0e**（真机实测：SDK 发的是
     * `0x8d`、`d = 参数号`、`e = 1`；0x0e 那条在这台机器上被拒 rc=1）。
     * 应答 264 字节，最前面 4 字节（BE32）就是参数值（参数 1 → 1）。
     */
    { "RDPARAM",    { 0x8d, 0, 0 },      { 1, 0, 0 },      { 1, 0, 0 },      1, false },
    /*
     * 刀补（`cnc_rdtofs`，0x08）：d = 刀补号、e = 1，**`arg2 = 1000`**（SDK 给的
     * 这一格真机上也确实带着）。应答 8 字节：data@0、dec@6（真机 `…00 0a 00 03` →
     * dec = 3，即 0.000 mm）。
     */
    { "RDTOFS",     { 0x08, 0, 0 },      { 1, 0, 0 },      { 1, 0, 0 },      1, false,
                     { 1000, 0, 0 },     { 0, 0, 0 } },
    /*
     * 写刀补（`cnc_wrtofs`）= **0x09**（读 0x08 + 1），一帧就够：
     *
     *     Cb       code 0x09、d = 刀补号、e = 1、arg2 = 1000 + 刀补类型、tag0/tag1 = 0
     *     载荷     8 字节 = BE32 值（0.001mm 为单位）+ BE16 0、BE16 0xffff
     *     块长度格 [0..2) = 0x1c + 8 = 0x24（写这一侧载荷是挂在块后面的，§11.13）
     *
     * 2026-09 对 NCGuide 0i-MF Plus 实测：写 0x3333 到 1 号刀补（type 1）→ 机床回
     * 块返回码 0；再读 1 号刀补 → `00003333 000a 0003`（13.107mm），**写得进**。
     * 上一轮猜的 0x16/0x8e 两条是"读码 + 1"，这一条同样是"读码 + 1"，但决定成败的
     * 是载荷形状与块长度，不是码本身。
     */
    { "WRTOFS",     { 0x09, 0, 0 },      { 1, 0, 0 },      { 1, 0, 0 },      1, false,
                     { 1000, 0, 0 },     { 0, 0, 0 } },
    /*
     * 刀补表信息（`cnc_rdtofsinfo`）：一个 **0x0a**，应答载荷 8 字节
     * —— 本机回 `0000 0190 0002 0000` → `use_no` = 0x0190 = **400**（这台机床有 400
     * 个刀补号）。刀具列表就是靠它定"读到第几号"（cnc_rdtooldata 这台机器回
     * EW_FUNC=1，不给，见 §11.13）。
     */
    { "RDTOFSINFO", { 0x0a, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    /*
     * 负载扭矩（`cnc_loadtorq`）：一个 **0xfd**，d = 电机号（0 = 伺服）、e = 轴号
     * （**1 起**：X=1）。应答载荷 4 字节。本机静止时恒 0，且 d/e 超出机床范围会回
     * EW_RANGE（试过 d=3/e=7）—— 值在载荷里，本机带不动载，**定标未核**（§11.13）。
     */
    { "TORQUE",     { 0xfd, 0, 0 },      { 0, 0, 0 },      { 1, 0, 0 },      1, false },
    /*
     * 删程序（`cnc_delete`）= 一个 **0x05**，d = 程序号（O 后面的那个数）。本机
     * （模拟器）回 EW_ATTRIB=5，即"机床不收这条"，帧按官方 SDK 抄的（§11.13）。
     */
    { "DELPROG",    { 0x05, 0, 0 },      { 1, 0, 0 },      { 0, 0, 0 },      1, false },
    /*
     * 写 CNC 参数（`cnc_wrparam`）。官方 SDK 对着这台机器发的是 **0xa0**、
     * d = 参数号、e = 1、**不带载荷**（体就是 0x1c），机床回块返回码 0 而 SDK 自己
     * 报 EW_LENGTH —— 说明值没送出去。这一条按写刀补那个形状补载荷试
     * （BE32 值 + 0000 + ffff），**成不成由机床说了算**（见 §11.13 的记录：
     * 收下了就是这一条，不收就退回"机床不提供"）。
     */
    { "WRPARAM",    { 0xa0, 0, 0 },      { 1, 0, 0 },      { 1, 0, 0 },      1, false },
    /*
     * 写宏变量（`cnc_wrmacro`）= **0x16**（读 0x15 + 1），载荷 8 字节，形状与刀补
     * 那条一致。这台模拟器没开用户宏变量（读 0x15 回 EW_NOOPT=6），所以只核到"帧
     * 能发出去、机床怎么答"，值没写进去（§11.13 写实情）。
     */
    { "WRMACRO",    { 0x16, 0, 0 },      { 1, 0, 0 },      { 1, 0, 0 },      1, false },
    /*
     * 程序目录（`cnc_rdprogdir3`）：**码 0x06、`d` = 0、`e` = 8（一次要几条）、
     * `arg2` = 1**（官方 SDK 对这台机器发的就是这个形状；原来写 `d = 0x13` 是照
     * 假机床定的）。应答是 72 字节一条的记录，一条一个程序（§2.8.4）。
     */
    { "RDPROGDIR3", { 0x06, 0, 0 },      { 0, 0, 0 },      { 8, 0, 0 },      1, false,
                     { 1, 0, 0 },        { 0, 0, 0 } },
    { "EXEPRGNAME2",{ 0xfc, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    /*
     * 伺服延迟量（现场口径就是**跟踪误差**）：官方 SDK 的 `cnc_srvdelay` 只发**一条**
     * 块——`0x26`，d = 9，e = ALL_AXES（假机床实测的请求帧，见 01 册 §2.4）。d = 0..3
     * 是四种位置（RDPOSITION 那一条），d = 9 才是延迟量，所以这里不能复用 RDPOSITION。
     * 应答每轴一条 **8 字节记录**，值在记录第 0 个 int32（大端）——依据是官方库
     * `fwlibNCG.dll` 里 `cnc_srvdelay` 那一层：每轴步长 `eax*8`、值取记录第 0 个 dword
     * 再写进 `ODBAXIS.data[i]`（§2.5 反汇编）。
     */
    { "SV_DELAY",   { 0x26, 0, 0 },      { 9, 0, 0 },      { 0xffffffff, 0, 0 }, 1, false },
    /*
     * ODBSYS（= `cnc_sysinfo`）：会话探针那条 `code 24` 的应答载荷，18 字节
     * （addinfo / max_axis / cnc_type / mt_type / series / version / axes）。
     * 2026-09 真机实测：`code 24` 拿到它（rc=0），而下面那条 `0x0e` 被机床拒
     * （rc=1）—— 官方库的 `cnc_sysinfo` 读的也是这一条（见 01 册 §2.3）。
     */
    { "ODBSYS",     { NCL_FOCAS_CODE_SYSINFO, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, 1, false },
    /*
     * 连接期的"能力块"：FS0i 那版 `libfwlib32.so` 反汇编里看到的是 `0x0e` +
     * `d=e=0x26f0`（§2.3 的 step 3）。真机（0i-MD）上这条回 rc=1，所以它只当
     * ODBSYS 的**退路**（见 focas_values.c 的 odbsys_payload）。
     */
    { "VERSION",    { 0x0e, 0, 0 },      { 0x26f0, 0, 0 }, { 0x26f0, 0, 0 }, 1, false },
    { "RDBLKCOUNT", { 0x35, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    /*
     * 操作面板信号（`cnc_rdopnlsgnl`，官方文档 Misc/cnc_rdopnlsgnl.xml）：Cb 码
     * **0x5d**，`d` 是"读哪几路信号"的位掩码（bit 5 = 进给倍率、bit 3 = 快移倍率、
     * bit 6 = 主轴倍率但**只有 15i 有**、bit 7..12 = 单段/机床锁/空运行/记忆保护/
     * 暂停…），`e` = 0。应答载荷就是从 **@0 开始的一串 BE16**，顺序与 `IODBSGNL`
     * 一致（`mode`@0、`hndl_ax`@2、`hndl_mv`@4、`rpd_ovrd`@6、`jog_ovrd`@8、
     * **`feed_ovrd`@0xa**、`spdl_ovrd`@0xc、`blck_del`@0xe、…）。
     *
     * 这里 `d` 给 `0xffff`（"全都要"）：位掩码只决定机床回哪几路，回来的仍是整个
     * 结构体，偏移才站得住（官方 SDK 自己发的是 0）。倍率的**码值→百分比**换算在
     * 文档里写死了：`feed_ovrd` 的 0..20 就是 0%..200%，每级 10%（见 focas_values.c
     * 里的 `ncl_focas_feed_override`）。
     */
    { "RDSGNL",     { 0x5d, 0, 0 },      { 0xffff, 0, 0 }, { 0, 0, 0 },      1, false },
    /*
     * 正在执行的程序段（`cnc_rdexecprog`）：一个 `0x20`，**`d` = 要多少字节的文本**
     * （官方 SDK 给 0x594 = 1428，真机回 504 字节）。应答体 = 4 字节 + 文本
     * （ASCII、NUL/0 补齐），真机实测：`M98P3001\n\nG49\n\nT01\nD1\nG0G43H1Z100.\nM…`
     */
    { "EXECPROG",   { 0x20, 0, 0 },      { 0x594, 0, 0 },  { 0, 0, 0 },      1, false },
    /*
     * 模态 G 码（`cnc_rdgcode`）：一个 `0x96`，**`d` = 第几组**（0..23 都能答；
     * 24 以上回 rc=3）。应答 12 字节，**代码在 @6（BE16，值是码 ×10）** ——
     * 真机：type=8 → 0x0050 = 80 = "G80"、type=20 → 0x0083 = 131 = "G13.1"。
     * 与 `cnc_rdalmmsg2` 一样，`d` 要按次给，所以语义层走 `call("payload")` 的覆盖。
     */
    { "RDGCODE",    { 0x96, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    /*
     * 轴名（`cnc_rdaxisname`）：一个 `0x89`，应答 **每轴 4 字节** = 轴名 2 字节 +
     * 2 字节代码（真机 X/Y/Z 都是 `58 00 94 06` / `59 00 …` / `5a 00 …`）。
     * 轴类型（linear / rotary）这台机器上没有可分辨的那一格，按命名约定推（见语义层）。
     */
    { "AXISNAME",   { 0x89, 0, 0 },      { 0, 0, 0 },      { 0, 0, 0 },      1, false },
    /*
     * 伺服负载（`cnc_rdsvmeter`）：真机实测**一条 `0x56`** 就够（d=1，e=0 =
     * 全部轴），应答载荷 256 字节 = 32 根轴 × 8（§2.8）。官方库还会捎上 `0xa4`
     * （轴数）与 `0x89`（轴表）两条上下文块，但机床单收这一条也认。
     */
    { "SVMETER",    { 0x56, 0, 0 },      { 1, 0, 0 },      { 0, 0, 0 },      1, false },
    /*
     * 主轴那一族（`cnc_rdspmeter`）：`0x40` + `d` 选量（4 = 负载、5 = 转速），
     * 应答 64 字节 = 8 根主轴 × 8。真机实测单块即 rc=0（§2.8）。
     */
    { "SPLOAD",     { 0x40, 0, 0 },      { 4, 0, 0 },      { 0xffffffff, 0, 0 }, 1, false },
    { "SPSPEED",    { 0x40, 0, 0 },      { 5, 0, 0 },      { 0xffffffff, 0, 0 }, 1, false },
    /*
     * 报警消息（`cnc_rdalmmsg2`）：一个 `0x23`，`d` 是类型（-1 = 全部）、`e` 是要几条，
     * **`arg2 = 2` 才填文本、`arg3 = 64` 是要多少字节的文本**（真机实测：这两格给 0
     * 时载荷只有 16 字节的抬头，没有消息文本）。没报警时载荷 0 字节。
     */
    { "ALMMSG",     { 0x23, 0, 0 },      { 0xffffffff, 0, 0 }, { 10, 0, 0 }, 1, false,
                     { 2, 0, 0 },        { 64, 0, 0 } },
    /* 下面这些**码已核、应答布局还没核**（要么值不在载荷 0 处，要么是结构体数组）：
     * 表里先记着码，语义层暂时按 NCL_ERR_UNAVAILABLE 回，等真机抓一次再启用。
     *   ABSOLUTE/MACHINE/RELATIVE/DISTANCE  0x26，d = 位置类型，e = ALL_AXES
     *   RDSVLOAD                            0x56 + 0x89，主轴/伺服负载
     *   RDSPLOAD                            0x40（d=4 负载 / d=5 转速）+ 0x8a
     *   RDALMMSG2                           0x23，报警消息（d = 类型，e = 条数）
     *   RDOPMODE                            0x57，主轴调整模式
     *   RDaxisdata / rdexecprog / rdgcode / rdwkcdshft 也都在这一档
     * （RDPOSITION 与 SV_DELAY 已经出了这一档：前者块布局 NCGuide 实测，后者按
     *   官方库的取值步长反推，见 §2.4/§2.5.1。）
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
