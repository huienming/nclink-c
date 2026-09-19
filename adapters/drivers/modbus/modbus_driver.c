/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Modbus (TCP, RTU, RTU over TCP).
 *
 * A session is one socket or one serial port, and every exchange goes through
 * one mutex: a 485 bus is half duplex and even a TCP device dislikes two
 * requests in flight, so there is never more than one outstanding request
 * (15-MODBUS.md §7.7).
 *
 * read_batch merges neighbouring elements into as few requests as the function
 * code allows (125 registers, 2000 bits, a small allowed gap) and decodes every
 * element from the answer, honouring the configured register byte order.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink_adapter/ncl_modbus.h"
#include "core/serial_port.h"
#include "modbus/ncl_modbus_driver.h"

#define MB_MAX_FRAME 300
#define MB_MAX_ITEMS 2048

typedef enum {
    MB_MODE_TCP = 0,
    MB_MODE_RTU,
    MB_MODE_RTU_TCP
} mb_mode;

typedef struct {
    mb_mode               mode;
    char                 *host;
    unsigned              port;
    unsigned              unit;
    ncl_socket           *socket;
    ncl_serial           *serial;
    ncl_serial_options    serial_options;
    char                 *serial_device;
    unsigned              connect_timeout_ms;
    unsigned              timeout_ms;
    unsigned              retries;
    unsigned              merge_gap;
    bool                  base_one;
    ncl_modbus_word_order order;
    uint16_t              transaction;
    ncl_mutex            *mutex; /**< one exchange at a time */
    uint8_t               tx[MB_MAX_FRAME];
    uint8_t               rx[MB_MAX_FRAME];
} mb_ctx;

static unsigned json_uint(const ncl_json *object, const char *key,
                          unsigned fallback)
{
    long long value = ncl_json_obj_get_int(object, key, (long long)fallback);

    return value < 0 ? 0u : (unsigned)value;
}

/* --------------------------------------------------------------- session -- */

static void mb_close_session(mb_ctx *ctx)
{
    if (ctx->socket != NULL) {
        ncl_socket_close(ctx->socket);
        ctx->socket = NULL;
    }
    if (ctx->serial != NULL) {
        ncl_serial_close(ctx->serial);
        ctx->serial = NULL;
    }
}

static ncl_err mb_open_session(mb_ctx *ctx)
{
    char err[256];

    if (ctx->socket != NULL || ctx->serial != NULL) {
        return NCL_OK;
    }
    err[0] = '\0';
    if (ctx->mode == MB_MODE_RTU) {
        ctx->serial = ncl_serial_open(&ctx->serial_options, err, sizeof(err));
        return ctx->serial != NULL ? NCL_OK : NCL_DRV_ERR_TRANSPORT(0x10);
    }
    ctx->socket = ncl_socket_connect(ctx->host, ctx->port, ctx->connect_timeout_ms,
                                     err, sizeof(err));
    return ctx->socket != NULL ? NCL_OK : NCL_DRV_ERR_TRANSPORT(0x11);
}

/* ------------------------------------------------------------- RTU read -- */

/**
 * Collect one RTU reply. RTU has no length field, so the reply is gathered in
 * steps: two bytes say how long it is (or that it is the 5 byte exception
 * form), and the rest follows. A quiet line ends the collection, which is what
 * makes a USB converter that hands the frame over in pieces work (§7.5).
 */
static ncl_err mb_rtu_read_reply(mb_ctx *ctx, const uint8_t **reply,
                                 size_t *reply_len)
{
    size_t got = 0;
    int64_t deadline = ncl_time_monotonic_millis() + ctx->timeout_ms;

    *reply = NULL;
    *reply_len = 0;
    for (;;) {
        size_t total = 0;
        int64_t now = ncl_time_monotonic_millis();
        int rc;

        if (got >= 3) {
            ncl_err err = ncl_modbus_rtu_reply_size(ctx->rx, got, &total);

            if (err != NCL_OK && err != NCL_ERR_RANGE) {
                return err; /* a function code we never asked for */
            }
            if (total != 0 && got >= total) {
                break;
            }
        }
        if (now >= deadline) {
            return NCL_DRV_ERR_TRANSPORT(0x12); /* nothing arrived in time */
        }
        if (total > sizeof(ctx->rx)) {
            return NCL_DRV_ERR_PROTOCOL(0x0C);
        }
        rc = ncl_serial_read(ctx->serial, ctx->rx + got,
                             (total != 0 ? total : got + 1u) - got,
                             (unsigned)(deadline - now));
        if (rc < 0) {
            return NCL_DRV_ERR_TRANSPORT(0x13);
        }
        if (rc == 0) {
            if (got == 0) {
                continue; /* the first byte has not arrived yet */
            }
            if (got < 5) {
                return NCL_DRV_ERR_TRANSPORT(0x14); /* shorter than any reply */
            }
            break; /* the line went quiet: that is the frame */
        }
        got += (size_t)rc;
    }
    return ncl_modbus_rtu_split(ctx->rx, got, (uint8_t)ctx->unit, reply,
                                reply_len, NULL);
}

