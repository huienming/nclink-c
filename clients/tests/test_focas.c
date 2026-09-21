/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * FOCAS: the codec against the golden frames of
 * protocal/docs/01-FANUC-CNC-FOCAS.md §2.1/§2.3, and the driver against a mock
 * machine that answers the way the vendor SDK needs - one reply block per
 * request block, which is the rule that turned "-17 forever" into rc=0.
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink/clients/focas.h"
#include "focas/ncl_focas_pdu.h"

/* ---------------------------------------------------------------- helpers -- */

static void put_u16be(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static uint16_t get_u16be(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8) | in[1]);
}

static void put_u32be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void put_float_be(uint8_t *out, float number)
{
    uint32_t bits = 0;

    memcpy(&bits, &number, sizeof(bits));
    put_u32be(out, bits);
}

/** Compare @p len bytes against a hex string with the spaces taken out. */
static bool hex_equals(const uint8_t *data, size_t len, const char *hex)
{
    char clean[512];
    size_t n = 0;
    size_t i;
    size_t k = 0;

    for (i = 0; hex[i] != '\0' && n + 1 < sizeof(clean); i++) {
        if (hex[i] != ' ') {
            clean[n++] = hex[i];
        }
    }
    clean[n] = '\0';
    if (n != len * 2u) {
        return false;
    }
    for (i = 0; i < len; i++) {
        int hi = -1;
        int lo = -1;

        if (clean[k] >= '0' && clean[k] <= '9') {
            hi = clean[k] - '0';
        } else if (clean[k] >= 'a' && clean[k] <= 'f') {
            hi = clean[k] - 'a' + 10;
        }
        k++;
        if (clean[k] >= '0' && clean[k] <= '9') {
            lo = clean[k] - '0';
        } else if (clean[k] >= 'a' && clean[k] <= 'f') {
            lo = clean[k] - 'a' + 10;
        }
        k++;
        if (hi < 0 || lo < 0 || data[i] != (uint8_t)((hi << 4) | lo)) {
            printf("      byte %u: got %02x want %02x%02x\n", (unsigned)i,
                   data[i], hi, lo);
            return false;
        }
    }
    return true;
}

/* -------------------------------------------------------------- the codec -- */

static void test_golden_frames(void)
{
    uint8_t frame[256];
    uint8_t body[128];
    ncl_focas_pdu pdu;
    size_t total = 0;
    size_t used;
    ncl_focas_cb cb;
    ncl_err err;
    const ncl_focas_item *item;

    NCL_TEST_CASE("hello frame matches §2.1");
    body[0] = 0;
    body[1] = 1;
    total = ncl_focas_build(frame, sizeof(frame), NCL_FOCAS_FUNC_HELLO,
                            NCL_FOCAS_DIR_REQ, body, 2);
    NCL_CHECK_EQ_INT(total, 12);
    NCL_CHECK(hex_equals(frame, total, "a0a0a0a0 0001 01 01 0002 0001"));

    NCL_TEST_CASE("a block-less func 0x21 frame matches §2.1");
    total = ncl_focas_build(frame, sizeof(frame), NCL_FOCAS_FUNC_CMD,
                            NCL_FOCAS_DIR_REQ, NULL, 0);
    NCL_CHECK_EQ_INT(total, 10);
    NCL_CHECK(hex_equals(frame, total, "a0a0a0a0 0001 21 01 0000"));

    NCL_TEST_CASE("the negotiation frame matches the §2.3 capture");
    used = ncl_focas_body_begin(body, sizeof(body));
    NCL_CHECK_EQ_INT(used, 2);
    ncl_focas_cb_init(&cb, 14);
    cb.arg0 = 0x26f0;
    cb.arg1 = 0x26f0;
    used = ncl_focas_body_add(body, sizeof(body), used, &cb);
    NCL_CHECK_EQ_INT(used, 2u + NCL_FOCAS_CB_SIZE);
    total = ncl_focas_build(frame, sizeof(frame), NCL_FOCAS_FUNC_CMD,
                            NCL_FOCAS_DIR_REQ, body, used);
    NCL_CHECK_EQ_INT(total, 40);
    NCL_CHECK(hex_equals(frame, total,
                         "a0a0a0a0 0001 21 01 001e"
                         " 0001 001c 0001 0001 000e"
                         " 0000 26f0 0000 26f0"
                         " 0000 0000 0000 0000"
                         " 0000 0000"));

    NCL_TEST_CASE("the STATINFO request matches the §2.3 capture");
    item = ncl_focas_item_lookup("STATINFO");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        size_t i;

        NCL_CHECK_EQ_INT(item->cb_count, 3);
        NCL_CHECK_EQ_INT(item->cbs[0], 25);
        NCL_CHECK_EQ_INT(item->cbs[1], 225);
        NCL_CHECK_EQ_INT(item->cbs[2], 152);
        used = ncl_focas_body_begin(body, sizeof(body));
        for (i = 0; i < item->cb_count; i++) {
            ncl_focas_cb one;

            ncl_focas_cb_init(&one, item->cbs[i]);
            used = ncl_focas_body_add(body, sizeof(body), used, &one);
            NCL_CHECK(used != 0);
        }
        total = ncl_focas_build(frame, sizeof(frame), NCL_FOCAS_FUNC_CMD,
                                NCL_FOCAS_DIR_REQ, body, used);
        NCL_CHECK_EQ_INT(total, 96);
        NCL_CHECK(hex_equals(frame, total,
                             "a0a0a0a0 0001 21 01 0056"
                             " 0003"
                             " 001c 0001 0001 0019 00000000 00000000 00000000 00000000 0000 0000"
                             " 001c 0001 0001 00e1 00000000 00000000 00000000 00000000 0000 0000"
                             " 001c 0001 0001 0098 00000000 00000000 00000000 00000000 0000 0000"));
    }

    NCL_TEST_CASE("split reads the header back");
    body[0] = 0;
    body[1] = 2;
    total = ncl_focas_build(frame, sizeof(frame), NCL_FOCAS_FUNC_HELLO,
                            NCL_FOCAS_DIR_REQ, body, 2);
    err = ncl_focas_split(frame, 10, &pdu, &total);
    NCL_CHECK_EQ_INT(err, NCL_ERR_RANGE); /* the body is still arriving */
    NCL_CHECK_EQ_INT(total, 12);
    err = ncl_focas_split(frame, total, &pdu, NULL);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(pdu.type, NCL_FOCAS_TYPE_V1);
    NCL_CHECK_EQ_INT(pdu.func, NCL_FOCAS_FUNC_HELLO);
    NCL_CHECK_EQ_INT(pdu.dir, NCL_FOCAS_DIR_REQ);
    NCL_CHECK_EQ_INT(pdu.length, 2);

    NCL_TEST_CASE("a wrong magic is refused");
    frame[1] = 0x0A;
    err = ncl_focas_split(frame, 12, &pdu, NULL);
    NCL_CHECK_EQ_INT(err, NCL_FOCAS_ERR_MAGIC);
    NCL_TEST_CASE("a direction outside 1..4 is refused");
    frame[1] = 0xA0;
    frame[7] = 0;
    err = ncl_focas_split(frame, 12, &pdu, NULL);
    NCL_CHECK_EQ_INT(err, NCL_FOCAS_ERR_HEADER);
}

