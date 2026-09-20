/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - Siemens S7comm over ISO-on-TCP, the byte level
 * (protocal/docs/03-SIEMENS-S7-PLC.md).
 *
 * Three layers, each with its own length field, which is the classic source of
 * confusion (§7.4):
 *
 *   TPKT     4 bytes: version, reserved, total length (includes the header)
 *   COTP     connection request/confirm and the data transfer header
 *   S7comm   10 byte header (protocol 0x32) + parameter area + data area
 *
 * Everything is big endian, and every function here is pure so the golden
 * frames of §5 and the test suite's own PLC stand in for hardware.
 */
#ifndef NCL_S7_H
#define NCL_S7_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_json.h"
#include "nclink_adapter/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================ TPKT === */

/** TPKT header + payload; the length covers both. */
size_t ncl_s7_tpkt(uint8_t *out, size_t cap, const uint8_t *payload,
                   size_t payload_len);

/** Validate a TPKT frame and point at its payload. */
ncl_err ncl_s7_tpkt_split(const uint8_t *frame, size_t len,
                          const uint8_t **payload, size_t *payload_len,
                          size_t *frame_len);

/* ================================================================ COTP === */

#define NCL_S7_COTP_CR 0xE0u /**< connection request  */
#define NCL_S7_COTP_CC 0xD0u /**< connection confirm  */
#define NCL_S7_COTP_DR 0x80u /**< disconnect request  */
#define NCL_S7_COTP_DT 0xF0u /**< data transfer       */

/* PDU size codes of the COTP "negotiation" parameter (§2). */
#define NCL_S7_PDU_128 0x07u
#define NCL_S7_PDU_256 0x08u
#define NCL_S7_PDU_512 0x09u
#define NCL_S7_PDU_1024 0x0Au
#define NCL_S7_PDU_2048 0x0Bu
#define NCL_S7_PDU_4096 0x0Cu
#define NCL_S7_PDU_8192 0x0Du

/** TSAP of rack/slot: S7-300/400 use slot 2, S7-1200/1500 slot 1 (§7.6). */
uint16_t ncl_s7_tsap(unsigned rack, unsigned slot);

/** COTP connection request wrapped in TPKT. */
size_t ncl_s7_cotp_cr(uint8_t *out, size_t cap, uint16_t calling_tsap,
                      uint16_t called_tsap, uint8_t pdu_size_code);

/**
 * Split the COTP section of a frame (the bytes ncl_s7_tpkt_split() hands back,
 * i.e. CR, CC or DT without the TPKT header). *pdu_size_code receives the value
 * of the 0xC0 parameter (0 when the frame does not carry one), *payload the
 * bytes after the COTP header - the S7 PDU of a DT frame.
 */
ncl_err ncl_s7_cotp_split(const uint8_t *frame, size_t len, uint8_t *pdu_type,
                          uint8_t *pdu_size_code, const uint8_t **payload,
                          size_t *payload_len, size_t *frame_len);

/** Wrap an S7 PDU in TPKT + COTP data transfer. */
size_t ncl_s7_cotp_dt(uint8_t *out, size_t cap, const uint8_t *s7, size_t s7_len);

/* ================================================================ S7comm == */

#define NCL_S7_PROTOCOL_ID 0x32u

#define NCL_S7_ROSCTR_JOB 0x01u
#define NCL_S7_ROSCTR_ACK 0x02u
#define NCL_S7_ROSCTR_ACK_DATA 0x03u
#define NCL_S7_ROSCTR_USERDATA 0x07u

#define NCL_S7_FUNC_READ_VAR 0x04u
#define NCL_S7_FUNC_WRITE_VAR 0x05u
#define NCL_S7_FUNC_SETUP_COMM 0xF0u
#define NCL_S7_FUNC_READ_SZL 0x1Cu
#define NCL_S7_FUNC_PLC_STOP 0x29u

/* Transport sizes of the S7ANY item (§3.4). */
#define NCL_S7_TS_BIT 0x01u
#define NCL_S7_TS_BYTE 0x02u
#define NCL_S7_TS_CHAR 0x03u
#define NCL_S7_TS_WORD 0x04u
#define NCL_S7_TS_INT 0x05u
#define NCL_S7_TS_DWORD 0x06u
#define NCL_S7_TS_DINT 0x07u
#define NCL_S7_TS_REAL 0x08u
/* The reply uses the byte oriented sizes instead. */
#define NCL_S7_TS_REPLY_BIT 0x03u
#define NCL_S7_TS_REPLY_BYTE 0x04u

/* Areas (§4). */
#define NCL_S7_AREA_I 0x81u
#define NCL_S7_AREA_Q 0x82u
#define NCL_S7_AREA_M 0x83u
#define NCL_S7_AREA_DB 0x84u
#define NCL_S7_AREA_C 0x1Cu
#define NCL_S7_AREA_T 0x1Du