/* -------------------------------------------------------------- exchange -- */

/**
 * One request/response. On success @p reply points into the driver's receive
 * buffer and stays valid until the next exchange; it is NULL for a broadcast.
 */
static ncl_err mb_exchange(mb_ctx *ctx, const uint8_t *pdu, size_t pdu_len,
                           const uint8_t **reply, size_t *reply_len)
{
    size_t frame_len;

    *reply = NULL;
    *reply_len = 0;

    if (ctx->mode == MB_MODE_RTU) {
        frame_len = ncl_modbus_rtu_frame(ctx->tx, sizeof(ctx->tx),
                                         (uint8_t)ctx->unit, pdu, pdu_len);
        if (frame_len == 0) {
            return NCL_ERR_RANGE;
        }
        if (ncl_serial_write(ctx->serial, ctx->tx, frame_len) != NCL_OK) {
            return NCL_DRV_ERR_TRANSPORT(0x15);
        }
        if (ctx->unit == 0) {
            return NCL_OK; /* broadcast: no device answers (§7.6) */
        }
        return mb_rtu_read_reply(ctx, reply, reply_len);
    }

    if (ctx->mode == MB_MODE_RTU_TCP) {
        size_t got = 0;

        frame_len = ncl_modbus_rtu_frame(ctx->tx, sizeof(ctx->tx),
                                         (uint8_t)ctx->unit, pdu, pdu_len);
        if (frame_len == 0) {
            return NCL_ERR_RANGE;
        }
        if (ncl_socket_send(ctx->socket, ctx->tx, frame_len) != NCL_OK) {
            return NCL_DRV_ERR_TRANSPORT(0x16);
        }
        if (ctx->unit == 0) {
            return NCL_OK;
        }
        for (;;) {
            size_t total = 0;
            ncl_err err;
            int rc;

            if (got >= 3) {
                err = ncl_modbus_rtu_reply_size(ctx->rx, got, &total);
                if (err != NCL_OK && err != NCL_ERR_RANGE) {
                    return err;
                }
                if (total != 0 && got >= total) {
                    break;
                }
            }
            if (total > sizeof(ctx->rx)) {
                return NCL_DRV_ERR_PROTOCOL(0x0C);
            }
            rc = ncl_socket_recv(ctx->socket, ctx->rx + got,
                                 (total != 0 ? total : got + 1u) - got,
                                 ctx->timeout_ms);
            if (rc <= 0) {
                ncl_socket_shutdown(ctx->socket);
                return NCL_DRV_ERR_TRANSPORT(0x17);
            }
            got += (size_t)rc;
        }
        return ncl_modbus_rtu_split(ctx->rx, got, (uint8_t)ctx->unit, reply,
                                    reply_len, NULL);
    }

    /* TCP: MBAP header first, then the rest of the announced length. */
    frame_len = ncl_modbus_tcp_frame(ctx->tx, sizeof(ctx->tx), ++ctx->transaction,
                                     (uint8_t)ctx->unit, pdu, pdu_len);
    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    if (ncl_socket_send(ctx->socket, ctx->tx, frame_len) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x18);
    }
    if (ncl_socket_recv_exact(ctx->socket, ctx->rx, 7, ctx->timeout_ms) != NCL_OK) {
        ncl_socket_shutdown(ctx->socket);
        return NCL_DRV_ERR_TRANSPORT(0x19);
    }
    {
        size_t length = ((size_t)ctx->rx[4] << 8) | ctx->rx[5];

        if (length < 2 || length > (MB_MAX_FRAME - 6u)) {
            ncl_socket_shutdown(ctx->socket);
            return NCL_DRV_ERR_PROTOCOL(0x0A);
        }
        if (ncl_socket_recv_exact(ctx->socket, ctx->rx + 7, length - 1u,
                                  ctx->timeout_ms) != NCL_OK) {
            ncl_socket_shutdown(ctx->socket);
            return NCL_DRV_ERR_TRANSPORT(0x1A);
        }
        return ncl_modbus_tcp_split(ctx->rx, length + 6u, ctx->transaction, reply,
                                    reply_len, NULL);
    }
}