static void test_hello_reply(void)
{
    uint8_t body[16u + 8u * 2u];
    size_t records = 0;
    ncl_err err;

    NCL_TEST_CASE("the func 1 reply is 16 + 8n bytes");
    memset(body, 0, sizeof(body));
    put_u16be(body + 8, 2);
    err = ncl_focas_hello_reply(body, sizeof(body), &records);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(records, 2);

    NCL_TEST_CASE("16 + 8n is not optional (§2.2 rule 5)");
    err = ncl_focas_hello_reply(body, sizeof(body) - 1u, &records);
    NCL_CHECK_EQ_INT(err, NCL_FOCAS_ERR_LENGTH);
    err = ncl_focas_hello_reply(body, 12, &records);
    NCL_CHECK_EQ_INT(err, NCL_FOCAS_ERR_LENGTH);

    NCL_TEST_CASE("hello fields are big endian");
    put_u16be(body + 2, 0x0002);
    NCL_CHECK_EQ_INT(ncl_focas_hello_field(body, sizeof(body), 2), 2);
    put_u16be(body + 16, 0x1234);
    NCL_CHECK_EQ_INT(ncl_focas_hello_field(body, sizeof(body), 16), 0x1234);
    NCL_CHECK_EQ_INT(ncl_focas_hello_field(body, 16, 16), 0); /* past the header */
}

/** Build a reply body with @p count blocks out of the given payloads. */
static size_t build_blocks(uint8_t *out, size_t cap, size_t count,
                           const uint8_t *const payload[], const size_t lens[])
{
    size_t used = 2;
    size_t i;

    if (cap < 2u) {
        return 0;
    }
    put_u16be(out, (uint16_t)count);
    for (i = 0; i < count; i++) {
        size_t plen = lens[i];
        size_t size = 16u + plen;

        if (used + size > cap) {
            return 0;
        }
        memset(out + used, 0, size);
        put_u16be(out + used, (uint16_t)size);
        put_u16be(out + used + 8, 0); /* return code: the machine said OK */
        put_u16be(out + used + 14, (uint16_t)plen);
        if (plen > 0) {
            memcpy(out + used + 16, payload[i], plen);
        }
        used += size;
    }
    return used;
}

static void test_blocks(void)
{
    uint8_t body[256];
    uint8_t first[4];
    uint8_t second[4];
    const uint8_t *payloads[2];
    size_t lens[2];
    const uint8_t *block = NULL;
    size_t block_len = 0;
    size_t payload_len = 0;
    const uint8_t *payload;
    size_t used;
    ncl_err err;

    put_u32be(first, 0x11223344u);
    put_u32be(second, 0xAABBCCDDu);
    payloads[0] = first;
    payloads[1] = second;
    lens[0] = sizeof(first);
    lens[1] = sizeof(second);
    used = build_blocks(body, sizeof(body), 2, payloads, lens);
    NCL_CHECK(used > 0);

    NCL_TEST_CASE("the block count lives in body[0..2)");
    NCL_CHECK_EQ_INT(ncl_focas_block_count(body, used), 2);

    NCL_TEST_CASE("blocks are walked with their own size field");
    err = ncl_focas_block_at(body, used, 0, &block, &block_len);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(ncl_focas_block_code(block, block_len), 0);
    payload = ncl_focas_block_payload(block, block_len, &payload_len);
    NCL_CHECK_EQ_INT(payload_len, 4);
    NCL_CHECK_EQ_INT(get_u16be(payload), 0x1122);
    err = ncl_focas_block_at(body, used, 1, &block, &block_len);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    payload = ncl_focas_block_payload(block, block_len, &payload_len);
    NCL_CHECK_EQ_INT(get_u16be(payload), 0xAABB);

    NCL_TEST_CASE("an index past the count is the §2.3 rule 1 error");
    err = ncl_focas_block_at(body, used, 2, &block, &block_len);
    NCL_CHECK_EQ_INT(err, NCL_FOCAS_ERR_RB_MISSING);
    /* a body shorter than the count it claims is a length complaint */
    NCL_CHECK_EQ_INT(ncl_focas_check_blocks(body, 2, NULL),
                     NCL_FOCAS_ERR_LENGTH);
    /* no block at all is the count complaint */
    NCL_CHECK_EQ_INT(ncl_focas_check_blocks(body, 1, NULL),
                     NCL_FOCAS_ERR_RB_COUNT);

    NCL_TEST_CASE("a block whose return code is not zero is refused");
    put_u16be(body + 2 + 8, 0xFFFF); /* block 0 says -1 */
    err = ncl_focas_check_blocks(body, used, NULL);
    NCL_CHECK_EQ_INT(err, NCL_FOCAS_ERR_RB_CODE);
    put_u16be(body + 2 + 8, 0);
    NCL_CHECK_EQ_INT(ncl_focas_check_blocks(body, used, NULL), NCL_OK);
}

