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

static uint32_t get_u32be(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | (uint32_t)in[3];
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

    NCL_TEST_CASE("the func 1 reply carries the count the machine declares");
    memset(body, 0, sizeof(body));
    put_u16be(body + 8, 2);
    err = ncl_focas_hello_reply(body, sizeof(body), &records);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(records, 2);

    /*
     * 体长不要求 16 + 8n：真机（0i-MD）回的是 **360 字节**、`[8..10)` 写 8，官方 SDK
     * 收下它并 rc=0（01 册 §2.3）。只有"短于 16 字节的块头"才不算握手应答。
     */
    NCL_TEST_CASE("a reply longer than 16 + 8n is still a hello reply (§2.3, 真机)");
    err = ncl_focas_hello_reply(body, sizeof(body) - 1u, &records);
    NCL_CHECK_EQ_INT(err, NCL_OK);
    NCL_CHECK_EQ_INT(records, 2);

    NCL_TEST_CASE("a body shorter than the 16 byte record header is refused");
    err = ncl_focas_hello_reply(body, 12, &records);
    NCL_CHECK_EQ_INT(err, NCL_FOCAS_ERR_LENGTH);
    err = ncl_focas_hello_reply(body, 16, &records);
    NCL_CHECK_EQ_INT(err, NCL_OK);

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
        /* 真机（官方 SDK 也是）发的是 d=0、e=8（一次要几条）、arg2=1 */
        NCL_CHECK_EQ_INT(item->arg0[0], 0);
        NCL_CHECK_EQ_INT(item->arg1[0], 8);
        NCL_CHECK_EQ_INT(item->arg2[0], 1);
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
    /* 操作面板信号（进给倍率就在里面）：Cb 0x5d，d 是"读哪几路"的位掩码 */
    item = ncl_focas_item_lookup("RDSGNL");
    NCL_CHECK(item != NULL && item->cbs[0] == 0x5d && item->arg0[0] == 0xffff);
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
    ncl_thread *conns[8]; /**< 一条连接一个线程（会话是两条 TCP，§2.1） */
    size_t      conn_count;
    bool        stop;
    int         requests;
    uint8_t     last_func;
    size_t      last_blocks;
    uint16_t    last_code; /**< 最后一次请求里第 1 个块的码 */
    uint8_t     hello[16u + 8u * 2u];
    size_t      hello_len;
    /* 一块最多铺多少字节：参数那条真机回 264、程序目录一页 8×72 = 576，
     * 所以留 1024。
     * （**别按"测试用不到"缩小**：读取端是按 payload_len 从这儿拷的，写超了就是
     * 越界读，症状是"第二条记录的内容不对"这种莫名其妙的失败。） */
    uint8_t     payload[NCL_FOCAS_ITEM_CBS][1024];
    size_t      payload_len[NCL_FOCAS_ITEM_CBS];
    size_t      payload_count;
    /* 程序目录（`0x06`）的号表：填了它就照真机的样子按 `d`/`e` 分页回
     * （一个块、载荷 = 72 字节一条）。§11.16.1 的翻页就拿它测。 */
    uint16_t    dir_numbers[16];
    size_t      dir_count;
    bool        dir_ignore_range; /**< 不认 `d` 的坏样子：每页都把头条重铺 */
    int         dir_requests;     /**< 目录请求发了几条 */
    /* 轴名表（`cnc_rdaxisname` = 0x89）：每轴 4 字节 = 名字 2 字节 + 2 字节代码 */
    char        axis_names[8][4];
    size_t      axis_name_count;
    /* 工件零点偏移（`cnc_rdzofs` = 0x0b / `cnc_wrzofs` = 0x0c）：32 条 8 字节记录 */
    uint8_t     zofs[256];
    int         zofs_requested;   /**< 最后一次 0x0b 的偏移号（d 那格） */
    int         zofs_writes;      /**< 收到几条 0x0c */
    int         zofs_write_axis;  /**< 最后一次写的轴号（arg2） */
    int32_t     zofs_write_raw;   /**< 最后一次写的原始值（载荷前 4 字节） */
    int         short_by; /**< reply with fewer blocks than asked */
    uint16_t    block_rc; /**< 应答块里的返回码（机床说 1/6 就是"这台没有"） */
    /* 传输三件套的状态回执：非 0 时，`0x13` end 的应答按真机的样子回
     * **方向 3 + 体前 4 字节的返回码 + 接着 2 字节细码**（01 册 §11.14）。 */
    int         transfer_status;
    int         transfer_detail; /**< 体 [4..6) 那个细码（ODBERR.err_no） */
    /* 真机上 `0x11` start 也会用同一个形状回状态（目录名不对就是这条），
     * 这里默认只挂在 end 上，要测 start 那条就把它打开。 */
    bool        transfer_status_on_start;
    /* 两条 TCP 的分工（§11.18）：真机上传输帧只认第一条、命令帧只认第二条，
     * 发错的那一族**一声不响就断**。这里把那两条连接各自**服务过哪一族**记下来，
     * 测试好断言"没发错道"。 */
    int         transfer_channel; /**< 服务过传输帧的那条连接（1 起，0 = 没有） */
    int         cmd_channel;      /**< 服务过命令帧的那条连接（1 起，0 = 没有） */
    /* 最后一次请求的**体**原样留一份：写那一侧的帧形状（块长、tag0/tag1、载荷）
     * 在 01 册 §11.13 里是逐字节核过的，这里就照那一份对。 */
    uint8_t     last_request[640]; /**< 程序下行的 start 体就有 516 字节 */
    size_t      last_request_len;
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
        put_u16be(out + used + 8, mock->block_rc); /* 块返回码：非 0 = 机床不认 */
        put_u16be(out + used + 14, (uint16_t)plen);
        if (plen > 0) {
            memcpy(out + used + 16, mock->payload[k], plen);
        }
        used += size;
    }
    return used;
}

/** 一条连接（会话是两条：传输通道 hello 1、命令通道 hello 2，§2.1 / §11.18）。 */
typedef struct {
    focas_mock *mock;
    ncl_socket *peer;
    int         index;     /**< 第几条 TCP（1 起），只用来断言"没发错道" */
    unsigned    hello;     /**< 这条连接 hello 里的计数器：**角色就是它定的** */
    uint8_t     buf[4096]; /**< 没读齐的字节留在这儿，等一下再来 */
    size_t      used;
} mock_conn;

/**
 * 机床对两条 TCP 的分工（§11.18，2026-09-23 在这台 0i-MF 上核出来）。
 *
 * 分的是 **hello 里那个计数器**（不是"先连上的那条"：把两边的计数器对调，角色跟着
 * 对调 —— 真机实测）：计数器 **1** 那条只收传输帧（`0x11/0x12/0x13`、`0x15/0x18/0x19`），
 * 计数器 **2** 那条只收命令帧（`0x21`）。发错那一条的：**连应答都不给，直接把连接
 * 断掉**。mock 照着做，这样"0x11 发到命令通道上"这种错就再也过不了测试。
 */
static bool mock_channel_accepts(unsigned hello, uint8_t func)
{
    bool transfer;

    if (func == NCL_FOCAS_FUNC_HELLO || func == NCL_FOCAS_FUNC_BYE) {
        return true; /* hello / bye 两条都收（§2.1） */
    }
    transfer = func == NCL_FOCAS_FUNC_DWN_START || func == NCL_FOCAS_FUNC_DWN_DATA ||
               func == NCL_FOCAS_FUNC_DWN_END || func == NCL_FOCAS_FUNC_UP_START ||
               func == 0x18u || func == 0x19u; /* 上行三件套（本实现还没发过） */
    return (hello & 1u) != 0u ? transfer : !transfer;
}