/** Re-send on transport failures, as 15-MODBUS.md §5 asks. */
static ncl_err mb_request(mb_ctx *ctx, const uint8_t *pdu, size_t pdu_len,
                          const uint8_t **reply, size_t *reply_len)
{
    unsigned attempt = 0;

    for (;;) {
        ncl_err err = mb_exchange(ctx, pdu, pdu_len, reply, reply_len);

        if (ncl_driver_error_tier(err) != 1 || attempt >= ctx->retries) {
            return err;
        }
        attempt++;
        if (ctx->mode == MB_MODE_RTU) {
            ncl_serial_flush(ctx->serial); /* only a quiet moment is needed */
        } else {
            mb_close_session(ctx);
            if (mb_open_session(ctx) != NCL_OK) {
                return NCL_DRV_ERR_TRANSPORT(0x1B);
            }
        }
    }
}

/* ---------------------------------------------------------------- items --- */

/** One value to move: where it goes in the caller's array, and on the bus. */
typedef struct {
    size_t    address_index;
    size_t    element;
    uint32_t  offset;     /**< register / coil offset in the request  */
    uint16_t  width;      /**< registers the element occupies         */
    size_t    text_chars; /**< characters, for NCL_DTYPE_STRING       */
    ncl_dtype dtype;
} mb_item;

typedef struct {
    uint32_t start;
    uint32_t count;
} mb_span;

static bool area_is_bits(ncl_modbus_area area)
{
    return area == NCL_MB_COIL || area == NCL_MB_DISCRETE;
}

/** True when the address sits in one of the two bit areas. */
static bool address_is_bits(const ncl_address *address)
{
    ncl_modbus_area area;

    return ncl_modbus_area_parse(address->area, &area) && area_is_bits(area);
}

/** Registers (or bits) one element of @p address occupies. */
static uint16_t element_width(const ncl_address *address)
{
    unsigned registers;

    if (address->dtype == NCL_DTYPE_STRING) {
        return (uint16_t)(((size_t)address->length + 1u) / 2u);
    }
    if (address_is_bits(address)) {
        return 1;
    }
    registers = ncl_modbus_registers_per_element(address->dtype);
    return registers == 0 ? 1 : (uint16_t)registers;
}

static int compare_items(const void *a, const void *b)
{
    const mb_item *left = (const mb_item *)a;
    const mb_item *right = (const mb_item *)b;

    if (left->offset != right->offset) {
        return left->offset < right->offset ? -1 : 1;
    }
    return left->element < right->element ? -1 : (left->element > right->element);
}

/** Expand the addresses into the elements a read has to fetch. */
static ncl_err build_items(mb_ctx *ctx, const ncl_address *addresses,
                           size_t count, mb_item *items, size_t *item_count)
{
    size_t i;
    size_t used = 0;

    for (i = 0; i < count; i++) {
        if (addresses[i].dtype == NCL_DTYPE_STRING) {
            if (used + 1 > MB_MAX_ITEMS) {
                return NCL_ERR_RANGE;
            }
            items[used].address_index = i;
            items[used].element = 0;
            items[used].offset = (uint32_t)addresses[i].offset;
            items[used].width = element_width(&addresses[i]);
            items[used].text_chars = (size_t)addresses[i].length;
            items[used].dtype = NCL_DTYPE_STRING;
            used++;
            continue;
        }
        {
            size_t element;
            uint16_t width = element_width(&addresses[i]);

            for (element = 0; element < (size_t)addresses[i].length; element++) {
                if (used + 1 > MB_MAX_ITEMS) {
                    return NCL_ERR_RANGE;
                }
                items[used].address_index = i;
                items[used].element = element;
                items[used].offset =
                    (uint32_t)(addresses[i].offset + (int64_t)element * width);
                items[used].width = width;
                items[used].text_chars = 0;
                items[used].dtype = addresses[i].dtype;
                used++;
            }
        }
    }
    if (ctx->base_one) {
        for (i = 0; i < used; i++) {
            if (items[i].offset == 0) {
                return NCL_DRV_ERR_BUSINESS(0x21); /* 40000 does not exist */
            }
            items[i].offset -= 1u;
        }
    }
    *item_count = used;
    return NCL_OK;
}

/**
 * Store one decoded value: a single element becomes the scalar itself, while an
 * address asking for length > 1 collects its elements into an array. The
 * elements of one address arrive in element order, so a plain append is right.
 */
static void store_item(ncl_json **slots, const ncl_address *addresses,
                       const mb_item *item, ncl_json *value)
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

