/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Siemens S7comm over ISO-on-TCP.
 *
 * The session is the three step handshake of §2 - TCP, COTP connection
 * request/confirm, then Setup Communication to agree on the PDU size - and
 * after that every request is a Read or Write Var PDU inside TPKT + COTP DT.
 *
 * The negotiated PDU size is what limits a batch: a Read Var is one request
 * carrying as many items as fit, which is what makes reading a point table
 * cheap on this protocol.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink_adapter/ncl_s7.h"
#include "s7/ncl_s7_driver.h"

#define S7_MAX_FRAME 2048
#define S7_MAX_ITEMS 64

typedef struct {
    char        *host;
    unsigned     port;
    unsigned     rack;
    unsigned     slot;
    uint16_t     want_pdu;
    uint16_t     pdu_size; /**< the size the PLC agreed to */
    uint16_t     pdu_ref;
    ncl_socket  *socket;
    unsigned     connect_timeout_ms;
    unsigned     timeout_ms;
    unsigned     retries;
    ncl_mutex   *mutex;
    uint8_t      tx[S7_MAX_FRAME];
    uint8_t      rx[S7_MAX_FRAME];
    size_t       last_tx_len; /**< the frame the audit should show */
    size_t       last_rx_len;
} s7_ctx;

static unsigned json_uint(const ncl_json *object, const char *key,
                          unsigned fallback)
{
    long long value = ncl_json_obj_get_int(object, key, (long long)fallback);

    return value < 0 ? 0u : (unsigned)value;
}

/* --------------------------------------------------------------- session -- */

static void s7_close_session(s7_ctx *ctx)
{
    if (ctx->socket != NULL) {
        ncl_socket_close(ctx->socket);
        ctx->socket = NULL;
    }
}

/** Read one TPKT frame off the socket; *payload points into the buffer. */
static ncl_err s7_read_tpkt(s7_ctx *ctx, const uint8_t **payload,
                            size_t *payload_len)
{
    size_t total;
    ncl_err err;

    if (ncl_socket_recv_exact(ctx->socket, ctx->rx, 4, ctx->timeout_ms) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x31);
    }
    total = ((size_t)ctx->rx[2] << 8) | ctx->rx[3];
    if (total < 4 || total > sizeof(ctx->rx)) {
        return NCL_DRV_ERR_PROTOCOL(0x0A);
    }
    if (total > 4 &&
        ncl_socket_recv_exact(ctx->socket, ctx->rx + 4, total - 4u,
                              ctx->timeout_ms) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x32);
    }
    ctx->last_rx_len = total;
    err = ncl_s7_tpkt_split(ctx->rx, total, payload, payload_len, NULL);
    return err;
}

/** Send an S7 PDU wrapped in TPKT + COTP DT. */
static ncl_err s7_send_pdu(s7_ctx *ctx, const uint8_t *pdu, size_t pdu_len)
{
    size_t frame_len = ncl_s7_cotp_dt(ctx->tx, sizeof(ctx->tx), pdu, pdu_len);

    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    ctx->last_tx_len = frame_len;
    ctx->last_rx_len = 0;
    return ncl_socket_send(ctx->socket, ctx->tx, frame_len) == NCL_OK
               ? NCL_OK
               : NCL_DRV_ERR_TRANSPORT(0x33);
}

