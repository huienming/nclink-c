/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - FANUC FOCAS / Fwlib32 over TCP.
 *
 * The session is the connection plus the negotiation of §2.3: a `func 1`
 * hello, then the capability probe the vendor SDK sends before any data call.
 * After that every read is one `func 0x21` frame carrying command blocks and
 * one reply carrying the same number of answer blocks - the rule that took
 * this protocol from "-17 forever" to the SDK answering rc=0.
 *
 * Every exchange goes through one mutex: one session, one request in flight.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink/clients/focas.h"
#include "focas/ncl_focas_pdu.h"

/** The body size class of §2.2 rule 4 keeps a reply under 3470 bytes. */
#define FOCAS_MAX_BODY 4096
#define FOCAS_MAX_FRAME (NCL_FOCAS_HEADER + FOCAS_MAX_BODY)
/** 一个请求最多几个块：`cnc_rdposition` 一族实测 9 个（§2.5），留到 12。 */
#define FOCAS_MAX_CB NCL_FOCAS_ITEM_CBS

typedef struct {
    char      *host;
    unsigned   port;
    ncl_socket *socket;
    unsigned   connect_timeout_ms;
    unsigned   timeout_ms;
    unsigned   retries;
    bool       negotiate;
    unsigned   hello_counter;
    ncl_mutex *mutex;
    uint8_t    tx[FOCAS_MAX_FRAME];
    uint8_t    rx[FOCAS_MAX_FRAME];
    size_t     last_tx_len; /**< what the audit should show */
    size_t     last_rx_len;
    /* what the hello and the probe found, reported by call("session") */
    bool       session;
    size_t     hello_records; /**< n from the `func 1` reply */
    uint16_t   hello_field2;  /**< body[2..4), the branch §2.3 documents */
    size_t     probe_blocks;  /**< blocks the machine offers */
} focas_ctx;

static unsigned json_uint(const ncl_json *object, const char *key,
                          unsigned fallback)
{
    long long value = ncl_json_obj_get_int(object, key, (long long)fallback);

    return value < 0 ? 0u : (unsigned)value;
}

/* --------------------------------------------------------------- session -- */

static void focas_close_session(focas_ctx *ctx)
{
    if (ctx->socket != NULL) {
        ncl_socket_close(ctx->socket);
        ctx->socket = NULL;
    }
    ctx->session = false;
}

/* -------------------------------------------------------------- exchange -- */

/**
 * One frame out, one frame in. The reply is read in two calls: 10 bytes of
 * header, which say how much body follows (§2.2), then the body itself.
 *
 * @p body_out borrows into the context's receive buffer and stays valid until
 * the next exchange.
 */
