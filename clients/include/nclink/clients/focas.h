/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - FANUC FOCAS / Fwlib32, the PDU layer.
 *
 * Everything here comes from protocal/docs/01-FANUC-CNC-FOCAS.md §2.1-§2.3:
 * §2.1 is the handshake measured against a fake machine, §2.2 is the frame
 * format and the four acceptance rules read out of `libfwlib32.so`'s
 * `Pdu::send` / `Pdu::receive`, and §2.3 is the reply body - which is what
 * finally made `cnc_allclibhndl3` answer `rc=0` without a real machine.
 *
 *   frame = 10 byte header + body, all fields big endian u16:
 *
 *     [0..4)   A0 A0 A0 A0            magic
 *     [4..6)   type                   request always 0001; on a reply it picks
 *                                     the body size class (<=2 / 3 / >3)
 *     [6]      func                   the command's function code
 *     [7]      dir                    request 1; a reply must be 1..4
 *     [8..10)  body length in bytes
 *     [10..)   body
 *
 * The body of a *request* is a command list: `count u2` followed by `count`
 * command blocks, each 28 bytes (§2.3):
 *
 *     [0..2)   block size in bytes (28)      [2..4)   first      (1)
 *     [4..6)   index                         [6..8)   code
 *     [8..12)  arg0                          [12..16) arg1
 *     [16..20) arg2                          [20..24) arg3
 *     [24..26) tag0                          [26..28) tag1
 *
 * The body of a *reply* has the same shape, and `Pdu::getRbPos` walks it with
 * the block's own size field (§2.3):
 *
 *     [0..2)   block count N                       i must be < N
 *     [2..)    N blocks back to back:
 *                [0..2)   block size in bytes
 *                [2..4)   ecode                [8..10)  return code (see below)
 *                [10..12) detail1              [12..14) detail2
 *                [14..16) payload byte count   [16..)   payload
 *
 * `Pdu::getRb(i)` throws when the block's return code is not zero, so a reply
 * is only usable when every block a caller touches has `[8..10) == 0`.
 *
 * Two things the SDK does that a client has to copy:
 *
 *   1. **the reply to `func 1` is a different layout** - 16 bytes of header
 *      followed by n eight byte records, and the body length must be exactly
 *      `16 + 8n` with `n = be16(body[8..10))` (§2.2 rule 5). It is not a
 *      command list.
 *   2. **the reply block count must equal the request's block count** - one
 *      block short and the SDK's own `getRb(last)` walks past the end and the
 *      call comes back as `-17 (EW_PROTOCOL)`. That was the last thing that
 *      kept this protocol dark; see NCL_FOCAS_ERR_RB_MISSING.
 *
 * The item names and codes in ncl_focas_item() are the twelve SDK calls whose
 * request frames were captured (§2.3).
 */
#ifndef NCL_FOCAS_H
#define NCL_FOCAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink_adapter/ncl_driver.h"

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
 * The reply to `func 1` (§2.2 rule 5): 16 bytes of header, then n eight byte
 * records, body length exactly `16 + 8n`.
 * @p records receives n on success. NCL_FOCAS_ERR_LENGTH when it does not fit
 * that shape.
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
    uint16_t    cbs[3];  /**< command codes the request carries             */
    uint32_t    arg0[3]; /**< arg0 of each block (the SDK leaves them 0 or 1) */
    uint32_t    arg1[3]; /**< arg1 of each block                            */
    uint8_t     cb_count;/**< 1 or 2 or 3                                   */
    bool        scalar;  /**< true: block k holds a scalar at payload 0;
                              false: block 0's payload is the whole array  */
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

#ifdef __cplusplus
}
#endif

#endif /* NCL_FOCAS_H */
