/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Mitsubishi CNC M70/M80 (MELDAS / GIOP), the byte level
 * (protocal/docs/05-MITSUBISHI-CNC-M70-MELDAS.md).
 *
 * The private "mocha*" operation set of the M700 series rides on CORBA GIOP
 * 1.0: a request is exactly 80 bytes, little endian throughout (the GIOP flags
 * byte says so), and the reply is matched by request id. Everything here is a
 * pure function, so the frame of §6.1 and the test suite's own machine stand in
 * for hardware.
 *
 * The three details §8 warns about are baked into this file: the length field
 * is "total - 12", the operation name is 13 bytes including its NUL and is
 * followed by a fixed 00 00 00 03, and a CString reply keeps its length at [36]
 * with the text starting at [40].
 */
#ifndef NCL_MELDAS_H
#define NCL_MELDAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_json.h"
#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================ 请求 ==== */

#define NCL_MELDAS_GET_DATA "mochaGetData"
#define NCL_MELDAS_SET_DATA "mochaSetData"

/** One request is this long, always (§3.1). */
#define NCL_MELDAS_REQUEST_BYTES 80u

/** The four fields that decide what a request means. */
typedef struct {
    uint32_t command;  /**< §4 command code, e.g. 0x25              */
    uint32_t subcode;  /**< §4 sub code, the rest of the meaning    */
    uint32_t count;    /**< elements to read, usually 1             */
    uint32_t address;  /**< axis number or I/O address (§5)         */
    uint32_t want;     /**< expected reply type (§3.3)              */
} ncl_meldas_request;

/* The expected reply type, and the type marker a reply carries. */
#define NCL_MELDAS_TYPE_BYTE 0x01u
#define NCL_MELDAS_TYPE_INT16 0x02u
#define NCL_MELDAS_TYPE_INT32 0x03u
#define NCL_MELDAS_TYPE_DOUBLE 0x05u
#define NCL_MELDAS_TYPE_DOUBLE10 0x06u
#define NCL_MELDAS_TYPE_STRING 0x10u
#define NCL_MELDAS_TYPE_STRING188 0xBCu

/** Build one request frame. Returns 80, or 0 when the operation is unusable. */
size_t ncl_meldas_build_request(uint8_t *out, size_t cap, uint32_t request_id,
                                const char *operation,
                                const ncl_meldas_request *fields);

/* ================================================================ 响应 ==== */

/** What a reply turned out to hold. */
typedef struct {
    uint8_t   type;    /**< the marker at [28], 0 when there is no data */
    bool      present; /**< false for the "IDL" no data answer (§3.2)    */
    long long integer; /**< BYTE / INT16 / INT32                        */
    double    real;    /**< DOUBLE                                      */
    char      text[256]; /**< CString                                   */
} ncl_meldas_value;

/**
 * Check the reply header, its request id and its data, as the delivered
 * CheckFrame does (§6.2). Answers NCL_ERR_RANGE while the reply is short.
 */
ncl_err ncl_meldas_parse_reply(const uint8_t *reply, size_t len,
                               uint32_t want_id, ncl_meldas_value *out,
                               char *err, size_t err_len);

/** The parsed value as JSON: a number, a string, or null for "no data". */
ncl_json *ncl_meldas_value_to_json(const ncl_meldas_value *value,
                                   ncl_dtype dtype);

/** Decode the 10 byte extended double of §3.3 (x87 layout, see the header). */
double ncl_meldas_double10(const uint8_t *bytes);

/* ================================================================ 语义 ==== */

/**
 * The named commands of §4: "machine_position", "spindle_load", ... and the raw
 * form "0x25/2". Answers false for a name this table does not know.
 */
bool ncl_meldas_command_lookup(const char *name, uint32_t *command,
                               uint32_t *subcode, uint32_t *want,
                               const char **canonical);

/** True for the commands whose address field names an axis (§4, §5). */
bool ncl_meldas_command_uses_axis(uint32_t command, uint32_t subcode);

/**
 * The wire value of an axis: §5 notes the C# delivery uses bit coding (X=1,
 * Y=2, Z=4) while the C++ samples number the axes 1..n. @p bit_mode picks the
 * bit coding, and @p axis is 1 based either way.
 */
uint32_t ncl_meldas_axis(unsigned axis, bool bit_mode);

/** The type to ask for, and to expect, from a point's declared type. */
uint32_t ncl_meldas_want_for(ncl_dtype dtype);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MELDAS_H */