static ncl_err focas_exchange(focas_ctx *ctx, uint8_t func, const uint8_t *body,
                              size_t body_len, ncl_focas_pdu *pdu,
                              const uint8_t **body_out, size_t *body_out_len)
{
    size_t frame_len;
    size_t total = 0;
    unsigned attempt = 0;
    ncl_err err = NCL_OK;

    frame_len = ncl_focas_build(ctx->tx, sizeof(ctx->tx), func,
                                NCL_FOCAS_DIR_REQ, body, body_len);
    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    ctx->last_tx_len = frame_len;
    ctx->last_rx_len = 0;
    for (attempt = 0;; attempt++) {
        if (ncl_socket_send(ctx->socket, ctx->tx, frame_len) != NCL_OK) {
            err = NCL_DRV_ERR_TRANSPORT(0x90);
        } else if (ncl_socket_recv_exact(ctx->socket, ctx->rx, NCL_FOCAS_HEADER,
                                        ctx->timeout_ms) != NCL_OK) {
            err = NCL_DRV_ERR_TRANSPORT(0x91);
        } else {
            err = ncl_focas_split(ctx->rx, NCL_FOCAS_HEADER, pdu, &total);
            if (err == NCL_ERR_RANGE) {
                /* the header was fine and pinned the frame's total length */
                if (total > sizeof(ctx->rx)) {
                    err = NCL_FOCAS_ERR_LENGTH;
                } else if (ncl_socket_recv_exact(ctx->socket,
                                                 ctx->rx + NCL_FOCAS_HEADER,
                                                 total - NCL_FOCAS_HEADER,
                                                 ctx->timeout_ms) != NCL_OK) {
                    err = NCL_DRV_ERR_TRANSPORT(0x92);
                } else {
                    err = ncl_focas_split(ctx->rx, total, pdu, NULL);
                }
            }
        }
        if (err == NCL_OK) {
            break;
        }
        if (err != NCL_DRV_ERR_TRANSPORT(0x90) && err != NCL_DRV_ERR_TRANSPORT(0x91) &&
            err != NCL_DRV_ERR_TRANSPORT(0x92)) {
            break; /* a protocol complaint will not get better by retrying */
        }
        if (attempt >= ctx->retries) {
            break;
        }
    }
    if (err != NCL_OK) {
        if (ncl_driver_error_tier(err) == 1) {
            ncl_socket_shutdown(ctx->socket); /* a dead link must be re-opened */
            ctx->session = false;
        }
        return err;
    }
    ctx->last_rx_len = NCL_FOCAS_HEADER + pdu->length;
    /* §2.2 rule 2: the reply's func must be the one we asked for. */
    if (pdu->func != func) {
        return NCL_FOCAS_ERR_HEADER;
    }
    /* §2.2 rule 3: a reply's direction is 1..4, and the SDK's requests get 2. */
    if (pdu->dir != NCL_FOCAS_DIR_RESP) {
        return NCL_FOCAS_ERR_HEADER;
    }
    if (body_out != NULL) {
        *body_out = ctx->rx + NCL_FOCAS_HEADER;
    }
    if (body_out_len != NULL) {
        *body_out_len = pdu->length;
    }
    return NCL_OK;
}

/** One `func 0x21` exchange whose reply must be a usable command list. */
static ncl_err focas_command(focas_ctx *ctx, const uint8_t *body, size_t body_len,
                             const uint8_t **reply, size_t *reply_len)
{
    ncl_focas_pdu pdu;

    memset(&pdu, 0, sizeof(pdu));
    return focas_exchange(ctx, NCL_FOCAS_FUNC_CMD, body, body_len, &pdu, reply,
                          reply_len);
}

/**
 * 一帧发出去、**不等应答**。程序下行的数据帧就是这种（func 0x12、dir 4）：官方
 * SDK 发完立刻发下一块，机床不回；回了反而把它带歪（§2.4 实测）。
 */
static ncl_err focas_send_only(focas_ctx *ctx, uint8_t func, uint8_t dir,
                              const uint8_t *body, size_t body_len)
{
    size_t frame_len = ncl_focas_build(ctx->tx, sizeof(ctx->tx), func, dir, body,
                                       body_len);

    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    ctx->last_tx_len = frame_len;
    ctx->last_rx_len = 0;
    if (ncl_socket_send(ctx->socket, ctx->tx, frame_len) != NCL_OK) {
        ncl_socket_shutdown(ctx->socket);
        ctx->session = false;
        return NCL_DRV_ERR_TRANSPORT(0x90);
    }
    return NCL_OK;
}

/**
 * 程序上/下行的 start 帧体（§2.4，516 字节定长）：`[1]` 是数据种类（0 NC 程序、
 * 1 刀补、2 参数…），`[4..6)` 固定 `"N:"`，`[6..)` 是目录名/文件名。
 */
static ncl_err focas_transfer_start_body(uint8_t *body, short type,
                                         const char *name)
{
    size_t len = name != NULL ? strlen(name) : 0;

    if (len > NCL_FOCAS_TRANSFER_BODY - 6u || type < 0 || type > 0xFF) {
        return NCL_ERR_RANGE;
    }
    memset(body, 0, NCL_FOCAS_TRANSFER_BODY);
    body[0] = 0x00;
    body[1] = (uint8_t)type;
    body[2] = 0x00;
    body[3] = 0x01;
    body[4] = 'N';
    body[5] = ':';
    if (len > 0) {
        memcpy(body + 6, name, len);
    }
    return NCL_OK;
}

