/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * FANUC FOCAS / Fwlib32 - 协议层（帧与命令/回复块）。
 *
 * 现场不用读这个头：接机床请用 nclink/clients/focas.h 的语义接口。这里是写 client
 * 的人（或查抓包的人）要看的东西 ——
 *
 *   - 帧格式与握手：protocal/docs/01-FANUC-CNC-FOCAS.md §2.1-§2.3；
 *   - 命令块 / 回复块布局：下面的 ncl_focas_cb_* 与 ncl_focas_block_*；
 *   - item 码表：ncl_focas_item_lookup()（§2.3 抓到的十二个 SDK 调用）；
 *   - 驱动的构造与地址模型：ncl_focas_create()。语义层（focas_values.c）就是站在
 *     驱动上读 item 的：area 是 item 名（"STATINFO"、"ACTF@4"…），offset 是回复块
 *     号，dtype/length 说怎么解；没抓到的功能码用 ops->read_raw 试（"func u1 | body"，
 *     回 hex），那是逆向用的逃逸口，不是接设备用的接口。
 */
#ifndef NCL_FOCAS_PDU_H
#define NCL_FOCAS_PDU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif
/* ============================================================== framing == */

#define NCL_FOCAS_HEADER   10u /**< bytes in front of every body   */
#define NCL_FOCAS_CB_SIZE  28u /**< one command block              */
#define NCL_FOCAS_MAGIC0   0xA0u
#define NCL_FOCAS_DIR_REQ  0x01u /**< the direction a request carries */
#define NCL_FOCAS_DIR_RESP 0x02u /**< what the SDK expects on a reply */
#define NCL_FOCAS_TYPE_V1  0x0001u

/** Function codes seen on the wire (§2.1, §2.3). */
#define NCL_FOCAS_FUNC_HELLO 0x01u /**< the 12 byte session hello        */
#define NCL_FOCAS_FUNC_CMD 0x21u   /**< "here is a command list"         */
#define NCL_FOCAS_FUNC_BYE 0x02u   /**< session end (the SDK sends two)  */

/**
 * 会话里两条 TCP 的 `hello` 计数器（§2.1）：**第一条**是控制通道，**第二条**是
 * 数据通道，命令一律走数据通道。2026-09 官方 SDK 对着一台真机（0i-MD）抄下来的：
 * 控制通道只发 hello，数据通道 hello 完接一条 `code 24` 的探针，然后才是业务调用。
 * 往控制通道上发命令，机床直接把连接 RST 掉（同轮实测）。
 */
#define NCL_FOCAS_HELLO_CONTROL 1u
#define NCL_FOCAS_HELLO_DATA 2u

/**
 * 会话探针的块码：`func 0x21` 一个 `code 24` 的块，应答载荷就是 `cnc_sysinfo` 的
 * **ODBSYS**（真机实测 18 字节，§2.3）。官方库的 `cnc_sysinfo` 就是这么读的。
 */
#define NCL_FOCAS_CODE_SYSINFO 24u

/*
 * 程序上下行的功能码（§2.4，官方 SDK 实测）：下行 start/data/end = 0x11/0x12/0x13，
 * 上行 start/data = 0x15/0x18。两个 start 的体都是**定长 516 字节**
 * （[1] = 数据种类、[4..6) = "N:"、[6..) = 目录名/文件名）。
 */
#define NCL_FOCAS_FUNC_DWN_START 0x11u
#define NCL_FOCAS_FUNC_DWN_DATA 0x12u
#define NCL_FOCAS_FUNC_DWN_END 0x13u
#define NCL_FOCAS_FUNC_UP_START 0x15u
#define NCL_FOCAS_FUNC_UP_DATA 0x18u
/** 数据帧的方向：发完就走，机床不应答（SDK 的 `dir = 4` 那条路）。 */
#define NCL_FOCAS_DIR_DATA 0x04u
/** start 帧的体长（定长，官方 SDK 就是这么发的）。 */
#define NCL_FOCAS_TRANSFER_BODY 516u
/** 一块数据多少字节（官方建议 1024-1400，以太网单帧上限 1460）。 */
#define NCL_FOCAS_TRANSFER_CHUNK 1400u

/** 一个 item 最多带几个命令块：`cnc_rdposition` 一族实测是 9 个（§2.5）。 */
#define NCL_FOCAS_ITEM_CBS 12u

/** The 10 byte header, values in host order. */
typedef struct {
    uint16_t type;   /**< [4..6) */
    uint8_t  func;   /**< [6]    */
    uint8_t  dir;    /**< [7]    */
    uint16_t length; /**< [8..10) body bytes, magic excluded */
} ncl_focas_pdu;