/** The COTP handshake: connection request, then the confirm. */
static ncl_err s7_cotp_connect(s7_ctx *ctx)
{
    uint8_t request[32];
    const uint8_t *cotp = NULL;
    size_t cotp_len = 0;
    uint8_t pdu_type = 0;
    uint8_t pdu_code = 0;
    size_t frame_len = ncl_s7_cotp_cr(request, sizeof(request), 0x0100,
                                      ncl_s7_tsap(ctx->rack, ctx->slot),
                                      NCL_S7_PDU_1024);
    ncl_err err;

    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    /* Through ctx->tx rather than straight from the local buffer, so the audit
     * trail can show the connect request like every other frame. */
    memcpy(ctx->tx, request, frame_len);
    ctx->last_tx_len = frame_len;
    ctx->last_rx_len = 0;
    if (ncl_socket_send(ctx->socket, ctx->tx, frame_len) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x34);
    }
    err = s7_read_tpkt(ctx, &cotp, &cotp_len);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_s7_cotp_split(cotp, cotp_len, &pdu_type, &pdu_code, NULL, NULL,
                            NULL);
    if (err != NCL_OK) {
        return err;
    }
    if (pdu_type != NCL_S7_COTP_CC) {
        /* A disconnect request or anything else means the PLC did not accept
         * the rack/slot we asked for. */
        return NCL_DRV_ERR_PROTOCOL(0x0B);
    }
    if (pdu_code != 0) {
        /* The device's own ceiling; our setup request stays below it. */
        ctx->pdu_size = pdu_code == NCL_S7_PDU_128   ? 128
                        : pdu_code == NCL_S7_PDU_256 ? 256
                        : pdu_code == NCL_S7_PDU_512 ? 512
                        : pdu_code == NCL_S7_PDU_1024 ? 1024
                        : pdu_code == NCL_S7_PDU_2048 ? 2048
                                                      : 480;
    }
    return NCL_OK;
}

/** Setup Communication (0xF0): agree on the PDU size before any var access. */
static ncl_err s7_setup_communication(s7_ctx *ctx)
{
    uint8_t params[8];
    uint8_t pdu[32];
    const uint8_t *cotp = NULL;
    size_t cotp_len = 0;
    const uint8_t *s7 = NULL;
    size_t s7_len = 0;
    ncl_s7_view view;
    uint16_t pdu_size = 0;
    size_t pdu_len;
    size_t at;
    ncl_err err;

    at = ncl_s7_setup_params(params, sizeof(params), 1, 1, ctx->want_pdu);
    if (at == 0) {
        return NCL_ERR_RANGE;
    }
    pdu_len = ncl_s7_pdu(pdu, sizeof(pdu), NCL_S7_ROSCTR_JOB, ++ctx->pdu_ref,
                         params, at, NULL, 0);
    if (pdu_len == 0) {
        return NCL_ERR_RANGE;
    }
    err = s7_send_pdu(ctx, pdu, pdu_len);
    if (err != NCL_OK) {
        return err;
    }
    err = s7_read_tpkt(ctx, &cotp, &cotp_len);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_s7_cotp_split(cotp, cotp_len, NULL, NULL, &s7, &s7_len, NULL);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_s7_pdu_split(s7, s7_len, &view, &pdu_len);
    if (err != NCL_OK) {
        return err;
    }
    if (view.rosctr != NCL_S7_ROSCTR_ACK_DATA) {
        return NCL_DRV_ERR_PROTOCOL(0x0C);
    }
    err = ncl_s7_setup_reply(view.params, view.params_len, &pdu_size);
    if (err != NCL_OK) {
        return err;
    }
    if (pdu_size < 240 || pdu_size > 960) {
        return NCL_DRV_ERR_PROTOCOL(0x0D); /* outside what the standard allows */
    }
    if (ctx->pdu_size != 0 && pdu_size > ctx->pdu_size) {
        pdu_size = ctx->pdu_size; /* keep the device's own ceiling */
    }
    ctx->pdu_size = pdu_size;
    return NCL_OK;
}

static ncl_err s7_open_session(s7_ctx *ctx)
{
    char err[256];
    ncl_err result;

    if (ctx->socket != NULL) {
        return NCL_OK;
    }
    err[0] = '\0';
    ctx->socket = ncl_socket_connect(ctx->host, ctx->port, ctx->connect_timeout_ms,
                                     err, sizeof(err));
    if (ctx->socket == NULL) {
        return NCL_DRV_ERR_TRANSPORT(0x10);
    }
    result = s7_cotp_connect(ctx);
    if (result == NCL_OK) {
        result = s7_setup_communication(ctx);
    }
    if (result != NCL_OK) {
        s7_close_session(ctx);
        return result;
    }
    return NCL_OK;
}

