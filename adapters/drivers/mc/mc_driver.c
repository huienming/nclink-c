/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Mitsubishi MC / SLMP over TCP.
 *
 * Same shape as the Modbus driver: one session, one request in flight, a
 * merged read plan and a decode pass. What differs is the protocol's own
 * little endian world and its way of naming devices, which the codec owns.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink_adapter/ncl_mc.h"
#include "mc/ncl_mc_driver.h"

#define MC_MAX_FRAME 2100   /* 960 words of data plus headers */
#define MC_MAX_ITEMS 2048
#define MC_MAX_WORDS 960    /* one batch read / write (§6)    */

typedef struct {
    char          *host;
    unsigned       port;
    ncl_mc_header  header;
    ncl_socket    *socket;
    unsigned       connect_timeout_ms;
    unsigned       timeout_ms;
    unsigned       retries;
    unsigned       merge_gap;
    ncl_mutex     *mutex;
    uint8_t        tx[MC_MAX_FRAME];
    uint8_t        rx[MC_MAX_FRAME];
    size_t         last_tx_len; /**< the frame the audit should show */
    size_t         last_rx_len;
} mc_ctx;

static unsigned json_uint(const ncl_json *object, const char *key,
                          unsigned fallback)
{
    long long value = ncl_json_obj_get_int(object, key, (long long)fallback);

    return value < 0 ? 0u : (unsigned)value;
}

/* --------------------------------------------------------------- session -- */

static void mc_close_session(mc_ctx *ctx)
{
    if (ctx->socket != NULL) {
        ncl_socket_close(ctx->socket);
        ctx->socket = NULL;
    }
}

static ncl_err mc_open_session(mc_ctx *ctx)
{
    char err[256];

    if (ctx->socket != NULL) {
        return NCL_OK;
    }
    err[0] = '\0';
    ctx->socket = ncl_socket_connect(ctx->host, ctx->port, ctx->connect_timeout_ms,
                                     err, sizeof(err));
    return ctx->socket != NULL ? NCL_OK : NCL_DRV_ERR_TRANSPORT(0x10);
}

/* -------------------------------------------------------------- exchange -- */

/**
 * One request/response. The reply is read in two steps - the fixed header says
 * how long the frame is - and the device's own end code is checked here, so
 * every caller sees a tiered error instead of a raw code.
 */
static ncl_err mc_exchange(mc_ctx *ctx, uint16_t command, uint16_t subcommand,
                           const uint8_t *data, size_t data_len,
                           ncl_mc_reply *reply)
{
    size_t frame_len;
    size_t header_len = ctx->header.frame == NCL_MC_FRAME_4E ? 11u : 9u;
    size_t length;
    size_t total;
    ncl_err err;
    char message[160];

    if (ctx->header.frame == NCL_MC_FRAME_4E) {
        ctx->header.serial++; /* the device echoes it back (§3.3) */
    }
    frame_len = ncl_mc_request(ctx->tx, sizeof(ctx->tx), &ctx->header, command,
                               subcommand, data, data_len);
    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    ctx->last_tx_len = frame_len;
    ctx->last_rx_len = 0;
    if (ncl_socket_send(ctx->socket, ctx->tx, frame_len) != NCL_OK) {
        return NCL_DRV_ERR_TRANSPORT(0x11);
    }
    /* The end code and the data length live in the fixed part of the reply. */
    if (ncl_socket_recv_exact(ctx->socket, ctx->rx, header_len,
                              ctx->timeout_ms) != NCL_OK) {
        ncl_socket_shutdown(ctx->socket);
        return NCL_DRV_ERR_TRANSPORT(0x12);
    }
    length = (size_t)(ctx->rx[header_len - 2] |
                      ((uint16_t)ctx->rx[header_len - 1] << 8));
    if (length < 2 || header_len + length > sizeof(ctx->rx)) {
        ncl_socket_shutdown(ctx->socket);
        return NCL_DRV_ERR_PROTOCOL(0x0A);
    }
    if (ncl_socket_recv_exact(ctx->socket, ctx->rx + header_len, length,
                              ctx->timeout_ms) != NCL_OK) {
        ncl_socket_shutdown(ctx->socket);
        return NCL_DRV_ERR_TRANSPORT(0x13);
    }
    total = header_len + length;
    ctx->last_rx_len = total;
    err = ncl_mc_split_reply(ctx->rx, total, ctx->header.frame,
                             ctx->header.serial, reply, &frame_len);
    if (err != NCL_OK) {
        return err;
    }
    message[0] = '\0';
    return ncl_mc_check_end_code(reply->end_code, message, sizeof(message));
}

