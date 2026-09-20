/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Omron FINS over TCP.
 *
 * The shape is the one the Modbus and MC drivers share: one session, one
 * request in flight, a merged read plan, a decode pass. FINS adds the node
 * address allocation handshake a TCP link has to do before its first frame.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink/clients/fins.h"
#include "fins/ncl_fins_driver.h"

#define FINS_MAX_FRAME 2100
#define FINS_MAX_ITEMS 2048
#define FINS_MAX_WORDS 999 /* §8.6 */

typedef struct {
    char           *host;
    unsigned        port;
    uint32_t        client_node;
    bool            has_da1;
    uint8_t         da1;
    bool            has_sa1;
    uint8_t         sa1;
    ncl_fins_header header;
    ncl_socket     *socket;
    unsigned        connect_timeout_ms;
    unsigned        timeout_ms;
    unsigned        retries;
    unsigned        merge_gap;
    ncl_mutex      *mutex;
    uint8_t         tx[FINS_MAX_FRAME];
    uint8_t         rx[FINS_MAX_FRAME];
    size_t          last_tx_len; /**< the frame the audit should show */
    size_t          last_rx_len;
} fins_ctx;

static unsigned json_uint(const ncl_json *object, const char *key,
                          unsigned fallback)
{
    long long value = ncl_json_obj_get_int(object, key, (long long)fallback);

    return value < 0 ? 0u : (unsigned)value;
}

/* --------------------------------------------------------------- session -- */

static void fins_close_session(fins_ctx *ctx)
{
    if (ctx->socket != NULL) {
        ncl_socket_close(ctx->socket);
        ctx->socket = NULL;
    }
}

/**
 * Read one transport frame: the 16 byte header announces how much follows.
 * *payload points into the driver's receive buffer.
 */
static ncl_err fins_read_frame(fins_ctx *ctx, uint32_t *command,
                               uint32_t *error, const uint8_t **payload,
                               size_t *payload_len)
{
    size_t length;

    if (ncl_socket_recv_exact(ctx->socket, ctx->rx, 16, ctx->timeout_ms) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x21);
    }
    length = ((size_t)ctx->rx[4] << 24) | ((size_t)ctx->rx[5] << 16) |
             ((size_t)ctx->rx[6] << 8) | ctx->rx[7];
    if (length < 8 || 8u + length > sizeof(ctx->rx)) {
        return NCL_DRV_ERR_PROTOCOL(0x0A);
    }
    if (length > 8 &&
        ncl_socket_recv_exact(ctx->socket, ctx->rx + 16, length - 8u,
                              ctx->timeout_ms) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x22);
    }
    ctx->last_rx_len = 8u + length;
    return ncl_fins_tcp_split(ctx->rx, 8u + length, command, error, payload,
                              payload_len, NULL);
}

/** The handshake of §2: ask for a node number and keep what comes back. */
static ncl_err fins_handshake(fins_ctx *ctx)
{
    uint32_t command = 0;
    uint32_t error = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    uint8_t client_node = 0;
    uint8_t server_node = 0;
    size_t frame_len;
    ncl_err err;

    frame_len = ncl_fins_node_request(ctx->tx, sizeof(ctx->tx), ctx->client_node);
    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    ctx->last_tx_len = frame_len;
    ctx->last_rx_len = 0;
    if (ncl_socket_send(ctx->socket, ctx->tx, frame_len) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x23);
    }
    err = fins_read_frame(ctx, &command, &error, &payload, &payload_len);
    if (err != NCL_OK) {
        return err;
    }
    if (command != NCL_FINS_TCP_NODE_ADDRESS || error != 0) {
        return NCL_DRV_ERR_PROTOCOL(0x0B); /* the PLC refused the handshake */
    }
    err = ncl_fins_node_response(payload, payload_len, &client_node, &server_node);
    if (err != NCL_OK) {
        return err;
    }
    if (!ctx->has_da1) {
        ctx->header.da1 = server_node;
    }
    if (!ctx->has_sa1) {
        ctx->header.sa1 = client_node;
    }
    return NCL_OK;
}

