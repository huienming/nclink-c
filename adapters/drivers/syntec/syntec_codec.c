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
 * §10.7: one numbering space for the whole controller. The file transfer
 * service starts at 1 with the declaration order of FileTransferCmd, the
 * Dipole service at 178, and 200 is the KrnlAPI command.
 */
static const struct {
    const char *name;
    uint16_t    cmd_id;
} kCommands[] = {
    {"FileSendStart", NCL_SYNTEC_CMD_FILE_FIRST},
    {"FileSending", NCL_SYNTEC_CMD_FILE_FIRST + 1u},
    {"FileRecvStart", NCL_SYNTEC_CMD_FILE_FIRST + 2u},
    {"FileRecving", NCL_SYNTEC_CMD_FILE_FIRST + 3u},
    {"GetAllFileList", NCL_SYNTEC_CMD_FILE_FIRST + 4u},
    {"Install", NCL_SYNTEC_CMD_FILE_FIRST + 5u},
    {"FileExist", NCL_SYNTEC_CMD_FILE_FIRST + 6u},
    {"DirExist", NCL_SYNTEC_CMD_FILE_FIRST + 7u},
    {"FileNew", NCL_SYNTEC_CMD_FILE_FIRST + 8u},
    {"FileDelete", NCL_SYNTEC_CMD_FILE_FIRST + 9u},
    {"FileCopy", NCL_SYNTEC_CMD_FILE_FIRST + 10u},
    {"FileMove", NCL_SYNTEC_CMD_FILE_FIRST + 11u},
    {"DirCreate", NCL_SYNTEC_CMD_FILE_FIRST + 12u},
    {"DipoleFirst", NCL_SYNTEC_CMD_DIPOLE_FIRST},
    {"DipoleSecond", NCL_SYNTEC_CMD_DIPOLE_FIRST + 1u},
    {"DipoleThird", NCL_SYNTEC_CMD_DIPOLE_FIRST + 2u},
    {"KrnlAPI", NCL_SYNTEC_CMD_KRML_API},
};

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
    if (cmd_id >= NCL_SYNTEC_CMD_FILE_FIRST && cmd_id <= 17u) {
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