/** 一帧的体处理：回 true 表示"回了，连接继续"。 */
static bool mock_serve(mock_conn *conn, const uint8_t *frame, const ncl_focas_pdu *pdu)
{
    focas_mock *mock = conn->mock;
    uint8_t body[1024];
    uint8_t reply[2048];
    size_t body_len;
    size_t frame_len;
    size_t blocks;

    mock->requests++;
    mock->last_func = pdu->func;
    if (mock->seen_count < sizeof(mock->seen)) {
        mock->seen[mock->seen_count++] = pdu->func;
    }
    if (pdu->func == NCL_FOCAS_FUNC_HELLO) {
        /* 这一条连接的角色就写在这个计数器里（§11.18），先把它记下来 */
        conn->hello = pdu->length >= 2u ? frame[NCL_FOCAS_HEADER + 1u] : 0u;
    } else if (pdu->func != NCL_FOCAS_FUNC_BYE) {
        if (mock_channel_accepts(conn->hello, pdu->func)) {
            bool transfer = (conn->hello & 1u) != 0u;

            if (transfer) {
                mock->transfer_channel = conn->index;
            } else {
                mock->cmd_channel = conn->index;
            }
        } else {
            return false; /* 发错道了：机床就是这么断的 */
        }
    }
    if (pdu->func == NCL_FOCAS_FUNC_DWN_DATA) {
        /* 数据帧：收下程序文本，**不回**（官方 SDK 就是这么发的）。 */
        size_t keep = pdu->length;

        if (keep > sizeof(mock->transfer) - mock->transfer_bytes) {
            keep = sizeof(mock->transfer) - mock->transfer_bytes;
        }
        memcpy(mock->transfer + mock->transfer_bytes, frame + NCL_FOCAS_HEADER,
               keep);
        mock->transfer_bytes += keep;
        mock->transfer_dir = pdu->dir;
        return true;
    }
    /*
     * 程序目录那一族（`Cb 0x06`）且号表填了：按请求里的 `d`（起始号）/`e`（要几条）
     * 铺一页 72 字节的记录 —— 真机就是这么回的（一页最多 8 条，翻页靠 `d`）。
     */
    if (pdu->func == NCL_FOCAS_FUNC_CMD && pdu->length >= 18u &&
        get_u16be(frame + NCL_FOCAS_HEADER + 8u) == 0x06u && mock->dir_count > 0) {
        long long d = (int32_t)get_u32be(frame + NCL_FOCAS_HEADER + 10u);
        long long e = (int32_t)get_u32be(frame + NCL_FOCAS_HEADER + 14u);
        size_t used = 0;
        size_t k;

        mock->dir_requests++;
        if (e <= 0 || e > (long long)(sizeof(mock->dir_numbers) /
                                      sizeof(mock->dir_numbers[0]))) {
            e = 8;
        }
        memset(mock->payload[0], 0, sizeof(mock->payload[0]));
        for (k = 0; k < mock->dir_count && (long long)(used / 72u) < e; k++) {
            if (!mock->dir_ignore_range &&
                (long long)mock->dir_numbers[k] < d) {
                continue;
            }
            put_u16be(mock->payload[0] + used + 2, mock->dir_numbers[k]);
            used += 72u;
        }
        mock->payload_len[0] = used;
        mock->payload_count = 1;
    }
    /*
     * 轴名表（`Cb 0x89`）：每轴 4 字节，名字 2 字节 + 2 字节代码。工件坐标那族读记录时
     * 要按**机床自己报的轴名**落键（车床没有 Y，不能按顺序硬排），所以 mock 也得答。
     */
    if (pdu->func == NCL_FOCAS_FUNC_CMD && pdu->length >= 18u &&
        get_u16be(frame + NCL_FOCAS_HEADER + 8u) == 0x89u &&
        mock->axis_name_count > 0) {
        size_t k;

        memset(mock->payload[0], 0, sizeof(mock->payload[0]));
        for (k = 0; k < mock->axis_name_count; k++) {
            put_u16be(mock->payload[0] + k * 4u,
                      (uint16_t)mock->axis_names[k][0]);
        }
        mock->payload_len[0] = mock->axis_name_count * 4u;
        mock->payload_count = 1;
    }
    /*
     * 工件零点偏移：`0x0b` 读（回 32 条 8 字节记录）、`0x0c` 写（载荷 = `[值 4 字节]`
     * + `00 00` + `ff ff`，`arg2` = 轴号 1 起）—— 写了就更新那张表，好让"写后复核"
     * 这条测试真的走一遍。
     */
    if (pdu->func == NCL_FOCAS_FUNC_CMD && pdu->length >= 18u &&
        (get_u16be(frame + NCL_FOCAS_HEADER + 8u) == 0x0bu ||
         get_u16be(frame + NCL_FOCAS_HEADER + 8u) == 0x0cu)) {
        int code = get_u16be(frame + NCL_FOCAS_HEADER + 8u);
        int d = (int)(int32_t)get_u32be(frame + NCL_FOCAS_HEADER + 10u);

        mock->zofs_requested = d;
        if (code == 0x0cu) {
            int axis = (int)(int32_t)get_u32be(frame + NCL_FOCAS_HEADER + 18u);
            size_t at = (size_t)(axis - 1) * 8u;

            mock->zofs_writes++;
            mock->zofs_write_axis = axis;
            mock->zofs_write_raw =
                (int32_t)get_u32be(frame + NCL_FOCAS_HEADER + 30u);
            if (axis >= 1 && at + 8u <= sizeof(mock->zofs)) {
                uint32_t raw = (uint32_t)mock->zofs_write_raw;

                mock->zofs[at] = (uint8_t)(raw >> 24);
                mock->zofs[at + 1u] = (uint8_t)(raw >> 16);
                mock->zofs[at + 2u] = (uint8_t)(raw >> 8);
                mock->zofs[at + 3u] = (uint8_t)raw;
            }
        }
        memset(mock->payload[0], 0, sizeof(mock->payload[0]));
        memcpy(mock->payload[0], mock->zofs, sizeof(mock->zofs));
        mock->payload_len[0] = sizeof(mock->zofs);
        mock->payload_count = 1;
    }
    if (pdu->func == NCL_FOCAS_FUNC_HELLO) {
        body_len = mock->hello_len;
        memcpy(body, mock->hello, body_len);
    } else if (pdu->func == NCL_FOCAS_FUNC_CMD ||
               pdu->func == NCL_FOCAS_FUNC_DWN_START ||
               pdu->func == NCL_FOCAS_FUNC_DWN_END) {
        if (pdu->length <= sizeof(mock->last_request)) {
            memcpy(mock->last_request, frame + NCL_FOCAS_HEADER, pdu->length);
            mock->last_request_len = pdu->length;
        }
        blocks = 0;
        if (pdu->length >= 2u) {
            blocks = get_u16be(frame + NCL_FOCAS_HEADER);
        }
        mock->last_blocks = blocks;
        /* 第 1 个块的码（块内 [6..8)）—— 会话探针必须是 code 24 那一条（§2.8） */
        mock->last_code = pdu->length >= 2u + 8u
                              ? get_u16be(frame + NCL_FOCAS_HEADER + 8u)
                              : 0;
        if (blocks == 0) {
            blocks = 1; /* §2.2 rule 6: a 0x21 reply needs a block */
        }
        if (mock->short_by > 0 && blocks > (size_t)mock->short_by) {
            blocks -= (size_t)mock->short_by;
        }
        body_len = mock_block_body(body, sizeof(body), blocks, mock);
    } else if (pdu->func == NCL_FOCAS_FUNC_BYE) {
        body_len = 0; /* 机床对 bye 回一条空的（§2.1 实测） */
    } else {
        return false;
    }
    /* 传输三件套的状态回执：真机上 `0x13` end 的应答是**方向 3**、体前 4 字节是
     * 机床返回码（`00000005` = EW_ATTRIB）。这里按那个形状回。 */
    if ((pdu->func == NCL_FOCAS_FUNC_DWN_END ||
         (mock->transfer_status_on_start &&
          pdu->func == NCL_FOCAS_FUNC_DWN_START)) &&
        mock->transfer_status != 0) {
        memset(body, 0, 8);
        body[3] = (uint8_t)mock->transfer_status;    /* 前 4 字节 = 返回码（大端） */
        body[5] = (uint8_t)mock->transfer_detail;    /* [4..6) = 细码（大端） */
        frame_len = ncl_focas_build(reply, sizeof(reply), pdu->func, 3u, body, 8);
        if (frame_len == 0 ||
            ncl_socket_send(conn->peer, reply, frame_len) != NCL_OK) {
            return false;
        }
        return true;
    }
    frame_len = ncl_focas_build(reply, sizeof(reply), pdu->func,
                                NCL_FOCAS_DIR_RESP, body, body_len);
    if (frame_len == 0 || ncl_socket_send(conn->peer, reply, frame_len) != NCL_OK) {
        return false;
    }
    return true;
}

/**
 * 一条连接一个线程：客户端先开控制通道（hello 计数器 1）、再开数据通道
 * （计数器 2），两条都要在开着的时候被回应 —— 单线程的 accept 循环会把第二条
 * 挡在门外（真机也确实收两条，§2.1）。
 *
 * 读用 100 ms 的短超时循环，攒够一帧再处理：这样 mock_stop() 能很快 join 上，
 * 不用等一条空闲连接的自然超时。
 */
static void mock_conn_main(void *arg)
{
    mock_conn *conn = (mock_conn *)arg;
    focas_mock *mock = conn->mock;

    while (!mock->stop) {
        ncl_focas_pdu pdu;
        size_t total = 0;
        ncl_err split;
        int got;

        if (conn->used >= NCL_FOCAS_HEADER) {
            memset(&pdu, 0, sizeof(pdu));
            split = ncl_focas_split(conn->buf, conn->used, &pdu, &total);
            /* 体长为 0 的帧（bye、传输的 end）split 直接回 OK——原来这里只认
             * NCL_ERR_RANGE，把 end 帧当成 bye 断掉了。 */
            if (split == NCL_OK) {
                if (!mock_serve(conn, conn->buf, &pdu)) {
                    break;
                }
                memmove(conn->buf, conn->buf + total, conn->used - total);
                conn->used -= total;
                continue;
            }
            if (split != NCL_ERR_RANGE || total > sizeof(conn->buf)) {
                break;
            }
            if (conn->used >= total) {
                continue; /* 走不到：split 回 OK 时就处理过了 */
            }
        }
        got = ncl_socket_recv(conn->peer, conn->buf + conn->used,
                              sizeof(conn->buf) - conn->used, 100);
        if (got == 0 || got == -1) {
            break; /* 对端关了，或者出错了 */
        }
        if (got != NCL_SOCKET_TIMEOUT) {
            conn->used += (size_t)got;
        }
    }
    ncl_socket_close(conn->peer);
    ncl_free_safe(conn);
}