static ncl_err fins_open_session(fins_ctx *ctx)
{
    char err[256];

    if (ctx->socket != NULL) {
        return NCL_OK;
    }
    err[0] = '\0';
    ctx->socket = ncl_socket_connect(ctx->host, ctx->port, ctx->connect_timeout_ms,
                                     err, sizeof(err));
    if (ctx->socket == NULL) {
        return NCL_DRV_ERR_TRANSPORT(0x10);
    }
    {
        ncl_err handshake = fins_handshake(ctx);

        if (handshake != NCL_OK) {
            fins_close_session(ctx);
            return handshake;
        }
    }
    return NCL_OK;
}

/* -------------------------------------------------------------- exchange -- */

/**
 * Send one command and check the reply's end code. On success @p body points at
 * the bytes after the three reply header bytes (end code, MRES, SRES).
 */
static ncl_err fins_exchange(fins_ctx *ctx, uint16_t command,
                             const uint8_t *data, size_t data_len,
                             const uint8_t **body, size_t *body_len)
{
    uint32_t tcp_command = 0;
    uint32_t tcp_error = 0;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    ncl_fins_header reply_header;
    uint16_t reply_command = 0;
    size_t frame_len;
    size_t fins_len;
    ncl_err err;
    char message[160];

    fins_len = ncl_fins_frame(ctx->tx + 20, sizeof(ctx->tx) - 20, &ctx->header,
                              command, data, data_len);
    if (fins_len == 0) {
        return NCL_ERR_RANGE;
    }
    frame_len = ncl_fins_tcp_frame(ctx->tx, sizeof(ctx->tx), NCL_FINS_TCP_DATA_SEND,
                                   ctx->tx + 20, fins_len);
    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    ctx->last_tx_len = frame_len;
    ctx->last_rx_len = 0;
    if (ncl_socket_send(ctx->socket, ctx->tx, frame_len) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x24);
    }
    err = fins_read_frame(ctx, &tcp_command, &tcp_error, &payload, &payload_len);
    if (err != NCL_OK) {
        return err;
    }
    if (tcp_command != NCL_FINS_TCP_DATA_SEND) {
        /* A terminate or error frame means the PLC closed the logical link. */
        fins_close_session(ctx);
        return NCL_DRV_ERR_TRANSPORT(0x25);
    }
    if (tcp_error != 0) {
        return NCL_DRV_ERR_PROTOCOL(0x0C);
    }
    err = ncl_fins_split(payload, payload_len, &reply_header, &reply_command, body,
                         body_len, NULL);
    if (err != NCL_OK) {
        return err;
    }
    if (reply_command != command) {
        return NCL_DRV_ERR_PROTOCOL(0x0D); /* an answer to something else */
    }
    message[0] = '\0';
    return ncl_fins_reply_begin(*body, *body_len, body, body_len, message,
                                sizeof(message));
}

static ncl_err fins_request(fins_ctx *ctx, uint16_t command,
                            const uint8_t *data, size_t data_len,
                            const uint8_t **body, size_t *body_len)
{
    unsigned attempt = 0;

    for (;;) {
        ncl_err err = fins_exchange(ctx, command, data, data_len, body, body_len);

        if (ncl_driver_error_tier(err) != 1 || attempt >= ctx->retries) {
            return err;
        }
        attempt++;
        fins_close_session(ctx);
        if (fins_open_session(ctx) != NCL_OK) {
            return NCL_DRV_ERR_TRANSPORT(0x26);
        }
    }
}

/* ---------------------------------------------------------------- items --- */

typedef struct {
    size_t    address_index;
    size_t    element;
    uint16_t  offset;
    size_t    text_chars;
    ncl_dtype dtype;
    bool      bits;
} fins_item;

typedef struct {
    uint16_t start;
    uint16_t points;
} fins_span;

static int compare_items(const void *a, const void *b)
{
    const fins_item *left = (const fins_item *)a;
    const fins_item *right = (const fins_item *)b;

    if (left->offset != right->offset) {
        return left->offset < right->offset ? -1 : 1;
    }
    return left->element < right->element ? -1 : (left->element > right->element);
}

/** A point is read in bit access when it is a bit or names one. */
static bool wants_bits(const ncl_address *address)
{
    if (address->dtype == NCL_DTYPE_BIT) {
        return true;
    }
    if (address->bit >= 0) {
        return true; /* a bit of a word area is a bit access too */
    }
    return false;
}