/** Read one area of the batch: merge into spans, request, decode. */
static ncl_err mb_read_area(mb_ctx *ctx, const ncl_address *addresses,
                            const mb_item *items, size_t item_count,
                            ncl_modbus_area area, ncl_json **slots)
{
    mb_item *group;
    mb_span *spans;
    size_t group_count = 0;
    size_t span_count = 0;
    uint32_t limit = ncl_modbus_area_max_read(area);
    unsigned gap;
    uint8_t function = ncl_modbus_area_read_code(area);
    bool bits = area_is_bits(area);
    ncl_err result = NCL_OK;
    size_t i;
    size_t j;

    group = (mb_item *)ncl_mem_calloc(item_count, sizeof(*group));
    spans = (mb_span *)ncl_mem_calloc(item_count, sizeof(*spans));
    if (group == NULL || spans == NULL) {
        ncl_free_safe(group);
        ncl_free_safe(spans);
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < item_count; i++) {
        ncl_modbus_area other;

        if (ncl_modbus_area_parse(addresses[items[i].address_index].area, &other) &&
            other == area) {
            group[group_count++] = items[i];
        }
    }
    qsort(group, group_count, sizeof(*group), compare_items);
    gap = bits ? ctx->merge_gap * 2u : ctx->merge_gap;
    for (i = 0; i < group_count; i++) {
        uint32_t end = group[i].offset + group[i].width;

        if (span_count > 0) {
            mb_span *last = &spans[span_count - 1];

            if (group[i].offset <= last->start + last->count + gap &&
                end - last->start <= limit) {
                last->count = end - last->start;
                continue;
            }
        }
        spans[span_count].start = group[i].offset;
        spans[span_count].count = group[i].width;
        span_count++;
    }

    for (i = 0; i < span_count && result == NCL_OK; i++) {
        uint8_t pdu[8];
        size_t pdu_len = ncl_modbus_read_pdu(pdu, sizeof(pdu), function,
                                             (uint16_t)spans[i].start,
                                             (uint16_t)spans[i].count);
        const uint8_t *reply = NULL;
        size_t reply_len = 0;
        size_t expect_bytes =
            bits ? (spans[i].count + 7u) / 8u : (size_t)spans[i].count * 2u;
        const uint8_t *data = NULL;
        char message[160];

        if (pdu_len == 0) {
            result = NCL_ERR_RANGE;
            break;
        }
        result = mb_request(ctx, pdu, pdu_len, &reply, &reply_len);
        if (result != NCL_OK) {
            break;
        }
        if (reply == NULL) {
            result = NCL_DRV_ERR_BUSINESS(0x24); /* a read cannot broadcast */
            break;
        }
        message[0] = '\0';
        result = ncl_modbus_check_read_reply(reply, reply_len, function,
                                             expect_bytes, &data, message,
                                             sizeof(message));
        if (result != NCL_OK) {
            break;
        }
        for (j = 0; j < group_count; j++) {
            mb_item *item = &group[j];
            ncl_json *value = NULL;

            if (item->offset < spans[i].start ||
                item->offset + item->width > spans[i].start + spans[i].count) {
                continue;
            }
            if (bits) {
                uint32_t bit = item->offset - spans[i].start;

                value = ncl_json_new_bool((data[bit / 8u] >> (bit % 8u)) & 1u);
            } else {
                ncl_err decode = ncl_modbus_decode(
                    data, expect_bytes,
                    (size_t)(item->offset - spans[i].start) * 2u, item->dtype,
                    item->text_chars, ctx->order, &value);

                if (decode != NCL_OK) {
                    result = decode;
                    break;
                }
            }
            store_item(slots, addresses, item, value);
        }
    }
    ncl_free_safe(group);
    ncl_free_safe(spans);
    return result;
}