static void test_items(void)
{
    const ncl_focas_item *item;
    uint16_t code = 0;

    NCL_TEST_CASE("the §2.3 item table resolves names case insensitively");
    item = ncl_focas_item_lookup("actf");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        NCL_CHECK_EQ_INT(item->cb_count, 1);
        NCL_CHECK_EQ_INT(item->cbs[0], 0x24);
    }
    item = ncl_focas_item_lookup("RDPROGDIR3");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        NCL_CHECK_EQ_INT(item->cbs[0], 0x06);
        NCL_CHECK_EQ_INT(item->arg0[0], 0x13);
        NCL_CHECK_EQ_INT(item->arg1[0], 1);
    }
    item = ncl_focas_item_lookup("RDCOUNT");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        NCL_CHECK_EQ_INT(item->cbs[0], 0x8b);
        /* 件数是 0x8b 的 d=e=0；d=e=1 那一支是刀具寿命（cnc_rdlife） ——
         * 2026-09 拿官方 SDK 逐条问过（tools/site-probe/focas_sdk_probe.*）。 */
        NCL_CHECK_EQ_INT(item->arg0[0], 0);
        NCL_CHECK_EQ_INT(item->arg1[0], 0);
    }
    item = ncl_focas_item_lookup("RDLIFE");
    NCL_CHECK(item != NULL);
    if (item != NULL) {
        NCL_CHECK_EQ_INT(item->cbs[0], 0x8b);
        NCL_CHECK_EQ_INT(item->arg0[0], 1);
        NCL_CHECK_EQ_INT(item->arg1[0], 1);
    }
    /* 这一轮新核出来的码：程序号 / 行号 / 报警状态 / 刀具组数 / 时钟 */
    item = ncl_focas_item_lookup("RDPRG");
    NCL_CHECK(item != NULL && item->cbs[0] == 0x1c);
    item = ncl_focas_item_lookup("RDSEQ");
    NCL_CHECK(item != NULL && item->cbs[0] == 0x1d);
    item = ncl_focas_item_lookup("RDALM");
    NCL_CHECK(item != NULL && item->cbs[0] == 0x1a);
    item = ncl_focas_item_lookup("RDNGROUP");
    NCL_CHECK(item != NULL && item->cbs[0] == 0x4a);
    item = ncl_focas_item_lookup("RDTIMER");
    NCL_CHECK(item != NULL && item->cbs[0] == 0x120);
    item = ncl_focas_item_lookup("RDTIMER2");
    NCL_CHECK(item != NULL && item->cbs[0] == 0x120 && item->arg0[0] == 2);
    item = ncl_focas_item_lookup("RDBLKCOUNT");
    NCL_CHECK(item != NULL && item->cbs[0] == 0x35);
    /* 伺服延迟量（= 跟踪误差）：0x26 的 d = 9，e = ALL_AXES（假机床实测的请求帧） */
    item = ncl_focas_item_lookup("SV_DELAY");
    NCL_CHECK(item != NULL && item->cbs[0] == 0x26 && item->arg0[0] == 9 &&
              item->arg1[0] == 0xffffffffu);
    NCL_CHECK(ncl_focas_item_lookup("no-such-item") == NULL);

    NCL_TEST_CASE("a bare code is parsed instead");
    NCL_CHECK(ncl_focas_parse_code("36", &code));
    NCL_CHECK_EQ_INT(code, 36);
    NCL_CHECK(ncl_focas_parse_code("0x24", &code));
    NCL_CHECK_EQ_INT(code, 0x24);
    NCL_CHECK(ncl_focas_parse_code("CB:0x8b", &code));
    NCL_CHECK_EQ_INT(code, 0x8b);
    NCL_CHECK(!ncl_focas_parse_code("nope", &code));
    NCL_CHECK(!ncl_focas_parse_code("CB:", &code));
}