static uint8_t element_bit(const ncl_address *address)
{
    return address->bit >= 0 ? (uint8_t)address->bit : 0;
}

static uint16_t element_points(const ncl_address *address, bool bits, bool *ok)
{
    if (bits) {
        return (uint16_t)address->length;
    }
    if (address->dtype == NCL_DTYPE_STRING) {
        return (uint16_t)(((size_t)address->length + 1u) / 2u);
    }
    {
        size_t width = ncl_fins_element_bytes(address->dtype, 0) / 2u;

        if (width == 0) {
            *ok = false;
            return 0;
        }
        return (uint16_t)(width * (size_t)address->length);
    }
}

/**
 * How many items the batch can expand to: a string address is one item,
 * anything else is one item per element. Sizing the scratch array from the
 * batch instead of always reserving FINS_MAX_ITEMS keeps a three point read
 * from taking 80 KiB out of a device's pool (2048 * 40 bytes); FINS_MAX_ITEMS
 * is still the ceiling, and build_items refuses an oversized batch with its own
 * error code rather than writing past what was allocated.
 */
static size_t batch_capacity(const ncl_address *addresses, size_t count)
{
    size_t total = 0;
    size_t i;

    for (i = 0; i < count; i++) {
        size_t width = addresses[i].dtype == NCL_DTYPE_STRING
                           ? 1u
                           : (size_t)addresses[i].length;

        if (width > FINS_MAX_ITEMS - total) {
            return FINS_MAX_ITEMS;
        }
        total += width;
    }
    return total == 0u ? 1u : total;
}

static ncl_err build_items(const ncl_address *addresses, size_t count,
                           fins_item *items, size_t capacity, size_t *item_count,
                           const char *area_name, bool want_bits)
{
    size_t i;
    size_t used = 0;

    for (i = 0; i < count; i++) {
        size_t element;
        bool bits;
        bool ok = true;
        uint16_t points;

        if (!ncl_streq_ignore_case(addresses[i].area, area_name)) {
            continue;
        }
        bits = wants_bits(&addresses[i]);
        if (bits != want_bits) {
            continue;
        }
        if (addresses[i].offset < 0 || addresses[i].offset > 0xFFFF) {
            return NCL_DRV_ERR_BUSINESS(0x40);
        }
        if (addresses[i].dtype == NCL_DTYPE_STRING) {
            if (used + 1 > capacity) {
                return NCL_ERR_RANGE;
            }
            items[used].address_index = i;
            items[used].element = 0;
            items[used].offset = (uint16_t)addresses[i].offset;
            items[used].text_chars = (size_t)addresses[i].length;
            items[used].dtype = NCL_DTYPE_STRING;
            items[used].bits = false;
            used++;
            continue;
        }
        points = element_points(&addresses[i], bits, &ok);
        if (!ok || used + (size_t)addresses[i].length > capacity) {
            return NCL_DRV_ERR_BUSINESS(0x41);
        }
        for (element = 0; element < (size_t)addresses[i].length; element++) {
            uint16_t step = bits ? 1
                                 : (uint16_t)(points / (uint16_t)addresses[i].length);

            items[used].address_index = i;
            items[used].element = element;
            items[used].offset =
                (uint16_t)(addresses[i].offset + (int64_t)element * step);
            items[used].text_chars = 0;
            items[used].dtype = bits ? NCL_DTYPE_BIT : addresses[i].dtype;
            items[used].bits = bits;
            used++;
        }
    }
    *item_count = used;
    return NCL_OK;
}

static void store_item(ncl_json **slots, const ncl_address *addresses,
                       const fins_item *item, ncl_json *value)
{
    if (addresses[item->address_index].length <= 1 ||
        addresses[item->address_index].dtype == NCL_DTYPE_STRING) {
        ncl_json_free(slots[item->address_index]);
        slots[item->address_index] = value;
        return;
    }
    if (slots[item->address_index] == NULL) {
        slots[item->address_index] = ncl_json_new_array();
        if (slots[item->address_index] == NULL) {
            ncl_json_free(value);
            return;
        }
    }
    (void)ncl_json_arr_push(slots[item->address_index], value);
}

/* ---------------------------------------------------------------- read ---- */