static ncl_err mc_request(mc_ctx *ctx, uint16_t command, uint16_t subcommand,
                          const uint8_t *data, size_t data_len,
                          ncl_mc_reply *reply)
{
    unsigned attempt = 0;
    ncl_err err;

    for (;;) {
        err = mc_exchange(ctx, command, subcommand, data, data_len, reply);
        if (ncl_driver_error_tier(err) != 1 || attempt >= ctx->retries) {
            return err;
        }
        attempt++;
        mc_close_session(ctx);
        if (mc_open_session(ctx) != NCL_OK) {
            return NCL_DRV_ERR_TRANSPORT(0x14);
        }
    }
}

/* ---------------------------------------------------------------- items --- */

typedef struct {
    size_t    address_index;
    size_t    element;
    uint32_t  offset; /**< device number (already in wire form) */
    size_t    text_chars;
    ncl_dtype dtype;
    bool      bits;   /**< read/written in bit units */
} mc_item;

typedef struct {
    uint32_t start;
    uint32_t points;
} mc_span;

static int compare_items(const void *a, const void *b)
{
    const mc_item *left = (const mc_item *)a;
    const mc_item *right = (const mc_item *)b;

    if (left->offset != right->offset) {
        return left->offset < right->offset ? -1 : 1;
    }
    return left->element < right->element ? -1 : (left->element > right->element);
}

/** True when the element travels as one bit (a bit device, or a word's bit). */
static bool element_is_bit(const ncl_address *address, bool device_is_bit)
{
    return address->dtype == NCL_DTYPE_BIT || address->bit >= 0 || device_is_bit;
}

/** Points one element of @p address occupies on the wire. */
static uint32_t element_points(const ncl_address *address, bool bits)
{
    if (bits) {
        return 1;
    }
    if (address->dtype == NCL_DTYPE_STRING) {
        return (uint32_t)(((size_t)address->length + 1u) / 2u);
    }
    return (uint32_t)(ncl_mc_element_bytes(address->dtype, 0) / 2u);
}

/**
 * How many items the batch can expand to: a string address is one item,
 * anything else is one item per element. Sizing the scratch array from the
 * batch instead of always reserving MC_MAX_ITEMS keeps a three point read from
 * taking 80 KiB out of a device's pool (2048 * 40 bytes); MC_MAX_ITEMS is still
 * the ceiling, and build_items reports an oversized batch as NCL_ERR_RANGE
 * rather than writing past what was allocated.
 */
static size_t batch_capacity(const ncl_address *addresses, size_t count)
{
    size_t total = 0;
    size_t i;

    for (i = 0; i < count; i++) {
        size_t width = addresses[i].dtype == NCL_DTYPE_STRING
                           ? 1u
                           : (size_t)addresses[i].length;

        if (width > MC_MAX_ITEMS - total) {
            return MC_MAX_ITEMS;
        }
        total += width;
    }
    return total == 0u ? 1u : total;
}

/**
 * Expand the addresses of one group - one device type, one unit - into the
 * points a single request carries. Addresses of another device or the other
 * unit are left to their own group.
 */