static void test_decode(void)
{
    uint8_t data[16];
    ncl_json *value = NULL;
    long long number = 0;
    double real = 0;

    NCL_TEST_CASE("values are big endian");
    put_u32be(data, 0x11223344u);
    NCL_CHECK_EQ_INT(ncl_focas_decode(data, 4, NCL_DTYPE_INT32, 1, &value), NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 0x11223344);
    ncl_json_free(value);
    value = NULL;

    put_u16be(data, 0xFFFE);
    NCL_CHECK_EQ_INT(ncl_focas_decode(data, 2, NCL_DTYPE_INT16, 1, &value), NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, -2);
    ncl_json_free(value);
    value = NULL;

    put_float_be(data, 42.5f);
    NCL_CHECK_EQ_INT(ncl_focas_decode(data, 4, NCL_DTYPE_FLOAT32, 1, &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_double(value, &real));
    NCL_CHECK(real > 42.49 && real < 42.51);
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("an array comes back when length > 1");
    put_u16be(data, 1);
    put_u16be(data + 2, 2);
    put_u16be(data + 4, 3);
    NCL_CHECK_EQ_INT(ncl_focas_decode(data, 6, NCL_DTYPE_INT16, 3, &value), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_json_arr_len(value), 3);
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("a short payload is a range error, not a short read");
    NCL_CHECK_EQ_INT(ncl_focas_decode(data, 3, NCL_DTYPE_INT32, 1, &value),
                     NCL_ERR_RANGE);
}

/* ------------------------------------------------------------- the mock -- */

typedef struct {
    ncl_socket *listener;
    unsigned    port;
    ncl_thread *thread;
    bool        stop;
    int         requests;
    uint8_t     last_func;
    size_t      last_blocks;
    uint8_t     hello[16u + 8u * 2u];
    size_t      hello_len;
    uint8_t     payload[NCL_FOCAS_ITEM_CBS][80];
    size_t      payload_len[NCL_FOCAS_ITEM_CBS];
    size_t      payload_count;
    int         short_by; /**< reply with fewer blocks than asked */
    /* 程序上下行（func 0x11/0x12/0x13）：数据帧收下来、不回，别的照块回 */
    uint8_t     transfer[1024];
    size_t      transfer_bytes;
    uint8_t     transfer_dir;
    uint8_t     seen[16];
    size_t      seen_count;
} focas_mock;

static size_t mock_block_body(uint8_t *out, size_t cap, size_t count,
                              const focas_mock *mock)
{
    size_t used = 2;
    size_t i;

    put_u16be(out, (uint16_t)count);
    for (i = 0; i < count; i++) {
        size_t k = i < mock->payload_count ? i : mock->payload_count - 1u;
        size_t plen = mock->payload_len[k];
        size_t size = 16u + plen;

        if (used + size > cap) {
            return 0;
        }
        memset(out + used, 0, size);
        put_u16be(out + used, (uint16_t)size);
        put_u16be(out + used + 8, 0);
        put_u16be(out + used + 14, (uint16_t)plen);
        if (plen > 0) {
            memcpy(out + used + 16, mock->payload[k], plen);
        }
        used += size;
    }
    return used;
}

static void mock_main(void *arg)
{
    focas_mock *mock = (focas_mock *)arg;

    while (!mock->stop) {
        ncl_socket *peer = ncl_socket_accept(mock->listener, 200);

        if (peer == NULL) {
            continue;
        }
        for (;;) {
            uint8_t header[NCL_FOCAS_HEADER];
            uint8_t frame[2048];
            uint8_t body[1024];
            uint8_t reply[2048];
            ncl_focas_pdu pdu;
            size_t total = 0;
            size_t body_len;
            size_t frame_len;
            size_t blocks;

            if (ncl_socket_recv_exact(peer, header, sizeof(header), 2000) != NCL_OK) {
                break;
            }
            memcpy(frame, header, sizeof(header));
            memset(&pdu, 0, sizeof(pdu));
            {
                ncl_err split = ncl_focas_split(frame, sizeof(header), &pdu,
                                                &total);

                if (split == NCL_ERR_RANGE) {
                    if (total > sizeof(frame)) {
                        break;
                    }
                    if (pdu.length > 0 &&
                        ncl_socket_recv_exact(peer, frame + sizeof(header),
                                              pdu.length, 2000) != NCL_OK) {
                        break;
                    }
                    split = ncl_focas_split(frame, total, &pdu, NULL);
                }
                /* 体长为 0 的帧（bye、传输的 end）split 直接回 OK——原来这里只认
                 * NCL_ERR_RANGE，把 end 帧当成 bye 断掉了。 */
                if (split != NCL_OK) {
                    break;
                }
            }
            mock->requests++;
            mock->last_func = pdu.func;
            if (mock->seen_count < sizeof(mock->seen)) {
                mock->seen[mock->seen_count++] = pdu.func;
            }
            if (pdu.func == NCL_FOCAS_FUNC_DWN_DATA) {
                /* 数据帧：收下程序文本，**不回**（官方 SDK 就是这么发的）。 */
                size_t keep = pdu.length;

                if (keep > sizeof(mock->transfer) - mock->transfer_bytes) {
                    keep = sizeof(mock->transfer) - mock->transfer_bytes;
                }
                memcpy(mock->transfer + mock->transfer_bytes,
                       frame + sizeof(header), keep);
                mock->transfer_bytes += keep;
                mock->transfer_dir = pdu.dir;
                continue;
            }
            if (pdu.func == NCL_FOCAS_FUNC_HELLO) {
                body_len = mock->hello_len;
                memcpy(body, mock->hello, body_len);
            } else if (pdu.func == NCL_FOCAS_FUNC_CMD ||
                       pdu.func == NCL_FOCAS_FUNC_DWN_START ||
                       pdu.func == NCL_FOCAS_FUNC_DWN_END) {
                blocks = 0;
                if (pdu.length >= 2u) {
                    blocks = get_u16be(frame + sizeof(header));
                }
                mock->last_blocks = blocks;
                if (blocks == 0) {
                    blocks = 1; /* §2.2 rule 6: a 0x21 reply needs a block */
                }
                if (mock->short_by > 0 && blocks > (size_t)mock->short_by) {
                    blocks -= (size_t)mock->short_by;
                }
                body_len = mock_block_body(body, sizeof(body), blocks, mock);
            } else {
                break; /* the bye: the SDK closes here, so does the mock */
            }
            if (body_len == 0) {
                break;
            }
            frame_len = ncl_focas_build(reply, sizeof(reply), pdu.func,
                                        NCL_FOCAS_DIR_RESP, body, body_len);
            if (frame_len == 0 ||
                ncl_socket_send(peer, reply, frame_len) != NCL_OK) {
                break;
            }
        }
        ncl_socket_close(peer);
    }
}

static focas_mock *mock_start(void)
{
    focas_mock *mock = (focas_mock *)ncl_mem_calloc(1, sizeof(*mock));

    if (mock == NULL) {
        return NULL;
    }
    mock->listener = ncl_socket_listen(0, NULL, 0);
    if (mock->listener == NULL) {
        ncl_free_safe(mock);
        return NULL;
    }
    mock->port = ncl_socket_local_port(mock->listener);
    /* the hello reply: 16 bytes of header, no records, field 2 = 0 so the
     * driver takes the §2.3 "else" branch and sends the code 14 probe */
    memset(mock->hello, 0, sizeof(mock->hello));
    put_u16be(mock->hello + 8, 0);
    mock->hello_len = 16;
    mock->payload_count = 1;
    mock->payload_len[0] = 4;
    put_u32be(mock->payload[0], 12345);
    mock->thread = ncl_thread_start(mock_main, mock);
    if (mock->thread == NULL) {
        ncl_socket_close(mock->listener);
        ncl_free_safe(mock);
        return NULL;
    }
    return mock;
}

static void mock_stop(focas_mock *mock)
{
    if (mock == NULL) {
        return;
    }
    mock->stop = true;
    ncl_thread_join(mock->thread);
    ncl_socket_close(mock->listener);
    ncl_free_safe(mock);
}

static ncl_driver *focas_driver(focas_mock *mock, const char *extra)
{
    ncl_driver *driver;
    ncl_strbuf json;
    ncl_json *params;

    /* 和适配器一样：驱动直接造出来（没有按名字查的注册表）。 */
    driver = ncl_focas_create();
    if (driver == NULL) {
        return NULL;
    }
    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"host\":\"127.0.0.1\",\"port\":%u,"
                            "\"timeoutMs\":800,\"retries\":0%s}",
                            mock->port, extra != NULL ? extra : "");
    params = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    if (params == NULL || driver->ops->create(driver, params) != NCL_OK) {
        ncl_json_free(params);
        driver->ops->destroy(driver);
        return NULL;
    }
    ncl_json_free(params);
    return driver;
}

static ncl_err read_point(ncl_driver *driver, const char *area, long long block,
                          int length, const char *dtype, ncl_json **value)
{
    ncl_strbuf json;
    ncl_json *node;
    ncl_address address;
    ncl_err err;

    ncl_strbuf_init(&json);
    (void)ncl_strbuf_printf(&json,
                            "{\"area\":\"%s\",\"offset\":%lld,\"length\":%d,"
                            "\"dtype\":\"%s\"}",
                            area, block, length, dtype);
    node = ncl_json_parse_cstr(ncl_strbuf_cstr(&json), NULL);
    ncl_strbuf_free(&json);
    if (node == NULL) {
        return NCL_ERR_PARSE;
    }
    err = ncl_address_from_json(node, &address);
    ncl_json_free(node);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_driver_read_one(driver, &address, value);
    ncl_address_clear(&address);
    return err;
}