static ncl_err fins_read_group(fins_ctx *ctx, const ncl_address *addresses,
                               const fins_item *group, size_t group_count,
                               uint8_t area_code, bool bits, ncl_json **slots)
{
    fins_item *sorted;
    fins_span *spans;
    size_t span_count = 0;
    ncl_err result = NCL_OK;
    uint16_t limit = FINS_MAX_WORDS;
    size_t i;

    sorted = (fins_item *)ncl_mem_calloc(group_count, sizeof(*sorted));
    spans = (fins_span *)ncl_mem_calloc(group_count, sizeof(*spans));
    if (sorted == NULL || spans == NULL) {
        ncl_free_safe(sorted);
        ncl_free_safe(spans);
        return NCL_ERR_NOMEM;
    }
    memcpy(sorted, group, group_count * sizeof(*sorted));
    qsort(sorted, group_count, sizeof(*sorted), compare_items);
    for (i = 0; i < group_count; i++) {
        const ncl_address *address = &addresses[sorted[i].address_index];
        bool ok = true;
        uint16_t points = element_points(address, bits, &ok);

        if (!ok) {
            result = NCL_DRV_ERR_BUSINESS(0x41);
            break;
        }
        if (span_count > 0) {
            fins_span *last = &spans[span_count - 1];
            uint32_t end = (uint32_t)sorted[i].offset + points;

            if (sorted[i].offset <= last->start + last->points + ctx->merge_gap &&
                end - last->start <= limit) {
                last->points = (uint16_t)(end - last->start);
                continue;
            }
        }
        spans[span_count].start = sorted[i].offset;
        spans[span_count].points = points;
        span_count++;
    }

    for (i = 0; i < span_count && result == NCL_OK; i++) {
        uint8_t body[8];
        const uint8_t *reply = NULL;
        size_t reply_len = 0;
        const ncl_address *first = &addresses[0];
        uint8_t bit = 0;
        size_t j;

        /* The bit index travels with the head of the span. */
        for (j = 0; j < group_count; j++) {
            if (sorted[j].offset == spans[i].start) {
                first = &addresses[sorted[j].address_index];
                bit = element_bit(first);
                break;
            }
        }
        if (ncl_fins_read_body(body, sizeof(body), area_code, spans[i].start,
                               bits ? bit : 0x00, spans[i].points) == 0) {
            result = NCL_ERR_RANGE;
            break;
        }
        result = fins_request(ctx, NCL_FINS_CMD_MEMORY_READ, body, 6, &reply,
                              &reply_len);
        if (result != NCL_OK) {
            break;
        }
        for (j = 0; j < group_count; j++) {
            const fins_item *item = &sorted[j];
            const ncl_address *address = &addresses[item->address_index];
            bool ok = true;
            uint16_t points = element_points(address, bits, &ok);
            size_t bytes = ncl_fins_element_bytes(item->dtype, item->text_chars);
            size_t offset;
            ncl_json *value = NULL;
            ncl_err decode;

            if (!ok || item->offset < spans[i].start ||
                item->offset + points > spans[i].start + spans[i].points) {
                continue;
            }
            offset = bits ? (size_t)(item->offset - spans[i].start)
                          : (size_t)(item->offset - spans[i].start) * 2u;
            decode = ncl_fins_decode(reply, reply_len, offset, item->dtype,
                                     item->text_chars, &value);
            if (decode != NCL_OK) {
                result = decode;
                break;
            }
            (void)bytes;
            store_item(slots, addresses, item, value);
        }
    }
    ncl_free_safe(sorted);
    ncl_free_safe(spans);
    return result;
}

