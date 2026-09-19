/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - SYNTEC RemoteCNC, the packet layer.
 *
 * Everything here comes from protocal/docs/10-SYNTEC-新代-RemoteCNC.md §10,
 * which is itself read out of the delivered assemblies: the controller side
 * (OCAPIServer_WinCE.exe) marshals its request structures straight onto the
 * wire (`ByteArrayToStructure` / `StructureToByteArray`), so the packet is a
 * C structure laid out with the .NET default rules.
 *
 *   packet   12 bytes : Length u4 | CmdID u2 | (2 pad) | Reserved u4
 *   function  8 bytes : uFuncID u2 | uSerial u1 | Reserved u1 | IHeader u4
 *   body     N bytes  : the function's own request structure
 *
 * `Length` counts the bytes after the 12 byte header - `ReceivePackets` in the
 * server reads the header with a 12 byte `BeginReceive` and then continues for
 * `Length - received` more (§10.4). All fields are little endian (the assem-
 * blies run on x86 / WinCE ARM and use BitConverter).
 *
 * The two CmdID ranges that are known so far (§10.6, §10.7):
 *   file transfer 1..17, Dipole 178/179/180 and 200 (the KrnlAPI one).
 * A CmdID the table has no name for still travels: the point map can name it
 * in the raw "178" form, which is how an undocumented command is tried.
 */
#ifndef NCL_SYNTEC_H
#define NCL_SYNTEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_json.h"
#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================== packets == */

#define NCL_SYNTEC_PACKET_HEADER 12u
#define NCL_SYNTEC_FUNCTION_HEADER 8u

/** The 12 byte packet header, as the server's CTCPCMD_PacketStart. */
typedef struct {
    uint32_t length;   /**< content bytes, header excluded */
    uint16_t cmd_id;   /**< command number (§10.7: one space for the whole box) */
    uint32_t reserved; /**< written as 0, with 2 pad bytes before it */
} ncl_syntec_packet;

/** The 8 byte function header, as CTCPFunctionCmdSend_Header. */
typedef struct {
    uint16_t func_id;  /**< uFuncID                                       */
    uint8_t  serial;   /**< uSerial, echoed back                          */
    uint32_t header;   /**< IHeader, written as 0 by the delivered client */
} ncl_syntec_function;

/**
 * Build one packet: header + function header + @p body.
 * Returns the frame length, or 0 when it does not fit.
 */
size_t ncl_syntec_build(uint8_t *out, size_t cap, uint16_t cmd_id,
                        const ncl_syntec_function *function, const void *body,
                        size_t body_len);

/** A parsed packet: the headers plus a borrowed view of the body. */
typedef struct {
    ncl_syntec_packet   packet;
    ncl_syntec_function function;
    const uint8_t      *body; /**< borrowed, may be empty */
    size_t              body_len;
} ncl_syntec_view;

/**
 * Check and split one packet. Answers NCL_ERR_RANGE while the frame is still
 * arriving (the caller reads 12 bytes, then `Length` more).
 */
ncl_err ncl_syntec_split(const uint8_t *frame, size_t len, ncl_syntec_view *out,
                         size_t *frame_len);

/* ============================================================= commands == */

/* The CmdIDs whose values are known (§10.6, §10.7). */
#define NCL_SYNTEC_CMD_FILE_FIRST 1u   /**< FileTransferCmd 1..17   */
#define NCL_SYNTEC_CMD_DIPOLE_FIRST 178u
#define NCL_SYNTEC_CMD_KRML_API 200u   /**< carries MMI_Request_KrnlAPI */

/**
 * Look a command name up: "KrnlAPI", "FileExist", "DirCreate", ... or the raw
 * decimal form ("178", "200") for a command the table does not name.
 */
bool ncl_syntec_cmd_lookup(const char *name, uint16_t *cmd_id,
                           const char **canonical);
/** Name of a known CmdID, or NULL. */
const char *ncl_syntec_cmd_name(uint16_t cmd_id);

/** The service a CmdID belongs to, for logs ("FileTransfer", "Dipole", ...). */
const char *ncl_syntec_cmd_service(uint16_t cmd_id);

/**
 * §10.10: the data codes of the client's `EDataType` and `EDevice_Type` enums
 * ("DT_PART_COUNT" 43, "DT_CNC_STATUS" 41, "DT_MACHINEPOS" 0, ...). A KrnlAPI
 * read carries one of these as its `dwCode`.
 */
bool ncl_syntec_data_code(const char *name, int32_t *code);
/** Name of a data code, or NULL. The first match wins (the two enums overlap). */
const char *ncl_syntec_data_code_name(int32_t code);

/* ================================================================= data == */

/** CRC-16 with the reversed 0xA001 polynomial, as the client library has it. */
uint16_t ncl_syntec_crc16(const uint8_t *data, size_t len);

/**
 * The MMI_Request_KrnlAPI body: uFuncID u2, dwCode i4, dwSizeIn i4,
 * dwSizeOut i4, then the input bytes.
 *
 * The pointer field of the C structure cannot travel, so the bytes it pointed
 * at follow the structure - **this placement is the one part of the packet
 * that still needs a capture to confirm** (§10.8); the header fields are read
 * straight out of the metadata.
 */
size_t ncl_syntec_krnl_body(uint8_t *out, size_t cap, uint16_t func_id,
                            int32_t code, size_t size_in, size_t size_out,
                            const void *in, size_t in_len);

/** The four fields of that body, as parsed from a reply. */
typedef struct {
    uint16_t func_id;
    int32_t  code;
    int32_t  size_in;
    int32_t  size_out;
} ncl_syntec_krnl_request;

/** Read the four fields back (the input bytes are ignored). */
ncl_err ncl_syntec_krnl_parse(const uint8_t *body, size_t len,
                              ncl_syntec_krnl_request *out);

/** A string request body: u4 length + the characters (§10.2). */
size_t ncl_syntec_path_body(uint8_t *out, size_t cap, uint16_t func_id,
                            const char *path);

#ifdef __cplusplus
}
#endif

#endif /* NCL_SYNTEC_H */