static void test_driver(void)
{
    focas_mock *mock = mock_start();
    ncl_driver *driver;
    ncl_json *value = NULL;
    long long number = 0;
    double real = 0;

    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    driver = focas_driver(mock, NULL);
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        mock_stop(mock);
        return;
    }

    NCL_TEST_CASE("open negotiates the §2.3 handshake");
    NCL_CHECK_EQ_INT(driver->ops->open(driver), NCL_OK);
    NCL_CHECK(driver->ops->is_connected(driver));
    /* hello + the 0 block probe + the code 14 probe */
    NCL_CHECK_EQ_INT(mock->requests, 3);
    NCL_CHECK_EQ_INT(mock->last_func, NCL_FOCAS_FUNC_CMD);
    NCL_CHECK_EQ_INT(mock->last_blocks, 1);

    NCL_TEST_CASE("a read gets the block the point asked for");
    NCL_CHECK_EQ_INT(read_point(driver, "RDCOUNT", 0, 1, "int32", &value), NCL_OK);
    NCL_CHECK(value != NULL);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 12345);
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(mock->requests, 4);

    NCL_TEST_CASE("a value is big endian and scaled by the point's dtype");
    put_u32be(mock->payload[0], 0x42F40000u); /* 122.0f */
    mock->payload_len[0] = 4;
    NCL_CHECK_EQ_INT(read_point(driver, "ACTF", 0, 1, "float32", &value), NCL_OK);
    NCL_CHECK(ncl_json_as_double(value, &real));
    NCL_CHECK(real > 121.9 && real < 122.1);
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("STATINFO's blocks are read one by one (§2.3 ODBST split)");
    mock->payload_count = 3;
    mock->payload_len[0] = 6;
    put_u16be(mock->payload[0], 11);
    put_u16be(mock->payload[0] + 2, 22);
    put_u16be(mock->payload[0] + 4, 33);
    mock->payload_len[1] = 2;
    put_u16be(mock->payload[1], 7);
    mock->payload_len[2] = 2;
    put_u16be(mock->payload[2], 9);
    NCL_CHECK_EQ_INT(read_point(driver, "STATINFO", 1, 1, "int16", &value), NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 7);
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(read_point(driver, "STATINFO", 2, 1, "int16", &value), NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 9);
    ncl_json_free(value);
    value = NULL;
    NCL_CHECK_EQ_INT(read_point(driver, "STATINFO", 0, 3, "int16", &value), NCL_OK);
    NCL_CHECK_EQ_INT(ncl_json_arr_len(value), 3);
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("an item the table does not know is sent as a bare code");
    mock->payload_len[0] = 4;
    put_u32be(mock->payload[0], 0x0000002Au);
    NCL_CHECK_EQ_INT(read_point(driver, "0x22", 0, 1, "int32", &value), NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 42);
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("a block index past the reply is the rule 1 error");
    NCL_CHECK_EQ_INT(read_point(driver, "RDCOUNT", 4, 1, "int32", &value),
                     NCL_FOCAS_ERR_RB_MISSING);

    NCL_TEST_CASE("write is not supported (no frame was captured)");
    {
        ncl_address address;
        ncl_json *node = ncl_json_parse_cstr(
            "{\"area\":\"RDCOUNT\",\"offset\":0,\"length\":1,\"dtype\":\"int32\"}",
            NULL);
        ncl_json *scalar = ncl_json_new_int(1);

        NCL_CHECK(node != NULL);
        NCL_CHECK(ncl_address_from_json(node, &address) == NCL_OK);
        /* 驱动没有 write_batch（FOCAS 只读，没抓到写帧）：骨架对空缺的位回
         * NOT_SUPPORTED，所以行为还是"写被拒"。 */
        NCL_CHECK_EQ_INT(ncl_driver_write_one(driver, &address, scalar),
                         NCL_ERR_NOT_SUPPORTED);
        ncl_address_clear(&address);
        ncl_json_free(scalar);
        ncl_json_free(node);
    }

    NCL_TEST_CASE("call(\"items\") lists what the table knows");
    NCL_CHECK_EQ_INT(driver->ops->call(driver, "items", NULL, &value), NCL_OK);
    NCL_CHECK(value != NULL);
    NCL_CHECK(ncl_json_arr_len(value) >= 8);
    ncl_json_free(value);
    value = NULL;
    NCL_TEST_CASE("call(\"session\") reports the hello");
    NCL_CHECK_EQ_INT(driver->ops->call(driver, "session", NULL, &value), NCL_OK);
    NCL_CHECK(value != NULL);
    NCL_CHECK_EQ_INT(ncl_json_obj_get_int(value, "helloField2", -1), 0);
    NCL_CHECK_EQ_INT(ncl_json_obj_get_int(value, "probeBlocks", -1), 1);
    ncl_json_free(value);
    value = NULL;
    NCL_TEST_CASE("an unknown operation is refused");
    NCL_CHECK_EQ_INT(driver->ops->call(driver, "nope", NULL, &value),
                     NCL_DRV_ERR_PROTOCOL(0x94));

    NCL_TEST_CASE("the last exchange reaches the audit trail");
    {
        ncl_driver_raw raw;

        memset(&raw, 0, sizeof(raw));
        ncl_driver_last_raw(driver, &raw);
        NCL_CHECK(raw.request != NULL && raw.request_len >= 10);
        NCL_CHECK(raw.reply != NULL && raw.reply_len >= 10);
        NCL_CHECK_EQ_INT(raw.request[6], NCL_FOCAS_FUNC_CMD);
    }

    driver->ops->close(driver);
    NCL_CHECK(!driver->ops->is_connected(driver));
    driver->ops->destroy(driver);
    mock_stop(mock);
}

static void test_driver_short_reply(void)
{
    focas_mock *mock = mock_start();
    ncl_driver *driver;
    ncl_json *value = NULL;

    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    driver = focas_driver(mock, NULL);
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        mock_stop(mock);
        return;
    }
    NCL_CHECK_EQ_INT(driver->ops->open(driver), NCL_OK);

    NCL_TEST_CASE("one block short is exactly what §2.3 rule 1 is about");
    /* STATINFO asks for three blocks; the machine answers two */
    mock->payload_count = 3;
    mock->payload_len[0] = 4;
    put_u16be(mock->payload[0], 11);
    put_u16be(mock->payload[0] + 2, 22);
    mock->payload_len[1] = 2;
    put_u16be(mock->payload[1], 7);
    mock->payload_len[2] = 2;
    put_u16be(mock->payload[2], 9);
    mock->short_by = 1;
    NCL_CHECK_EQ_INT(read_point(driver, "STATINFO", 2, 1, "int16", &value),
                     NCL_FOCAS_ERR_RB_MISSING);
    NCL_CHECK(value == NULL);
    NCL_CHECK_EQ_INT(read_point(driver, "STATINFO", 1, 1, "int16", &value),
                     NCL_OK); /* the blocks that did arrive still read */
    ncl_json_free(value);
    value = NULL;

    driver->ops->destroy(driver);
    mock_stop(mock);
}