static void mock_main(void *arg)
{
    focas_mock *mock = (focas_mock *)arg;

    while (!mock->stop) {
        ncl_socket *peer = ncl_socket_accept(mock->listener, 200);
        mock_conn *conn;
        ncl_thread *thread;

        if (peer == NULL) {
            continue;
        }
        conn = (mock_conn *)ncl_mem_calloc(1, sizeof(*conn));
        if (conn == NULL || mock->conn_count >= 8) {
            ncl_mem_free(conn);
            ncl_socket_close(peer);
            continue;
        }
        conn->mock = mock;
        conn->peer = peer;
        conn->index = (int)mock->conn_count + 1; /* 第一条 = 传输通道（§11.18） */
        thread = ncl_thread_start(mock_conn_main, conn);
        if (thread == NULL) {
            ncl_socket_close(peer);
            ncl_mem_free(conn);
            continue;
        }
        mock->conns[mock->conn_count++] = thread;
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
    size_t i;

    if (mock == NULL) {
        return;
    }
    mock->stop = true;
    ncl_thread_join(mock->thread);
    ncl_socket_close(mock->listener);
    for (i = 0; i < mock->conn_count; i++) {
        ncl_thread_join(mock->conns[i]);
    }
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

    NCL_TEST_CASE("open builds the session: two connections, hello each, then the probe");
    NCL_CHECK_EQ_INT(driver->ops->open(driver), NCL_OK);
    NCL_CHECK(driver->ops->is_connected(driver));
    /* 控制通道 hello(1) + 数据通道 hello(2) + 那个 code 24 的会话探针（§2.1） */
    NCL_CHECK_EQ_INT(mock->requests, 3);
    NCL_CHECK_EQ_INT(mock->last_func, NCL_FOCAS_FUNC_CMD);
    NCL_CHECK_EQ_INT(mock->last_blocks, 1);
    /* 探针是**一个** code 24 的块（真机上 SDK 就是这么发的，§2.8）——
     * 不是"按握手应答的记录数发一串 code 24"。 */
    NCL_CHECK_EQ_INT(mock->last_code, NCL_FOCAS_CODE_SYSINFO);

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
    NCL_TEST_CASE("\"negotiate\":false stops after the two hellos");
    NCL_CHECK_EQ_INT(driver->ops->open(driver), NCL_OK);
    /* 两条连接各一条 hello；不发会话探针 */
    NCL_CHECK_EQ_INT(mock->requests, 2);
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
 * 进给速度/模式/系统信息）各自把值放对地方，就应当读得回来；还没核准的那几条
 * （负载/刀补/参数/宏变量/工件坐标/模态）回 NCL_ERR_UNAVAILABLE，理由里写清要抓
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

    /*
     * 件数（PART_COUNT）：2026-09-23 改成读 **6711 号参数**（0i 上"加工件数"就在这一号，
     * 现场那份服务的 `getPartCount` 也是这么读的）—— 原来的 `cnc_rdcount`（0x8b）是
     * "刀具寿命计数器"，机床要开寿命管理选件才答，这台机器回 EW_NOOPT（01 册 §11.13）。
     * 参数这一路的值 = 载荷 @0 的 BE32（dec 在 @6）。
     */
    NCL_TEST_CASE("件数：读 6711 号参数（@8 是值；d = e = 6711）");
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_count = 1;
    mock->payload_len[0] = 16;
    put_u32be(mock->payload[0], 6711); /* 号 */
    put_u32be(mock->payload[0] + 4, 1);/* 条数 */
    put_u32be(mock->payload[0] + 8, 952); /* 值 */
    NCL_CHECK_EQ_INT(ncl_focas_part_count(focas, &number), NCL_OK);
    NCL_CHECK_EQ_INT(number, 952);
    /* 请求里 d 与 e 都得是 6711（写死 1 的话参数 6711 读不出来） */
    NCL_CHECK_EQ_INT((int)(int32_t)get_u32be(mock->last_request + 10), 6711);
    NCL_CHECK_EQ_INT((int)(int32_t)get_u32be(mock->last_request + 14), 6711);

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

    /*
     * 进给倍率：走 `cnc_rdopnlsgnl`（Cb 0x5d）的 `IODBSGNL.feed_ovrd`，载荷 @0xa 的
     * BE16 是**信号码**；官方文档把它换算成百分比写死了（码 × 10 = %，0..20）。
     * 这里把 @0xa 填成 13 = 130%，并给 @0xe（blck_del 那一格）一个干扰值。
     */
    NCL_TEST_CASE("进给倍率：操作面板信号 feed_ovrd@0xa，码 × 10 = %");
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_count = 1;
    mock->payload_len[0] = 26;
    put_u16be(mock->payload[0] + 0x0a, 13); /* 13 → 130% */
    put_u16be(mock->payload[0] + 0x0e, 1);  /* blck_del，不该被当倍率读走 */
    NCL_CHECK_EQ_INT(ncl_focas_feed_override(focas, &real), NCL_OK);
    NCL_CHECK(real > 129.9 && real < 130.1);
    put_u16be(mock->payload[0] + 0x0a, 25); /* 文档只定义 0..20 */
    NCL_CHECK_EQ_INT(ncl_focas_feed_override(focas, &real), NCL_ERR_RANGE);

    /*
     * 型号 / 版本：来源是连接期那条能力块（Cb 0x0e d=e=0x26f0）里的 ODBSYS。
     * 这里铺的是 NCGuide 上实测那一串（01 册 §2.5）：
     *   addinfo=0x4206、max_axis=32、cnc_type=" 0"、mt_type=" M"、
     *   series="D4G2"、version="49.0"、axes="03"
     * 所以 MODEL = "0M D4G2"（cnc_type + mt_type 去空格 + " " + series）、
     * VERSION = "49.0"。两格都是**空格补齐**的 ASCII，读的时候要两头去空格。
     */
    NCL_TEST_CASE("型号与版本：ODBSYS（能力块）里的 ASCII 格");
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_count = 1;
    mock->payload_len[0] = 18;
    {
        static const char kOdbsys[18] = {'\x42', '\x06', '\x00', '\x20',
                                         ' ',    '0',    ' ',    'M',
                                         'D',    '4',    'G',    '2',
                                         '4',    '9',    '.',    '0',
                                         '0',    '3'};
        memcpy(mock->payload[0], kOdbsys, sizeof(kOdbsys));
    }
    NCL_CHECK_EQ_INT(ncl_focas_model(focas, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "0M D4G2");
    NCL_CHECK_EQ_INT(ncl_focas_version(focas, text, sizeof(text)), NCL_OK);
    NCL_CHECK_EQ_STR(text, "49.0");
    /* 系统信息（cnc_sysinfo）就是这一条载荷拆开的（真机也走这一条 code 24） */
    {
        ncl_json *info = NULL;

        NCL_CHECK_EQ_INT(ncl_focas_system(focas, &info), NCL_OK);
        NCL_CHECK(info != NULL);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(info, "addinfo", -1), 0x4206);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(info, "maxAxis", -1), 0x20);
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(info, "cncType"), "0");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(info, "machineType"), "M");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(info, "series"), "D4G2");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(info, "version"), "49.0");
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(info, "axes"), "03");
        ncl_json_free(info);
    }

    /*
     * 进给速度（ACTF）/主轴转速（ACTS）也是"每轴一条 8 字节记录"（§2.8 真机实测）：
     * data@0、dec@6。原来假机床铺的是 float32 @index*4 —— 真机上第 2 根轴起全错。
     */
    NCL_TEST_CASE("进给速度：ACTF 每轴一条 8 字节记录（第 2 根轴在载荷 @8）");
    mock->payload_count = 9;
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_len[0] = 16;
    put_u32be(mock->payload[0], 1000000);      /* X：1000.000 */
    put_u16be(mock->payload[0] + 6, 3);
    put_u32be(mock->payload[0] + 8, 2500500);  /* Y：2500.500 */
    put_u16be(mock->payload[0] + 14, 3);
    NCL_CHECK_EQ_INT(
        ncl_focas_axis_feedrate(focas, NCL_FOCAS_AXIS_Y, &real), NCL_OK);
    NCL_CHECK(real > 2500.4 && real < 2500.6);

    NCL_TEST_CASE("坐标：cnc_rdposition 的 8 字节记录（8 块帧，§2.8 真机实测）");
    mock->payload_count = 9; /* 请求带 8 个块，块 i 取载荷 i */
    {
        int i;

        for (i = 0; i < 9; i++) {
            memset(mock->payload[i], 0, sizeof(mock->payload[i]));
            mock->payload_len[i] = 40; /* 5 根轴 × 8 字节 */
        }
    }
    /* 块 1 = 绝对位置：轴 X（data=12345、dec=3 → 12.345） */
    put_u32be(mock->payload[1], 12345);
    put_u16be(mock->payload[1] + 4, 0x000a); /* 真机上这 2 字节恒为 0x000a */
    put_u16be(mock->payload[1] + 6, 3);
    /* 第二根轴在同一个载荷的 8 字节处（data=67890、dec=3 → 67.89） */
    put_u32be(mock->payload[1] + 8, 67890);
    put_u16be(mock->payload[1] + 12, 0x000a);
    put_u16be(mock->payload[1] + 14, 3);
    /* 块 2 = 机械坐标（data=100000、dec=3 → 100.000） */
    put_u32be(mock->payload[2], 100000);
    put_u16be(mock->payload[2] + 6, 3);

    NCL_CHECK_EQ_INT(ncl_focas_axis_position(focas, NCL_FOCAS_AXIS_X, &real),
                     NCL_OK);
    NCL_CHECK(real > 12.34 && real < 12.35);
    NCL_CHECK_EQ_INT(ncl_focas_axis_position(focas, NCL_FOCAS_AXIS_Y, &real),
                     NCL_OK);
    NCL_CHECK(real > 67.88 && real < 67.90);
    NCL_CHECK_EQ_INT(
        ncl_focas_axis_position_machine(focas, NCL_FOCAS_AXIS_X, &real), NCL_OK);
    NCL_CHECK(real > 99.99 && real < 100.01);

    NCL_TEST_CASE("跟踪误差与指令位置：指令 = 实际（位置那一族）− 延迟量（SV_DELAY）");
    /*
     * SV_DELAY 一条请求只带一个 Cb，所以应答就是块 1（假机床按"载荷序号 = 块号"
     * 铺，块 1 取载荷 0）。记录 8 字节、值在第 0 个 int32（大端）—— 依据是 x64 以太网库
     * fwlibe64.dll 里 `shr ax,3`（长度/8 = 轴数）与 `[载荷 + i*8 + 0x10]`（每轴 8 字节）
     * 那几行，见 01 册 §2.5.2 —— 与位置那一族是**同一个 8 字节记录形状**（§2.8 真机
     * 实测），所以这里后 4 字节也照真机铺 `00 0a 00 00`（延迟量的小数位跟位置走，
     * 不在这一条里，见 cnc_getfigure 口径）。
     * 这里再留一根**负延迟**（Y = −2.500）：指令位置要往实际位置外面走。
     */
    mock->payload_count = 9;
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_len[0] = 40; /* 5 根轴 × 8 字节 */
    put_u32be(mock->payload[0], 1234);                  /* X：+1.234 */
    put_u16be(mock->payload[0] + 4, 0x000a);
    put_u32be(mock->payload[0] + 8, 0xFFFFF63Cu);       /* Y：−2.500 */
    put_u16be(mock->payload[0] + 12, 0x000a);
    put_u32be(mock->payload[0] + 16, 0);                /* Z：静止 */
    put_u16be(mock->payload[0] + 20, 0x000a);

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

    /*
     * 轴类型（`cnc_rdaxisname`，0x89）：每轴 4 字节 = 名字 2 字节 + 2 字节代码。
     * 这台机器只给名字，类型按 FANUC 命名约定推（X/Y/Z/U/V/W 直线、A/B/C 回转）。
     */
    NCL_TEST_CASE("轴类型：名字从 0x89 来，linear/rotary 按命名约定");
    mock->payload_count = 1;
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_len[0] = 20; /* 5 轴 × 4 字节：X Y Z A C */
    mock->payload[0][0] = 'X';
    put_u16be(mock->payload[0] + 2, 0x0694);
    mock->payload[0][4] = 'Y';
    put_u16be(mock->payload[0] + 6, 0x0694);
    mock->payload[0][8] = 'Z';
    put_u16be(mock->payload[0] + 10, 0x0694);
    mock->payload[0][12] = 'A';
    put_u16be(mock->payload[0] + 14, 0x0694);
    mock->payload[0][16] = 'C';
    put_u16be(mock->payload[0] + 18, 0x0694);
    NCL_CHECK_EQ_INT(ncl_focas_axis_type(focas, NCL_FOCAS_AXIS_X, text,
                                         sizeof(text)),
                     NCL_OK);
    NCL_CHECK_EQ_STR(text, "linear");
    NCL_CHECK_EQ_INT(ncl_focas_axis_type(focas, NCL_FOCAS_AXIS_A, text,
                                         sizeof(text)),
                     NCL_OK);
    NCL_CHECK_EQ_STR(text, "rotary");

    NCL_TEST_CASE("模式与急停：aut 在块 2，manual/run/急停在块 0 的载荷里");
    /*
     * 布局是"斜坡载荷 + 官方 SDK 填它自己的 ODBST"钉出来的（01 册 §2.3）：
     * 块 0 载荷 = manual, run, edit, motion, mstb, emergency, …；块 1 = dummy；
     * 块 2 = aut。三态 = 急停优先 → running（run）→ free。
     */
    mock->payload_count = 9; /* 假机床按"块 i 取载荷 i"铺，别让前面的用例改小它 */
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

    NCL_TEST_CASE("伺服/主轴负载：也是每轴一条 8 字节记录（§2.8 真机实测）");
    mock->payload_count = 9;
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_len[0] = 40;
    put_u32be(mock->payload[0], 0);
    put_u32be(mock->payload[0] + 8, 45500); /* Y = 45.5 % */
    put_u16be(mock->payload[0] + 14, 3);
    NCL_CHECK_EQ_INT(ncl_focas_axis_load(focas, NCL_FOCAS_AXIS_Y, &real),
                     NCL_OK);
    NCL_CHECK(real > 45.4 && real < 45.6);
    /* 主轴负载：`cnc_rdspmeter` 一条 0x40，8 根主轴 × 8 字节 */
    put_u32be(mock->payload[0], 12300); /* S1 = 12.3 % */
    put_u16be(mock->payload[0] + 6, 3);
    NCL_CHECK_EQ_INT(ncl_focas_spindle_load(focas, 0, &real), NCL_OK);
    NCL_CHECK(real > 12.29 && real < 12.31);

    NCL_TEST_CASE("报警：没有报警时载荷 0 字节 → 空数组（真机实测）");
    mock->payload_count = 1;
    mock->payload_len[0] = 0; /* 机床没报警就是 0 字节 */
    {
        ncl_json *json = NULL;

        NCL_CHECK_EQ_INT(ncl_focas_alarm(focas, &json), NCL_OK);
        NCL_CHECK(json != NULL);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(json), 0);
        ncl_json_free(json);
    }

    /*
     * 一条报警：真机上就是 80 字节（16 字节抬头 + arg3=64 字节文本区），
     * 抬头 = 报警号(BE32)@0、类型(BE32)@4、轴号@8、文本长度@12，文本在 @16。
     * 这里照真机那条 SV 报警铺：号 75、类型 3、文本 4 字节（GB2312 的"保护"）。
     */
    NCL_TEST_CASE("报警：一条记录（号 75 / 类型 3 / 文本 4 字节）");
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_len[0] = 80;
    put_u32be(mock->payload[0], 75);
    put_u32be(mock->payload[0] + 4, 3);
    put_u32be(mock->payload[0] + 12, 4);
    mock->payload[0][16] = 0xB1;
    mock->payload[0][17] = 0xA3;
    mock->payload[0][18] = 0xBB;
    mock->payload[0][19] = 0xA4;
    {
        ncl_json *json = NULL;
        const ncl_json *first;

        NCL_CHECK_EQ_INT(ncl_focas_alarm(focas, &json), NCL_OK);
        NCL_CHECK(json != NULL);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(json), 1);
        first = ncl_json_arr_get(json, 0);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(first, "number", -1), 75);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(first, "type", -1), 3);
        /* 机床给的是 GB2312（4 字节），出门是 UTF-8 的"保护"（6 字节）。 */
        NCL_CHECK_EQ_INT(
            (int)strlen(ncl_json_obj_get_string(first, "text")), 6);
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(first, "text"),
                         "\xE4\xBF\x9D\xE6\x8A\xA4");
        ncl_json_free(json);
    }

    /*
     * 刀补 / 宏变量这两条：一个块、`d = e = 号`，应答就是那条 8 字节记录
     * （值@0 + 小数位@6）。假机床这里按真机的形状铺：刀补 1 = 12.345、宏变量 100。
     */
    NCL_TEST_CASE("刀补/宏变量：一个块 + d = e = 号，值是那条 8 字节记录");
    mock->payload_count = 1;
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_len[0] = 8;
    put_u32be(mock->payload[0], 12345);
    put_u16be(mock->payload[0] + 4, 0x000a);
    put_u16be(mock->payload[0] + 6, 3);
    {
        ncl_json *json = NULL;

        NCL_CHECK_EQ_INT(ncl_focas_tool_offset(focas, 1, &json), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(json, "number", -1), 1);
        NCL_CHECK(ncl_json_obj_get_double(json, "value", -1.0) > 12.34 &&
                  ncl_json_obj_get_double(json, "value", -1.0) < 12.35);
        ncl_json_free(json);
        json = NULL;

        NCL_CHECK_EQ_INT(ncl_focas_macro_variable(focas, 100, &json), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(json, "number", -1), 100);
        ncl_json_free(json);
        json = NULL;

    }

    /*
     * 参数那一条**不是**这个形状：载荷前三格都是 BE32（@0 号、@4 条数、**@8 值**）。
     * 以前按 @0 读，读到的是"参数号自己" —— 只有 1 号参数看着对（01 册 §11.13）。
     */
    NCL_TEST_CASE("参数：载荷 @8 才是值（@0 是参数号，@4 是条数）");
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_len[0] = 16;
    put_u32be(mock->payload[0], 1);    /* 号 */
    put_u32be(mock->payload[0] + 4, 0);/* 条数 */
    put_u32be(mock->payload[0] + 8, 1);/* 值 */
    {
        ncl_json *json = NULL;

        NCL_CHECK_EQ_INT(ncl_focas_parameter(focas, 1, &json), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(json, "number", -1), 1);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(json, "raw", -1), 1);
        ncl_json_free(json);
    }

    NCL_TEST_CASE("机床回空载荷 = 没有这个号（NCL_ERR_NOT_FOUND，不是解析错）");
    mock->payload_len[0] = 0; /* 真机对"不存在的号"就是 0 字节 */
    {
        ncl_json *json = NULL;

        NCL_CHECK_EQ_INT(ncl_focas_macro_variable(focas, 100, &json),
                         NCL_ERR_NOT_FOUND);
        NCL_CHECK(json == NULL);
        NCL_CHECK_EQ_INT(ncl_focas_tool_offset(focas, 9, &json),
                         NCL_ERR_NOT_FOUND);
    }

    /*
     * 程序目录：72 字节一条（号在 @2 的 BE16、注释在 @8），真机两条的样子 ==
     * O2001 "(DEMOMAINGEAR)" / O3000 "(SUBGEAR)"。
     */
    NCL_TEST_CASE("程序目录：72 字节一条，号 @2、注释 @8");
    mock->payload_count = 1;
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_len[0] = 144; /* 2 条 × 72 */
    put_u16be(mock->payload[0] + 2, 2001);
    memcpy(mock->payload[0] + 4, "M 0 ", 4);
    memcpy(mock->payload[0] + 8, "(DEMOMAINGEAR)", 14);
    put_u16be(mock->payload[0] + 72 + 2, 3000);
    memcpy(mock->payload[0] + 72 + 8, "(SUBGEAR)", 9);
    {
        ncl_json *json = NULL;
        const ncl_json *first;

        NCL_CHECK_EQ_INT(ncl_focas_program_directory(focas, &json), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_json_arr_len(json), 2);
        first = ncl_json_arr_get(json, 0);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(first, "number", -1), 2001);
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(first, "comment"),
                         "(DEMOMAINGEAR)");
        first = ncl_json_arr_get(json, 1);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(first, "number", -1), 3000);
        NCL_CHECK_EQ_STR(ncl_json_obj_get_string(first, "comment"),
                         "(SUBGEAR)");
        ncl_json_free(json);
    }

    /*
     * 翻页（§11.16.1）：真机一次只回 8 条，得顺着 `d`（起始程序号）问下去。
     * 这一台库里 12 个程序，早先只发一条请求 → 列表少一半；现在要把整张表拿全，
     * 而且**不认 `d` 的机床**（每页都把头条重铺）也不能列出重复的号。
     */
    NCL_TEST_CASE("程序目录翻页：一次 8 条，顺着 `d` 把整张表问全、不重复");
    {
        ncl_json *json = NULL;
        size_t k;

        mock->dir_count = 12;
        for (k = 0; k < mock->dir_count; k++) {
            mock->dir_numbers[k] = (uint16_t)(100u + (unsigned)k * 10u);
        }
        mock->dir_ignore_range = false;
        mock->dir_requests = 0;
        NCL_CHECK_EQ_INT(ncl_focas_program_directory(focas, &json), NCL_OK);
        NCL_CHECK(json != NULL);
        if (json != NULL) {
            NCL_CHECK_EQ_INT((int)ncl_json_arr_len(json), 12);
            NCL_CHECK_EQ_INT(ncl_json_obj_get_int(ncl_json_arr_get(json, 0),
                                                  "number", -1), 100);
            NCL_CHECK_EQ_INT(ncl_json_obj_get_int(ncl_json_arr_get(json, 11),
                                                  "number", -1), 210);
            ncl_json_free(json);
        }
        /* 8 + 4 + 0：机床不告诉你一共几条，最后那次"空页"是必须的 */
        NCL_CHECK_EQ_INT(mock->dir_requests, 3);

        /* 机床不认 `d`：第二页全是已经收过的号 → 收下 0 条就停，列表里没有重复 */
        mock->dir_ignore_range = true;
        mock->dir_requests = 0;
        json = NULL;
        NCL_CHECK_EQ_INT(ncl_focas_program_directory(focas, &json), NCL_OK);
        if (json != NULL) {
            NCL_CHECK_EQ_INT((int)ncl_json_arr_len(json), 8);
            ncl_json_free(json);
        }
        NCL_CHECK_EQ_INT(mock->dir_requests, 2);
        mock->dir_count = 0;
        mock->dir_ignore_range = false;
    }

    /*
     * 工件零点偏移（`cnc_rdzofs` = 0x0b，见 01 册 §11.19）：载荷 = 32 条 8 字节记录，
     * 第 i 根轴的值在 `@8×(i-1)`；键用**机床自己报的轴名**。写是 0x0c，载荷与写刀补
     * 同形（`[值][00 00][ff ff]`）、`arg2` = 轴号（1 起），写完读回来复核。
     */
    NCL_TEST_CASE("工件坐标系：读 G54（每轴一条记录）、写 G54 X 后读回来复核");
    {
        ncl_json *json = NULL;
        const ncl_json *g54;
        size_t k;

        /* 假机床：X/Y/Z 三轴（0x89 的轴名表），坐标系里 dec = 3 */
        mock->axis_name_count = 3;
        memcpy(mock->axis_names[0], "X", 2);
        memcpy(mock->axis_names[1], "Y", 2);
        memcpy(mock->axis_names[2], "Z", 2);
        for (k = 0; k < 3; k++) {
            mock->zofs[k * 8u + 4u] = 0x00;
            mock->zofs[k * 8u + 5u] = 0x0a;
            mock->zofs[k * 8u + 6u] = 0x00;
            mock->zofs[k * 8u + 7u] = 0x03; /* dec = 3 */
        }
        /* 先在"机床"上放个值：G54 的 X = 12345（dec=3 → 12.345） */
        put_u32be(mock->zofs, 12345u);
        NCL_CHECK_EQ_INT(ncl_focas_work_offset(focas, "G54", &json), NCL_OK);
        if (json != NULL) {
            double x = ncl_json_obj_get_double(json, "x", -1.0);

            NCL_CHECK(x > 12.344 && x < 12.346); /* 12345 / 10^3 */
            NCL_CHECK(ncl_json_obj_get_double(json, "y", -1.0) == 0.0);
            ncl_json_free(json);
        }
        /* 请求认到的偏移号（0x0b 的 d；mock 记下来的那格）——G54 → 1 */
        NCL_CHECK_EQ_INT((int)mock->zofs_requested, 1);

        /* 写：G54 的 X（轴号 1）= -1.5 → 原始 -1500 */
        NCL_CHECK_EQ_INT(ncl_focas_work_offset_write(focas, "G54", 1, -1.5),
                         NCL_OK);
        NCL_CHECK_EQ_INT(mock->zofs_writes, 1);
        NCL_CHECK_EQ_INT(mock->zofs_write_axis, 1);
        NCL_CHECK_EQ_INT((int)mock->zofs_write_raw, -1500);
        /* 帧形状看 mock 记下来的那几格（last_request 会被后面的复核读覆盖） */
        /* 写后复核读回来的也应该是新值 */
        NCL_CHECK_EQ_INT(ncl_focas_work_offset(focas, "G54", &json), NCL_OK);
        if (json != NULL) {
            double back = ncl_json_obj_get_double(json, "x", 0.0);

            NCL_CHECK(back == -1.5); /* -1500 / 10^3 */
            ncl_json_free(json);
        }
        /* 名字认得全：外部 / G54 / G54.1P3 */
        json = NULL;
        NCL_CHECK_EQ_INT(ncl_focas_work_offset(focas, "G59", &json), NCL_OK);
        NCL_CHECK_EQ_INT((int)mock->zofs_requested, 6);
        ncl_json_free(json);
        json = NULL;
        NCL_CHECK_EQ_INT(ncl_focas_work_offset(focas, "EXT", &json), NCL_OK);
        NCL_CHECK_EQ_INT((int)mock->zofs_requested, 0);
        ncl_json_free(json);
        json = NULL;
        NCL_CHECK_EQ_INT(ncl_focas_work_offset(focas, "G54.1P3", &json), NCL_OK);
        NCL_CHECK_EQ_INT((int)mock->zofs_requested, 9);
        ncl_json_free(json);
        /* 名字认不出：本地就挡下来，不发帧 */
        NCL_CHECK_EQ_INT(ncl_focas_work_offset(focas, "G99", &json),
                         NCL_ERR_INVALID_ARG);
        mock->axis_name_count = 0; /* 后面的用例自己摆 0x89 的应答 */
        (void)g54;
    }

    /*
     * 当前刀号：`cnc_rdcommand`（0x97，d = -1 全读模态非 G 码）回 12 字节一条的
     * 指令值记录（`[adrs][num][flag(2)][cmd_val(4)][dec_val(4)]`），找 `'T'` 那条。
     */
    NCL_TEST_CASE("当前刀号：0x97 的指令值里找 adrs = 'T' 那条的 cmd_val");
    {
        long long tool = -1;
        size_t i;

        memset(mock->payload[0], 0, 64);
        mock->payload_len[0] = 12 * 3;
        mock->payload_count = 1;
        mock->payload[0][0] = (uint8_t)'M';
        mock->payload[0][12] = (uint8_t)'T';
        put_u32be(mock->payload[0] + 12 + 4, 7); /* T7 */
        mock->payload[0][24] = (uint8_t)'S';
        put_u32be(mock->payload[0] + 24 + 4, 1200);
        NCL_CHECK_EQ_INT(ncl_focas_tool_number(focas, &tool), NCL_OK);
        NCL_CHECK_EQ_INT((int)tool, 7);
        NCL_CHECK_EQ_INT(get_u16be(mock->last_request + 8), 0x97);
        NCL_CHECK_EQ_INT((int)(int32_t)get_u32be(mock->last_request + 10), -1);
        (void)i;

        /* 机床没给 T 那条 → 如实回"读不到"，不报 0 */
        memset(mock->payload[0], 0, 64);
        mock->payload_len[0] = 12 * 2;
        mock->payload[0][0] = (uint8_t)'M';
        mock->payload[0][12] = (uint8_t)'S';
        NCL_CHECK_EQ_INT(ncl_focas_tool_number(focas, &tool),
                         NCL_ERR_NOT_FOUND);
    }

    /* 轴电流（安培）= 0x56 的 d=3（同一格里 d=1 是负载表 %）。 */
    NCL_TEST_CASE("轴电流：cnc_rdsvmeter 0x56 的 d=3（安培）");
    {
        double amps = 0.0;
        ncl_json *json = NULL;

        memset(mock->payload[0], 0, 64);
        mock->payload_len[0] = 8;
        put_u32be(mock->payload[0], 3200); /* 3.2 A（dec=3）*/
        mock->payload[0][6] = 0x00;
        mock->payload[0][7] = 0x03;
        NCL_CHECK_EQ_INT(ncl_focas_axis_current(focas, NCL_FOCAS_AXIS_X, &amps),
                         NCL_OK);
        NCL_CHECK(amps > 3.199 && amps < 3.201); /* 3200 / 10^3 */
        NCL_CHECK_EQ_INT(get_u16be(mock->last_request + 8), 0x56);
        NCL_CHECK_EQ_INT((int)(int32_t)get_u32be(mock->last_request + 10), 3);
        ncl_json_free(json);
    }

    /*
     * 执行中的程序段（`cnc_rdexecprog`，0x20）：体 = 4 字节 + ASCII 文本（0 补齐）。
     * 这台机器回的是**从执行位置起的整段程序文本**（真机 515 字节），所以这里也照
     * 多行铺，验证"整段都拿回来、尾部的 0 与空白去掉"。
     */
    NCL_TEST_CASE("执行中的程序段：4 字节头 + ASCII 文本（0 补齐），整段都要");
    mock->payload_count = 1;
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    memcpy(mock->payload[0] + 4, "M98P3001\n\nG49\n\nT01\nD1\nG0G43H1Z100.", 34);
    mock->payload_len[0] = 4 + 34 + 8; /* 尾部补 0 */
    NCL_CHECK_EQ_INT(ncl_focas_executed_block(focas, text, sizeof(text)), NCL_OK);
    NCL_CHECK(strstr(text, "T01") != NULL);
    NCL_CHECK(strstr(text, "M98P3001") != NULL);
    NCL_CHECK_EQ_INT((int)strlen(text), 34); /* 尾部那 8 个 0 不算 */

    /*
     * 模态：每一组一个 12 字节，代码在 @6（BE16，码 ×10）。真机第 8 组是 0x0050
     * （flag=0 → "G80"）、第 20 组 0x0083 + flag=1（"G13.1"）。
     */
    NCL_TEST_CASE("模态：码 @6、小数标志 @10（1 → 带一位小数）");
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_len[0] = 12;
    put_u16be(mock->payload[0] + 6, 80); /* G80 */
    {
        ncl_json *json = NULL;

        NCL_CHECK_EQ_INT(ncl_focas_modal(focas, &json), NCL_OK);
        NCL_CHECK(json != NULL);
        NCL_CHECK(ncl_json_arr_len(json) > 0);
        ncl_json_free(json);
    }
    /* 带小数的那个：131 + flag=1 → G13.1 */
    put_u16be(mock->payload[0] + 6, 131);
    put_u16be(mock->payload[0] + 10, 1);
    {
        ncl_json *json = NULL;

        NCL_CHECK_EQ_INT(ncl_focas_modal(focas, &json), NCL_OK);
        NCL_CHECK_EQ_STR(ncl_json_as_string(ncl_json_arr_get(json, 0)), "G13.1");
        ncl_json_free(json);
    }

    NCL_TEST_CASE("合成进给速度：三根直线轴里最大的那个（ACTF）");
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_len[0] = 24; /* 3 根轴 × 8 字节 */
    put_u32be(mock->payload[0], 1000000);      /* X：1000.000 */
    put_u16be(mock->payload[0] + 6, 3);
    put_u32be(mock->payload[0] + 8, 1234000);  /* Y：1234.000 ← 最大的 */
    put_u16be(mock->payload[0] + 14, 3);
    put_u32be(mock->payload[0] + 16, 0);       /* Z：0 */
    put_u16be(mock->payload[0] + 22, 3);
    NCL_CHECK_EQ_INT(ncl_focas_feed_speed(focas, &real), NCL_OK);
    NCL_CHECK(real > 1233.9 && real < 1234.1);

    NCL_TEST_CASE("还没核准的那几条回 NCL_ERR_UNAVAILABLE，并说清要抓哪一帧");
    {
        ncl_json *json = NULL;

        /* 模态已经实现了（见上），这里换成子程序号 —— 要 `cnc_rdexecprog3`。 */
        NCL_CHECK_EQ_INT(ncl_focas_subprogram_number(focas, &number),
                         NCL_ERR_UNAVAILABLE);
        NCL_CHECK(strstr(ncl_focas_last_error(focas), "cnc_rdexecprog3") != NULL);
        NCL_CHECK_EQ_INT(ncl_focas_tool_life(focas, 1, &number),
                         NCL_ERR_UNAVAILABLE);
    }

    ncl_focas_close(focas);
    mock_stop(mock);
}