static ncl_err build_items(const ncl_address *addresses, size_t count,
                           mc_item *items, size_t capacity, size_t *item_count,
                           const char *device, bool bit_device, bool want_bits)
{
    size_t i;
    size_t used = 0;

    for (i = 0; i < count; i++) {
        size_t element;
        bool element_bits;

        if (!ncl_streq_ignore_case(addresses[i].area, device)) {
            continue;
        }
        element_bits = element_is_bit(&addresses[i], bit_device);
        if (element_bits != want_bits) {
            continue; /* another unit: another request */
        }
        if (addresses[i].dtype == NCL_DTYPE_STRING) {
            if (used + 1 > capacity) {
                return NCL_ERR_RANGE;
            }
            items[used].address_index = i;
            items[used].element = 0;
            items[used].offset = ncl_mc_wire_address(
                (uint32_t)addresses[i].offset, addresses[i].bit, !bit_device);
            items[used].text_chars = (size_t)addresses[i].length;
            items[used].dtype = NCL_DTYPE_STRING;
            items[used].bits = false;
            used++;
            continue;
        }
        for (element = 0; element < (size_t)addresses[i].length; element++) {
            uint32_t points = element_points(&addresses[i], element_bits);

            if (used + 1 > capacity) {
                return NCL_ERR_RANGE;
            }
            items[used].address_index = i;
            items[used].element = element;
            items[used].offset = ncl_mc_wire_address(
                                   (uint32_t)addresses[i].offset, addresses[i].bit,
                                   !bit_device) +
                                 (uint32_t)element * points;
            items[used].text_chars = 0;
            /* A bit device reads bits whatever the point map declared. */
            items[used].dtype = element_bits ? NCL_DTYPE_BIT
                                             : addresses[i].dtype;
            items[used].bits = element_bits;
            used++;
        }
    }
    *item_count = used;
    return NCL_OK;
}

static void store_item(ncl_json **slots, const ncl_address *addresses,
                       const mc_item *item, ncl_json *value)
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

/** Read one device type: merge into spans, request, decode. */
static ncl_err mc_read_group(mc_ctx *ctx, const ncl_address *addresses,
                             const mc_item *group, size_t group_count,
                             uint8_t device_code, bool bits, ncl_json **slots)
{
    mc_item *sorted;
    mc_span *spans;
    size_t span_count = 0;
    ncl_err result = NCL_OK;
    size_t i;
    uint32_t limit = bits ? MC_MAX_WORDS * 16u : MC_MAX_WORDS;

    sorted = (mc_item *)ncl_mem_calloc(group_count, sizeof(*sorted));
    spans = (mc_span *)ncl_mem_calloc(group_count, sizeof(*spans));
    if (sorted == NULL || spans == NULL) {
        ncl_free_safe(sorted);
        ncl_free_safe(spans);
        return NCL_ERR_NOMEM;
    }
    memcpy(sorted, group, group_count * sizeof(*sorted));
    qsort(sorted, group_count, sizeof(*sorted), compare_items);
    for (i = 0; i < group_count; i++) {
        uint32_t points = bits ? 1u : element_points(&addresses[sorted[i].address_index],
                                                     bits);

        if (sorted[i].dtype == NCL_DTYPE_STRING) {
            points = (uint32_t)(((size_t)addresses[sorted[i].address_index].length +
                                 1u) /
                                2u);
        }
        if (span_count > 0) {
            mc_span *last = &spans[span_count - 1];
            uint32_t end = sorted[i].offset + points;

            if (sorted[i].offset <= last->start + last->points + ctx->merge_gap &&
                end - last->start <= limit) {
                last->points = end - last->start;
                continue;
            }
        }
        spans[span_count].start = sorted[i].offset;
        spans[span_count].points = points;
        span_count++;
    }

    for (i = 0; i < span_count && result == NCL_OK; i++) {
        uint8_t spec[8];
        size_t spec_len = ncl_mc_device_spec(spec, sizeof(spec), device_code,
                                             spans[i].start,
                                             (uint16_t)spans[i].points);
        ncl_mc_reply reply;
        size_t j;

        if (spec_len == 0) {
            result = NCL_ERR_RANGE;
            break;
        }
        memset(&reply, 0, sizeof(reply));
        result = mc_request(ctx, NCL_MC_CMD_BATCH_READ,
                            bits ? NCL_MC_SUB_BIT : NCL_MC_SUB_WORD, spec,
                            spec_len, &reply);
        if (result != NCL_OK) {
            break;
        }
        for (j = 0; j < group_count; j++) {
            const mc_item *item = &sorted[j];
            uint32_t width = bits ? 1u : element_points(&addresses[item->address_index],
                                                        bits);
            const uint8_t *payload;
            size_t payload_len;
            ncl_json *value = NULL;
            ncl_err decode;

            if (item->dtype == NCL_DTYPE_STRING) {
                width = (uint32_t)(((size_t)addresses[item->address_index].length +
                                    1u) /
                                   2u);
            }
            if (item->offset < spans[i].start ||
                item->offset + width > spans[i].start + spans[i].points) {
                continue;
            }
            /* Bit units answer one byte per point; word units two. */
            payload = reply.data +
                      (size_t)(item->offset - spans[i].start) * (bits ? 1u : 2u);
            payload_len = reply.data_len -
                          (size_t)(item->offset - spans[i].start) * (bits ? 1u : 2u);
            decode = ncl_mc_decode(payload, payload_len, 0, item->dtype,
                                   item->text_chars, &value);
            if (decode != NCL_OK) {
                result = decode;
                break;
            }
            store_item(slots, addresses, item, value);
        }
    }
    ncl_free_safe(sorted);
    ncl_free_safe(spans);
    return result;
}