static void test_driver_payload_offset(void)
{
    focas_mock *mock = mock_start();
    ncl_driver *driver;
    ncl_json *value = NULL;
    long long number = 0;
    double real = 0;

    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    driver = focas_driver(mock, NULL);
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        mock_stop(mock);
        return;
    }
    NCL_TEST_CASE("open negotiates");
    NCL_CHECK_EQ_INT(driver->ops->open(driver), NCL_OK);

    NCL_TEST_CASE("\"ACTF@4\" reads the payload from byte 4 (the second axis)");
    mock->payload_count = 1;
    mock->payload_len[0] = 8;
    put_u32be(mock->payload[0], 0x42F40000u); /* 122.0f */
    put_u32be(mock->payload[0] + 4, 0x42360000u); /* 45.5f */
    NCL_CHECK_EQ_INT(read_point(driver, "ACTF@4", 0, 1, "float32", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_double(value, &real));
    NCL_CHECK(real > 45.4 && real < 45.6);
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("\"STATINFO@12\" is still the three block STATINFO request");
    mock->payload_count = 3;
    mock->payload_len[0] = 18;
    put_u16be(mock->payload[0] + 0, 11);  /* manual */
    put_u16be(mock->payload[0] + 12, 1);  /* alarm  */
    mock->payload_len[1] = 2;
    put_u16be(mock->payload[1], 7);
    mock->payload_len[2] = 2;
    put_u16be(mock->payload[2], 9);
    NCL_CHECK_EQ_INT(read_point(driver, "STATINFO@12", 0, 1, "int16", &value),
                     NCL_OK);
    NCL_CHECK(ncl_json_as_int(value, &number));
    NCL_CHECK_EQ_INT(number, 1);
    NCL_CHECK_EQ_INT(mock->last_blocks, 3);
    ncl_json_free(value);
    value = NULL;

    NCL_TEST_CASE("a payload offset past the block is a range error");
    mock->payload_count = 1;
    mock->payload_len[0] = 4;
    NCL_CHECK_EQ_INT(read_point(driver, "ACTF@64", 0, 1, "float32", &value),
                     NCL_ERR_RANGE);

    NCL_TEST_CASE("a name whose tail is not a number is taken whole");
    NCL_CHECK_EQ_INT(read_point(driver, "AXIS@0", 0, 1, "int16", &value),
                     NCL_ERR_INVALID_DATA_NAME);

    driver->ops->destroy(driver);
    mock_stop(mock);
}

static void test_driver_no_negotiate(void)
{
    focas_mock *mock = mock_start();
    ncl_driver *driver;

    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    driver = focas_driver(mock, ",\"negotiate\":false");
    NCL_CHECK(driver != NULL);
    if (driver == NULL) {
        mock_stop(mock);
        return;
    }
    NCL_TEST_CASE("\"negotiate\":false stops after the hello");
    NCL_CHECK_EQ_INT(driver->ops->open(driver), NCL_OK);
    NCL_CHECK_EQ_INT(mock->requests, 1);
    NCL_CHECK_EQ_INT(mock->last_func, NCL_FOCAS_FUNC_HELLO);

    driver->ops->destroy(driver);
    mock_stop(mock);
}

/*
 * 语义层里"帧还没抓到"的那三条：它们现在回 NCL_ERR_UNAVAILABLE，理由里带一句"要抓
 * 哪一帧"（排障时从 ncl_focas_last_error() 看）。会话不用连机床 —— open() 本来就不
 * 连（第一次读才连），所以这一段是离线的。
 */
/*
 * 语义层对着一台假机床跑一遍：真读的那几条（程序号/行号/报警状态/刀具组数/时钟/
 * 进给速度/模式）各自把值放对地方，就应当读得回来；还没核准的那几条（位置/负载/
 * 刀补/参数/宏变量/工件坐标/模态/系统）回 NCL_ERR_UNAVAILABLE，理由里写清要抓
 * 哪一帧。
 */