/*
 * 程序下行（PC → CNC）是三件套：func 0x11（start，516 字节体）→ 0x12（数据帧，
 * dir=4，体就是程序文本，机床不回）→ 0x13（end；下载的错都在这条回）。这一段拿
 * 假机床把帧序与文本内容验一遍（码与体长来自官方 SDK 实测，见 01 册 §2.4）。
 */
/**
 * 刀具那一族（2026-09 对 NCGuide 0i-MF Plus 核过的那几条，01 册 §11.13）：
 *
 *   - 刀补号上限来自 `cnc_rdtofsinfo`（0x0a）载荷 @2 的 `use_no`；
 *   - 一条刀补是 8 字节记录（值 @0、dec @6），类型落在 Cb 的 `arg2 = 1000 + type`；
 *   - **写刀补（0x09）的帧形状**：块长 = 0x1c + 载荷、`tag0` = 0、
 *     **`tag1` = 载荷长度**、载荷 8 字节 = BE32 值 + 0000 + ffff。
 *     最后这一格是拿官方 SDK 的帧逐字节对出来的（tag0 写长度会被机床回 EW_LENGTH）；
 *   - 机床回块返回码 1（EW_FUNC）/ 6（EW_NOOPT）时翻成 `NCL_ERR_UNAVAILABLE`。
 */