/* -------------------------------------------------------------- exchange -- */

/**
 * Send one Job PDU and return the Ack-Data view. The reply's PDU reference is
 * checked, which is what keeps a stale answer from being read as data (§7.5).
 */
static ncl_err s7_exchange(s7_ctx *ctx, const uint8_t *params, size_t params_len,
                           const uint8_t *data, size_t data_len,
                           ncl_s7_view *view)
{
    uint16_t reference = ++ctx->pdu_ref;
    /* Its own buffer: the TPKT/COTP framing of s7_send_pdu() writes into
     * ctx->tx, so the PDU must not live there. */
    uint8_t pdu[S7_MAX_FRAME];
    size_t pdu_len;
    const uint8_t *cotp = NULL;
    size_t cotp_len = 0;
    const uint8_t *s7 = NULL;
    size_t s7_len = 0;
    char message[160];
    ncl_err err;

    pdu_len = ncl_s7_pdu(pdu, sizeof(pdu), NCL_S7_ROSCTR_JOB, reference, params,
                         params_len, data, data_len);
    if (pdu_len == 0) {
        return NCL_ERR_RANGE;
    }
    err = s7_send_pdu(ctx, pdu, pdu_len);
    if (err != NCL_OK) {
        return err;
    }
    err = s7_read_tpkt(ctx, &cotp, &cotp_len);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_s7_cotp_split(cotp, cotp_len, NULL, NULL, &s7, &s7_len, NULL);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_s7_pdu_split(s7, s7_len, view, NULL);
    if (err != NCL_OK) {
        return err;
    }
    if (view->rosctr != NCL_S7_ROSCTR_ACK_DATA &&
        view->rosctr != NCL_S7_ROSCTR_ACK) {
        return NCL_DRV_ERR_PROTOCOL(0x0E);
    }
    if (view->pdu_ref != reference) {
        return NCL_DRV_ERR_PROTOCOL(0x0F);
    }
    message[0] = '\0';
    return ncl_s7_check_pdu_error(view->error_class, view->error_code, message,
                                  sizeof(message));
}

static ncl_err s7_request(s7_ctx *ctx, const uint8_t *params, size_t params_len,
                          const uint8_t *data, size_t data_len,
                          ncl_s7_view *view)
{
    unsigned attempt = 0;

    for (;;) {
        ncl_err err = s7_exchange(ctx, params, params_len, data, data_len, view);

        if (ncl_driver_error_tier(err) != 1 || attempt >= ctx->retries) {
            return err;
        }
        attempt++;
        /* A lost link must be re-established, handshake included. */
        s7_close_session(ctx);
        if (s7_open_session(ctx) != NCL_OK) {
            return NCL_DRV_ERR_TRANSPORT(0x35);
        }
    }
}

/* ---------------------------------------------------------------- read ---- */

/** How many items of one Read Var fit in the negotiated PDU. */
static size_t s7_items_per_request(const s7_ctx *ctx)
{
    size_t overhead = 7 + 10 + 2; /* TPKT+COTP + S7 header + function/count */
    size_t room = ctx->pdu_size > overhead ? ctx->pdu_size - overhead : 0;
    size_t items = room / NCL_S7_ITEM_BYTES;

    if (items == 0) {
        items = 1;
    }
    return items > S7_MAX_ITEMS ? S7_MAX_ITEMS : items;
}