static void test_semantics(void)
{
    focas_mock *mock = mock_start();
    ncl_focas_config config;
    ncl_focas *focas;
    char *err = NULL;
    char text[64];
    long long number = 0;
    double real = 0.0;

    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    ncl_focas_config_default(&config);
    config.host = "127.0.0.1";
    config.port = mock->port;
    focas = ncl_focas_open(&config, &err);
    NCL_CHECK(focas != NULL);
    if (focas == NULL) {
        ncl_free_safe(err);
        mock_stop(mock);
        return;
    }

    NCL_TEST_CASE("程序号：一条应答两个 short（@2 运行中、@6 主程序）");
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_count = 1;
    mock->payload_len[0] = 8;
    put_u16be(mock->payload[0] + 2, 1234);
    put_u16be(mock->payload[0] + 6, 5678);
    NCL_CHECK_EQ_INT(ncl_focas_program_number(focas, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 1234);
    NCL_CHECK_EQ_INT(ncl_focas_main_program_number(focas, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 5678);

    NCL_TEST_CASE("程序行号：载荷 @0 的 BE32，出门是文本（表 7 的 LINE_NUMBER）");
    mock->payload_len[0] = 4;
    put_u32be(mock->payload[0], 4321);
    NCL_CHECK_EQ_INT(ncl_focas_line_number(focas, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "N4321");

    NCL_TEST_CASE("报警状态位：0 = 无报警，非 0 就是有报警");
    mock->payload_len[0] = 4;
    put_u32be(mock->payload[0], 0);
    NCL_CHECK_EQ_INT(ncl_focas_alarm_status(focas, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 0);
    put_u32be(mock->payload[0], 0x00001040u); /* SV + OT 两位 */
    NCL_CHECK_EQ_INT(ncl_focas_alarm_status(focas, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 0x1040);

    NCL_TEST_CASE("刀具组数与时钟（时钟回合计秒数）");
    mock->payload_len[0] = 4;
    put_u32be(mock->payload[0], 12);
    NCL_CHECK_EQ_INT(ncl_focas_tool_group_count(focas, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 12);
    mock->payload_len[0] = 8;
    put_u32be(mock->payload[0], 90);      /* 90 分钟 */
    put_u32be(mock->payload[0] + 4, 500); /* 500 毫秒 */
    NCL_CHECK_EQ_INT(ncl_focas_timer(focas, NCL_FOCAS_TIMER_CUTTING, &number),
                     NCL_OK);
    NCL_CHECK_EQ_INT(number, 90 * 60);

    NCL_TEST_CASE("进给速度：ACTF 每轴一个 float（第 2 根轴在载荷 @4）");
    mock->payload_len[0] = 8;
    put_float_be(mock->payload[0], 1000.0f);
    put_float_be(mock->payload[0] + 4, 2500.5f);
    NCL_CHECK_EQ_INT(
        ncl_focas_axis_feedrate(focas, NCL_FOCAS_AXIS_Y, &real), NCL_OK);
    NCL_CHECK(real > 2500.4 && real < 2500.6);

    NCL_TEST_CASE("坐标：cnc_rdposition 的 POSELM（NCGuide 实测的形状）");
    mock->payload_count = 9; /* 这条请求带 9 个块（§2.5） */
    {
        int i;

        for (i = 0; i < 9; i++) {
            memset(mock->payload[i], 0, sizeof(mock->payload[i]));
            mock->payload_len[i] = 60; /* 5 根轴 × POSELM 12 字节 */
        }
    }
    /* 块 1 = 绝对位置：轴 X 的 POSELM（data=12345、dec=3 → 12.345） */
    put_u32be(mock->payload[1], 12345);
    put_u16be(mock->payload[1] + 4, 3);
    put_u16be(mock->payload[1] + 6, 0);  /* unit = mm  */
    put_u16be(mock->payload[1] + 8, 1);  /* disp = 显示 */
    mock->payload[1][10] = 'X';
    /* 第二根轴在同一个载荷的 12 字节处（data=6789、dec=2 → 67.89） */
    put_u32be(mock->payload[1] + 12, 6789);
    put_u16be(mock->payload[1] + 16, 2);
    /* 块 2 = 机械坐标（data=100000、dec=3 → 100.000） */
    put_u32be(mock->payload[2], 100000);
    put_u16be(mock->payload[2] + 4, 3);

    NCL_CHECK_EQ_INT(ncl_focas_axis_position(focas, NCL_FOCAS_AXIS_X, &real),
                     NCL_OK);
    NCL_CHECK(real > 12.34 && real < 12.35);
    NCL_CHECK_EQ_INT(ncl_focas_axis_position(focas, NCL_FOCAS_AXIS_Y, &real),
                     NCL_OK);
    NCL_CHECK(real > 67.88 && real < 67.90);
    NCL_CHECK_EQ_INT(
        ncl_focas_axis_position_machine(focas, NCL_FOCAS_AXIS_X, &real), NCL_OK);
    NCL_CHECK(real > 99.99 && real < 100.01);

    NCL_TEST_CASE("跟踪误差与指令位置：指令 = 实际（POSELM）− 延迟量（SV_DELAY）");
    /*
     * SV_DELAY 一条请求只带一个 Cb，所以应答就是块 1（假机床按"载荷序号 = 块号"
     * 铺，块 1 取载荷 0）。记录 8 字节：值在第 0 个 int32（大端），[4..6) 是小数位。
     * 这里故意留一根**负延迟**（Y = −2.500）：指令位置要往实际位置外面走。
     */
    mock->payload_count = 9; /* RDPOSITION 那条仍要 9 个块 */
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_len[0] = 40; /* 5 根轴 × 8 字节 */
    put_u32be(mock->payload[0], 1234);                  /* X：+1.234 */
    put_u16be(mock->payload[0] + 4, 3);
    put_u32be(mock->payload[0] + 8, 0xFFFFF63Cu);       /* Y：−2.500 */
    put_u16be(mock->payload[0] + 12, 3);
    put_u32be(mock->payload[0] + 16, 0);                /* Z：静止 */
    put_u16be(mock->payload[0] + 20, 3);

    NCL_CHECK_EQ_INT(ncl_focas_axis_srv_delay(focas, NCL_FOCAS_AXIS_X, &real),
                     NCL_OK);
    NCL_CHECK(real > 1.233 && real < 1.235);
    /* X：实际 12.345 − 1.234 = 11.111 */
    NCL_CHECK_EQ_INT(ncl_focas_axis_position_cmd(focas, NCL_FOCAS_AXIS_X, &real),
                     NCL_OK);
    NCL_CHECK(real > 11.110 && real < 11.112);
    /* Y：实际 67.89 − (−2.500) = 70.39 */
    NCL_CHECK_EQ_INT(ncl_focas_axis_position_cmd(focas, NCL_FOCAS_AXIS_Y, &real),
                     NCL_OK);
    NCL_CHECK(real > 70.38 && real < 70.40);
    /* Z：静止，延迟 0 → 指令 = 实际 */
    NCL_CHECK_EQ_INT(ncl_focas_axis_position_cmd(focas, NCL_FOCAS_AXIS_Z, &real),
                     NCL_OK);
    NCL_CHECK(real > -0.001 && real < 0.001);
    /* 轴号越界是参数错，不是读不到 */
    NCL_CHECK_EQ_INT(ncl_focas_axis_srv_delay(focas, (ncl_focas_axis)77, &real),
                     NCL_ERR_RANGE);

    NCL_TEST_CASE("模式与急停：aut 在块 2，manual/run/急停在块 0 的载荷里");
    /*
     * 布局是"斜坡载荷 + 官方 SDK 填它自己的 ODBST"钉出来的（01 册 §2.3）：
     * 块 0 载荷 = manual, run, edit, motion, mstb, emergency, …；块 1 = dummy；
     * 块 2 = aut。三态 = 急停优先 → running（run）→ free。
     */
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_len[0] = 18;
    put_u16be(mock->payload[0], 0);      /* manual    = 0 */
    put_u16be(mock->payload[0] + 2, 1);  /* run       = 1 */
    put_u16be(mock->payload[0] + 10, 1); /* emergency = 1 */
    memset(mock->payload[2], 0, sizeof(mock->payload[2]));
    mock->payload_len[2] = 2;
    put_u16be(mock->payload[2], 1);      /* aut = 1 */
    NCL_CHECK_EQ_INT(ncl_focas_mode(focas, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "auto");
    /* aut 撤掉、manual 拉起来 → manual（manual 在块 0 载荷的下标 0） */
    put_u16be(mock->payload[2], 0);
    put_u16be(mock->payload[0], 1);
    NCL_CHECK_EQ_INT(ncl_focas_mode(focas, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "manual");
    put_u16be(mock->payload[2], 1);
    put_u16be(mock->payload[0], 0);
    NCL_CHECK_EQ_INT(ncl_focas_mode(focas, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "auto");
    NCL_CHECK_EQ_INT(ncl_focas_status(focas, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "holding"); /* 急停优先于 run */
    {
        bool on = false;

        NCL_CHECK_EQ_INT(ncl_focas_emergency(focas, &on), NCL_OK);
        NCL_CHECK(on);
    }
    /* 急停撤掉 → running；run 也撤掉 → free */
    put_u16be(mock->payload[0] + 10, 0);
    NCL_CHECK_EQ_INT(ncl_focas_status(focas, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "running");
    put_u16be(mock->payload[0] + 2, 0);
    NCL_CHECK_EQ_INT(ncl_focas_status(focas, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "free");

    NCL_TEST_CASE("还没核准的那几条回 NCL_ERR_UNAVAILABLE，并说清要抓哪一帧");
    /* 坐标（POSELM）和指令位置（实际 − 跟踪误差）都已经能读了，这里留的还是
     * "还没核准"的几条。 */
    NCL_CHECK_EQ_INT(ncl_focas_axis_load(focas, NCL_FOCAS_AXIS_X, &real),
                     NCL_ERR_UNAVAILABLE);
    NCL_CHECK(strstr(ncl_focas_last_error(focas), "cnc_rdsvmeter") != NULL);
    NCL_CHECK_EQ_INT(ncl_focas_spindle_load(focas, 0, &real),
                     NCL_ERR_UNAVAILABLE);
    {
        ncl_json *json = NULL;

        NCL_CHECK_EQ_INT(ncl_focas_macro_variable(focas, 1, &json),
                         NCL_ERR_UNAVAILABLE);
        NCL_CHECK(strstr(ncl_focas_last_error(focas), "cnc_rdmacro") != NULL);
        NCL_CHECK_EQ_INT(ncl_focas_parameter(focas, 1, &json),
                         NCL_ERR_UNAVAILABLE);
        NCL_CHECK_EQ_INT(ncl_focas_modal(focas, &json), NCL_ERR_UNAVAILABLE);
        NCL_CHECK_EQ_INT(ncl_focas_system(focas, &json), NCL_ERR_UNAVAILABLE);
    }

    ncl_focas_close(focas);
    mock_stop(mock);
}

/*
 * 程序下行（PC → CNC）是三件套：func 0x11（start，516 字节体）→ 0x12（数据帧，
 * dir=4，体就是程序文本，机床不回）→ 0x13（end；下载的错都在这条回）。这一段拿
 * 假机床把帧序与文本内容验一遍（码与体长来自官方 SDK 实测，见 01 册 §2.4）。
 */
static void test_program_transfer(void)
{
    static const char kProgram[] = "N100 G0 X0 Y0\nN110 M3 S1200\n";
    focas_mock *mock = mock_start();
    ncl_focas_config config;
    ncl_focas *focas;
    char *err = NULL;
    char *program = NULL;
    size_t len = 0;

    NCL_CHECK(mock != NULL);
    if (mock == NULL) {
        return;
    }
    ncl_focas_config_default(&config);
    config.host = "127.0.0.1";
    config.port = mock->port;
    focas = ncl_focas_open(&config, &err);
    NCL_CHECK(focas != NULL);
    if (focas == NULL) {
        ncl_free_safe(err);
        mock_stop(mock);
        return;
    }

    NCL_TEST_CASE("程序下发：0x11 start → 0x12 数据帧（dir 4，无应答）→ 0x13 end");
    NCL_CHECK_EQ_INT(ncl_focas_program_download(focas, 0, NULL, kProgram),
                     NCL_OK);
    NCL_CHECK_EQ_INT((int)mock->transfer_bytes, (int)strlen(kProgram));
    NCL_CHECK(memcmp(mock->transfer, kProgram, strlen(kProgram)) == 0);
    NCL_CHECK_EQ_INT(mock->transfer_dir, NCL_FOCAS_DIR_DATA);
    NCL_CHECK_EQ_INT(mock->last_func, NCL_FOCAS_FUNC_DWN_END);
    /* 帧序：握手（hello + 两条探测）+ start / data / end 各一条 */
    NCL_CHECK_EQ_INT((int)mock->seen_count, 6);
    NCL_CHECK_EQ_INT(mock->seen[3], NCL_FOCAS_FUNC_DWN_START);
    NCL_CHECK_EQ_INT(mock->seen[4], NCL_FOCAS_FUNC_DWN_DATA);
    NCL_CHECK_EQ_INT(mock->seen[5], NCL_FOCAS_FUNC_DWN_END);

    NCL_TEST_CASE("程序上传：请求码已核、应答待核，先回 NCL_ERR_UNAVAILABLE");
    NCL_CHECK_EQ_INT(ncl_focas_program_upload(focas, 0, NULL, &program, &len),
                     NCL_ERR_UNAVAILABLE);
    NCL_CHECK(program == NULL);
    NCL_CHECK(strstr(ncl_focas_last_error(focas), "cnc_upload4") != NULL);

    ncl_focas_close(focas);
    mock_stop(mock);
}

static void test_not_yet(void)
{
    ncl_focas_config config;
    ncl_focas *focas;
    char *err = NULL;
    ncl_json *value = NULL;
    double position = 0.0;

    ncl_focas_config_default(&config);
    config.host = "127.0.0.1";
    focas = ncl_focas_open(&config, &err);
    NCL_CHECK(focas != NULL);
    if (focas == NULL) {
        printf("    %s\n", err != NULL ? err : "?");
        ncl_free_safe(err);
        return;
    }

    NCL_TEST_CASE("还没抓到帧的三条调用回 NCL_ERR_UNAVAILABLE，并说清要抓哪一帧");
    NCL_CHECK_EQ_INT(ncl_focas_alarm(focas, &value), NCL_ERR_UNAVAILABLE);
    NCL_CHECK(value == NULL); /* 宁可没有值，也不编一个 */
    NCL_CHECK(strstr(ncl_focas_last_error(focas), "cnc_rdalmmsg2") != NULL);

    /* 指令位置已经实现了（实际位置 − 跟踪误差，见 test_semantics），这里换一条
     * 还没核准的：合成进给速度要 `cnc_rddynamic2` 的 OBDDY2。 */
    NCL_CHECK_EQ_INT(ncl_focas_feed_speed(focas, &position),
                     NCL_ERR_UNAVAILABLE);
    NCL_CHECK(strstr(ncl_focas_last_error(focas), "cnc_rddynamic2") != NULL);
    /* 轴号越界仍旧是参数错，不是"还没有" */
    NCL_CHECK_EQ_INT(ncl_focas_axis_srv_delay(focas, (ncl_focas_axis)77,
                                              &position),
                     NCL_ERR_RANGE);

    NCL_CHECK_EQ_INT(ncl_focas_tool_list(focas, &value), NCL_ERR_UNAVAILABLE);
    NCL_CHECK(value == NULL);
    NCL_CHECK(strstr(ncl_focas_last_error(focas), "cnc_rdtooldata") != NULL);

    ncl_focas_close(focas);
}

NCL_TEST_MAIN_BEGIN()
    test_golden_frames();
    test_hello_reply();
    test_blocks();
    test_items();
    test_decode();
    test_driver();
    test_driver_short_reply();
    test_driver_payload_offset();
    test_driver_no_negotiate();
    test_not_yet();
    test_semantics();
    test_program_transfer();
NCL_TEST_MAIN_END()