static void test_tool_tables(void)
{
    focas_mock *mock = mock_start();
    ncl_focas_config config;
    ncl_focas *focas;
    char *err = NULL;
    ncl_json *one = NULL;
    long long count = 0;

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

    NCL_TEST_CASE("刀补号上限：cnc_rdtofsinfo（0x0a）载荷 @2 的 use_no");
    memset(mock->payload[0], 0, sizeof(mock->payload[0]));
    mock->payload_count = 1;
    mock->payload_len[0] = 8;
    put_u16be(mock->payload[0] + 2, 400);
    put_u16be(mock->payload[0] + 4, 2);
    NCL_CHECK_EQ_INT(ncl_focas_tool_offset_count(focas, &count), NCL_OK);
    NCL_CHECK_EQ_INT(count, 400);

    /* 一条刀补：真机回 `00000008 000a 0003` → 0.008mm（dec 在 @6）。 */
    NCL_TEST_CASE("一条刀补：8 字节记录，值 @0 / dec @6");
    mock->payload_len[0] = 8;
    put_u32be(mock->payload[0], 8);
    put_u16be(mock->payload[0] + 4, 10);
    put_u16be(mock->payload[0] + 6, 3);
    one = NULL;
    NCL_CHECK_EQ_INT(ncl_focas_tool_offset_typed(focas, 1, 1, &one), NCL_OK);
    NCL_CHECK(one != NULL);
    if (one != NULL) {
        NCL_CHECK_EQ_INT((long long)(ncl_json_obj_get_double(one, "value", -1) *
                                     1000.0 + 0.5),
                         8);
        NCL_CHECK_EQ_INT(ncl_json_obj_get_int(one, "raw", -1), 8);
        ncl_json_free(one);
    }

    NCL_TEST_CASE("写刀补（0x09）：块长 0x24、tag1 = 载荷长度、载荷 8 字节");
    NCL_CHECK_EQ_INT(ncl_focas_tool_offset_write_typed(focas, 1, 1, 2.2345),
                     NCL_OK);
    NCL_CHECK(mock->last_request_len == 2u + 28u + 8u);
    if (mock->last_request_len == 2u + 28u + 8u) {
        const uint8_t *req = mock->last_request;

        /* 体 = 2 字节块数 + 28 字节块（size/first/index/code/d/e/a2/a3/tag0/tag1）
         * + 载荷。下面所有偏移都是**体**里的偏移。 */
        NCL_CHECK_EQ_INT(get_u16be(req), 1);         /* 块数 */
        NCL_CHECK_EQ_INT(get_u16be(req + 2), 0x24);  /* 块长 = 0x1c + 8 */
        NCL_CHECK_EQ_INT(get_u16be(req + 8), 0x09);  /* 码：读 0x08 + 1 */
        NCL_CHECK_EQ_INT((int)(int32_t)get_u32be(req + 10), 1);    /* d */
        NCL_CHECK_EQ_INT((int)(int32_t)get_u32be(req + 14), 1);    /* e */
        NCL_CHECK_EQ_INT((int)(int32_t)get_u32be(req + 18), 1001); /* 1000+type */
        NCL_CHECK_EQ_INT((int)(int32_t)get_u32be(req + 22), 0);    /* a3 */
        NCL_CHECK_EQ_INT(get_u16be(req + 26), 0);    /* tag0 = 0 */
        NCL_CHECK_EQ_INT(get_u16be(req + 28), 8);    /* tag1 = 载荷长度 */
        /* 载荷 = BE32(2.2345 → 0.001mm 那一档 = 2234/2235) + 0000 + ffff */
        NCL_CHECK_EQ_INT((int)(int32_t)get_u32be(req + 30) >= 2234 &&
                             (int32_t)get_u32be(req + 30) <= 2235,
                         1);
        NCL_CHECK_EQ_INT(get_u16be(req + 34), 0);
        NCL_CHECK_EQ_INT(get_u16be(req + 36), 0xFFFF);
    }

    NCL_TEST_CASE("机床说 EW_FUNC=1 / EW_NOOPT=6 → NCL_ERR_UNAVAILABLE");
    mock->block_rc = 6;
    one = NULL;
    NCL_CHECK_EQ_INT(ncl_focas_tool_offset_typed(focas, 1, 1, &one),
                     NCL_ERR_UNAVAILABLE);
    NCL_CHECK(one == NULL);
    mock->block_rc = 1;
    NCL_CHECK_EQ_INT(ncl_focas_tool_offset_typed(focas, 1, 1, &one),
                     NCL_ERR_UNAVAILABLE);
    /* 别的码仍旧是"模块错"，别混成一类 */
    mock->block_rc = 3;
    NCL_CHECK_EQ_INT(ncl_focas_tool_offset_typed(focas, 1, 1, &one),
                     NCL_FOCAS_ERR_RB_CODE);
    mock->block_rc = 0;

    NCL_TEST_CASE("写参数：写后复核，值没变就回 UNAVAILABLE（不假装写成功）");
    mock->payload_len[0] = 16;
    put_u32be(mock->payload[0], 1);     /* 号 */
    put_u32be(mock->payload[0] + 4, 0); /* 条数 */
    put_u32be(mock->payload[0] + 8, 1); /* 值是 1（机床里就是 1） */
    /* 写一个**不一样**的值：假机床照原样回 1 → 复核不过 → UNAVAILABLE */
    NCL_CHECK_EQ_INT(ncl_focas_parameter_write(focas, 1, "0"),
                     NCL_ERR_UNAVAILABLE);
    /* 写回原值：不需要"变化"，照常算成功 */
    NCL_CHECK_EQ_INT(ncl_focas_parameter_write(focas, 1, "1"), NCL_OK);

    ncl_focas_close(focas);
    mock_stop(mock);
}

