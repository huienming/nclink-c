/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - HEIDENHAIN LSV2, the byte level (protocal/docs/
 * 07-HEIDENHAIN-LSV2.md).
 *
 * The friendliest framing of the whole set: a four byte big endian payload
 * length, a four character command name, then the payload. Requests and
 * responses have the same shape, so one builder and one splitter cover both.
 *
 * Everything here is a pure function. §7's traps are handled: the length is big
 * endian (the opposite of the PLC protocols), a string payload includes its NUL
 * in the length, and a long transfer is a loop of S_FL blocks rather than one
 * big frame.
 */
#ifndef NCL_LSV2_H
#define NCL_LSV2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_json.h"
#include "nclink/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================ 帧 ====== */

/** Length + command + payload. Returns the frame length, or 0 when it cannot. */
size_t ncl_lsv2_frame(uint8_t *out, size_t cap, const char *command,
                      const void *payload, size_t payload_len);

/** Split one frame: @p name receives the four characters, NUL terminated. */
ncl_err ncl_lsv2_split(const uint8_t *frame, size_t len, char name[5],
                       const uint8_t **payload, size_t *payload_len,
                       size_t *frame_len);

/** A payload of a NUL terminated string, length included (§7.2). */
size_t ncl_lsv2_string_payload(char *out, size_t cap, const char *text);

/* ================================================================ 表 ====== */

/** True for the 45 commands and 23 responses of §4. */
bool ncl_lsv2_command_known(const char *name);
/** Human readable name of a command or response, or NULL. */
const char *ncl_lsv2_command_text(const char *name);

/** "OK" / "TIMEOUT" / ... for the status codes of §6 (0 is success). */
const char *ncl_lsv2_status_text(int code);
/** Status code -> tiered error. A timeout is a transport problem (§6). */
ncl_err ncl_lsv2_check_status(int code, char *message, size_t message_len);

/* ============================================================== 数据模型 = */

/** Memory types of §5.1, as used by R_MB / R_PR. */
bool ncl_lsv2_memory_type(const char *name, unsigned *code);

/** Execution state of §5.3 and program state of §5.4, by value. */
const char *ncl_lsv2_exec_state(unsigned value);
const char *ncl_lsv2_pgm_state(unsigned value);

/** The R_MB payload: a four byte address and one length byte. */
size_t ncl_lsv2_read_memory_payload(uint8_t *out, size_t cap, int64_t address,
                                    unsigned count);

/**
 * Decode a S_MB payload into one value. The bytes are taken as written (big
 * endian within the field); §7 does not state the byte order of the memory
 * area, so this is the reading to confirm on a machine.
 */
ncl_err ncl_lsv2_decode_memory(const uint8_t *payload, size_t len,
                               ncl_dtype dtype, size_t text_chars, ncl_json **out);

#ifdef __cplusplus
}
#endif

#endif /* NCL_LSV2_H */
