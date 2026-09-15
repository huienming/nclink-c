/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * Fuzz harness for the wire decoders: JSON DOM, NC-Link messages and the MQTT
 * 5.0 packet codec. Everything it touches is "untrusted bytes off the wire".
 *
 * Two ways to run it:
 *   libFuzzer (clang):  clang -fsanitize=fuzzer,address -DNCL_FUZZER \
 *                       -DFUZZ_TARGET=<0..2> ... -o fuzz && ./fuzz -max_total_time=30
 *   plain regression:   gcc ... -o fuzz_regress && ./fuzz_regress 20000
 *                       (feeds pseudo random inputs; a crash is a finding)
 *
 * FUZZ_TARGET: 0 = JSON, 1 = NC-Link message, 2 = MQTT packet codec.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_json.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_mqtt.h"

#ifndef FUZZ_TARGET
#define FUZZ_TARGET 0
#endif

static void run_json(const uint8_t *data, size_t size) {
    ncl_strbuf err;
    ncl_json *value;

    ncl_strbuf_init(&err);
    value = ncl_json_parse((const char *)data, size, &err);
    if (value != NULL) {
        char *text = ncl_json_write_string(value);
        free(text);
        ncl_json_free(value);
    }
    ncl_strbuf_free(&err);
}

static void run_message(const uint8_t *data, size_t size) {
    static const char *const topics[] = {"Query/Request/V1", "Set/Request/V1",
                                         "Sample/V1/ch1", "Event/V1",
                                         "Method/Call/Request/V1", "Ping/V1"};
    ncl_message *msg =
        ncl_message_parse(topics[size % 6], (const char *)data, size);

    if (msg != NULL) {
        char *text = ncl_message_write_string(msg);
        (void)ncl_message_is_valid(msg);
        free(text);
        ncl_message_free(msg);
    }
}

static void run_mqtt(const uint8_t *data, size_t size) {
    ncl_mqtt_packet_type type = NCL_MQTT_PKT_PUBLISH;
    uint8_t flags = 0;
    uint32_t remaining = 0;
    size_t header_len = 0;

    if (ncl_mqtt_peek_header(data, size, &type, &flags, &remaining, &header_len)) {
        const unsigned char *body = data + header_len;
        size_t body_len = size - header_len;

        if (type == NCL_MQTT_PKT_PUBLISH) {
            ncl_mqtt_publish publish;
            if (ncl_mqtt_decode_publish(flags, body, body_len, &publish) == NCL_OK) {
                ncl_mqtt_publish_free(&publish);
            }
        } else if (type == NCL_MQTT_PKT_SUBSCRIBE) {
            ncl_mqtt_suback suback;
            if (ncl_mqtt_decode_suback(body, body_len, &suback) == NCL_OK) {
                ncl_mqtt_suback_free(&suback);
            }
        } else if (type == NCL_MQTT_PKT_DISCONNECT) {
            ncl_mqtt_disconnect info;
            if (ncl_mqtt_decode_disconnect(body, body_len, &info) == NCL_OK) {
                ncl_mqtt_disconnect_free(&info);
            }
        }
    }
    {
        /* Property blocks and the remaining length field are the classic
         * decoding traps, so hit them with the same bytes. */
        ncl_mqtt_properties props;
        size_t consumed = 0;
        uint32_t value = 0;

        ncl_mqtt_properties_init(&props);
        if (ncl_mqtt_properties_read(data, size, &props, &consumed) == NCL_OK) {
            ncl_mqtt_properties_free(&props);
        }
        (void)ncl_mqtt_varint_decode(data, size, &value);
    }
}

static void run_one(const uint8_t *data, size_t size) {
#if FUZZ_TARGET == 1
    run_message(data, size);
#elif FUZZ_TARGET == 2
    run_mqtt(data, size);
#else
    run_json(data, size);
#endif
}

#if defined(NCL_FUZZER)
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    run_one(data, size);
    return 0;
}
#else
int main(int argc, char **argv) {
    unsigned iterations = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 20000;
    uint8_t buffer[4096];
    uint32_t state = 0x12345678u;
    unsigned i;

    for (i = 0; i < iterations; i++) {
        size_t len = 0;
        size_t j;

        /* xorshift: deterministic, so a crash reproduces from the iteration. */
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        len = (size_t)(state % sizeof(buffer));

        for (j = 0; j < len; j++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            /* Mostly printable/structural bytes, sometimes raw noise. */
            buffer[j] = (uint8_t)((j % 7 == 0) ? (state & 0xFF)
                                               : (state % 96) + 32);
        }
        run_one(buffer, len);
#if FUZZ_TARGET == 2
        /* Also feed a plausible MQTT fixed header so the decoder is reached. */
        if (len > 4) {
            buffer[0] = (uint8_t)((3 << 4) | (state & 0x0F));
        }
#endif
    }
    printf("fuzz target %d: %u inputs, no crash\n", FUZZ_TARGET, iterations);
    return 0;
}
#endif