/** Read one group of items into @p slots (one element each). */
static ncl_err s7_read_group(s7_ctx *ctx, const ncl_address *addresses,
                             const size_t *indexes, size_t count,
                             ncl_json **slots)
{
    ncl_s7_item items[S7_MAX_ITEMS];
    size_t byte_len[S7_MAX_ITEMS];
    uint8_t params[2 + S7_MAX_ITEMS * NCL_S7_ITEM_BYTES];
    ncl_s7_view view;
    ncl_s7_reply_item replies[S7_MAX_ITEMS];
    size_t params_len;
    size_t i;
    ncl_err err;

    for (i = 0; i < count; i++) {
        if (!ncl_s7_item_for(&addresses[indexes[i]], &items[i], &byte_len[i])) {
            return NCL_DRV_ERR_BUSINESS(0x50); /* unusable area or type */
        }
    }
    params_len = ncl_s7_var_params(params, sizeof(params), NCL_S7_FUNC_READ_VAR,
                                   items, count);
    if (params_len == 0) {
        return NCL_ERR_RANGE;
    }
    memset(&view, 0, sizeof(view));
    memset(replies, 0, sizeof(replies));
    err = s7_request(ctx, params, params_len, NULL, 0, &view);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_s7_read_reply(view.data, view.data_len, count, replies, NULL);
    if (err != NCL_OK) {
        return err;
    }
    for (i = 0; i < count; i++) {
        const ncl_address *address = &addresses[indexes[i]];
        char message[160];
        ncl_err item_err;

        message[0] = '\0';
        item_err = ncl_s7_check_return_code(replies[i].return_code, message,
                                            sizeof(message));
        if (item_err != NCL_OK) {
            return item_err; /* one bad item fails the group, as snap7 does */
        }
        {
            ncl_json *value = NULL;

            err = ncl_s7_decode(replies[i].bytes, replies[i].byte_len,
                                0,
                                ncl_s7_effective_dtype(address),
                                (size_t)address->length, &value);
            if (err != NCL_OK) {
                return err;
            }
            ncl_json_free(slots[indexes[i]]);
            slots[indexes[i]] = value;
        }
    }
    return NCL_OK;
}

static ncl_err s7_read_batch(ncl_driver *self, const ncl_address *addresses,
                             size_t count, ncl_json **values)
{
    s7_ctx *ctx = (s7_ctx *)self->ctx;
    ncl_json **slots = NULL;
    ncl_json *array = ncl_json_new_array();
    size_t per_request = s7_items_per_request(ctx);
    ncl_err result = NCL_OK;
    size_t done = 0;
    size_t i;

    if (values == NULL || (count > 0 && addresses == NULL)) {
        ncl_json_free(array);
        return NCL_ERR_INVALID_ARG;
    }
    *values = NULL;
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    if (count == 0) {
        *values = array;
        return NCL_OK;
    }
    slots = (ncl_json **)ncl_mem_calloc(count, sizeof(*slots));
    if (slots == NULL) {
        ncl_json_free(array);
        return NCL_ERR_NOMEM;
    }
    /* One Read Var per PDU-sized group of items. */
    while (done < count && result == NCL_OK) {
        size_t group = count - done;
        size_t *indexes = (size_t *)ncl_mem_calloc(group, sizeof(*indexes));

        if (indexes == NULL) {
            result = NCL_ERR_NOMEM;
            break;
        }
        if (group > per_request) {
            group = per_request;
        }
        for (i = 0; i < group; i++) {
            indexes[i] = done + i;
        }
        result = s7_read_group(ctx, addresses, indexes, group, slots);
        ncl_free_safe(indexes);
        done += group;
    }
    if (result == NCL_OK) {
        for (i = 0; i < count; i++) {
            (void)ncl_json_arr_push(
                array, slots[i] != NULL ? slots[i] : ncl_json_new_int(0));
            slots[i] = NULL;
        }
        *values = array;
        array = NULL;
    }
    if (slots != NULL) {
        for (i = 0; i < count; i++) {
            ncl_json_free(slots[i]);
        }
    }
    ncl_free_safe(slots);
    ncl_json_free(array);
    return result;
}

/* --------------------------------------------------------------- write ---- */