static void test_program_transfer(void)
{
    /* 程序正文**第一行必须是程序号**（FANUC 的规矩，用户口径 + 官方 SDK 的帧，
     * 01 册 §11.14）：机床是从这一行认程序号的。 */
    static const char kProgram[] = "O0001\nN100 G0 X0 Y0\nN110 M3 S1200\n";
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
    /*
     * 送出去的必须是**机床要的那份格式**（官方 spec `cnc_download4.xml` 的
     * "NC data format" + 它给的例子 `"\nO1234\nG1F0.3W10.\nM30\n%"`）：
     * 开头补一个 LF、结尾补一个 `%`。少了这两样机床在 end 回 `EW_ATTRIB`（§11.15）。
     */
    {
        static const char kFramed[] = "\nO0001\nN100 G0 X0 Y0\nN110 M3 S1200\n%";

        NCL_CHECK_EQ_INT((int)mock->transfer_bytes, (int)strlen(kFramed));
        NCL_CHECK(memcmp(mock->transfer, kFramed, strlen(kFramed)) == 0);
    }
    NCL_CHECK_EQ_INT(mock->transfer_dir, NCL_FOCAS_DIR_DATA);
    NCL_CHECK_EQ_INT(mock->last_func, NCL_FOCAS_FUNC_DWN_END);
    /*
     * 分道（§11.18）：传输三件套必须走**第一条**、命令帧必须走**第二条**。
     * mock 已经在收帧时把发错道的连接断掉了，这里再明着记一笔 —— 万一以后
     * 有人把 `focas_transfer_socket()` 又改回 `ctx->socket`，这条断言先红。
     */
    NCL_CHECK_EQ_INT(mock->transfer_channel, 1);
    NCL_CHECK_EQ_INT(mock->cmd_channel, 2);
    /* 帧序：握手（hello + 两条探测）+ start / data / end 各一条 */
    NCL_CHECK_EQ_INT((int)mock->seen_count, 6);
    NCL_CHECK_EQ_INT(mock->seen[3], NCL_FOCAS_FUNC_DWN_START);
    NCL_CHECK_EQ_INT(mock->seen[4], NCL_FOCAS_FUNC_DWN_DATA);
    NCL_CHECK_EQ_INT(mock->seen[5], NCL_FOCAS_FUNC_DWN_END);

    NCL_TEST_CASE("程序正文没有程序号那一行（O 开头）→ 本地挡下来，不发帧");
    mock->seen_count = 0;
    NCL_CHECK_EQ_INT(ncl_focas_program_download(focas, 0, NULL,
                                                "N100 G0 X0 Y0\nM30\n"),
                     NCL_ERR_INVALID_ARG);
    NCL_CHECK_EQ_INT((int)mock->seen_count, 0); /* 一帧都没发出去 */

    NCL_TEST_CASE("start 帧要的是**目录**：文件路径给进来会取目录那一段");
    NCL_CHECK_EQ_INT(ncl_focas_program_download(
                         focas, 0, "//CNC_MEM/USER/PATH1/O0001", kProgram),
                     NCL_OK);
    /* 体里 [3] = 1、[4..6) = "N:"、[6..) = 目录名（带上最后那个 '/'） */
    NCL_CHECK_EQ_INT(mock->last_request[3], 0x01);
    NCL_CHECK(memcmp(mock->last_request + 4, "N:", 2) == 0);
    NCL_CHECK(memcmp(mock->last_request + 6, "//CNC_MEM/USER/PATH1/", 21) == 0);
    NCL_CHECK(mock->last_request[27] == 0x00); /* 文件名那一段没有被带进去 */

    NCL_TEST_CASE("已经带 LF/% 的正文原样发（spec 的例子）");
    mock->transfer_bytes = 0;
    {
        static const char kExample[] = "\nO1234\nG1F0.3W10.\nM30\n%";

        NCL_CHECK_EQ_INT(ncl_focas_program_download(focas, 0, NULL, kExample),
                         NCL_OK);
        NCL_CHECK_EQ_INT((int)mock->transfer_bytes, (int)strlen(kExample));
        NCL_CHECK(memcmp(mock->transfer, kExample, strlen(kExample)) == 0);
    }

    /*
     * 传输三件套的状态回执：真机上 `0x13` end 的应答是**方向 3**、体前 4 字节是
     * 机床返回码。普通调用把方向 3 当"没有这个数"，传输这一族得按返回码解释 ——
     * 这条就是核这个（01 册 §11.14：SDK 对这份程序拿到的就是 `00000005`）。
     */
    NCL_TEST_CASE("下行 end 的状态回执（方向 3 + EW_DATA=5 + 细码）→ 报模块错并带上原因");
    mock->transfer_status = 5;
    mock->transfer_detail = 4; /* = 这个程序号已经登记过（§11.18 实测） */
    NCL_CHECK_EQ_INT(ncl_focas_program_download(focas, 0, NULL, kProgram),
                     NCL_FOCAS_ERR_TRANSFER2(5, 4));
    NCL_CHECK(strstr(ncl_focas_last_error(focas), "EW_DATA") != NULL);
    NCL_CHECK(strstr(ncl_focas_last_error(focas), "细码 4") != NULL);
    NCL_CHECK(strstr(ncl_focas_last_error(focas), "已经登记过") != NULL);
    mock->transfer_status = 0;
    mock->transfer_detail = 0;

    NCL_TEST_CASE("下行 start 的细码 1（目录名不对）也翻成人话");
    mock->transfer_status = 5;
    mock->transfer_detail = 1;
    mock->transfer_status_on_start = true;
    NCL_CHECK_EQ_INT(ncl_focas_program_download(focas, 0, NULL, kProgram),
                     NCL_FOCAS_ERR_TRANSFER2(5, 1));
    NCL_CHECK(strstr(ncl_focas_last_error(focas), "目录名不对") != NULL);
    mock->transfer_status_on_start = false;
    mock->transfer_status = 0;
    mock->transfer_detail = 0;

    /*
     * 取程序：走 **`cnc_rdpdf_line`（Cb 0xf0）** —— 按文件名按行读内容。
     * 帧（01 册 §11.16 抓的）：`d` = 起始行号、`e` = 读几行、`tag1` = 载荷长度、
     * 块长 = 0x1c + 载荷、载荷 = **256 字节**的程序路径（NUL 补齐）；应答体就是正文。
     *
     * 末行没有 `'\n'` = 这一段就是程序结尾（spec："最后一行没读到 EOB 就不算一行"），
     * 所以下面铺的假正文结尾不带换行 —— 一次就收，不会绕圈。
     */
    NCL_TEST_CASE("取程序：Cb 0xf0 + 256 字节路径，应答体就是程序正文");
    {
        static const char kText[] = "O3001(SUBPOCKET)\nM99\n%";

        memset(mock->payload[0], 0, sizeof(mock->payload[0]));
        mock->payload_count = 1;
        memcpy(mock->payload[0], kText, sizeof(kText) - 1u);
        mock->payload_len[0] = sizeof(kText) - 1u;
        program = NULL;
        len = 0;
        NCL_CHECK_EQ_INT(
            ncl_focas_program_upload(focas, 0, "//CNC_MEM/USER/PATH1/O3001",
                                     &program, &len),
            NCL_OK);
        NCL_CHECK(program != NULL);
        if (program != NULL) {
            NCL_CHECK_EQ_INT((int)len, (int)strlen(kText));
            NCL_CHECK(memcmp(program, kText, strlen(kText)) == 0);
        }
        ncl_free_safe(program);
        /* 请求帧：一块、码 0xf0、d = 0（第一行）、e = 64（一次读几行）、
         * 块长 284 = 28 + 256、tag1 = 256、载荷第 6 个字节起是路径。 */
        NCL_CHECK_EQ_INT((int)mock->last_request_len, 2 + 28 + 256);
        NCL_CHECK_EQ_INT(get_u16be(mock->last_request + 2), 284);
        NCL_CHECK_EQ_INT(get_u16be(mock->last_request + 8), 0xF0);
        NCL_CHECK_EQ_INT((int)(int32_t)get_u32be(mock->last_request + 10), 0);
        NCL_CHECK_EQ_INT((int)(int32_t)get_u32be(mock->last_request + 14), 64);
        NCL_CHECK_EQ_INT(get_u16be(mock->last_request + 28), 256); /* tag1 */
        NCL_CHECK(memcmp(mock->last_request + 30, "//CNC_MEM/USER/PATH1/O3001",
                         25) == 0);
    }

    /*
     * 机床 `cnc_pdf_add` 建出来的小程序内容长这样：`O2200%`（末尾 `%`、**没有换行**）。
     * 以前拿"数到的换行数"当"有没有读到东西"，这种内容数出来 0 行 → 误报"没有这个程序"。
     */
    NCL_TEST_CASE("单行且末尾没有换行的程序（`O2200%`）也要读得回来");
    {
        static const char kOne[] = "O2200%";

        memset(mock->payload[0], 0, sizeof(mock->payload[0]));
        memcpy(mock->payload[0], kOne, sizeof(kOne) - 1u);
        mock->payload_len[0] = sizeof(kOne) - 1u;
        program = NULL;
        len = 0;
        NCL_CHECK_EQ_INT(
            ncl_focas_program_upload(focas, 0, "//CNC_MEM/USER/PATH1/O2200",
                                     &program, &len),
            NCL_OK);
        NCL_CHECK_EQ_INT((int)len, 6);
        NCL_CHECK(program != NULL && memcmp(program, kOne, 6) == 0);
        ncl_free_safe(program);
    }

    NCL_TEST_CASE("只给文件名（没有 '/'）→ 自己补上默认文件夹再问");
    mock->payload_len[0] = 0;
    program = NULL;
    NCL_CHECK_EQ_INT(ncl_focas_program_upload(focas, 0, "O3001", &program,
                                              &len),
                     NCL_ERR_NOT_FOUND); /* 假机床回空载荷 = 读不到 */
    NCL_CHECK(program == NULL);
    NCL_CHECK_EQ_INT(mock->last_request_len, 2 + 28 + 256);
    NCL_CHECK(memcmp(mock->last_request + 30, "//CNC_MEM/USER/PATH1/O3001",
                     25) == 0);

    NCL_TEST_CASE("只有 NC 程序（type 0）能这么读，别的类型如实回 UNAVAILABLE");
    mock->payload_len[0] = 0;
    program = NULL;
    NCL_CHECK_EQ_INT(ncl_focas_program_upload(focas, 1, "O3001", &program, &len),
                     NCL_ERR_UNAVAILABLE);
    NCL_CHECK(program == NULL);

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
    long long number = 0;

    ncl_focas_config_default(&config);
    config.host = "127.0.0.1";
    focas = ncl_focas_open(&config, &err);
    NCL_CHECK(focas != NULL);
    if (focas == NULL) {
        printf("    %s\n", err != NULL ? err : "?");
        ncl_free_safe(err);
        return;
    }

    NCL_TEST_CASE("还没抓到帧的几条调用回 NCL_ERR_UNAVAILABLE，并说清要抓哪一帧");
    /* 报警、程序目录、工件坐标都实现了（见 test_semantics：0x0b/0x0c 那一族），
     * 这里换成"子程序号"——官方库对 `cnc_rdexecprog3` 一帧都不发。 */
    NCL_CHECK_EQ_INT(ncl_focas_subprogram_number(focas, &number),
                     NCL_ERR_UNAVAILABLE);
    NCL_CHECK(strstr(ncl_focas_last_error(focas), "cnc_rdexecprog3") != NULL);

    /* 合成进给速度与模态都已经实现了（见 test_semantics），这里换一条还没核准的：
     * 主轴倍率要 `IODBSGNL.spdl_ovrd`（现代系列没有那一格）。 */
    NCL_CHECK_EQ_INT(ncl_focas_spindle_override(focas, &position),
                     NCL_ERR_UNAVAILABLE);
    NCL_CHECK(strstr(ncl_focas_last_error(focas), "spdl_ovrd") != NULL);
    /* 轴号越界仍旧是参数错，不是"还没有" */
    NCL_CHECK_EQ_INT(ncl_focas_axis_srv_delay(focas, (ncl_focas_axis)77,
                                              &position),
                     NCL_ERR_RANGE);

    /* 刀具表已经是真读的（见 test_tool_tables）；这里换成"这台机床没开选件"的两条：
     * 宏变量表（用户宏变量）与刀具寿命（寿命管理）在 0i-MF 上都回 EW_NOOPT。 */
    NCL_CHECK_EQ_INT(ncl_focas_variable_table(focas, &value),
                     NCL_ERR_UNAVAILABLE);
    NCL_CHECK(value == NULL);
    NCL_CHECK(strstr(ncl_focas_last_error(focas), "cnc_rdmacror") != NULL);
    NCL_CHECK_EQ_INT(ncl_focas_tool_life(focas, 1, &number),
                     NCL_ERR_UNAVAILABLE);
    NCL_CHECK(strstr(ncl_focas_last_error(focas), "cnc_rdlife") != NULL);

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
    test_tool_tables();
    test_program_transfer();
NCL_TEST_MAIN_END()
