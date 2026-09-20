/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * SYNTEC RemoteCNC packet layer: the 12 byte header and 8 byte function header
 * of 10-SYNTEC-新代-RemoteCNC.md §10, byte for byte.
 */
#include <stdio.h>
#include <string.h>

#include "ncl_test.h"

#include "nclink/clients/syntec.h"

static bool binary_equal(const uint8_t *actual, const char *expected, size_t len)
{
    return memcmp(actual, expected, len) == 0;
}

static void test_header(void)
{
    ncl_syntec_function function;
    uint8_t frame[64];
    size_t len;

    NCL_TEST_CASE("the packet header is 12 bytes: Length, CmdID, pad, Reserved");
    memset(&function, 0, sizeof(function));
    function.func_id = 0x0007;
    function.serial = 0x2A;
    len = ncl_syntec_build(frame, sizeof(frame), NCL_SYNTEC_CMD_KRML_API,
                           &function, NULL, 0);
    NCL_CHECK_EQ_INT(len, 20); /* 12 + 8, no body */
    NCL_CHECK(binary_equal(frame, "\x08\x00\x00\x00" /* Length = 8   */
                                  "\xC8\x00"         /* CmdID  = 200 */
                                  "\x00\x00"         /* the 2 pad    */
                                  "\x00\x00\x00\x00" /* Reserved     */
                                  "\x07\x00"         /* uFuncID      */
                                  "\x2A"             /* uSerial      */
                                  "\x00"             /* Reserved     */
                                  "\x00\x00\x00\x00" /* IHeader      */,
                           20));

    NCL_TEST_CASE("Length counts everything after the header");
    {
        const uint8_t body[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
        ncl_syntec_view view;
        size_t frame_len = 0;

        function.serial = 1;
        len = ncl_syntec_build(frame, sizeof(frame), 7u, &function, body,
                               sizeof(body));
        NCL_CHECK_EQ_INT(len, 12 + 8 + 5);
        NCL_CHECK_EQ_INT(frame[0], 13); /* 8 byte function header + 5 body */
        NCL_CHECK_EQ_INT(ncl_syntec_split(frame, len, &view, &frame_len), NCL_OK);
        NCL_CHECK_EQ_INT(frame_len, len);
        NCL_CHECK_EQ_INT(view.packet.cmd_id, 7);
        NCL_CHECK_EQ_INT(view.packet.length, 13);
        NCL_CHECK_EQ_INT(view.function.func_id, 0x0007);
        NCL_CHECK_EQ_INT(view.function.serial, 1);
        NCL_CHECK_EQ_INT(view.body_len, 5);
        NCL_CHECK(memcmp(view.body, body, sizeof(body)) == 0);

        NCL_TEST_CASE("a half arrived packet is not an error");
        NCL_CHECK_EQ_INT(ncl_syntec_split(frame, 11, &view, NULL), NCL_ERR_RANGE);
        NCL_CHECK_EQ_INT(ncl_syntec_split(frame, 20, &view, NULL), NCL_ERR_RANGE);
    }

    NCL_TEST_CASE("a packet too short to hold a function header is refused");
    {
        uint8_t bad[12];

        memset(bad, 0, sizeof(bad));
        bad[0] = 4; /* Length = 4, less than the 8 byte function header */
        {
            ncl_syntec_view view;

            NCL_CHECK_EQ_INT(ncl_syntec_split(bad, sizeof(bad), &view, NULL),
                             NCL_DRV_ERR_PROTOCOL(0xA0));
        }
    }

    NCL_TEST_CASE("building refuses what does not fit");
    NCL_CHECK_EQ_INT(ncl_syntec_build(frame, 19, 1, &function, NULL, 0), 0);
    NCL_CHECK_EQ_INT(ncl_syntec_build(NULL, sizeof(frame), 1, &function, NULL, 0),
                     0);
}

static void test_commands(void)
{
    uint16_t cmd = 0;
    const char *canonical = NULL;
    int32_t code = 0;

    NCL_TEST_CASE("the known command numbers of §10.6 / §10.7");
    NCL_CHECK(ncl_syntec_cmd_lookup("FileSendStart", &cmd, &canonical));
    NCL_CHECK_EQ_INT(cmd, 1);
    NCL_CHECK(ncl_syntec_cmd_lookup("FileExist", &cmd, &canonical));
    NCL_CHECK_EQ_INT(cmd, 11);
    NCL_CHECK(ncl_syntec_cmd_lookup("DirCreate", &cmd, &canonical));
    NCL_CHECK_EQ_INT(cmd, 17);
    NCL_CHECK(ncl_syntec_cmd_lookup("KrnlAPI", &cmd, &canonical));
    NCL_CHECK_EQ_INT(cmd, 200);
    NCL_CHECK(ncl_syntec_cmd_lookup("ResMgrRemoteLookup", &cmd, &canonical));
    NCL_CHECK_EQ_INT(cmd, 178);
    NCL_CHECK(ncl_syntec_cmd_lookup("RemoteProgExecute", &cmd, &canonical));
    NCL_CHECK_EQ_INT(cmd, 180);
    NCL_CHECK(ncl_syntec_cmd_lookup("NcShutdown", &cmd, &canonical));
    NCL_CHECK_EQ_INT(cmd, 87);
    /* 枚举里带显式值：声明顺序骗人，这两个是实测值 */
    NCL_CHECK(ncl_syntec_cmd_lookup("GetAllFileList", &cmd, NULL));
    NCL_CHECK_EQ_INT(cmd, 8);
    NCL_CHECK(ncl_syntec_cmd_lookup("Install", &cmd, NULL));
    NCL_CHECK_EQ_INT(cmd, 48);
    NCL_CHECK(!ncl_syntec_cmd_lookup("NoSuchCommand", &cmd, NULL));

    NCL_TEST_CASE("the data codes of §10.10 are the enum values");
    NCL_CHECK(ncl_syntec_data_code("DT_PART_COUNT", &code));
    NCL_CHECK_EQ_INT(code, 43);
    NCL_CHECK(ncl_syntec_data_code("dt_cnc_status", &code));
    NCL_CHECK_EQ_INT(code, 41);
    NCL_CHECK(ncl_syntec_data_code("DT_MACHINEPOS", &code));
    NCL_CHECK_EQ_INT(code, 0);
    NCL_CHECK(ncl_syntec_data_code("DT_BUFFEROVERFLOW", &code));
    NCL_CHECK_EQ_INT(code, 500);
    NCL_CHECK(ncl_syntec_data_code("STATE_VARIABLE", &code));
    NCL_CHECK_EQ_INT(code, 9);
    NCL_CHECK(ncl_syntec_data_code("NOT_DEFINE", &code));
    NCL_CHECK_EQ_INT(code, -1);
    NCL_CHECK(!ncl_syntec_data_code("DT_NOPE", &code));
    NCL_CHECK_EQ_STR(ncl_syntec_data_code_name(43), "DT_PART_COUNT");
    NCL_CHECK(ncl_syntec_data_code_name(9999) == NULL);

    NCL_TEST_CASE("a bare command number is accepted, as §10.7 numbers one space");
    NCL_CHECK(ncl_syntec_cmd_lookup("179", &cmd, &canonical));
    NCL_CHECK_EQ_INT(cmd, 179);
    NCL_CHECK(ncl_syntec_cmd_lookup("60000", &cmd, NULL));
    NCL_CHECK_EQ_INT(cmd, 60000);
    NCL_CHECK(!ncl_syntec_cmd_lookup("70000", &cmd, NULL)); /* past u2 */
    NCL_CHECK(!ncl_syntec_cmd_lookup("12x", &cmd, NULL));

    NCL_TEST_CASE("names and services come back");
    NCL_CHECK_EQ_STR(ncl_syntec_cmd_name(200), "KrnlAPI");
    NCL_CHECK_EQ_STR(ncl_syntec_cmd_name(11), "FileExist");
    NCL_CHECK(ncl_syntec_cmd_name(999) == NULL);
    NCL_CHECK_EQ_STR(ncl_syntec_cmd_service(11), "FileTransfer");
    NCL_CHECK_EQ_STR(ncl_syntec_cmd_service(48), "FileTransfer"); /* Install */
    NCL_CHECK_EQ_STR(ncl_syntec_cmd_service(178), "Dipole");
    NCL_CHECK_EQ_STR(ncl_syntec_cmd_service(200), "Dipole");
    NCL_CHECK_EQ_STR(ncl_syntec_cmd_service(900), "?");
}

static void test_readings(void)
{
    const ncl_syntec_reading *reading;

    NCL_TEST_CASE("§10.12: the named readings carry the code the workers load");
    reading = ncl_syntec_reading_lookup("part_count");
    NCL_CHECK(reading != NULL);
    if (reading != NULL) {
        NCL_CHECK_EQ_INT(reading->code, 1000);
        NCL_CHECK_EQ_INT(reading->cmd_id, 200); /* KrnlAPI */
        NCL_CHECK_EQ_STR(reading->name, "part_count");
    }
    /* The client's own spelling, case and underscores ignored */
    reading = ncl_syntec_reading_lookup("READ_part_count");
    NCL_CHECK(reading != NULL);
    if (reading != NULL) {
        NCL_CHECK_EQ_INT(reading->code, 1000);
    }
    reading = ncl_syntec_reading_lookup("PartCount");
    NCL_CHECK(reading != NULL);
    if (reading != NULL) {
        NCL_CHECK_EQ_INT(reading->code, 1000);
    }
    reading = ncl_syntec_reading_lookup("READ_part_count_good");
    NCL_CHECK(reading != NULL);
    if (reading != NULL) {
        NCL_CHECK_EQ_INT(reading->code, 1002);
    }
    reading = ncl_syntec_reading_lookup("part_count_bad");
    NCL_CHECK(reading != NULL);
    if (reading != NULL) {
        NCL_CHECK_EQ_INT(reading->code, 1004);
    }
    reading = ncl_syntec_reading_lookup("spindle_700");
    NCL_CHECK(reading != NULL);
    if (reading != NULL) {
        NCL_CHECK_EQ_INT(reading->code, 700);
    }
    reading = ncl_syntec_reading_lookup("READ_spindle_771");
    NCL_CHECK(reading != NULL);
    if (reading != NULL) {
        NCL_CHECK_EQ_INT(reading->code, 771);
    }
    NCL_CHECK(ncl_syntec_reading_lookup("part_count_worse") == NULL);
    NCL_CHECK(ncl_syntec_reading_lookup("KrnlAPI") == NULL); /* a command, not one */
    NCL_CHECK(ncl_syntec_reading_lookup("") == NULL);
    NCL_CHECK(ncl_syntec_reading_lookup(NULL) == NULL);
}

static void test_bodies(void)
{
    uint8_t body[64];
    size_t len;
    ncl_syntec_krnl_request request;

    NCL_TEST_CASE("the KrnlAPI body carries the four fields then the input");
    len = ncl_syntec_krnl_body(body, sizeof(body), 0x0001, 0x00000042, 0, 4,
                               "\xAA\xBB", 2);
    NCL_CHECK_EQ_INT(len, 16);
    NCL_CHECK(binary_equal(body, "\x01\x00"         /* uFuncID */
                                 "\x42\x00\x00\x00" /* dwCode  */
                                 "\x00\x00\x00\x00" /* dwSizeIn */
                                 "\x04\x00\x00\x00" /* dwSizeOut */
                                 "\xAA\xBB",
                           16));
    NCL_CHECK_EQ_INT(ncl_syntec_krnl_parse(body, len, &request), NCL_OK);
    NCL_CHECK_EQ_INT(request.func_id, 1);
    NCL_CHECK_EQ_INT(request.code, 0x42);
    NCL_CHECK_EQ_INT(request.size_out, 4);
    NCL_CHECK_EQ_INT(ncl_syntec_krnl_parse(body, 10, &request), NCL_ERR_RANGE);

    NCL_TEST_CASE("a path body is a length and the characters");
    len = ncl_syntec_path_body(body, sizeof(body), 0x0001, "M01:\\PRG\\A.nc");
    NCL_CHECK_EQ_INT(len, 6 + 14); /* "M01:\PRG\A.nc" + NUL */
    NCL_CHECK_EQ_INT(body[0], 0x01);
    NCL_CHECK_EQ_INT(body[2], 14); /* nFilePathLength counts the terminator */
    NCL_CHECK_EQ_STR((const char *)body + 6, "M01:\\PRG\\A.nc");
    NCL_CHECK_EQ_INT(ncl_syntec_path_body(body, 4, 1, "x"), 0);
}

static void test_crc(void)
{
    static const uint8_t kData[] = {0x01, 0x02, 0x03, 0x04};

    NCL_TEST_CASE("CRC-16 is the reversed 0xA001 polynomial, as the client has it");
    NCL_CHECK_EQ_INT(ncl_syntec_crc16(kData, sizeof(kData)), 0x2BA1);
    NCL_CHECK_EQ_INT(ncl_syntec_crc16((const uint8_t *)"123456789", 9), 0x4B37);
    NCL_CHECK_EQ_INT(ncl_syntec_crc16(NULL, 0), 0xFFFF);
}

NCL_TEST_MAIN_BEGIN()
    test_header();
    test_commands();
    test_readings();
    test_bodies();
    test_crc();
NCL_TEST_MAIN_END()