static ncl_err mc_read_batch(ncl_driver *self, const ncl_address *addresses,
                             size_t count, ncl_json **values)
{
    mc_ctx *ctx = (mc_ctx *)self->ctx;
    mc_item *items = NULL;
    ncl_json **slots = NULL;
    ncl_json *array = ncl_json_new_array();
    size_t item_count = 0;
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
    items = (mc_item *)ncl_mem_calloc(capacity, sizeof(*items));
    slots = (ncl_json **)ncl_mem_calloc(count, sizeof(*slots));
    if (items == NULL || slots == NULL) {
        result = NCL_ERR_NOMEM;
        goto done;
    }
    /* One request carries one device type in one unit, so the batch is split
     * by (device, unit) - a D word read and an M bit read are two requests. */
    for (i = 0; i < count && result == NCL_OK; i++) {
        uint8_t device_code = 0;
        ncl_mc_unit unit = NCL_MC_WORD;
        bool bits;
        size_t j;
        bool handled = false;

        if (!ncl_mc_device_lookup(addresses[i].area, &device_code, &unit)) {
            result = NCL_DRV_ERR_BUSINESS(0x30); /* unknown device */
            break;
        }
        bits = element_is_bit(&addresses[i], unit == NCL_MC_BIT);
        for (j = 0; j < i; j++) {
            ncl_mc_unit other_unit = NCL_MC_WORD;

            if (ncl_mc_device_lookup(addresses[j].area, NULL, &other_unit) &&
                ncl_streq_ignore_case(addresses[j].area, addresses[i].area) &&
                element_is_bit(&addresses[j], other_unit == NCL_MC_BIT) == bits) {
                handled = true;
                break;
            }
        }
        if (handled) {
            continue;
        }
        result = build_items(addresses, count, items, capacity, &item_count,
                             addresses[i].area, unit == NCL_MC_BIT, bits);
        if (result != NCL_OK) {
            break;
        }
        result = mc_read_group(ctx, addresses, items, item_count, device_code, bits,
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

/** Write one address: device spec, then the encoded points. */
static ncl_err mc_write_one(mc_ctx *ctx, const ncl_address *address,
                            const ncl_json *value)
{
    uint8_t payload[MC_MAX_FRAME];
    uint8_t request[MC_MAX_FRAME];
    uint8_t device_code = 0;
    ncl_mc_unit unit = NCL_MC_WORD;
    ncl_mc_reply reply;
    bool bits;
    size_t used = 0;
    uint32_t points;
    uint32_t start;
    size_t spec_len;
    size_t i;
    ncl_err err;

    if (!ncl_mc_device_lookup(address->area, &device_code, &unit)) {
        return NCL_DRV_ERR_BUSINESS(0x30); /* unknown device in the point map */
    }
    if (value == NULL) {
        return NCL_ERR_INVALID_VALUE;
    }
    bits = element_is_bit(address, unit == NCL_MC_BIT);
    if (address->dtype == NCL_DTYPE_STRING) {
        size_t written = 0;

        err = ncl_mc_encode(value, NCL_DTYPE_STRING, (size_t)address->length,
                            payload, sizeof(payload), &written);
        if (err != NCL_OK) {
            return err;
        }
        used = written;
    } else {
        for (i = 0; i < (size_t)address->length; i++) {
            const ncl_json *one = address->length > 1
                                      ? ncl_json_arr_get(value, i)
                                      : value;
            size_t written = 0;

            if (one == NULL) {
                return NCL_ERR_INVALID_VALUE;
            }
            err = ncl_mc_encode(one, bits ? NCL_DTYPE_BIT : address->dtype, 0,
                                payload + used, sizeof(payload) - used, &written);
            if (err != NCL_OK) {
                return err;
            }
            used += written;
        }
    }
    /* Word units travel two bytes per point, so an odd string is padded. */
    points = bits ? (uint32_t)used : (uint32_t)((used + 1u) / 2u);
    if (points == 0 ||
        points > (bits ? MC_MAX_WORDS * 16u : MC_MAX_WORDS)) {
        return NCL_ERR_RANGE;
    }
    if (!bits && used % 2u != 0) {
        payload[used] = 0;
        used++;
    }
    start = ncl_mc_wire_address((uint32_t)address->offset, address->bit,
                                unit == NCL_MC_WORD);
    spec_len = ncl_mc_device_spec(request, sizeof(request), device_code, start,
                                  (uint16_t)points);
    if (spec_len == 0 || spec_len + used > sizeof(request)) {
        return NCL_ERR_RANGE;
    }
    memcpy(request + spec_len, payload, used);
    memset(&reply, 0, sizeof(reply));
    return mc_request(ctx, NCL_MC_CMD_BATCH_WRITE,
                      bits ? NCL_MC_SUB_BIT : NCL_MC_SUB_WORD, request,
                      spec_len + used, &reply);
}

static ncl_err mc_write_batch(ncl_driver *self, const ncl_address *addresses,
                              const ncl_json *values, size_t count)
{
    mc_ctx *ctx = (mc_ctx *)self->ctx;
    size_t i;

    if (addresses == NULL || values == NULL ||
        ncl_json_type_of(values) != NCL_JSON_ARRAY ||
        ncl_json_arr_len(values) != count) {
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < count; i++) {
        ncl_err err = mc_write_one(ctx, &addresses[i],
                                   ncl_json_arr_get(values, i));

        if (err != NCL_OK) {
            return err;
        }
    }
    return NCL_OK;
}

/* ------------------------------------------------------------ raw / call -- */

/**
 * Raw access speaks the protocol's own command space: the caller passes the
 * command (2 bytes, little endian), the sub command and the data, and gets the
 * reply data back.
 */
static ncl_err mc_raw(ncl_driver *self, const void *frame, size_t frame_len,
                      ncl_driver_result *out, bool write)
{
    static const char kDigits[] = "0123456789abcdef";
    mc_ctx *ctx = (mc_ctx *)self->ctx;
    ncl_mc_reply reply;
    uint16_t command;
    uint16_t subcommand;
    ncl_err err;
    char *hex;
    size_t i;

    (void)write;
    if (frame == NULL || frame_len < 4 || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    command = (uint16_t)(((const uint8_t *)frame)[0] |
                         ((const uint8_t *)frame)[1] << 8);
    subcommand = (uint16_t)(((const uint8_t *)frame)[2] |
                            ((const uint8_t *)frame)[3] << 8);
    memset(&reply, 0, sizeof(reply));
    ncl_mutex_lock(ctx->mutex);
    err = mc_request(ctx, command, subcommand, (const uint8_t *)frame + 4,
                     frame_len - 4, &reply);
    ncl_mutex_unlock(ctx->mutex);
    if (err != NCL_OK) {
        return err;
    }
    ncl_driver_result_set_raw(out, reply.data, reply.data_len);
    hex = (char *)ncl_mem_alloc(reply.data_len * 2u + 1u);
    if (hex == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < reply.data_len; i++) {
        hex[i * 2] = kDigits[(reply.data[i] >> 4) & 0xF];
        hex[i * 2 + 1] = kDigits[reply.data[i] & 0xF];
    }
    hex[reply.data_len * 2] = '\0';
    ncl_driver_result_ok(out, ncl_json_new_string(hex));
    ncl_free_safe(hex);
    return NCL_OK;
}

static ncl_err mc_read_raw(ncl_driver *self, const void *frame, size_t frame_len,
                           ncl_driver_result *out)
{
    return mc_raw(self, frame, frame_len, out, false);
}

static ncl_err mc_write_raw(ncl_driver *self, const void *frame, size_t frame_len,
                            ncl_driver_result *out)
{
    return mc_raw(self, frame, frame_len, out, true);
}

/** The frames of the last exchange, for the audit trail (§6). */
static void mc_last_raw(const ncl_driver *self, ncl_driver_raw *out)
{
    const mc_ctx *ctx = (const mc_ctx *)self->ctx;

    out->request = ctx->last_tx_len > 0 ? ctx->tx : NULL;
    out->request_len = ctx->last_tx_len;
    out->reply = ctx->last_rx_len > 0 ? ctx->rx : NULL;
    out->reply_len = ctx->last_rx_len;
}

/**
 * The methods the protocol offers without touching a device address: the
 * loopback test (§2, good keep-alive), remote RUN/STOP, the CPU type and the
 * CPU status.
 */
static ncl_err mc_call(ncl_driver *self, const char *operation,
                       const ncl_json *params, ncl_json **result)
{
    mc_ctx *ctx = (mc_ctx *)self->ctx;
    ncl_mc_reply reply;
    ncl_err err;
    uint8_t data[8];
    size_t data_len = 0;
    uint16_t command;

    (void)params; /* the methods take no arguments */
    if (result != NULL) {
        *result = NULL;
    }
    if (ncl_streq_ignore_case(operation, "loopback")) {
        command = NCL_MC_CMD_LOOPBACK;
        data_len = 6;
        memcpy(data, "\x11\x22\x33\x44\x55\x66", 6);
    } else if (ncl_streq_ignore_case(operation, "remoteRun")) {
        command = NCL_MC_CMD_REMOTE_RUN;
        /* "clear the mode first" as the usual tooling sends it */
        data[0] = 0x01;
        data[1] = 0x00;
        data[2] = 0x00;
        data_len = 3;
    } else if (ncl_streq_ignore_case(operation, "remoteStop")) {
        command = NCL_MC_CMD_REMOTE_STOP;
        data_len = 0;
    } else if (ncl_streq_ignore_case(operation, "clearError")) {
        command = NCL_MC_CMD_CLEAR_ERROR;
        data_len = 2;
        data[0] = 0x01;
        data[1] = 0x00;
    } else if (ncl_streq_ignore_case(operation, "cpuType")) {
        command = NCL_MC_CMD_CPU_TYPE;
        data_len = 0;
    } else if (ncl_streq_ignore_case(operation, "cpuStatus")) {
        command = NCL_MC_CMD_CPU_STATUS;
        data_len = 0;
    } else {
        return NCL_DRV_ERR_PROTOCOL(0x0D); /* no such operation */
    }
    memset(&reply, 0, sizeof(reply));
    ncl_mutex_lock(ctx->mutex);
    err = mc_request(ctx, command, 0x0000, data, data_len, &reply);
    ncl_mutex_unlock(ctx->mutex);
    if (err != NCL_OK) {
        return err;
    }
    if (result != NULL) {
        *result = ncl_json_new_object();
        if (*result == NULL) {
            return NCL_ERR_NOMEM;
        }
        if (reply.data_len > 0) {
            char *text = ncl_strndup((const char *)reply.data, reply.data_len);

            if (text != NULL) {
                (void)ncl_json_obj_set_string(*result, "data", text);
                ncl_free_safe(text);
            }
        }
        (void)ncl_json_obj_set_int(*result, "bytes", (long long)reply.data_len);
    }
    return NCL_OK;
}

/* ---------------------------------------------------------------- driver -- */

static ncl_err mc_create(ncl_driver *self, const ncl_json *parameters)
{
    mc_ctx *ctx = (mc_ctx *)self->ctx;
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
    text = ncl_json_obj_get_string(parameters, "frame");
    if (text != NULL) {
        if (ncl_streq_ignore_case(text, "4e")) {
            ctx->header.frame = NCL_MC_FRAME_4E;
        } else if (ncl_streq_ignore_case(text, "3e")) {
            ctx->header.frame = NCL_MC_FRAME_3E;
        } else {
            return NCL_ERR_INVALID_ARG; /* only the binary frame types */
        }
    }
    ctx->port = json_uint(parameters, "port", ctx->port);
    ctx->connect_timeout_ms = json_uint(parameters, "connectTimeoutMs",
                                        ctx->connect_timeout_ms);
    ctx->timeout_ms = json_uint(parameters, "timeoutMs", ctx->timeout_ms);
    ctx->retries = json_uint(parameters, "retries", ctx->retries);
    ctx->merge_gap = json_uint(parameters, "mergeGap", ctx->merge_gap);
    ctx->header.network = (uint8_t)json_uint(parameters, "network",
                                             ctx->header.network);
    ctx->header.plc = (uint8_t)json_uint(parameters, "plc", ctx->header.plc);
    ctx->header.station = (uint8_t)json_uint(parameters, "station",
                                             ctx->header.station);
    ctx->header.module = (uint16_t)json_uint(parameters, "module",
                                             ctx->header.module);
    ctx->header.timer = (uint16_t)json_uint(parameters, "timer", ctx->header.timer);
    if (ctx->host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return NCL_OK;
}

static ncl_err mc_open(ncl_driver *self)
{
    mc_ctx *ctx = (mc_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = mc_open_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void mc_close(ncl_driver *self)
{
    mc_ctx *ctx = (mc_ctx *)self->ctx;

    ncl_mutex_lock(ctx->mutex);
    mc_close_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
}

static bool mc_is_connected(const ncl_driver *self)
{
    return ((const mc_ctx *)self->ctx)->socket != NULL;
}

static ncl_err mc_read_batch_locked(ncl_driver *self,
                                    const ncl_address *addresses, size_t count,
                                    ncl_json **values)
{
    mc_ctx *ctx = (mc_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = mc_read_batch(self, addresses, count, values);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static ncl_err mc_write_batch_locked(ncl_driver *self,
                                     const ncl_address *addresses,
                                     const ncl_json *values, size_t count)
{
    mc_ctx *ctx = (mc_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = mc_write_batch(self, addresses, values, count);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void mc_attach_event(ncl_driver *self, ncl_driver_event_fn fn, void *user)
{
    /* MC has no unsolicited traffic: alarms are read from the device. */
    (void)self;
    (void)fn;
    (void)user;
}

static void mc_destroy(ncl_driver *self)
{
    mc_ctx *ctx;

    if (self == NULL) {
        return;
    }
    ctx = (mc_ctx *)self->ctx;
    if (ctx != NULL) {
        mc_close_session(ctx);
        ncl_free_safe(ctx->host);
        if (ctx->mutex != NULL) {
            ncl_mutex_destroy(ctx->mutex);
        }
        ncl_free_safe(ctx);
    }
    ncl_free_safe(self);
}

static const ncl_driver_ops kMcOps = {
    "mc_tcp",       mc_create,
    mc_open,        mc_close,
    mc_is_connected, mc_read_batch_locked,
    mc_write_batch_locked, mc_read_raw,
    mc_write_raw,   mc_call,
    mc_attach_event, mc_destroy,
    mc_last_raw,
};

ncl_driver *ncl_mc_tcp_create(void)
{
    mc_ctx *ctx = (mc_ctx *)ncl_mem_calloc(1, sizeof(*ctx));
    ncl_driver *driver;

    if (ctx == NULL) {
        return NULL;
    }
    ctx->port = 5534; /* §1 */
    ctx->connect_timeout_ms = 3000;
    ctx->timeout_ms = 1000;
    ctx->retries = 1;
    ctx->merge_gap = 8;
    ncl_mc_header_default(&ctx->header, NCL_MC_FRAME_3E);
    ctx->mutex = ncl_mutex_create();
    if (ctx->mutex == NULL) {
        ncl_free_safe(ctx);
        return NULL;
    }
    driver = ncl_driver_new(&kMcOps, ctx);
    if (driver == NULL) {
        ncl_mutex_destroy(ctx->mutex);
        ncl_free_safe(ctx);
        return NULL;
    }
    return driver;
}