static ncl_err fins_read_batch(ncl_driver *self, const ncl_address *addresses,
                               size_t count, ncl_json **values)
{
    fins_ctx *ctx = (fins_ctx *)self->ctx;
    fins_item *items = NULL;
    ncl_json **slots = NULL;
    ncl_json *array = ncl_json_new_array();
    size_t capacity;
    ncl_err result = NCL_OK;
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
    capacity = batch_capacity(addresses, count);
    items = (fins_item *)ncl_mem_calloc(capacity, sizeof(*items));
    slots = (ncl_json **)ncl_mem_calloc(count, sizeof(*slots));
    if (items == NULL || slots == NULL) {
        result = NCL_ERR_NOMEM;
        goto done;
    }
    /* One request carries one area in one access mode. */
    for (i = 0; i < count && result == NCL_OK; i++) {
        uint8_t area_code = 0;
        bool bits;
        size_t item_count = 0;
        size_t j;
        bool handled = false;

        if (!ncl_fins_area_lookup(addresses[i].area, &area_code)) {
            result = NCL_DRV_ERR_BUSINESS(0x42); /* unknown area */
            break;
        }
        bits = wants_bits(&addresses[i]);
        for (j = 0; j < i; j++) {
            uint8_t other_code = 0;

            if (ncl_fins_area_lookup(addresses[j].area, &other_code) &&
                other_code == area_code &&
                wants_bits(&addresses[j]) == bits) {
                handled = true;
                break;
            }
        }
        if (handled) {
            continue;
        }
        result = build_items(addresses, count, items, capacity, &item_count,
                             addresses[i].area, bits);
        if (result != NCL_OK) {
            break;
        }
        result = fins_read_group(ctx, addresses, items, item_count, area_code, bits,
                                 slots);
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

done:
    if (slots != NULL) {
        for (i = 0; i < count; i++) {
            ncl_json_free(slots[i]);
        }
    }
    ncl_free_safe(slots);
    ncl_free_safe(items);
    ncl_json_free(array);
    return result;
}

/* --------------------------------------------------------------- write ---- */

static ncl_err fins_write_one(fins_ctx *ctx, const ncl_address *address,
                              const ncl_json *value)
{
    uint8_t body[FINS_MAX_FRAME];
    uint8_t payload[FINS_MAX_FRAME];
    uint8_t area_code = 0;
    bool bits;
    size_t used = 0;
    size_t i;
    uint16_t points;
    ncl_err err;

    if (!ncl_fins_area_lookup(address->area, &area_code)) {
        return NCL_DRV_ERR_BUSINESS(0x42);
    }
    if (value == NULL) {
        return NCL_ERR_INVALID_VALUE;
    }
    bits = wants_bits(address);
    if (address->dtype == NCL_DTYPE_STRING) {
        err = ncl_fins_encode(value, NCL_DTYPE_STRING, (size_t)address->length,
                              payload, sizeof(payload), &used);
        if (err != NCL_OK) {
            return err;
        }
    } else {
        for (i = 0; i < (size_t)address->length; i++) {
            const ncl_json *one = address->length > 1
                                      ? ncl_json_arr_get(value, i)
                                      : value;
            size_t written = 0;

            if (one == NULL) {
                return NCL_ERR_INVALID_VALUE;
            }
            err = ncl_fins_encode(one, bits ? NCL_DTYPE_BIT : address->dtype, 0,
                                  payload + used, sizeof(payload) - used, &written);
            if (err != NCL_OK) {
                return err;
            }
            used += written;
        }
    }
    points = bits ? (uint16_t)used : (uint16_t)((used + 1u) / 2u);
    if (points == 0 || points > FINS_MAX_WORDS) {
        return NCL_ERR_RANGE;
    }
    if (!bits && used % 2u != 0) {
        payload[used] = 0;
        used++;
    }
    if (ncl_fins_write_body(body, sizeof(body), area_code,
                            (uint16_t)address->offset,
                            bits ? element_bit(address) : 0x00, points, payload,
                            used) == 0) {
        return NCL_ERR_RANGE;
    }
    {
        const uint8_t *reply = NULL;
        size_t reply_len = 0;

        return fins_request(ctx, NCL_FINS_CMD_MEMORY_WRITE, body, 6u + used,
                            &reply, &reply_len);
    }
}

static ncl_err fins_write_batch(ncl_driver *self, const ncl_address *addresses,
                                const ncl_json *values, size_t count)
{
    fins_ctx *ctx = (fins_ctx *)self->ctx;
    size_t i;

    if (addresses == NULL || values == NULL ||
        ncl_json_type_of(values) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(values) != count) {
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < count; i++) {
        ncl_err err = fins_write_one(ctx, &addresses[i],
                                     ncl_json_arr_get(values, i));

        if (err != NCL_OK) {
            return err;
        }
    }
    return NCL_OK;
}

/* ------------------------------------------------------------ raw / call -- */

/** Raw access: "MRC SRC" followed by the command body; the answer is the reply body. */
static ncl_err fins_raw(ncl_driver *self, const void *frame, size_t frame_len,
                        ncl_driver_result *out)
{
    static const char kDigits[] = "0123456789abcdef";
    fins_ctx *ctx = (fins_ctx *)self->ctx;
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    uint16_t command;
    ncl_err err;
    char *hex;
    size_t i;

    if (frame == NULL || frame_len < 2 || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    command = (uint16_t)((((const uint8_t *)frame)[0] << 8) |
                         ((const uint8_t *)frame)[1]);
    ncl_mutex_lock(ctx->mutex);
    err = fins_request(ctx, command, (const uint8_t *)frame + 2, frame_len - 2,
                       &reply, &reply_len);
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

static ncl_err fins_read_raw(ncl_driver *self, const void *frame, size_t frame_len,
                             ncl_driver_result *out)
{
    return fins_raw(self, frame, frame_len, out);
}

static ncl_err fins_write_raw(ncl_driver *self, const void *frame, size_t frame_len,
                              ncl_driver_result *out)
{
    return fins_raw(self, frame, frame_len, out);
}

/** The frames of the last exchange, for the audit trail (§6). */
static void fins_last_raw(const ncl_driver *self, ncl_driver_raw *out)
{
    const fins_ctx *ctx = (const fins_ctx *)self->ctx;

    out->request = ctx->last_tx_len > 0 ? ctx->tx : NULL;
    out->request_len = ctx->last_tx_len;
    out->reply = ctx->last_rx_len > 0 ? ctx->rx : NULL;
    out->reply_len = ctx->last_rx_len;
}

/** The controller's own commands: RUN, STOP, status, clock and cycle time. */
static ncl_err fins_call(ncl_driver *self, const char *operation,
                         const ncl_json *params, ncl_json **result)
{
    fins_ctx *ctx = (fins_ctx *)self->ctx;
    /* RUN and STOP carry a two byte "run mode" parameter; 00 00 is the plain
     * "just do it" form the tooling sends. */
    static const uint8_t kRunMode[2] = {0x00, 0x00};
    const uint8_t *body = NULL;
    size_t body_len = 0;
    uint16_t command;
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    ncl_err err;

    (void)params;
    if (result != NULL) {
        *result = NULL;
    }
    if (ncl_streq_ignore_case(operation, "run")) {
        command = NCL_FINS_CMD_RUN;
        body = kRunMode;
        body_len = sizeof(kRunMode);
    } else if (ncl_streq_ignore_case(operation, "stop")) {
        command = NCL_FINS_CMD_STOP;
        body = kRunMode;
        body_len = sizeof(kRunMode);
    } else if (ncl_streq_ignore_case(operation, "controllerStatus")) {
        command = NCL_FINS_CMD_CONTROLLER_STATUS;
    } else if (ncl_streq_ignore_case(operation, "readClock")) {
        command = NCL_FINS_CMD_READ_CLOCK;
    } else if (ncl_streq_ignore_case(operation, "cycleTime")) {
        command = NCL_FINS_CMD_CYCLE_TIME;
    } else {
        return NCL_DRV_ERR_PROTOCOL(0x0E); /* no such operation */
    }
    ncl_mutex_lock(ctx->mutex);
    err = fins_request(ctx, command, body, body_len, &reply, &reply_len);
    ncl_mutex_unlock(ctx->mutex);
    if (err != NCL_OK) {
        return err;
    }
    if (result != NULL) {
        char *text = ncl_strndup((const char *)reply, reply_len);

        *result = ncl_json_new_object();
        if (*result == NULL) {
            ncl_free_safe(text);
            return NCL_ERR_NOMEM;
        }
        (void)ncl_json_obj_set_int(*result, "bytes", (long long)reply_len);
        if (text != NULL) {
            (void)ncl_json_obj_set_string(*result, "data", text);
            ncl_free_safe(text);
        }
    }
    return NCL_OK;
}

/* ---------------------------------------------------------------- driver -- */

static ncl_err fins_create(ncl_driver *self, const ncl_json *parameters)
{
    fins_ctx *ctx = (fins_ctx *)self->ctx;
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
    ctx->client_node = json_uint(parameters, "clientNode", ctx->client_node);
    ctx->connect_timeout_ms = json_uint(parameters, "connectTimeoutMs",
                                        ctx->connect_timeout_ms);
    ctx->timeout_ms = json_uint(parameters, "timeoutMs", ctx->timeout_ms);
    ctx->retries = json_uint(parameters, "retries", ctx->retries);
    ctx->merge_gap = json_uint(parameters, "mergeGap", ctx->merge_gap);
    ctx->header.dna = (uint8_t)json_uint(parameters, "dna", ctx->header.dna);
    ctx->header.sna = (uint8_t)json_uint(parameters, "sna", ctx->header.sna);
    ctx->header.sid = (uint8_t)json_uint(parameters, "sid", ctx->header.sid);
    if (ncl_json_obj_has(parameters, "da1")) {
        ctx->header.da1 = (uint8_t)json_uint(parameters, "da1", 0);
        ctx->has_da1 = true;
    }
    if (ncl_json_obj_has(parameters, "sa1")) {
        ctx->header.sa1 = (uint8_t)json_uint(parameters, "sa1", 0);
        ctx->has_sa1 = true;
    }
    if (ctx->host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return NCL_OK;
}

static ncl_err fins_open(ncl_driver *self)
{
    fins_ctx *ctx = (fins_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = fins_open_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void fins_close(ncl_driver *self)
{
    fins_ctx *ctx = (fins_ctx *)self->ctx;

    ncl_mutex_lock(ctx->mutex);
    fins_close_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
}

static bool fins_is_connected(const ncl_driver *self)
{
    return ((const fins_ctx *)self->ctx)->socket != NULL;
}

static ncl_err fins_read_batch_locked(ncl_driver *self,
                                      const ncl_address *addresses, size_t count,
                                      ncl_json **values)
{
    fins_ctx *ctx = (fins_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = fins_read_batch(self, addresses, count, values);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static ncl_err fins_write_batch_locked(ncl_driver *self,
                                       const ncl_address *addresses,
                                       const ncl_json *values, size_t count)
{
    fins_ctx *ctx = (fins_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = fins_write_batch(self, addresses, values, count);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void fins_attach_event(ncl_driver *self, ncl_driver_event_fn fn, void *user)
{
    /* FINS has no unsolicited traffic: the error log is read on demand. */
    (void)self;
    (void)fn;
    (void)user;
}

static void fins_destroy(ncl_driver *self)
{
    fins_ctx *ctx;

    if (self == NULL) {
        return;
    }
    ctx = (fins_ctx *)self->ctx;
    if (ctx != NULL) {
        fins_close_session(ctx);
        ncl_free_safe(ctx->host);
        if (ctx->mutex != NULL) {
            ncl_mutex_destroy(ctx->mutex);
        }
        ncl_free_safe(ctx);
    }
    ncl_free_safe(self);
}

static const ncl_driver_ops kFinsOps = {
    "fins_tcp",      fins_create,
    fins_open,       fins_close,
    fins_is_connected, fins_read_batch_locked,
    fins_write_batch_locked, fins_read_raw,
    fins_write_raw,  fins_call,
    fins_attach_event, fins_destroy,
    fins_last_raw,
};

ncl_driver *ncl_fins_tcp_create(void)
{
    fins_ctx *ctx = (fins_ctx *)ncl_mem_calloc(1, sizeof(*ctx));
    ncl_driver *driver;

    if (ctx == NULL) {
        return NULL;
    }
    ctx->port = 9600; /* §1 */
    ctx->client_node = 1;
    ctx->connect_timeout_ms = 3000;
    ctx->timeout_ms = 1000;
    ctx->retries = 1;
    ctx->merge_gap = 8;
    ncl_fins_header_default(&ctx->header);
    ctx->header.sid = 0x01;
    ctx->mutex = ncl_mutex_create();
    if (ctx->mutex == NULL) {
        ncl_free_safe(ctx);
        return NULL;
    }
    driver = ncl_driver_new(&kFinsOps, ctx);
    if (driver == NULL) {
        ncl_mutex_destroy(ctx->mutex);
        ncl_free_safe(ctx);
        return NULL;
    }
    return driver;
}
