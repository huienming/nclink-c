/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Mitsubishi MC / SLMP, the byte level (protocal/docs/
 * 06-MITSUBISHI-PLC-MC-SLMP.md).
 *
 * The 3E and 4E binary frames of the Q/L/iQ-R/FX5U families: header, command,
 * device specification, the end code and the data conversion. Everything is a
 * pure function over byte buffers, so the golden frames in the test suite stand
 * in for a PLC.
 *
 * Byte order: the binary frame is little endian throughout - header fields,
 * device addresses and the data words. Bit devices travel one point per byte
 * (0x00 / 0x01).
 *
 * Not implemented yet, and deliberately so: the ASCII encoding (its byte level
 * has no primary sample in the spec book, so it waits for a capture) and UDP
 * (the core's socket layer is TCP only).
 */
#ifndef NCL_MC_H
#define NCL_MC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_json.h"
#include "nclink/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================== devices == */

typedef enum {
    NCL_MC_BIT = 0,
    NCL_MC_WORD
} ncl_mc_unit;

/** Device code and unit of @p name ("D", "M", "X", "W", "TN", ...). */
bool ncl_mc_device_lookup(const char *name, uint8_t *code, ncl_mc_unit *unit);
/** Name of a device code, or NULL when unknown. */
const char *ncl_mc_device_name(uint8_t code);

/* =============================================================== headers == */

/** 3E (QnA compatible) or 4E (iQ-R, carries a serial number). */
typedef enum {
    NCL_MC_FRAME_3E = 0,
    NCL_MC_FRAME_4E
} ncl_mc_frame_type;

/** The fixed part of every request. */
typedef struct {
    ncl_mc_frame_type frame;
    uint8_t           network; /**< 0x00                      */
    uint8_t           plc;     /**< 0xFF = own station        */
    uint16_t          module;  /**< 0x03FF                    */
    uint8_t           station; /**< 0x00                      */
    uint16_t          timer;   /**< 0x0010 = 4 s              */
    uint16_t          serial;  /**< 4E only, echoed back      */
} ncl_mc_header;

/** Fill @p header with the usual values (network 0, station 0, 4 s timer). */
void ncl_mc_header_default(ncl_mc_header *header, ncl_mc_frame_type frame);

/* ================================================================ frames == */

/* Command codes of the functions this driver uses. */
#define NCL_MC_CMD_BATCH_READ 0x0401
#define NCL_MC_CMD_BATCH_WRITE 0x1401
#define NCL_MC_CMD_RANDOM_READ 0x0403
#define NCL_MC_CMD_LOOPBACK 0x0619
#define NCL_MC_CMD_CLEAR_ERROR 0x0611
#define NCL_MC_CMD_REMOTE_RUN 0x1001
#define NCL_MC_CMD_REMOTE_STOP 0x1002
#define NCL_MC_CMD_CPU_TYPE 0x1005
#define NCL_MC_CMD_CPU_STATUS 0x0101

/* Sub commands: word and bit units of the batch commands. */
#define NCL_MC_SUB_WORD 0x0000
#define NCL_MC_SUB_BIT 0x0001

/** Bytes the wire address of @p number / @p bit occupies in the device spec. */
uint32_t ncl_mc_wire_address(uint32_t number, int bit, bool word_device);

/**
 * Device specification of a batch request (binary): device code, wire address
 * (little endian) and the point count (little endian).
 */
size_t ncl_mc_device_spec(uint8_t *out, size_t cap, uint8_t device_code,
                          uint32_t address, uint16_t points);

/** Whole request frame: header + command + sub command + data. */
size_t ncl_mc_request(uint8_t *out, size_t cap, const ncl_mc_header *header,
                      uint16_t command, uint16_t subcommand, const uint8_t *data,
                      size_t data_len);

/** Bytes a request of @p data_len payload bytes occupies on the wire. */
size_t ncl_mc_request_size(ncl_mc_frame_type frame, size_t data_len);

/** The parsed part of a reply. */
typedef struct {
    uint16_t       end_code;
    const uint8_t *data;     /**< borrowed from the frame */
    size_t         data_len;
} ncl_mc_reply;

/**
 * Split one complete reply. Answers NCL_ERR_RANGE while @p len holds less than
 * the frame announces, so a caller can read the header first and the rest next.
 */
ncl_err ncl_mc_split_reply(const uint8_t *frame, size_t len,
                           ncl_mc_frame_type type, uint16_t want_serial,
                           ncl_mc_reply *out, size_t *frame_len);

/** End code -> tiered error. NCL_OK for 0x0000. */
ncl_err ncl_mc_check_end_code(uint16_t code, char *message, size_t message_len);

/* ================================================================= data === */

/** Bytes one element of @p dtype occupies (@p text_chars for a string). */
size_t ncl_mc_element_bytes(ncl_dtype dtype, size_t text_chars);

/** Decode one element at byte offset @p offset of a reply payload. */
ncl_err ncl_mc_decode(const uint8_t *data, size_t data_len, size_t offset,
                      ncl_dtype dtype, size_t text_chars, ncl_json **out);

/** Encode one element into @p out; *out_len receives the byte count. */
ncl_err ncl_mc_encode(const ncl_json *value, ncl_dtype dtype, size_t text_chars,
                      uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MC_H */