/** A magic value that is not A0 A0 A0 A0 was seen. */
#define NCL_FOCAS_ERR_MAGIC NCL_DRV_ERR_PROTOCOL(0xB0)
/** `[7]` outside 1..4, or a func that is not the expected one. */
#define NCL_FOCAS_ERR_HEADER NCL_DRV_ERR_PROTOCOL(0xB1)
/** The body length is neither `16 + 8n` nor a count plus whole blocks. */
#define NCL_FOCAS_ERR_LENGTH NCL_DRV_ERR_PROTOCOL(0xB2)
/** A block index past the end of the reply body. */
#define NCL_FOCAS_ERR_RB_MISSING NCL_DRV_ERR_PROTOCOL(0xB3)
/** A block says the machine refused the command (`[8..10)` non zero). */
#define NCL_FOCAS_ERR_RB_CODE NCL_DRV_ERR_PROTOCOL(0xB4)
/** A reply carried no command blocks where at least one is required. */
#define NCL_FOCAS_ERR_RB_COUNT NCL_DRV_ERR_PROTOCOL(0xB5)
/**
 * 机床用**方向 3** 的帧回"没有这个数"（真机实测：宏变量 100 / 刀补 2 / 参数 2 都是
 * 这样回的 —— 头里 `dir = 3`、体 8 字节 `00 00 ff ef 00 01 00 00`）。那不是协议错，
 * 是机床的一句"没有"，语义层把它翻成 `NCL_ERR_NOT_FOUND`。
 */
#define NCL_FOCAS_ERR_NO_DATA NCL_DRV_ERR_PROTOCOL(0xB6)

/**
 * Build one frame: magic, type 0001, @p func, @p dir, big endian body length.
 * Returns the frame length, or 0 when it does not fit @p cap.
 */
size_t ncl_focas_build(uint8_t *out, size_t cap, uint8_t func, uint8_t dir,
                       const void *body, size_t body_len);

/**
 * Check and split a frame. Answers NCL_ERR_RANGE while it is still arriving.
 * @p frame_len receives the frame's total length when the header is readable,
 * so a caller can read the rest in one more call.
 */
ncl_err ncl_focas_split(const uint8_t *frame, size_t len, ncl_focas_pdu *out,
                        size_t *frame_len);

/**
 * The reply to `func 1`: 16 bytes of header, then a record table.
 * @p records receives the count the body declares at its `[8..10)`.
 *
 * 这一条**只看"是不是一条握手应答"**：体长 ≥ 16 就收下。原来还要求
 * `body_len == 16 + 8*records`（那条是从 FS0i 那版 `libfwlib32.so` 反汇编里读出来
 * 的），真机（0i-MD）回的是 **360 字节**、而 `[8..10)` 写的是 **8** —— 官方 SDK
 * 自己收下了它（`cnc_allclibhndl3` rc=0），所以那条等式是对反汇编的误读。见 01 册
 * §2.3。
 */
ncl_err ncl_focas_hello_reply(const uint8_t *body, size_t body_len,
                              size_t *records);

/**
 * Big endian u16 of the `func 1` reply at byte @p offset. The body is
 * `16 + 8n` bytes, so offsets past 16 address the records.
 */
uint16_t ncl_focas_hello_field(const uint8_t *body, size_t body_len,
                               size_t offset);

/* ========================================================= command block == */

/** One command block, values in host order. */
typedef struct {
    uint16_t first; /**< [2..4), the SDK writes 1            */
    uint16_t index; /**< [4..6), the SDK writes 1 or the item index */
    uint16_t code;  /**< [6..8), the command / data code     */
    uint32_t arg0;  /**< [8..12)   */
    uint32_t arg1;  /**< [12..16)  */
    uint32_t arg2;  /**< [16..20)  */
    uint32_t arg3;  /**< [20..24)  */
    uint16_t tag0;  /**< [24..26)  */
    uint16_t tag1;  /**< [26..28)  */
} ncl_focas_cb;

/** `first = 1, index = 1`, everything else zero: the shape the SDK sends. */
void ncl_focas_cb_init(ncl_focas_cb *cb, uint16_t code);

/**
 * The three helpers a caller builds a request body with. Start with
 * ncl_focas_body_begin(), then call ncl_focas_body_add() per block; each add
 * rewrites the leading count and the returned body length.
 *
 *   size_t used = ncl_focas_body_begin(body, sizeof(body));
 *   used = ncl_focas_body_add(body, sizeof(body), used, &cb);
 */
size_t ncl_focas_body_begin(uint8_t *out, size_t cap);
size_t ncl_focas_body_add(uint8_t *out, size_t cap, size_t used,
                          const ncl_focas_cb *cb);

/**
 * Append a payload **behind the last block** - the write side of the protocol
 * (`cnc_wrtofs` 一族). Two things come out of the 2026-09 capture against the
 * NCGuide 0i-MF Plus:
 *
 *   - the block's own size field (`[0..2)`) has to grow by the payload length
 *     (write tool offset = `0x1c + 8 = 0x24`), and
 *   - `tag0` / `tag1` stay **0** - the earlier guess of putting the payload
 *     length in `tag0` is what the machine rejected (01 册 §11.12/§11.13).
 */