/** S7 header + parameter area + data area. */
size_t ncl_s7_pdu(uint8_t *out, size_t cap, uint8_t rosctr, uint16_t pdu_ref,
                  const uint8_t *params, size_t params_len, const uint8_t *data,
                  size_t data_len);

/** The parts of a reply PDU. */
typedef struct {
    uint8_t        rosctr;
    uint16_t       pdu_ref;
    uint16_t       error_class;
    uint16_t       error_code;
    const uint8_t *params; /**< borrowed */
    size_t         params_len;
    const uint8_t *data; /**< borrowed */
    size_t         data_len;
} ncl_s7_view;

/** Split a reply PDU; answers NCL_ERR_RANGE while it is still arriving. */
ncl_err ncl_s7_pdu_split(const uint8_t *pdu, size_t len, ncl_s7_view *out,
                         size_t *pdu_len);

/** Setup Communication parameters (function 0xF0). */
size_t ncl_s7_setup_params(uint8_t *out, size_t cap, uint16_t max_amq_calling,
                           uint16_t max_amq_called, uint16_t pdu_size);

/** Read the negotiated PDU size out of a Setup Communication reply. */
ncl_err ncl_s7_setup_reply(const uint8_t *params, size_t params_len,
                           uint16_t *pdu_size);

/* ------------------------------------------------------------ variables -- */

/** One S7ANY variable reference. */
typedef struct {
    uint8_t  area;      /**< 0x81 I, 0x82 Q, 0x83 M, 0x84 DB (§4)     */
    uint16_t db;        /**< DB number, 0 for the other areas         */
    uint32_t bit;       /**< byte offset * 8 + bit number             */
    uint8_t  tsize;     /**< transport size, e.g. NCL_S7_TS_REAL      */
    uint16_t elements;  /**< elements, or bits for a bit access       */
} ncl_s7_item;

/** The parameter area of a Read/Write Var: function, count, then the items. */
size_t ncl_s7_var_params(uint8_t *out, size_t cap, uint8_t function,
                         const ncl_s7_item *items, size_t count);

/** Bytes an item's parameter block occupies (12). */
#define NCL_S7_ITEM_BYTES 12u

/** The data area of a Write Var: one block per item. */
size_t ncl_s7_write_data(uint8_t *out, size_t cap, uint8_t tsize, uint16_t bits,
                         const uint8_t *bytes, size_t byte_len);

/** The data returned for one item of a Read Var reply. */
typedef struct {
    uint8_t        return_code; /**< 0xFF is success                     */
    uint8_t        tsize;
    uint16_t       bits; /**< the reply counts bits, not bytes          */
    const uint8_t *bytes;
    size_t         byte_len;
} ncl_s7_reply_item;

/**
 * Walk the data area of a Read Var reply: @p count items, @p items receives
 * one entry each. A failed item keeps its error class in return_code and has
 * no data; the caller decides how to report it.
 */
ncl_err ncl_s7_read_reply(const uint8_t *data, size_t data_len, size_t count,
                          ncl_s7_reply_item *items, size_t *items_filled);

/** Return code of a data item -> tiered error with a readable message. */
ncl_err ncl_s7_check_return_code(uint8_t code, char *message, size_t message_len);

/** The PLC's own error class/code of the PDU header (§6). */
ncl_err ncl_s7_check_pdu_error(uint16_t error_class, uint16_t error_code,
                               char *message, size_t message_len);

/* ================================================================= data === */

/** Transport size and element count for @p address, or false when unusable. */
bool ncl_s7_item_for(const ncl_address *address, ncl_s7_item *item,
                     size_t *byte_len);

/**
 * The type @p address is read and written as: a width suffix in the area name
 * ("MB"/"MW"/"MD"/"MX") wins over what the point map declared, because writing
 * it is what the suffix means. A bit index always means a bit.
 */
ncl_dtype ncl_s7_effective_dtype(const ncl_address *address);

/** Bytes @p elements of @p dtype occupy on the wire (a bit is one byte). */
size_t ncl_s7_image_bytes(ncl_dtype dtype, size_t elements);

/**
 * Area code and DB number of an area name ("I", "M", "DB1", "MB"...).
 * *implied receives the type a width suffix names ("MB" a byte, "MW" a word,
 * "MD" a dword, "MX" a bit) or -1 when the name carries no suffix.
 */
bool ncl_s7_area_lookup(const char *name, uint8_t *area, uint16_t *db,
                        ncl_dtype *implied);

/** Decode one element (big endian; a string carries S7's two header bytes). */
ncl_err ncl_s7_decode(const uint8_t *data, size_t data_len, size_t offset,
                      ncl_dtype dtype, size_t text_chars, ncl_json **out);

/** Encode one element into @p out; *out_len receives the byte count. */
ncl_err ncl_s7_encode(const ncl_json *value, ncl_dtype dtype, size_t text_chars,
                      uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* NCL_S7_H */
