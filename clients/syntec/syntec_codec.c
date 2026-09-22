/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - SYNTEC RemoteCNC packet layer (see ncl_syntec.h).
 *
 * The structures are marshalled by .NET with the default layout rules, so the
 * 12 byte header really has two pad bytes between CmdID and Reserved - the
 * server's `BeginReceive(..., 12, ...)` is what pins that down (§10.4).
 */

#include "nclink/clients/syntec.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value; /* little endian, like the delivered assemblies */
    out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t get_u32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

size_t ncl_syntec_build(uint8_t *out, size_t cap, uint16_t cmd_id,
                        const ncl_syntec_function *function, const void *body,
                        size_t body_len)
{
    size_t content = NCL_SYNTEC_FUNCTION_HEADER + body_len;
    size_t total = NCL_SYNTEC_PACKET_HEADER + content;

    if (out == NULL || function == NULL || cap < total ||
        content > 0xFFFFFFFFu || (body_len > 0 && body == NULL)) {
        return 0;
    }
    put_u32(out, (uint32_t)content); /* Length counts what follows the header */
    put_u16(out + 4, cmd_id);
    out[6] = 0; /* the two pad bytes the default layout leaves here */
    out[7] = 0;
    put_u32(out + 8, 0); /* Reserved */
    put_u16(out + 12, function->func_id);
    out[14] = function->serial;
    out[15] = 0; /* Reserved */
    put_u32(out + 16, function->header);
    if (body_len > 0) {
        memcpy(out + 20, body, body_len);
    }
    return total;
}