size_t ncl_focas_body_add_payload(uint8_t *out, size_t cap, size_t used,
                                 const uint8_t *data, size_t len);

/** Serialise one block on its own (28 bytes), for tests and golden samples. */
size_t ncl_focas_cb_write(uint8_t *out, size_t cap, const ncl_focas_cb *cb);

/* ========================================================= reply blocks == */

/** Block count in a reply body (`[0..2)`), 0 when the body is too short. */
size_t ncl_focas_block_count(const uint8_t *body, size_t body_len);

/**
 * Point @p block at reply block @p index (0 based) and give its byte length.
 * Answers NCL_FOCAS_ERR_RB_MISSING when @p index is not in the body - which is
 * the check that has to match the request's block count (§2.3 rule 1).
 */
ncl_err ncl_focas_block_at(const uint8_t *body, size_t body_len, size_t index,
                           const uint8_t **block, size_t *block_len);

/** The block's return code `[8..10)`, sign extended. 0 means "the machine said OK". */
int ncl_focas_block_code(const uint8_t *block, size_t block_len);

/** The block's declared payload byte count `[14..16)`. */
uint16_t ncl_focas_block_payload_len(const uint8_t *block, size_t block_len);

/**
 * The block's payload (everything after byte 16) and how much of it is really
 * there. The caller reads `min(*len, ncl_focas_block_payload_len())` bytes.
 */
const uint8_t *ncl_focas_block_payload(const uint8_t *block, size_t block_len,
                                       size_t *len);

/**
 * Walk every block of a reply and fail on the first non zero return code,
 * which is what `Pdu::getRb` does. @p index_out receives the offending block.
 */
ncl_err ncl_focas_check_blocks(const uint8_t *body, size_t body_len,
                               size_t *index_out);

/* ================================================================ items == */

/** One data item: a name, the blocks a request carries, and how to read them. */
typedef struct {
    const char *name;    /**< "ACTF", "STATINFO", ...                       */
    uint16_t    cbs[NCL_FOCAS_ITEM_CBS];   /**< command codes the request carries */
    uint32_t    arg0[NCL_FOCAS_ITEM_CBS];  /**< arg0 of each block                */
    uint32_t    arg1[NCL_FOCAS_ITEM_CBS];  /**< arg1 of each block                */
    uint8_t     cb_count;/**< how many of them the request carries          */
    bool        scalar;  /**< true: block k holds a scalar at payload 0;
                              false: block 0's payload is the whole array  */
    /**
     * 少数调用还要 Cb 的 `arg2`/`arg3` 两格（`cnc_rdalmmsg2` 就是：`arg2 = 2`
     * 才填消息文本、`arg3 = 64` 是要多少字节的文本 —— 真机实测，见 01 册 §2.8.2）。
     * 表里没写的行默认 0，与之前的形状一致。
     */
    uint32_t    arg2[NCL_FOCAS_ITEM_CBS];
    uint32_t    arg3[NCL_FOCAS_ITEM_CBS];
} ncl_focas_item;

/**
 * Look an item up by name (case insensitive). Returns NULL for a name the
 * table does not know - a caller then falls back to ncl_focas_parse_code() and
 * builds the single block item itself, which is how an undocumented code is
 * tried.
 */
const ncl_focas_item *ncl_focas_item_lookup(const char *name);

/**
 * Parse a command code written as a bare number: `"36"`, `"0x24"`, `"0x8b"`.
 * The `CB:` prefix is accepted too (`"CB:0x24"`), mostly so a configuration can
 * say "this is a raw block, not an item name".
 */
bool ncl_focas_parse_code(const char *text, uint16_t *code);

/** Name of a command code, or NULL. Several items share a code (`0x8b`). */
const char *ncl_focas_code_name(uint16_t code);

/* =============================================================== values == */

/**
 * Decode @p count elements of @p dtype out of @p data (big endian, as every
 * field on this wire is). *values receives a scalar, or an array when
 * @p count > 1. Unknown types answer NCL_ERR_INVALID_DATA_TYPE.
 */
ncl_err ncl_focas_decode(const uint8_t *data, size_t len, ncl_dtype dtype,
                         size_t count, ncl_json **values);

/* ============================================================ the driver == */

/**
 * 构造 FOCAS 驱动（会话 + item 表 + 错误分级 + 原始帧）。语义层用它；
 * 想自己下探到 item 一级的适配器也用它可以，但要先想清楚为什么语义层不够。
 */
ncl_driver *ncl_focas_create(void);

#ifdef __cplusplus
}
#endif

#endif /* NCL_FOCAS_PDU_H */