static ncl_err s7_write_one(s7_ctx *ctx, const ncl_address *address,
                            const ncl_json *value)
{
    ncl_s7_item item;
    uint8_t params[2 + NCL_S7_ITEM_BYTES];
    uint8_t data[S7_MAX_FRAME];
    uint8_t bytes[512];
    size_t expected = 0;
    size_t params_len;
    size_t data_len;
    size_t used = 0;
    uint16_t bits;
    ncl_s7_view view;
    ncl_err err;
    size_t i;

    if (!ncl_s7_item_for(address, &item, &expected)) {
        return NCL_DRV_ERR_BUSINESS(0x50);
    }
    if (value == NULL || expected > sizeof(bytes)) {
        return NCL_ERR_INVALID_VALUE;
    }
    if (address->dtype == NCL_DTYPE_STRING) {
        err = ncl_s7_encode(value, NCL_DTYPE_STRING, (size_t)address->length,
                            bytes, sizeof(bytes), &used);
        if (err != NCL_OK) {
            return err;
        }
        bits = (uint16_t)(used * 8u);
    } else {
        /* One element at a time: the point map may ask for a run of them. */
        ncl_dtype type = ncl_s7_effective_dtype(address);
        bool is_bit = type == NCL_DTYPE_BIT;
        size_t element_bytes =
            is_bit ? 1u : ncl_s7_image_bytes(type, 1);

        for (i = 0; i < (size_t)address->length; i++) {
            const ncl_json *one = address->length > 1
                                      ? ncl_json_arr_get(value, i)
                                      : value;
            size_t written = 0;

            if (one == NULL || used + element_bytes > sizeof(bytes)) {
                return NCL_ERR_INVALID_VALUE;
            }
            err = ncl_s7_encode(one, type, 0, bytes + used, sizeof(bytes) - used,
                                &written);
            if (err != NCL_OK) {
                return err;
            }
            used += written;
        }
        bits = is_bit ? (uint16_t)address->length : (uint16_t)(used * 8u);
    }
    params_len = ncl_s7_var_params(params, sizeof(params), NCL_S7_FUNC_WRITE_VAR,
                                   &item, 1);
    if (params_len == 0) {
        return NCL_ERR_RANGE;
    }
    /*
     * The write data item numbers its transport size the way the reply does
     * (0x03 bit, 0x04 byte oriented), not the way the S7ANY item does, so a
     * bit write is not confused with a byte write.
     */
    data_len = ncl_s7_write_data(data, sizeof(data),
                                 item.tsize == NCL_S7_TS_BIT
                                     ? NCL_S7_TS_REPLY_BIT
                                     : NCL_S7_TS_REPLY_BYTE,
                                 bits, bytes, used);
    if (data_len == 0) {
        return NCL_ERR_RANGE;
    }
    memset(&view, 0, sizeof(view));
    err = s7_request(ctx, params, params_len, data, data_len, &view);
    if (err != NCL_OK) {
        return err;
    }
    /* A Write Var answers with one return code per item in the data area. */
    if (view.data_len >= 1) {
        char message[160];

        message[0] = '\0';
        return ncl_s7_check_return_code(view.data[0], message, sizeof(message));
    }
    return NCL_OK;
}

static ncl_err s7_write_batch(ncl_driver *self, const ncl_address *addresses,
                              const ncl_json *values, size_t count)
{
    s7_ctx *ctx = (s7_ctx *)self->ctx;
    size_t i;

    if (addresses == NULL || values == NULL ||
        ncl_json_type_of(values) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(values) != count) {
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < count; i++) {
        ncl_err err = s7_write_one(ctx, &addresses[i],
                                   ncl_json_arr_get(values, i));

        if (err != NCL_OK) {
            return err;
        }
    }
    return NCL_OK;
}

/* ------------------------------------------------------------ raw / call -- */