static ncl_err mb_read_batch(ncl_driver *self, const ncl_address *addresses,
                             size_t count, ncl_json **values)
{
    mb_ctx *ctx = (mb_ctx *)self->ctx;
    mb_item *items = NULL;
    ncl_json **slots = NULL;
    ncl_json *array = ncl_json_new_array();
    size_t item_count = 0;
    ncl_err result;
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
    items = (mb_item *)ncl_mem_calloc(MB_MAX_ITEMS, sizeof(*items));
    slots = (ncl_json **)ncl_mem_calloc(count, sizeof(*slots));
    if (items == NULL || slots == NULL) {
        result = NCL_ERR_NOMEM;
        goto done;
    }
    result = build_items(ctx, addresses, count, items, &item_count);
    if (result != NCL_OK) {
        goto done;
    }

    /* One area per request: a device answers one function code at a time. */
    for (i = 0; i < item_count && result == NCL_OK; i++) {
        ncl_modbus_area area;
        size_t j;
        bool handled = false;

        if (!ncl_modbus_area_parse(addresses[items[i].address_index].area, &area)) {
            result = NCL_DRV_ERR_BUSINESS(0x20); /* unknown area in the point map */
            break;
        }
        for (j = 0; j < i; j++) {
            ncl_modbus_area other;

            if (ncl_modbus_area_parse(addresses[items[j].address_index].area,
                                      &other) &&
                other == area) {
                handled = true; /* this area was read at an earlier item */
                break;
            }
        }
        if (handled) {
            continue;
        }
        result = mb_read_area(ctx, addresses, items, item_count, area, slots);
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

/* ---------------------------------------------------------------- write --- */

/** Pack @p length bits of @p value (an array, or one scalar) LSB first. */
static ncl_err pack_coils(const ncl_json *value, size_t length, uint8_t *out,
                          size_t out_len)
{
    size_t i;

    memset(out, 0, out_len);
    for (i = 0; i < length; i++) {
        const ncl_json *element = ncl_json_type_of(value) == NCL_JSON_ARRAY
                                      ? ncl_json_arr_get(value, i)
                                      : value;
        bool set = false;
        double number = 0;

        if (element == NULL) {
            return NCL_ERR_INVALID_VALUE;
        }
        if (!ncl_json_as_bool(element, &set)) {
            if (!ncl_json_as_double(element, &number)) {
                return NCL_ERR_INVALID_VALUE;
            }
            set = number != 0;
        }
        if (set) {
            out[i / 8u] |= (uint8_t)(1u << (i % 8u));
        }
    }
    return NCL_OK;
}

static ncl_err mb_write_bits(mb_ctx *ctx, const ncl_address *address,
                             const ncl_json *value, uint16_t address_on_bus)
{
    uint8_t pdu[MB_MAX_FRAME];
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    char message[160];
    size_t pdu_len;
    ncl_err err;

    if (address->length <= 1) {
        const ncl_json *one = ncl_json_type_of(value) == NCL_JSON_ARRAY
                                  ? ncl_json_arr_get(value, 0)
                                  : value;
        bool set = false;
        double number = 0;
        uint16_t word;

        if (one == NULL) {
            return NCL_ERR_INVALID_VALUE;
        }
        if (!ncl_json_as_bool(one, &set)) {
            if (!ncl_json_as_double(one, &number)) {
                return NCL_ERR_INVALID_VALUE;
            }
            set = number != 0;
        }
        word = set ? 0xFF00 : 0x0000;
        pdu_len = ncl_modbus_write_single_pdu(pdu, sizeof(pdu),
                                              NCL_MB_FC_WRITE_COIL, address_on_bus,
                                              word);
        if (pdu_len == 0) {
            return NCL_ERR_RANGE;
        }
        err = mb_request(ctx, pdu, pdu_len, &reply, &reply_len);
        if (err != NCL_OK || reply == NULL) {
            return err;
        }
        return ncl_modbus_check_write_reply(reply, reply_len, NCL_MB_FC_WRITE_COIL,
                                            address_on_bus, word, message,
                                            sizeof(message));
    }
    {
        uint8_t packed[256];
        size_t bytes = ((size_t)address->length + 7u) / 8u;
        uint16_t count = (uint16_t)address->length;

        if (bytes > sizeof(packed) ||
            (unsigned)address->length > ncl_modbus_area_max_write(NCL_MB_COIL)) {
            return NCL_ERR_RANGE;
        }
        err = pack_coils(value, (size_t)address->length, packed, bytes);
        if (err != NCL_OK) {
            return err;
        }
        pdu_len = ncl_modbus_write_multi_pdu(pdu, sizeof(pdu),
                                             NCL_MB_FC_WRITE_COILS, address_on_bus,
                                             count, packed, bytes);
        if (pdu_len == 0) {
            return NCL_ERR_RANGE;
        }
        err = mb_request(ctx, pdu, pdu_len, &reply, &reply_len);
        if (err != NCL_OK || reply == NULL) {
            return err;
        }
        return ncl_modbus_check_write_reply(reply, reply_len,
                                            NCL_MB_FC_WRITE_COILS, address_on_bus,
                                            count, message, sizeof(message));
    }
}

static ncl_err mb_write_registers(mb_ctx *ctx, const ncl_address *address,
                                  const ncl_json *value, uint16_t address_on_bus)
{
    uint8_t payload[250];
    uint8_t pdu[MB_MAX_FRAME];
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    char message[160];
    size_t used = 0;
    size_t pdu_len;
    uint16_t registers;
    ncl_err err;

    if (address->dtype == NCL_DTYPE_STRING) {
        err = ncl_modbus_encode(value, NCL_DTYPE_STRING,
                                (size_t)address->length, ctx->order, payload,
                                sizeof(payload), &used);
        if (err != NCL_OK) {
            return err;
        }
        registers = (uint16_t)((used + 1u) / 2u);
        if (used % 2u != 0) {
            payload[used] = 0; /* the odd byte of a string is padded */
            used++;
        }
    } else {
        uint16_t width = element_width(address);
        size_t element;

        for (element = 0; element < (size_t)address->length; element++) {
            const ncl_json *one =
                address->length > 1 ? ncl_json_arr_get(value, element) : value;
            size_t written = 0;

            if (one == NULL || used + (size_t)width * 2u > sizeof(payload)) {
                return NCL_ERR_INVALID_VALUE;
            }
            err = ncl_modbus_encode(one, address->dtype, 0, ctx->order,
                                    payload + used, sizeof(payload) - used,
                                    &written);
            if (err != NCL_OK) {
                return err;
            }
            used += written;
        }
        registers = (uint16_t)(used / 2u);
    }
    if (registers == 0) {
        return NCL_ERR_INVALID_VALUE;
    }
    if (registers > ncl_modbus_area_max_write(NCL_MB_HOLDING) || used > 255) {
        return NCL_ERR_RANGE;
    }
    if (registers == 1) {
        uint16_t word = (uint16_t)((payload[0] << 8) | payload[1]);

        pdu_len = ncl_modbus_write_single_pdu(pdu, sizeof(pdu),
                                              NCL_MB_FC_WRITE_REGISTER,
                                              address_on_bus, word);
        if (pdu_len == 0) {
            return NCL_ERR_RANGE;
        }
        err = mb_request(ctx, pdu, pdu_len, &reply, &reply_len);
        if (err != NCL_OK || reply == NULL) {
            return err;
        }
        return ncl_modbus_check_write_reply(reply, reply_len,
                                            NCL_MB_FC_WRITE_REGISTER, address_on_bus,
                                            word, message, sizeof(message));
    }
    pdu_len = ncl_modbus_write_multi_pdu(pdu, sizeof(pdu),
                                         NCL_MB_FC_WRITE_REGISTERS, address_on_bus,
                                         registers, payload, used);
    if (pdu_len == 0) {
        return NCL_ERR_RANGE;
    }
    err = mb_request(ctx, pdu, pdu_len, &reply, &reply_len);
    if (err != NCL_OK || reply == NULL) {
        return err;
    }
    return ncl_modbus_check_write_reply(reply, reply_len,
                                        NCL_MB_FC_WRITE_REGISTERS, address_on_bus,
                                        registers, message, sizeof(message));
}

static ncl_err mb_write_batch(ncl_driver *self, const ncl_address *addresses,
                              const ncl_json *values, size_t count)
{
    mb_ctx *ctx = (mb_ctx *)self->ctx;
    ncl_err result = NCL_OK;
    size_t i;

    if (addresses == NULL || values == NULL ||
        ncl_json_type_of(values) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(values) != count) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_mutex_lock(ctx->mutex);
    for (i = 0; i < count; i++) {
        const ncl_address *address = &addresses[i];
        const ncl_json *value = ncl_json_arr_get(values, i);
        uint16_t address_on_bus;
        ncl_modbus_area area;

        if (!ncl_modbus_area_parse(address->area, &area)) {
            result = NCL_DRV_ERR_BUSINESS(0x20);
            break;
        }
        if (!ncl_modbus_area_is_writable(area)) {
            result = NCL_DRV_ERR_BUSINESS(0x22); /* 1x and 3x are read only */
            break;
        }
        if (address->offset < (ctx->base_one ? 1 : 0) || address->offset > 0xFFFF) {
            result = NCL_DRV_ERR_BUSINESS(0x23);
            break;
        }
        address_on_bus = (uint16_t)(address->offset - (ctx->base_one ? 1 : 0));
        if (value == NULL) {
            result = NCL_ERR_INVALID_VALUE;
            break;
        }
        result = address_is_bits(address)
                     ? mb_write_bits(ctx, address, value, address_on_bus)
                     : mb_write_registers(ctx, address, value, address_on_bus);
        if (result != NCL_OK) {
            break;
        }
    }
    ncl_mutex_unlock(ctx->mutex);
    return result;
}

/* ------------------------------------------------------------ raw / call -- */

/**
 * The raw escape hatch takes a PDU (function code + data), which is what a
 * diagnostics tool or a vendor extension needs, and answers with the raw reply
 * PDU in the envelope's "raw" plus its hex form in "value".
 */
static ncl_err mb_raw(ncl_driver *self, const void *frame, size_t frame_len,
                      ncl_driver_result *out)
{
    static const char kDigits[] = "0123456789abcdef";
    mb_ctx *ctx = (mb_ctx *)self->ctx;
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    ncl_err err;
    char *hex;
    size_t i;

    if (frame == NULL || frame_len == 0 || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_mutex_lock(ctx->mutex);
    err = mb_request(ctx, (const uint8_t *)frame, frame_len, &reply, &reply_len);
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

static ncl_err mb_read_raw(ncl_driver *self, const void *frame, size_t frame_len,
                           ncl_driver_result *out)
{
    return mb_raw(self, frame, frame_len, out);
}

static ncl_err mb_write_raw(ncl_driver *self, const void *frame, size_t frame_len,
                            ncl_driver_result *out)
{
    return mb_raw(self, frame, frame_len, out);
}

/**
 * The one Modbus operation worth exposing as a method: the diagnostics
 * loop-back test (function 0x08, sub-function 0x0000). It answers with the data
 * it echoed, the cheapest way to tell "the wire is alive" from "the device is
 * refusing".
 */
static ncl_err mb_call(ncl_driver *self, const char *operation,
                       const ncl_json *params, ncl_json **result)
{
    mb_ctx *ctx = (mb_ctx *)self->ctx;
    uint8_t pdu[5];
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    uint16_t pattern = (uint16_t)ncl_json_obj_get_int(params, "pattern", 0xA537);
    ncl_err err;

    if (result != NULL) {
        *result = NULL;
    }
    if (!ncl_streq_ignore_case(operation, "loopback")) {
        return NCL_DRV_ERR_PROTOCOL(0x0D); /* no such operation */
    }
    pdu[0] = 0x08;
    pdu[1] = 0x00; /* sub-function: return query data */
    pdu[2] = 0x00;
    pdu[3] = (uint8_t)(pattern >> 8);
    pdu[4] = (uint8_t)pattern;
    ncl_mutex_lock(ctx->mutex);
    err = mb_request(ctx, pdu, sizeof(pdu), &reply, &reply_len);
    ncl_mutex_unlock(ctx->mutex);
    if (err != NCL_OK) {
        return err;
    }
    if (reply == NULL || reply_len < 5 || reply[0] != 0x08) {
        return NCL_DRV_ERR_PROTOCOL(0x0E);
    }
    if (result != NULL) {
        *result = ncl_json_new_object();
        if (*result == NULL) {
            return NCL_ERR_NOMEM;
        }
        (void)ncl_json_obj_set_int(*result, "echo",
                                   (reply[3] << 8) | (int)reply[4]);
    }
    return NCL_OK;
}

/* ---------------------------------------------------------------- driver -- */

static ncl_err mb_create(ncl_driver *self, const ncl_json *parameters)
{
    mb_ctx *ctx = (mb_ctx *)self->ctx;
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
    text = ncl_json_obj_get_string(parameters, "serial");
    if (text == NULL) {
        text = ncl_json_obj_get_string(parameters, "serialPort");
    }
    if (text != NULL) {
        ncl_free_safe(ctx->serial_device);
        ctx->serial_device = ncl_strdup(text);
        if (ctx->serial_device == NULL) {
            return NCL_ERR_NOMEM;
        }
        ctx->serial_options.device = ctx->serial_device;
    }
    ctx->port = json_uint(parameters, "port", ctx->port);
    ctx->unit = json_uint(parameters, "unit", ctx->unit);
    ctx->connect_timeout_ms = json_uint(parameters, "connectTimeoutMs",
                                        ctx->connect_timeout_ms);
    ctx->timeout_ms = json_uint(parameters, "timeoutMs", ctx->timeout_ms);
    ctx->retries = json_uint(parameters, "retries", ctx->retries);
    ctx->merge_gap = json_uint(parameters, "mergeGap", ctx->merge_gap);
    ctx->base_one = ncl_json_obj_get_bool(parameters, "base1", ctx->base_one);
    ctx->serial_options.baud = json_uint(parameters, "baud",
                                         ctx->serial_options.baud);
    ctx->serial_options.data_bits = json_uint(parameters, "dataBits",
                                              ctx->serial_options.data_bits);
    ctx->serial_options.stop_bits = json_uint(parameters, "stopBits",
                                              ctx->serial_options.stop_bits);
    ctx->serial_options.inter_frame_ms =
        json_uint(parameters, "interFrameMs", ctx->serial_options.inter_frame_ms);
    text = ncl_json_obj_get_string(parameters, "parity");
    if (text != NULL && text[0] != '\0') {
        ctx->serial_options.parity = text[0];
    }
    text = ncl_json_obj_get_string(parameters, "wordOrder");
    if (text != NULL && !ncl_modbus_word_order_parse(text, &ctx->order)) {
        return NCL_ERR_INVALID_ARG;
    }
    /* Each transport has its own idea of a target. */
    if (ctx->mode == MB_MODE_RTU) {
        if (ctx->serial_device == NULL) {
            return NCL_ERR_INVALID_ARG; /* RTU needs a serial port, not a host */
        }
    } else if (ctx->host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return NCL_OK;
}

static ncl_err mb_open(ncl_driver *self)
{
    mb_ctx *ctx = (mb_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = mb_open_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void mb_close(ncl_driver *self)
{
    mb_ctx *ctx = (mb_ctx *)self->ctx;

    ncl_mutex_lock(ctx->mutex);
    mb_close_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
}

static bool mb_is_connected(const ncl_driver *self)
{
    const mb_ctx *ctx = (const mb_ctx *)self->ctx;

    return ctx->socket != NULL || ctx->serial != NULL;
}

static ncl_err mb_read_batch_locked(ncl_driver *self,
                                    const ncl_address *addresses, size_t count,
                                    ncl_json **values)
{
    mb_ctx *ctx = (mb_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = mb_read_batch(self, addresses, count, values);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void mb_attach_event(ncl_driver *self, ncl_driver_event_fn fn, void *user)
{
    /* Modbus has no unsolicited traffic: events come from polling alarms. */
    (void)self;
    (void)fn;
    (void)user;
}

static void mb_destroy(ncl_driver *self)
{
    mb_ctx *ctx;

    if (self == NULL) {
        return;
    }
    ctx = (mb_ctx *)self->ctx;
    if (ctx != NULL) {
        mb_close_session(ctx);
        ncl_free_safe(ctx->host);
        ncl_free_safe(ctx->serial_device);
        if (ctx->mutex != NULL) {
            ncl_mutex_destroy(ctx->mutex);
        }
        ncl_free_safe(ctx);
    }
    ncl_free_safe(self);
}

static ncl_driver *mb_create_driver(mb_mode mode, const ncl_driver_ops *ops)
{
    mb_ctx *ctx = (mb_ctx *)ncl_mem_calloc(1, sizeof(*ctx));
    ncl_driver *driver;

    if (ctx == NULL) {
        return NULL;
    }
    ctx->mode = mode;
    ctx->port = 502;
    ctx->unit = 1;
    ctx->connect_timeout_ms = 3000; /* 00-通用-实现约定 §3 */
    ctx->timeout_ms = 1000;
    ctx->retries = mode == MB_MODE_RTU ? 2 : 1;
    ctx->merge_gap = 8; /* 15-MODBUS.md §5.1 */
    ctx->order = NCL_MB_ORDER_ABCD;
    ctx->mutex = ncl_mutex_create();
    ncl_serial_options_default(&ctx->serial_options);
    if (ctx->mutex == NULL) {
        ncl_free_safe(ctx);
        return NULL;
    }
    driver = ncl_driver_new(ops, ctx);
    if (driver == NULL) {
        ncl_mutex_destroy(ctx->mutex);
        ncl_free_safe(ctx);
        return NULL;
    }
    return driver;
}

ncl_driver *ncl_modbus_tcp_create(void)
{
    static const ncl_driver_ops kOps = {
        "modbus_tcp",   mb_create,
        mb_open,        mb_close,
        mb_is_connected, mb_read_batch_locked,
        mb_write_batch, mb_read_raw,
        mb_write_raw,   mb_call,
        mb_attach_event, mb_destroy,
    };

    return mb_create_driver(MB_MODE_TCP, &kOps);
}

ncl_driver *ncl_modbus_rtu_create(void)
{
    static const ncl_driver_ops kOps = {
        "modbus_rtu",   mb_create,
        mb_open,        mb_close,
        mb_is_connected, mb_read_batch_locked,
        mb_write_batch, mb_read_raw,
        mb_write_raw,   mb_call,
        mb_attach_event, mb_destroy,
    };

    return mb_create_driver(MB_MODE_RTU, &kOps);
}

ncl_driver *ncl_modbus_rtu_tcp_create(void)
{
    static const ncl_driver_ops kOps = {
        "modbus_rtu_tcp", mb_create,
        mb_open,          mb_close,
        mb_is_connected,  mb_read_batch_locked,
        mb_write_batch,   mb_read_raw,
        mb_write_raw,     mb_call,
        mb_attach_event,  mb_destroy,
    };

    return mb_create_driver(MB_MODE_RTU_TCP, &kOps);
}
