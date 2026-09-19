/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - SYNTEC RemoteCNC packet layer (see ncl_syntec.h).
 *
 * The structures are marshalled by .NET with the default layout rules, so the
 * 12 byte header really has two pad bytes between CmdID and Reserved - the
 * server's `BeginReceive(..., 12, ...)` is what pins that down (§10.4).
 */

#include "nclink_adapter/ncl_syntec.h"

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
