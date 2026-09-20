/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Omron FINS, the byte level (protocal/docs/12-OMRON-FINS.md).
 *
 * FINS/TCP: the "FINS" transport header, the node address allocation handshake
 * a TCP link must do before anything else, the FINS frame itself, the memory
 * area commands and the end code table. All of it is big endian.
 *
 * Every function here is pure, so the golden frames of the spec book (§6) and
 * the test suite's own PLC stand in for hardware.
 *
 * Not implemented yet: FINS/UDP, HostLink and C-Mode (the core's socket layer
 * is TCP only, and the serial link is a separate framing on top of it).
 */
#ifndef NCL_FINS_H
#define NCL_FINS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_json.h"
#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================ areas == */

/** Area code of @p name ("CIO", "W", "H", "A", "D", "P", "C", "T", "E0_0"...). */
bool ncl_fins_area_lookup(const char *name, uint8_t *code);
/** Name of an area code, or NULL when unknown. */
const char *ncl_fins_area_name(uint8_t code);
/** True for the areas that are addressed in bits (CIO/W/H/A/TS/CS/...). */
bool ncl_fins_area_is_bit(uint8_t code);

/* =============================================================== frames == */

/* FINS/TCP commands (the header's command field). */
#define NCL_FINS_TCP_NODE_ADDRESS 0x00000000u
#define NCL_FINS_TCP_DATA_SEND 0x00000001u
#define NCL_FINS_TCP_DATA_RECEIVE 0x00000002u
#define NCL_FINS_TCP_ERROR 0x00000003u
#define NCL_FINS_TCP_TERMINATE 0x00000004u

/* MRC/SRC command pairs. */
#define NCL_FINS_CMD_MEMORY_READ 0x0101u
#define NCL_FINS_CMD_MEMORY_WRITE 0x0102u
#define NCL_FINS_CMD_RUN 0x0501u
#define NCL_FINS_CMD_STOP 0x0502u
#define NCL_FINS_CMD_CONTROLLER_STATUS 0x0601u
#define NCL_FINS_CMD_READ_CLOCK 0x0701u
#define NCL_FINS_CMD_CYCLE_TIME 0x2201u

/** "FINS" + length + command + error code + payload. */
size_t ncl_fins_tcp_frame(uint8_t *out, size_t cap, uint32_t command,
                          const uint8_t *payload, size_t payload_len);

/** Split one transport frame; @p payload points into @p frame. */
ncl_err ncl_fins_tcp_split(const uint8_t *frame, size_t len, uint32_t *command,
                           uint32_t *error, const uint8_t **payload,
                           size_t *payload_len, size_t *frame_len);

/**
 * The node address allocation request (§2). A TCP link sends it once, right
 * after connecting, and the PLC answers with the node numbers the FINS frames
 * have to carry.
 */
size_t ncl_fins_node_request(uint8_t *out, size_t cap, uint32_t client_node);

/** Read the two node numbers out of an allocation reply. */
ncl_err ncl_fins_node_response(const uint8_t *payload, size_t len,
                               uint8_t *client_node, uint8_t *server_node);

/** The ten fixed bytes of a FINS frame. */
typedef struct {
    uint8_t icf; /**< 0x80 needs a response                     */
    uint8_t rsv; /**< 0x00                                      */
    uint8_t gct; /**< 0x02 is the common value                  */
    uint8_t dna; /**< destination network, 0x00 = local         */
    uint8_t da1; /**< destination node (the PLC)                */
    uint8_t da2; /**< destination unit, 0x00 = CPU              */
    uint8_t sna; /**< source network                            */
    uint8_t sa1; /**< source node (this client)                 */
    uint8_t sa2; /**< source unit, 0x00                         */
    uint8_t sid; /**< service id, echoed back                   */
} ncl_fins_header;

void ncl_fins_header_default(ncl_fins_header *header);

/** Header + MRC/SRC + data. */
size_t ncl_fins_frame(uint8_t *out, size_t cap, const ncl_fins_header *header,
                      uint16_t command, const uint8_t *data, size_t data_len);

/** Split one FINS frame; @p data points into @p frame. */
ncl_err ncl_fins_split(const uint8_t *frame, size_t len, ncl_fins_header *header,
                       uint16_t *command, const uint8_t **data,
                       size_t *data_len, size_t *frame_len);

/* ============================================================= commands == */

/** Memory area read: area, address, bit (0x00 for a word), count. */
size_t ncl_fins_read_body(uint8_t *out, size_t cap, uint8_t area,
                          uint16_t address, uint8_t bit, uint16_t count);

/** Memory area write: as read, followed by the data. */
size_t ncl_fins_write_body(uint8_t *out, size_t cap, uint8_t area,
                           uint16_t address, uint8_t bit, uint16_t count,
                           const uint8_t *data, size_t data_len);

/**
 * Read the three reply bytes after the FINS header: the end code, MRES and
 * SRES. Answers NCL_ERR_RANGE while they have not all arrived, and the tiered
 * error when the PLC refused the command.
 */
ncl_err ncl_fins_reply_begin(const uint8_t *data, size_t data_len,
                             const uint8_t **payload, size_t *payload_len,
                             char *message, size_t message_len);

/** End code -> tiered error. NCL_OK for 0x00. */
ncl_err ncl_fins_check_end_code(uint8_t code, uint8_t detail, char *message,
                                size_t message_len);

/* ================================================================= data === */

/** Bytes one element of @p dtype occupies (@p text_chars for a string). */
size_t ncl_fins_element_bytes(ncl_dtype dtype, size_t text_chars);

/** Decode one element (big endian words, bits one byte each). */
ncl_err ncl_fins_decode(const uint8_t *data, size_t data_len, size_t offset,
                        ncl_dtype dtype, size_t text_chars, ncl_json **out);

/** Encode one element into @p out; *out_len receives the byte count. */
ncl_err ncl_fins_encode(const ncl_json *value, ncl_dtype dtype, size_t text_chars,
                        uint8_t *out, size_t out_cap, size_t *out_len);

/** Bytes sent for one element of @p address (a bit is one byte). */
size_t ncl_fins_wire_bytes(const ncl_address *address, bool bit_access);

#ifdef __cplusplus
}
#endif

#endif /* NCL_FINS_H */