/** Raw access: "function + parameters" or "function + params + data". */
static ncl_err s7_raw(ncl_driver *self, const void *frame, size_t frame_len,
                      ncl_driver_result *out)
{
    static const char kDigits[] = "0123456789abcdef";
    s7_ctx *ctx = (s7_ctx *)self->ctx;
    const uint8_t *bytes = (const uint8_t *)frame;
    size_t params_len;
    ncl_s7_view view;
    ncl_err err;
    char *hex;
    size_t i;

    if (frame == NULL || frame_len < 3 || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    /* The caller says how much of the given bytes is parameters: the first
     * byte is the function, the second is the parameter length. */
    params_len = bytes[1];
    if (params_len + 2u > frame_len) {
        return NCL_ERR_INVALID_ARG;
    }
    memset(&view, 0, sizeof(view));
    err = s7_request(ctx, bytes, params_len, bytes + 2 + params_len,
                     frame_len - 2 - params_len, &view);
    if (err != NCL_OK) {
        return err;
    }
    ncl_driver_result_set_raw(out, view.data, view.data_len);
    hex = (char *)ncl_mem_alloc(view.data_len * 2u + 1u);
    if (hex == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < view.data_len; i++) {
        hex[i * 2] = kDigits[(view.data[i] >> 4) & 0xF];
        hex[i * 2 + 1] = kDigits[view.data[i] & 0xF];
    }
    hex[view.data_len * 2] = '\0';
    ncl_driver_result_ok(out, ncl_json_new_string(hex));
    ncl_free_safe(hex);
    return NCL_OK;
}

static ncl_err s7_read_raw(ncl_driver *self, const void *frame, size_t frame_len,
                           ncl_driver_result *out)
{
    return s7_raw(self, frame, frame_len, out);
}

static ncl_err s7_write_raw(ncl_driver *self, const void *frame, size_t frame_len,
                            ncl_driver_result *out)
{
    return s7_raw(self, frame, frame_len, out);
}

/** The frames of the last exchange, for the audit trail (§6). */
static void s7_last_raw(const ncl_driver *self, ncl_driver_raw *out)
{
    const s7_ctx *ctx = (const s7_ctx *)self->ctx;

    out->request = ctx->last_tx_len > 0 ? ctx->tx : NULL;
    out->request_len = ctx->last_tx_len;
    out->reply = ctx->last_rx_len > 0 ? ctx->rx : NULL;
    out->reply_len = ctx->last_rx_len;
}

/**
 * PLC Stop (0x29) is the one controller command worth exposing, and it is a
 * dangerous one: the point map has to ask for it explicitly by name.
 */
static ncl_err s7_call(ncl_driver *self, const char *operation,
                       const ncl_json *params, ncl_json **result)
{
    s7_ctx *ctx = (s7_ctx *)self->ctx;
    uint8_t pdu_params[16];
    ncl_s7_view view;
    ncl_err err;

    (void)params;
    if (result != NULL) {
        *result = NULL;
    }
    if (!ncl_streq_ignore_case(operation, "stop")) {
        return NCL_DRV_ERR_PROTOCOL(0x10); /* no such operation */
    }
    /* PLC Stop: function 0x29, then "delete block" / "no block" markers. */
    pdu_params[0] = NCL_S7_FUNC_PLC_STOP;
    pdu_params[1] = 0x00; /* the following length     */
    pdu_params[2] = 0x00; /* the block to delete: none */
    pdu_params[3] = 0x00;
    memset(&view, 0, sizeof(view));
    ncl_mutex_lock(ctx->mutex);
    err = s7_request(ctx, pdu_params, 3, NULL, 0, &view);
    ncl_mutex_unlock(ctx->mutex);
    if (err != NCL_OK) {
        return err;
    }
    if (result != NULL) {
        *result = ncl_json_new_bool(true);
    }
    return NCL_OK;
}

/* ---------------------------------------------------------------- driver -- */

static ncl_err s7_create(ncl_driver *self, const ncl_json *parameters)
{
    s7_ctx *ctx = (s7_ctx *)self->ctx;
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
    ctx->rack = json_uint(parameters, "rack", ctx->rack);
    ctx->slot = json_uint(parameters, "slot", ctx->slot);
    ctx->want_pdu = (uint16_t)json_uint(parameters, "pduSize", ctx->want_pdu);
    ctx->connect_timeout_ms = json_uint(parameters, "connectTimeoutMs",
                                        ctx->connect_timeout_ms);
    ctx->timeout_ms = json_uint(parameters, "timeoutMs", ctx->timeout_ms);
    ctx->retries = json_uint(parameters, "retries", ctx->retries);
    if (ctx->want_pdu < 240 || ctx->want_pdu > 960) {
        return NCL_ERR_INVALID_ARG; /* §1: the standard range */
    }
    if (ctx->host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return NCL_OK;
}

static ncl_err s7_open(ncl_driver *self)
{
    s7_ctx *ctx = (s7_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = s7_open_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void s7_close(ncl_driver *self)
{
    s7_ctx *ctx = (s7_ctx *)self->ctx;

    ncl_mutex_lock(ctx->mutex);
    s7_close_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
}

static bool s7_is_connected(const ncl_driver *self)
{
    return ((const s7_ctx *)self->ctx)->socket != NULL;
}

static ncl_err s7_read_batch_locked(ncl_driver *self,
                                    const ncl_address *addresses, size_t count,
                                    ncl_json **values)
{
    s7_ctx *ctx = (s7_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = s7_read_batch(self, addresses, count, values);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static ncl_err s7_write_batch_locked(ncl_driver *self,
                                     const ncl_address *addresses,
                                     const ncl_json *values, size_t count)
{
    s7_ctx *ctx = (s7_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = s7_write_batch(self, addresses, values, count);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void s7_attach_event(ncl_driver *self, ncl_driver_event_fn fn, void *user)
{
    /* Diagnostics and alarms are read from SZL or the error log, on demand. */
    (void)self;
    (void)fn;
    (void)user;
}

static void s7_destroy(ncl_driver *self)
{
    s7_ctx *ctx;

    if (self == NULL) {
        return;
    }
    ctx = (s7_ctx *)self->ctx;
    if (ctx != NULL) {
        s7_close_session(ctx);
        ncl_free_safe(ctx->host);
        if (ctx->mutex != NULL) {
            ncl_mutex_destroy(ctx->mutex);
        }
        ncl_free_safe(ctx);
    }
    ncl_free_safe(self);
}

static const ncl_driver_ops kS7Ops = {
    "s7_tcp",        s7_create,
    s7_open,         s7_close,
    s7_is_connected, s7_read_batch_locked,
    s7_write_batch_locked, s7_read_raw,
    s7_write_raw,    s7_call,
    s7_attach_event, s7_destroy,
    s7_last_raw,
};

ncl_driver *ncl_s7_tcp_create(void)
{
    s7_ctx *ctx = (s7_ctx *)ncl_mem_calloc(1, sizeof(*ctx));
    ncl_driver *driver;

    if (ctx == NULL) {
        return NULL;
    }
    ctx->port = 102;
    ctx->rack = 0;
    ctx->slot = 2; /* S7-300/400; set 1 for S7-1200/1500 (§7.6) */
    ctx->want_pdu = 960;
    ctx->connect_timeout_ms = 3000;
    ctx->timeout_ms = 1000;
    ctx->retries = 1;
    ctx->mutex = ncl_mutex_create();
    if (ctx->mutex == NULL) {
        ncl_free_safe(ctx);
        return NULL;
    }
    driver = ncl_driver_new(&kS7Ops, ctx);
    if (driver == NULL) {
        ncl_mutex_destroy(ctx->mutex);
        ncl_free_safe(ctx);
        return NULL;
    }
    return driver;
}