/**
 * 程序下行（PC → CNC）：`cnc_dwnstart4` → 分块 `cnc_download4` → `cnc_dwnend4`。
 * 参数：`type`（数据种类，缺省 0 = NC 程序）、`dir`（目标目录/程序名，可省）、
 * `data`（程序文本）。
 *
 * 与官方库一致的几点：数据帧发完不等应答；一块 1400 字节以内；**错误在 end 帧
 * 才回**（`EW_DATA`/`EW_OVRFLOW` 一类），所以 end 没成功就是整条没落地。
 */
static ncl_err focas_program_download(focas_ctx *ctx, const ncl_json *params,
                                      ncl_json **result)
{
    ncl_focas_pdu pdu;
    uint8_t body[NCL_FOCAS_TRANSFER_BODY];
    const char *text = ncl_json_obj_get_string(params, "data");
    const char *dir = ncl_json_obj_get_string(params, "dir");
    long long type = ncl_json_obj_get_int(params, "type", 0);
    size_t total = text != NULL ? strlen(text) : 0;
    size_t sent = 0;
    ncl_err err;

    if (text == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    err = focas_transfer_start_body(body, (short)type, dir);
    if (err != NCL_OK) {
        return err;
    }
    memset(&pdu, 0, sizeof(pdu));
    err = focas_exchange(ctx, NCL_FOCAS_FUNC_DWN_START, body, sizeof(body), &pdu,
                         NULL, NULL);
    if (err != NCL_OK) {
        return err;
    }
    while (sent < total) {
        size_t chunk = total - sent;

        if (chunk > NCL_FOCAS_TRANSFER_CHUNK) {
            chunk = NCL_FOCAS_TRANSFER_CHUNK;
        }
        err = focas_send_only(ctx, NCL_FOCAS_FUNC_DWN_DATA, NCL_FOCAS_DIR_DATA,
                              (const uint8_t *)text + sent, chunk);
        if (err != NCL_OK) {
            return err;
        }
        sent += chunk;
    }
    memset(&pdu, 0, sizeof(pdu));
    err = focas_exchange(ctx, NCL_FOCAS_FUNC_DWN_END, NULL, 0, &pdu, NULL, NULL);
    if (err != NCL_OK) {
        return err; /* 下载的错都在这条上回 */
    }
    if (result != NULL) {
        ncl_json *object = ncl_json_new_object();

        if (object == NULL) {
            return NCL_ERR_NOMEM;
        }
        (void)ncl_json_obj_set_int(object, "type", type);
        (void)ncl_json_obj_set_int(object, "bytes", (long long)total);
        if (dir != NULL) {
            (void)ncl_json_obj_set_string(object, "dir", dir);
        }
        *result = object;
    }
    return NCL_OK;
}

/* ------------------------------------------------------------- handshake -- */

/**
 * §2.3: the SDK's session setup, copied step for step.
 *
 *   1. `func 1` with a 2 byte counter. The reply is the *other* layout:
 *      16 bytes of header plus n eight byte records, body length 16+8n.
 *   2. `func 0x21` with one block per record whose first short is not zero,
 *      plus (when the hello's field 2 says 2) a block of code 140.
 *   3. unless field 2 says 3, one more `func 0x21` with either code 141
 *      (field 2 == 2) or code 14 with the 0x26f0 arguments.
 */
static ncl_err focas_handshake(focas_ctx *ctx)
{
    uint8_t body[2u + FOCAS_MAX_CB * NCL_FOCAS_CB_SIZE];
    uint8_t hello[2];
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    size_t used;
    size_t records = 0;
    ncl_err err;
    size_t i;

    hello[0] = 0;
    hello[1] = (uint8_t)(ctx->hello_counter & 0xFFu);
    {
        ncl_focas_pdu pdu;

        memset(&pdu, 0, sizeof(pdu));
        err = focas_exchange(ctx, NCL_FOCAS_FUNC_HELLO, hello, sizeof(hello), &pdu,
                             &reply, &reply_len);
    }
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_focas_hello_reply(reply, reply_len, &records);
    if (err != NCL_OK) {
        return err;
    }
    ctx->hello_records = records;
    ctx->hello_field2 = ncl_focas_hello_field(reply, reply_len, 2);
    if (!ctx->negotiate) {
        ctx->session = true;
        return NCL_OK;
    }

    /* step 2: one block per record whose first short is not zero */
    used = ncl_focas_body_begin(body, sizeof(body));
    if (used == 0) {
        return NCL_ERR_RANGE;
    }
    for (i = 0; i < records; i++) {
        ncl_focas_cb cb;

        if (ncl_focas_hello_field(reply, reply_len, 16u + i * 8u) == 0) {
            continue; /* the SDK skips the zero records (it would not getRb) */
        }
        ncl_focas_cb_init(&cb, 24);
        cb.index = (uint16_t)(i + 1u);
        used = ncl_focas_body_add(body, sizeof(body), used, &cb);
        if (used == 0) {
            return NCL_ERR_RANGE;
        }
    }
    if (ctx->hello_field2 == 2u) {
        ncl_focas_cb cb;

        ncl_focas_cb_init(&cb, 140);
        used = ncl_focas_body_add(body, sizeof(body), used, &cb);
        if (used == 0) {
            return NCL_ERR_RANGE;
        }
    }
    err = focas_command(ctx, body, used, &reply, &reply_len);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_focas_check_blocks(reply, reply_len, NULL);
    if (err != NCL_OK) {
        return err;
    }
    ctx->probe_blocks = ncl_focas_block_count(reply, reply_len);
    if (ctx->hello_field2 == 3u) {
        ctx->session = true;
        return NCL_OK;
    }

    /* step 3: the second probe, whose block code says what the box is */
    used = ncl_focas_body_begin(body, sizeof(body));
    if (used == 0) {
        return NCL_ERR_RANGE;
    }
    {
        ncl_focas_cb cb;

        if (ctx->hello_field2 == 2u) {
            ncl_focas_cb_init(&cb, 141);
            cb.arg0 = 0x23c1;
            cb.arg1 = 0x23c1;
        } else {
            ncl_focas_cb_init(&cb, 14);
            cb.arg0 = 0x26f0;
            cb.arg1 = 0x26f0;
        }
        used = ncl_focas_body_add(body, sizeof(body), used, &cb);
    }
    if (used == 0) {
        return NCL_ERR_RANGE;
    }
    err = focas_command(ctx, body, used, &reply, &reply_len);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_focas_check_blocks(reply, reply_len, NULL);
    if (err != NCL_OK) {
        return err;
    }
    ctx->session = true;
    return NCL_OK;
}

static ncl_err focas_open_session(focas_ctx *ctx)
{
    char err[256];

    if (ctx->socket != NULL) {
        return NCL_OK;
    }
    err[0] = '\0';
    ctx->socket = ncl_socket_connect(ctx->host, ctx->port, ctx->connect_timeout_ms,
                                     err, sizeof(err));
    if (ctx->socket == NULL) {
        return NCL_DRV_ERR_TRANSPORT(0x93);
    }
    return NCL_OK;
}

/** Open on demand: connect, then negotiate once (§2.3). */
static ncl_err focas_ensure_session(focas_ctx *ctx)
{
    ncl_err err;

    err = focas_open_session(ctx);
    if (err != NCL_OK) {
        return err;
    }
    if (ctx->session) {
        return NCL_OK;
    }
    err = focas_handshake(ctx);
    if (err != NCL_OK) {
        ctx->session = false;
    }
    return err;
}

/* ----------------------------------------------------------------- reads -- */

/** Longest item name a point may write (the table's names are much shorter). */
#define FOCAS_MAX_ITEM_NAME 64

/**
 * Split a point's `area` into the item name and the byte offset inside the
 * reply block's payload: `"STATINFO@12"` is ODBST's `alarm` short, `"ACTF@4"`
 * the second axis of `cnc_actf`.
 *
 * The offset has to live in the name because the generic address model reads
 * `bit` as a *bit* index (ncl_address_from_json turns any `bit` into dtype
 * BIT), and a FOCAS payload offset is a byte count of a whole 16/32 bit field.
 * A name without '@', or one whose tail is not a decimal number, is taken
 * whole - the lookup then fails with the same error as any unknown item.
 *
 * Returns @p name, with *byte_offset set to 0 when the name carried none.
 */
static const char *focas_item_name(const char *area, char *name, size_t cap,
                                   size_t *byte_offset)
{
    const char *at;
    size_t len;

    *byte_offset = 0;
    at = area != NULL ? strrchr(area, '@') : NULL;
    if (at == NULL || at == area || at[1] == '\0') {
        return area;
    }
    {
        const char *p = at + 1;
        size_t offset = 0;

        while (*p >= '0' && *p <= '9') {
            offset = offset * 10u + (size_t)(*p - '0');
            p++;
        }
        if (*p != '\0' || offset > 0xFFFFu) {
            return area; /* "AXIS@0" style names stay whole */
        }
        *byte_offset = offset;
    }
    len = (size_t)(at - area);
    if (len + 1u > cap) {
        return area;
    }
    memcpy(name, area, len);
    name[len] = '\0';
    return name;
}

/**
 * Build the request body for one point: one block per command code of the
 * item. A name the table does not know is taken as a bare code, with the
 * arguments left zero - which is how a block the capture has no entry for is
 * still tried.
 */
/**
 * A FOCAS item name that ends in a digit - cnc_exeprgname2, cnc_rdprogdir3 -
 * arrives split, because the generic address parser reads a trailing run of
 * digits as an offset: "EXEPRGNAME2" becomes area "EXEPRGNAME" at offset 2.
 * Which one the site meant is decided by the item table, not by the spelling:
 * when the name as given is not one of ours, put the digits back on.
 *
 * @return @p item, or @p buffer holding the rejoined name.
 */
static const char *focas_rejoin_item(const char *item, long long offset,
                                     char *buffer, size_t cap)
{
    if (offset <= 0 || ncl_focas_item_lookup(item) != NULL) {
        return item;
    }
    if ((size_t)snprintf(buffer, cap, "%s%lld", item, offset) >= cap) {
        return item;
    }
    return ncl_focas_item_lookup(buffer) != NULL ? buffer : item;
}

static ncl_err focas_build_item(const char *area, uint8_t *body, size_t cap,
                                size_t *used)
{
    const ncl_focas_item *item = ncl_focas_item_lookup(area);
    ncl_focas_item raw;
    size_t i;
    size_t offset;

    if (item == NULL) {
        uint16_t code = 0;

        if (!ncl_focas_parse_code(area, &code)) {
            return NCL_ERR_INVALID_DATA_NAME;
        }
        memset(&raw, 0, sizeof(raw));
        raw.name = area;
        raw.cbs[0] = code;
        raw.cb_count = 1;
        item = &raw;
    }
    offset = ncl_focas_body_begin(body, cap);
    if (offset == 0) {
        return NCL_ERR_RANGE;
    }
    for (i = 0; i < item->cb_count; i++) {
        ncl_focas_cb cb;

        ncl_focas_cb_init(&cb, item->cbs[i]);
        cb.arg0 = item->arg0[i];
        cb.arg1 = item->arg1[i];
        offset = ncl_focas_body_add(body, cap, offset, &cb);
        if (offset == 0) {
            return NCL_ERR_RANGE;
        }
    }
    *used = offset;
    return NCL_OK;
}

static ncl_err focas_read_one(focas_ctx *ctx, const ncl_address *address,
                              ncl_json **value)
{
    char name[FOCAS_MAX_ITEM_NAME];
    char rejoined[FOCAS_MAX_ITEM_NAME];
    uint8_t body[FOCAS_MAX_CB * NCL_FOCAS_CB_SIZE + 2u];
    const uint8_t *reply = NULL;
    const uint8_t *block = NULL;
    const uint8_t *payload;
    const char *item;
    size_t reply_len = 0;
    size_t block_len = 0;
    size_t payload_len = 0;
    size_t used = 0;
    size_t named_offset = 0;
    size_t payload_offset;
    size_t width;
    long long block_index;
    ncl_err err;

    if (address->area == NULL || address->length < 1) {
        return NCL_ERR_INVALID_ARG;
    }
    block_index = address->offset;
    item = focas_item_name(address->area, name, sizeof(name), &named_offset);
    if (named_offset == 0) {
        const char *joined = focas_rejoin_item(item, block_index, rejoined,
                                               sizeof(rejoined));

        if (joined != item) {
            /* The digits were part of the item's name, not a block index. */
            item = joined;
            block_index = 0;
        }
    }
    err = focas_build_item(item, body, sizeof(body), &used);
    if (err != NCL_OK) {
        return err;
    }
    err = focas_ensure_session(ctx);
    if (err != NCL_OK) {
        return err;
    }
    err = focas_command(ctx, body, used, &reply, &reply_len);
    if (err != NCL_OK) {
        return err;
    }
    /* §2.3 rule 1: the reply must carry a block per request block, and each of
     * them must say "OK" - that is what the SDK's getRb enforces. */
    err = ncl_focas_check_blocks(reply, reply_len, NULL);
    if (err != NCL_OK) {
        return err;
    }
    if (block_index < 0 ||
        (size_t)block_index >= ncl_focas_block_count(reply, reply_len)) {
        return NCL_FOCAS_ERR_RB_MISSING;
    }
    err = ncl_focas_block_at(reply, reply_len, (size_t)block_index, &block,
                             &block_len);
    if (err != NCL_OK) {
        return err;
    }
    payload = ncl_focas_block_payload(block, block_len, &payload_len);
    if (payload == NULL) {
        return NCL_FOCAS_ERR_LENGTH;
    }
    {
        uint16_t declared = ncl_focas_block_payload_len(block, block_len);

        if (declared > 0 && (size_t)declared < payload_len) {
            payload_len = declared; /* believe the machine's own count */
        }
    }
    /* `bit` is the byte offset inside the block's payload; -1 means zero.
     * A point that named one (`"RDCOUNT@4"`) keeps its `bit` free, because the
     * generic address model reserves `bit` for a bit index. */
    payload_offset = address->bit > 0 ? (size_t)address->bit : named_offset;
    if (payload_offset > payload_len) {
        return NCL_ERR_RANGE;
    }
    payload += payload_offset;
    payload_len -= payload_offset;
    switch (address->dtype) {
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
    if (payload_len < width * (size_t)address->length) {
        return NCL_FOCAS_ERR_LENGTH;
    }
    return ncl_focas_decode(payload, payload_len, address->dtype,
                            (size_t)address->length, value);
}

static ncl_err focas_read_batch(ncl_driver *self, const ncl_address *addresses,
                                size_t count, ncl_json **values)
{
    focas_ctx *ctx = (focas_ctx *)self->ctx;
    ncl_json *array;
    ncl_err err = NCL_OK;
    size_t i;

    if (addresses == NULL || values == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    array = ncl_json_new_array();
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    ncl_mutex_lock(ctx->mutex);
    for (i = 0; i < count; i++) {
        ncl_json *value = NULL;

        err = focas_read_one(ctx, &addresses[i], &value);
        if (err != NCL_OK) {
            break;
        }
        if (ncl_json_arr_push(array, value != NULL ? value : ncl_json_new_null()) !=
            NCL_OK) {
            err = NCL_ERR_NOMEM;
            break;
        }
    }
    ncl_mutex_unlock(ctx->mutex);
    if (err != NCL_OK) {
        ncl_json_free(array);
        return err;
    }
    *values = array;
    return NCL_OK;
}

/* --------------------------------------------------------------- the rest -- */

static ncl_err focas_raw(ncl_driver *self, const void *frame, size_t frame_len,
                         ncl_driver_result *out)
{
    static const char kDigits[] = "0123456789abcdef";
    focas_ctx *ctx = (focas_ctx *)self->ctx;
    const uint8_t *bytes = (const uint8_t *)frame;
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    ncl_err err;
    char *hex;
    size_t i;

    if (frame == NULL || frame_len < 1u || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_mutex_lock(ctx->mutex);
    err = focas_ensure_session(ctx);
    if (err == NCL_OK) {
        err = focas_command(ctx, bytes + 1, frame_len - 1, &reply, &reply_len);
    }
    ncl_mutex_unlock(ctx->mutex);
    if (err != NCL_OK) {
        return err;
    }
    ncl_driver_result_set_raw(out, reply, reply_len);
    hex = (char *)ncl_mem_alloc(reply_len * 2u + 1u);
    if (hex == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < reply_len; i++) {
        hex[i * 2] = kDigits[(reply[i] >> 4) & 0xF];
        hex[i * 2 + 1] = kDigits[reply[i] & 0xF];
    }
    hex[reply_len * 2] = '\0';
    ncl_driver_result_ok(out, ncl_json_new_string(hex));
    ncl_free_safe(hex);
    return NCL_OK;
}

/** Session operations: what the table knows, and what the hello found. */
static ncl_err focas_call(ncl_driver *self, const char *operation,
                          const ncl_json *params, ncl_json **result)
{
    focas_ctx *ctx = (focas_ctx *)self->ctx;

    (void)params;
    if (result != NULL) {
        *result = NULL;
    }
    if (ncl_streq_ignore_case(operation, "items")) {
        static const char *const kNames[] = {
            "STATINFO", "ACTF",       "ACTS",  "RDCOUNT", "RDLIFE",
            "RDMACRO",  "RDPARAM",    "RDTOFS", "RDPROGDIR3", "EXEPRGNAME2",
        };
        ncl_json *array = ncl_json_new_array();
        size_t i;

        if (array == NULL) {
            return NCL_ERR_NOMEM;
        }
        for (i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
            const ncl_focas_item *item = ncl_focas_item_lookup(kNames[i]);
            ncl_json *entry = ncl_json_new_object();

            if (entry == NULL) {
                ncl_json_free(array);
                return NCL_ERR_NOMEM;
            }
            (void)ncl_json_obj_set_string(entry, "name", kNames[i]);
            (void)ncl_json_obj_set_int(entry, "blocks", item != NULL ? item->cb_count : 0);
            if (item != NULL && item->cb_count > 0) {
                (void)ncl_json_obj_set_int(entry, "code0", item->cbs[0]);
            }
            if (ncl_json_arr_push(array, entry) != NCL_OK) {
                ncl_json_free(array);
                return NCL_ERR_NOMEM;
            }
        }
        if (result != NULL) {
            *result = array;
        } else {
            ncl_json_free(array);
        }
        return NCL_OK;
    }
    if (ncl_streq_ignore_case(operation, "session")) {
        ncl_json *object = ncl_json_new_object();

        if (object == NULL) {
            return NCL_ERR_NOMEM;
        }
        (void)ncl_json_obj_set_string(object, "host", ctx->host != NULL ? ctx->host : "");
        (void)ncl_json_obj_set_int(object, "port", ctx->port);
        (void)ncl_json_obj_set_bool(object, "negotiated", ctx->session);
        (void)ncl_json_obj_set_int(object, "helloRecords",
                                   (long long)ctx->hello_records);
        (void)ncl_json_obj_set_int(object, "helloField2", ctx->hello_field2);
        (void)ncl_json_obj_set_int(object, "probeBlocks",
                                   (long long)ctx->probe_blocks);
        if (result != NULL) {
            *result = object;
        } else {
            ncl_json_free(object);
        }
        return NCL_OK;
    }
    /* 程序下行（PC → CNC）：cnc_dwnstart4 → 分块 cnc_download4 → cnc_dwnend4。
     * 帧与体长按官方 SDK 实测（01 册 §2.4），语义层只是把参数转过来。 */
    if (ncl_streq_ignore_case(operation, "download")) {
        ncl_err err;

        ncl_mutex_lock(ctx->mutex);
        err = focas_ensure_session(ctx);
        if (err == NCL_OK) {
            err = focas_program_download(ctx, params, result);
        }
        ncl_mutex_unlock(ctx->mutex);
        return err;
    }
    /* 程序上行（CNC → PC）：cnc_upstart4 → cnc_upload4 → cnc_upend4。请求码已核
     * （0x15 / 0x18），但**应答里程序文本的切法还没核**（SDK 里在 0x14fe70 里解，
     * 2026-09 反汇编到这一层没再往下），所以这里明确回"还没有"。 */
    if (ncl_streq_ignore_case(operation, "upload")) {
        return NCL_ERR_UNAVAILABLE;
    }
    return NCL_DRV_ERR_PROTOCOL(0x94); /* no such operation */
}

static ncl_err focas_create(ncl_driver *self, const ncl_json *parameters)
{
    focas_ctx *ctx = (focas_ctx *)self->ctx;
    const char *text;

    if (parameters == NULL) {
        return NCL_OK;
    }
    text = ncl_json_obj_get_string(parameters, "host");
    if (text != NULL) {
        ncl_free_safe(ctx->host);
        ctx->host = ncl_strdup(text);
        if (ctx->host == NULL) {
            return NCL_ERR_NOMEM;
        }
    }
    ctx->port = json_uint(parameters, "port", ctx->port);
    ctx->connect_timeout_ms = json_uint(parameters, "connectTimeoutMs",
                                        ctx->connect_timeout_ms);
    ctx->timeout_ms = json_uint(parameters, "timeoutMs", ctx->timeout_ms);
    ctx->retries = json_uint(parameters, "retries", ctx->retries);
    ctx->negotiate = ncl_json_obj_get_bool(parameters, "negotiate", ctx->negotiate);
    ctx->hello_counter = json_uint(parameters, "helloCounter", ctx->hello_counter);
    if (ctx->host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return NCL_OK;
}

static ncl_err focas_open(ncl_driver *self)
{
    focas_ctx *ctx = (focas_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = focas_ensure_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void focas_close(ncl_driver *self)
{
    focas_ctx *ctx = (focas_ctx *)self->ctx;

    ncl_mutex_lock(ctx->mutex);
    focas_close_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
}

static bool focas_is_connected(const ncl_driver *self)
{
    const focas_ctx *ctx = (const focas_ctx *)self->ctx;

    return ctx->socket != NULL && ctx->session;
}

static void focas_attach_event(ncl_driver *self, ncl_driver_event_fn fn,
                               void *user)
{
    (void)self;
    (void)fn;
    (void)user;
}

static void focas_last_raw(const ncl_driver *self, ncl_driver_raw *out)
{
    const focas_ctx *ctx = (const focas_ctx *)self->ctx;

    out->request = ctx->last_tx_len > 0 ? ctx->tx : NULL;
    out->request_len = ctx->last_tx_len;
    out->reply = ctx->last_rx_len > 0 ? ctx->rx : NULL;
    out->reply_len = ctx->last_rx_len;
}

static void focas_destroy(ncl_driver *self)
{
    focas_ctx *ctx;

    if (self == NULL) {
        return;
    }
    ctx = (focas_ctx *)self->ctx;
    if (ctx != NULL) {
        focas_close_session(ctx);
        ncl_free_safe(ctx->host);
        if (ctx->mutex != NULL) {
            ncl_mutex_destroy(ctx->mutex);
        }
        ncl_free_safe(ctx);
    }
    ncl_free_safe(self);
}

static const ncl_driver_ops kFocasOps = {
    "focas",                focas_create,
    focas_open,             focas_close,
    focas_is_connected,     focas_read_batch,
    NULL,                   focas_raw, /* 写：没抓到写帧；骨架对 NULL 回 NOT_SUPPORTED */
    focas_raw,              focas_call,
    focas_attach_event,     focas_destroy,
    focas_last_raw,
};

ncl_driver *ncl_focas_create(void)
{
    focas_ctx *ctx = (focas_ctx *)ncl_mem_calloc(1, sizeof(*ctx));
    ncl_driver *driver;

    if (ctx == NULL) {
        return NULL;
    }
    ctx->port = 8193; /* §2.1: FOCAS over Ethernet listens here */
    ctx->connect_timeout_ms = 3000;
    ctx->timeout_ms = 3000;
    ctx->retries = 0;
    ctx->negotiate = true;
    ctx->hello_counter = 1;
    ctx->mutex = ncl_mutex_create();
    if (ctx->mutex == NULL) {
        ncl_free_safe(ctx);
        return NULL;
    }
    driver = ncl_driver_new(&kFocasOps, ctx);
    if (driver == NULL) {
        ncl_mutex_destroy(ctx->mutex);
        ncl_free_safe(ctx);
        return NULL;
    }
    return driver;
}
