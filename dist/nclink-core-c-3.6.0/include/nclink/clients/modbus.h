/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Modbus framing and data conversion (15-MODBUS.md).
 *
 * The byte level of Modbus, kept apart from the socket so it can be tested
 * against golden frames without a device: MBAP framing for TCP, the CRC frame
 * for RTU, the PDU bodies of the read/write functions, the exception codes and
 * the register/byte order of multi-register values.
 *
 * Everything here is a pure function - no sockets, no state.
 */
#ifndef NCL_MODBUS_H
#define NCL_MODBUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_json.h"
#include "nclink/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================= areas == */

/** The four data areas of Modbus, by their traditional names. */
typedef enum {
    NCL_MB_COIL = 0,      /**< 0x, coils, read/write bits            */
    NCL_MB_DISCRETE,      /**< 1x, discrete inputs, read only bits   */
    NCL_MB_INPUT,         /**< 3x, input registers, read only words  */
    NCL_MB_HOLDING        /**< 4x, holding registers, read/write     */
} ncl_modbus_area;

/** "coil"/"0x", "discrete"/"1x", "input"/"3x", "holding"/"4x". */
bool ncl_modbus_area_parse(const char *text, ncl_modbus_area *out);
const char *ncl_modbus_area_name(ncl_modbus_area area);
bool ncl_modbus_area_is_writable(ncl_modbus_area area);
/** Read function code: 1, 2, 4 or 3. */
uint8_t ncl_modbus_area_read_code(ncl_modbus_area area);
/** How many elements one read may ask for (2000 bits, 125 registers). */
unsigned ncl_modbus_area_max_read(ncl_modbus_area area);
/** How many elements one write may carry (1968 bits, 123 registers). */
unsigned ncl_modbus_area_max_write(ncl_modbus_area area);

/* ============================================================ byte order == */

/**
 * How a value wider than a register is laid out over its registers. The
 * letters are the bytes of the value, most significant first: ABCD is the
 * natural order, CDAB the common "word swap" of PLC manuals.
 */
typedef enum {
    NCL_MB_ORDER_ABCD = 0,
    NCL_MB_ORDER_CDAB,
    NCL_MB_ORDER_BADC,
    NCL_MB_ORDER_DCBA
} ncl_modbus_word_order;

bool ncl_modbus_word_order_parse(const char *text, ncl_modbus_word_order *out);
const char *ncl_modbus_word_order_name(ncl_modbus_word_order order);

/** Registers one element of @p dtype occupies (1, 2 or 4; 0 for a bit). */
unsigned ncl_modbus_registers_per_element(ncl_dtype dtype);

/**
 * Decode one element from @p data (the register payload of a read reply, big
 * endian words) starting at byte offset @p offset. For NCL_DTYPE_STRING the
 * element is @p text_chars characters long and @p order only decides whether
 * the two bytes of a register are swapped.
 */
ncl_err ncl_modbus_decode(const uint8_t *data, size_t data_len, size_t offset,
                          ncl_dtype dtype, size_t text_chars,
                          ncl_modbus_word_order order, ncl_json **out);

/**
 * Encode one element of @p value into @p out (big endian words, permuted by
 * @p order). *out_len receives the byte count. Returns NCL_ERR_INVALID_VALUE
 * when the JSON value does not fit the type.
 */
ncl_err ncl_modbus_encode(const ncl_json *value, ncl_dtype dtype,
                          size_t text_chars, ncl_modbus_word_order order,
                          uint8_t *out, size_t out_cap, size_t *out_len);

/* ================================================================== CRC === */

/** Modbus CRC16 (polynomial 0xA001, seed 0xFFFF). */
uint16_t ncl_modbus_crc16(const uint8_t *data, size_t len);

/* ================================================================== PDU === */

/* Function codes used by the driver. */
#define NCL_MB_FC_READ_COILS 0x01
#define NCL_MB_FC_READ_DISCRETE 0x02
#define NCL_MB_FC_READ_HOLDING 0x03
#define NCL_MB_FC_READ_INPUT 0x04
#define NCL_MB_FC_WRITE_COIL 0x05
#define NCL_MB_FC_WRITE_REGISTER 0x06
#define NCL_MB_FC_WRITE_COILS 0x0F
#define NCL_MB_FC_WRITE_REGISTERS 0x10

/** Read request: function, start address, element count. */
size_t ncl_modbus_read_pdu(uint8_t *out, size_t cap, uint8_t function,
                           uint16_t address, uint16_t count);
/** Write single coil (0xFF00/0x0000) or single register. */
size_t ncl_modbus_write_single_pdu(uint8_t *out, size_t cap, uint8_t function,
                                   uint16_t address, uint16_t value);
/** Write multiple coils (packed bits) or multiple registers (big endian). */
size_t ncl_modbus_write_multi_pdu(uint8_t *out, size_t cap, uint8_t function,
                                  uint16_t address, uint16_t count,
                                  const uint8_t *data, size_t data_len);

/**
 * Turn an exception reply (@p function | 0x80) into a tiered error and a
 * readable message. Returns NCL_OK when the reply is not an exception.
 */
ncl_err ncl_modbus_check_exception(const uint8_t *pdu, size_t pdu_len,
                                   uint8_t function, char *message,
                                   size_t message_len);

/** Validate a read reply and point at its register payload. */
ncl_err ncl_modbus_check_read_reply(const uint8_t *pdu, size_t pdu_len,
                                    uint8_t function, size_t expect_bytes,
                                    const uint8_t **data, char *message,
                                    size_t message_len);

/** Validate the echo of a write reply (start address + count). */
ncl_err ncl_modbus_check_write_reply(const uint8_t *pdu, size_t pdu_len,
                                     uint8_t function, uint16_t address,
                                     uint16_t count, char *message,
                                     size_t message_len);

/* ============================================================ transport == */

/** MBAP header + PDU. Returns the frame length, or 0 when @p cap is too small. */
size_t ncl_modbus_tcp_frame(uint8_t *out, size_t cap, uint16_t transaction,
                            uint8_t unit, const uint8_t *pdu, size_t pdu_len);

/**
 * Validate one complete MBAP frame and split it: @p pdu points *into* @p frame
 * and @p frame_len receives the whole frame length.
 */
ncl_err ncl_modbus_tcp_split(const uint8_t *frame, size_t len,
                             uint16_t want_transaction, const uint8_t **pdu,
                             size_t *pdu_len, size_t *frame_len);

/** Unit address + PDU + CRC (low byte first). */
size_t ncl_modbus_rtu_frame(uint8_t *out, size_t cap, uint8_t unit,
                            const uint8_t *pdu, size_t pdu_len);

/** Validate one complete RTU frame (unit + CRC) and split it. */
ncl_err ncl_modbus_rtu_split(const uint8_t *frame, size_t len, uint8_t want_unit,
                             const uint8_t **pdu, size_t *pdu_len,
                             size_t *frame_len);

/**
 * Size of the RTU reply whose first @p head_len bytes are @p head: RTU has no
 * length field, so the reply is read in two steps (head, then the rest).
 * *total is 0 while @p head does not yet say how long the reply is.
 */
ncl_err ncl_modbus_rtu_reply_size(const uint8_t *head, size_t head_len,
                                  size_t *total);

#ifdef __cplusplus
}
#endif

#endif /* NCL_MODBUS_H */