ncl_err ncl_syntec_split(const uint8_t *frame, size_t len, ncl_syntec_view *out,
                         size_t *frame_len)
{
    size_t content;
    size_t total;

    if (frame == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (len < NCL_SYNTEC_PACKET_HEADER) {
        return NCL_ERR_RANGE; /* the header is still arriving */
    }
    content = get_u32(frame);
    if (content < NCL_SYNTEC_FUNCTION_HEADER) {
        return NCL_DRV_ERR_PROTOCOL(0xA0); /* a packet with no function header */
    }
    total = NCL_SYNTEC_PACKET_HEADER + content;
    if (len < total) {
        return NCL_ERR_RANGE;
    }
    memset(out, 0, sizeof(*out));
    out->packet.length = (uint32_t)content;
    out->packet.cmd_id = get_u16(frame + 4);
    out->packet.reserved = get_u32(frame + 8);
    out->function.func_id = get_u16(frame + NCL_SYNTEC_PACKET_HEADER);
    out->function.serial = frame[NCL_SYNTEC_PACKET_HEADER + 2];
    out->function.header = get_u32(frame + NCL_SYNTEC_PACKET_HEADER + 4);
    out->body = frame + NCL_SYNTEC_PACKET_HEADER + NCL_SYNTEC_FUNCTION_HEADER;
    out->body_len = content - NCL_SYNTEC_FUNCTION_HEADER;
    if (frame_len != NULL) {
        *frame_len = total;
    }
    return NCL_OK;
}

/* ============================================================= commands == */

/*
 * §10.7/§10.10: one numbering space for the whole controller. The numbers are
 * the enum values the assemblies carry - which is why GetAllFileList is 8 and
 * Install is 48 rather than the declaration order they look like.
 */
static const struct {
    const char *name;
    uint16_t    cmd_id;
} kCommands[] = {
    /* FileTransferCmd（实测值） */
    {"FileSendStart", 1},
    {"FileSending", 2},
    {"FileRecvStart", 3},
    {"FileRecving", 4},
    {"GetAllFileList", 8},
    {"FileExist", 11},
    {"DirExist", 12},
    {"FileNew", 13},
    {"FileDelete", 14},
    {"FileCopy", 15},
    {"FileMove", 16},
    {"DirCreate", 17},
    {"Install", 48},
    /* EFunctionID（实测值） */
    {"NcShutdown", 87},
    {"NcRestartCNC", 163},
    {"NcRequestUpdate", 165},
    {"NcStartControlSystem", 174},
    {"ResMgrRemoteLookup", 178},
    {"RemoteProgExecute", 180},
    {"KrnlAPI", NCL_SYNTEC_CMD_KRML_API},
    {"OnEventCall", 1}, /* AlarmCmd: TCPALARM_OnEventCall */
};

/*
 * §10.10: the data codes. `EDataType` is what a KrnlAPI read asks for with its
 * `dwCode`; `EDevice_Type` names the kind of device data a request selects.
 */
static const struct {
    const char *name;
    int32_t     code;
} kDataCodes[] = {
    /* Syntec.OpenCNC.EventTriggerEnum.EDataType */
    {"DT_MACHINEPOS", 0},        {"DT_SETTABLESET", 1},
    {"DT_BASICOFFSET", 2},       {"DT_G92OFFSET", 3},
    {"DT_HCSOFFSET", 4},         {"DT_HCSCHANGE", 5},
    {"DT_SYSTIME", 6},           {"DT_FEEDBACK", 7},
    {"DT_HOMING", 8},            {"DT_AXESALRM", 9},
    {"DT_TOOLINFO", 10},         {"DT_COORD", 11},
    {"DT_SERVOCMD", 12},         {"DT_DUALFEEDBACK", 13},
    {"DT_GLOBALVAR", 14},        {"DT_SYSTEMVAR", 15},
    {"DT_DEBUGVAR", 16},         {"DT_IBIT", 17},
    {"DT_OBIT", 18},             {"DT_CBIT", 19},
    {"DT_SBIT", 20},             {"DT_ABIT", 21},
    {"DT_SPINDLE_FEEDBACK_VEL", 22},
    {"DT_SPINDLE_SERVOCMD_VEL", 23},
    {"DT_REGISTER", 24},         {"DT_DEVICEVAL", 25},
    {"DT_ABSOLUTE_FEEDBACK", 26},
    {"DT_CNC_STATUS", 41},       {"DT_CNC_MAIN_PROGRAM", 42},
    {"DT_PART_COUNT", 43},       {"DT_BUFFEROVERFLOW", 500},
    /* Syntec.OpenCNC.EDevice_Type */
    {"L_REGISTER", 0},           {"GLOBAL_VARIABLE", 1},
    {"R_REGISTER", 2},           {"SYSTEM_VARIABLE", 3},
    {"I_BIT", 4},                {"O_BIT", 5},
    {"C_BIT", 6},                {"S_BIT", 7},
    {"A_BIT", 8},                {"STATE_VARIABLE", 9},
    {"PARAM", 10},               {"COORD_VARIABLE", 11},
    {"TIMER_STATE", 12},         {"TIMER_TYPE", 13},
    {"TIMER_SETTING", 14},       {"TIMER_ELAPSE", 15},
    {"COUNTER_STATE", 16},       {"COUNTER_TYPE", 17},
    {"COUNTER_SETTING", 18},     {"COUNTER_COUNT", 19},
    {"DEV_AX_VARIABLE", 20},     {"NOT_DEFINE", -1},
};

bool ncl_syntec_data_code(const char *name, int32_t *code)
{
    size_t i;

    if (ncl_str_is_blank(name)) {
        return false;
    }
    for (i = 0; i < sizeof(kDataCodes) / sizeof(kDataCodes[0]); i++) {
        if (ncl_streq_ignore_case(name, kDataCodes[i].name)) {
            if (code != NULL) {
                *code = kDataCodes[i].code;
            }
            return true;
        }
    }
    return false;
}

const char *ncl_syntec_data_code_name(int32_t code)
{
    size_t i;

    for (i = 0; i < sizeof(kDataCodes) / sizeof(kDataCodes[0]); i++) {
        if (kDataCodes[i].code == code) {
            return kDataCodes[i].name;
        }
    }
    return NULL;
}

/*
 * §10.12: the readings behind the client's thin API shells. The numbers are
 * what the `_GB` worker stubs load (`ldc.i4 <码>`), so they are evidence from
 * the delivered assemblies rather than a guess - but which numbering space they
 * belong to (KrnlAPI's dwCode, or a device number in pBufferIn) still needs one
 * capture on a real controller. They are read through KrnlAPI here.
 *
 * Only the two APIs the doc names a meaning for get a speaking name: 1000/1002/
 * 1004 are the total/good/bad part counters. For the spindle the doc gives two
 * numbers and no meaning, so they are named after the numbers themselves; 700
 * and 771 are the two workers that take a constant (the API has two more that
 * take the axis or channel as an argument, which a point map cannot express
 * yet).
 */
static const ncl_syntec_reading kReadings[] = {
    {"part_count", 1000, NCL_SYNTEC_CMD_KRML_API},
    {"part_count_good", 1002, NCL_SYNTEC_CMD_KRML_API},
    {"part_count_bad", 1004, NCL_SYNTEC_CMD_KRML_API},
    {"spindle_700", 700, NCL_SYNTEC_CMD_KRML_API},
    {"spindle_771", 771, NCL_SYNTEC_CMD_KRML_API},
};

/** Skip a leading "READ"/"read", which is how the doc writes the API names. */
static const char *drop_read_prefix(const char *name)
{
    static const char kRead[] = "read";
    size_t i;

    if (name == NULL) {
        return "";
    }
    for (i = 0; i < 4; i++) {
        if (name[i] == '\0' || tolower((unsigned char)name[i]) != kRead[i]) {
            return name;
        }
    }
    return name[4] == '\0' ? name : name + 4;
}

/** Case and underscores are ignored: "part_count" == "PartCount". */
static bool reading_name_matches(const char *left, const char *right)
{
    const char *a = drop_read_prefix(left);
    const char *b = drop_read_prefix(right);

    for (;;) {
        if (*a == '_') {
            a++;
            continue;
        }
        if (*b == '_') {
            b++;
            continue;
        }
        if (*a == '\0' || *b == '\0') {
            return *a == '\0' && *b == '\0';
        }
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
}

const ncl_syntec_reading *ncl_syntec_reading_lookup(const char *name)
{
    size_t i;

    if (ncl_str_is_blank(name)) {
        return NULL;
    }
    for (i = 0; i < sizeof(kReadings) / sizeof(kReadings[0]); i++) {
        if (reading_name_matches(name, kReadings[i].name)) {
            return &kReadings[i];
        }
    }
    return NULL;
}

bool ncl_syntec_cmd_lookup(const char *name, uint16_t *cmd_id,
                           const char **canonical)
{
    size_t i;
    char *end = NULL;
    unsigned long raw;

    if (ncl_str_is_blank(name)) {
        return false;
    }
    for (i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
        if (ncl_streq_ignore_case(name, kCommands[i].name)) {
            if (cmd_id != NULL) {
                *cmd_id = kCommands[i].cmd_id;
            }
            if (canonical != NULL) {
                *canonical = kCommands[i].name;
            }
            return true;
        }
    }
    /* A command number the table does not name still travels: §10.7 says the
     * numbering space is one, so a bare number is enough to try one. */
    raw = strtoul(name, &end, 10);
    if (end != name && end != NULL && *end == '\0' && raw <= 0xFFFFu) {
        if (cmd_id != NULL) {
            *cmd_id = (uint16_t)raw;
        }
        if (canonical != NULL) {
            *canonical = name;
        }
        return true;
    }
    return false;
}

const char *ncl_syntec_cmd_name(uint16_t cmd_id)
{
    size_t i;

    for (i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
        if (kCommands[i].cmd_id == cmd_id) {
            return kCommands[i].name;
        }
    }
    return NULL;
}

const char *ncl_syntec_cmd_service(uint16_t cmd_id)
{
    /* The enum values are not contiguous: GetAllFileList is 8 and Install is
     * 48, so the file transfer family is named explicitly. */
    if ((cmd_id >= 1u && cmd_id <= 17u) || cmd_id == 48u) {
        return "FileTransfer";
    }
    if (cmd_id == NCL_SYNTEC_CMD_KRML_API) {
        return "Dipole";
    }
    if (cmd_id >= NCL_SYNTEC_CMD_DIPOLE_FIRST &&
        cmd_id <= NCL_SYNTEC_CMD_DIPOLE_FIRST + 2u) {
        return "Dipole";
    }
    return "?";
}

/* ================================================================= data == */

uint16_t ncl_syntec_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t i;
    int bit;

    if (data == NULL) {
        return crc;
    }
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) != 0 ? (uint16_t)((crc >> 1) ^ 0xA001)
                                 : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

size_t ncl_syntec_krnl_body(uint8_t *out, size_t cap, uint16_t func_id,
                            int32_t code, size_t size_in, size_t size_out,
                            const void *in, size_t in_len)
{
    size_t total = 14u + in_len; /* 2 + 4 + 4 + 4, then the input bytes */

    if (out == NULL || cap < total || (in_len > 0 && in == NULL)) {
        return 0;
    }
    put_u16(out, func_id);
    put_u32(out + 2, (uint32_t)code);
    put_u32(out + 6, (uint32_t)size_in);
    put_u32(out + 10, (uint32_t)size_out);
    if (in_len > 0) {
        memcpy(out + 14, in, in_len);
    }
    return total;
}

ncl_err ncl_syntec_krnl_parse(const uint8_t *body, size_t len,
                              ncl_syntec_krnl_request *out)
{
    if (body == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (len < 14) {
        return NCL_ERR_RANGE;
    }
    out->func_id = get_u16(body);
    out->code = (int32_t)get_u32(body + 2);
    out->size_in = (int32_t)get_u32(body + 6);
    out->size_out = (int32_t)get_u32(body + 10);
    return NCL_OK;
}

size_t ncl_syntec_path_body(uint8_t *out, size_t cap, uint16_t func_id,
                            const char *path)
{
    size_t chars;
    size_t total;

    if (out == NULL || path == NULL) {
        return 0;
    }
    /* §10.2: MMI_Request_FileExist{uFuncID, nFilePathLength, szFilePath} -
     * the length is explicit, and the delivered tooling writes text with its
     * terminator, so it counts the NUL. */
    chars = strlen(path) + 1u;
    total = 2u + 4u + chars;
    if (cap < total || chars > 0xFFFFu) {
        return 0;
    }
    put_u16(out, func_id);
    put_u32(out + 2, (uint32_t)chars);
    memcpy(out + 6, path, chars);
    return total;
}

/* ================================================================ items == */

/*
 * §3.1: the nine items, exactly as captured off the box's own gateway
 * (tools/site-probe/syntec_probe.sh prints each request frame). The columns are
 * the ones the doc tabulates:
 *
 *   name            [6..7]   [10..11]  [16..19]  A       B      [32..35]
 *   STATUS          0000     0700      0407      8       4      1
 *   PART_COUNT      0000     0700      041a      8       1000   0
 *   LINE_NUMBER     0000     0700      0407      8       10     1
 *   PROGRAM         f105     071e      048c      0204    1      1
 *   FEED_SPEED      0000     0700      041a      8       700    0
 *   SPDL_SPEED      0000     0700      041a      8       771    0
 *   FEED_OVERRIDE   0000     0700      0407      8       19     1
 *   SPDL_OVERRIDE   0000     0700      0407      8       21     1
 *   WARNING         0101     0701      0428      1c78    40     115
 *
 * 700 / 1000 / 771 / 19 / 21 / 10 / 4 are the register / state numbers that
 * also appear in the client's own worker stubs (§10.12) - the same numbers,
 * which is the cross check that ties this table to the delivered client.
 */
static const ncl_syntec_item kItems[] = {
    {"STATUS", 0x0000u, 0x0700u, 0x0407u, 8u, 4u, 1u},
    {"PART_COUNT", 0x0000u, 0x0700u, 0x041au, 8u, 1000u, 0u},
    {"LINE_NUMBER", 0x0000u, 0x0700u, 0x0407u, 8u, 10u, 1u},
    {"PROGRAM", 0x05f1u, 0x071eu, 0x048cu, 0x0204u, 1u, 1u},
    {"FEED_SPEED", 0x0000u, 0x0700u, 0x041au, 8u, 700u, 0u},
    {"SPDL_SPEED", 0x0000u, 0x0700u, 0x041au, 8u, 771u, 0u},
    {"FEED_OVERRIDE", 0x0000u, 0x0700u, 0x0407u, 8u, 19u, 1u},
    {"SPDL_OVERRIDE", 0x0000u, 0x0700u, 0x0407u, 8u, 21u, 1u},
    {"WARNING", 0x0101u, 0x0701u, 0x0428u, 0x1c78u, 40u, 115u},
};

size_t ncl_syntec_item_count(void)
{
    return sizeof(kItems) / sizeof(kItems[0]);
}

const ncl_syntec_item *ncl_syntec_item_at(size_t index)
{
    return index < ncl_syntec_item_count() ? &kItems[index] : NULL;
}

const ncl_syntec_item *ncl_syntec_item_lookup(const char *name)
{
    size_t i;

    if (ncl_str_is_blank(name)) {
        return NULL;
    }
    for (i = 0; i < ncl_syntec_item_count(); i++) {
        if (reading_name_matches(name, kItems[i].name)) {
            return &kItems[i];
        }
    }
    return NULL;
}

size_t ncl_syntec_item_frame(uint8_t *out, size_t cap,
                             const ncl_syntec_item *item, uint32_t param_b,
                             uint8_t serial)
{
    if (out == NULL || item == NULL || cap < NCL_SYNTEC_ITEM_FRAME) {
        return 0;
    }
    /* Length counts the content: the 8 byte function header + the 16 byte body. */
    put_u32(out, (uint32_t)(NCL_SYNTEC_FUNCTION_HEADER + NCL_SYNTEC_ITEM_BODY));
    put_u16(out + 4, NCL_SYNTEC_CMD_ITEM);
    put_u16(out + 6, item->flags);
    put_u16(out + 8, (uint16_t)NCL_SYNTEC_CMD_KRML_API); /* the constant 200 */
    put_u16(out + 10, item->code);
    put_u16(out + 12, (uint16_t)NCL_SYNTEC_CMD_KRML_API); /* uFuncID */
    out[14] = serial;                                     /* uSerial */
    out[15] = 0;
    put_u32(out + 16, item->request);                     /* IHeader */
    put_u32(out + 20, 4u);                                /* type */
    put_u32(out + 24, item->param_a);
    put_u32(out + 28, param_b);
    put_u32(out + 32, item->flag);
    return NCL_SYNTEC_ITEM_FRAME;
}

bool ncl_syntec_item_u16(const uint8_t *frame, size_t len, uint16_t *value)
{
    if (frame == NULL || value == NULL ||
        len < NCL_SYNTEC_REPLY_BODY + 2u) {
        return false;
    }
    *value = get_u16(frame + NCL_SYNTEC_REPLY_BODY);
    return true;
}

bool ncl_syntec_item_i16(const uint8_t *frame, size_t len, size_t index,
                         int16_t *value)
{
    size_t at = NCL_SYNTEC_REPLY_BODY + index * 2u;

    if (frame == NULL || value == NULL || len < at + 2u) {
        return false;
    }
    *value = (int16_t)get_u16(frame + at);
    return true;
}

/*
 * §11.4：状态区与参数区共用同一种帧，只有请求号（§3.1 的 [16..19]）不同。
 * 把"请求号 + 索引 + 要几个字节"抽出来，两个读法都是它的一行调用。
 */
static size_t syntec_code_frame(uint8_t *out, size_t cap, uint32_t request,
                                unsigned index, size_t bytes, uint8_t serial)
{
    size_t answer = 4u + bytes; /* 回答里的那个 4 字节字 + 正文 */
    ncl_syntec_item item;

    if (out == NULL || cap < NCL_SYNTEC_ITEM_FRAME || answer > 0xFFFFFFFFu) {
        return 0;
    }
    memset(&item, 0, sizeof(item));
    item.name = "CODE";
    item.flags = 0x0000u;
    item.code = 0x0700u;
    item.request = request;
    item.param_a = (uint32_t)answer;
    item.param_b = (uint32_t)index;
    item.flag = 1u;
    return ncl_syntec_item_frame(out, cap, &item, (uint32_t)index, serial);
}

size_t ncl_syntec_zone_frame(uint8_t *out, size_t cap, unsigned zone,
                             size_t count, uint8_t serial)
{
    /* The zone read is the §3.1 frame shape with A/B doing the work:
     * A = 4 + 2*count (the answer's byte count), B = the zone number. */
    return syntec_code_frame(out, cap, NCL_SYNTEC_CODE_STATE_GET, zone,
                             count * 2u, serial);
}

size_t ncl_syntec_param_frame(uint8_t *out, size_t cap, unsigned param,
                              uint8_t serial)
{
    /* §11.4：一个参数是一个 i32，所以 A = 4 + 4，B = 参数号。 */
    return syntec_code_frame(out, cap, NCL_SYNTEC_CODE_PARAM_GET, param,
                             sizeof(int32_t), serial);
}

size_t ncl_syntec_param_capacity_frame(uint8_t *out, size_t cap, uint8_t serial)
{
    /* In 是空的（0 字节），应答正文是一个 i32：A = 4 + 4 = 8，B 用不上（0）。 */
    return syntec_code_frame(out, cap, NCL_SYNTEC_CODE_PARAM_CAPACITY, 0u,
                             sizeof(int32_t), serial);
}

size_t ncl_syntec_param_schema_frame(uint8_t *out, size_t cap, size_t count,
                                     uint8_t serial)
{
    /* In 是 { nLength }（4 字节），Out 是 count 条 TParamSpec。 */
    if (count > 0x00FFFFFFu) {
        return 0;
    }
    return syntec_code_frame(out, cap, NCL_SYNTEC_CODE_PARAM_SCHEMA,
                             (unsigned)count,
                             count * NCL_SYNTEC_PARAM_SPEC_SIZE, serial);
}

bool ncl_syntec_reply_i32(const uint8_t *frame, size_t len, int32_t *value)
{
    if (frame == NULL || value == NULL || len < NCL_SYNTEC_REPLY_BODY + 4u) {
        return false;
    }
    *value = (int32_t)get_u32(frame + NCL_SYNTEC_REPLY_BODY);
    return true;
}

bool ncl_syntec_item_empty(const uint8_t *frame, size_t len)
{
    return frame != NULL && len <= NCL_SYNTEC_REPLY_BODY;
}

bool ncl_syntec_item_text(const uint8_t *frame, size_t len, char *out,
                          size_t cap)
{
    size_t chars;

    if (frame == NULL || out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (len <= NCL_SYNTEC_REPLY_BODY) {
        return true;
    }
    chars = len - NCL_SYNTEC_REPLY_BODY;
    if (chars > cap - 1u) {
        chars = cap - 1u;
    }
    memcpy(out, frame + NCL_SYNTEC_REPLY_BODY, chars);
    out[chars] = '\0';
    /* Text arrives with its terminator (and sometimes blank padding). */
    while (chars > 0 && (out[chars - 1] == '\0' || out[chars - 1] == '\r' ||
                         out[chars - 1] == '\n' || out[chars - 1] == ' ')) {
        out[--chars] = '\0';
    }
    return true;
}
