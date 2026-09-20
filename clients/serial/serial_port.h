/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - portable serial port (RS-232 / RS-485).
 *
 * What the modem protocols need and nothing more: open 8-N-1 style settings,
 * write a frame, read what comes back within a timeout. Frame boundaries are
 * the caller's business (Modbus RTU uses the 3.5 character silence, which the
 * read interval timeout approximates - see 15-MODBUS.md §5).
 */
#ifndef NCL_SERIAL_PORT_H
#define NCL_SERIAL_PORT_H

#include <stdbool.h>
#include <stddef.h>

#include "nclink/ncl_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ncl_serial ncl_serial;

typedef struct {
    const char *device;  /**< "COM3" on Windows, "/dev/ttyUSB0" on POSIX */
    unsigned    baud;    /**< 9600 when 0 */
    char        parity;  /**< 'N' (default), 'E', 'O' */
    unsigned    data_bits; /**< 7 or 8 (default 8) */
    unsigned    stop_bits; /**< 1 (default) or 2 */
    /**
     * Silence that ends a read, milliseconds. This is the t3.5 stand-in: a
     * read returns once the line has been quiet for this long.
     */
    unsigned inter_frame_ms;
} ncl_serial_options;

/** Fill @p options with the usual defaults (9600-8-N-1, 20 ms inter frame). */
void ncl_serial_options_default(ncl_serial_options *options);

/** Open @p options->device, or NULL with a diagnostic in @p err. */
ncl_serial *ncl_serial_open(const ncl_serial_options *options, char *err,
                            size_t err_len);
void        ncl_serial_close(ncl_serial *port);
bool        ncl_serial_is_open(const ncl_serial *port);

/** Send exactly @p len bytes. */
ncl_err ncl_serial_write(ncl_serial *port, const void *data, size_t len);

/**
 * Read up to @p len bytes, stopping when the line goes quiet for
 * inter_frame_ms or after @p timeout_ms.
 * @return bytes read (> 0), 0 when nothing arrived in time, -1 on error.
 */
int ncl_serial_read(ncl_serial *port, void *buf, size_t len, unsigned timeout_ms);

/** Drop anything buffered in either direction. */
void ncl_serial_flush(ncl_serial *port);

#ifdef __cplusplus
}
#endif

#endif /* NCL_SERIAL_PORT_H */
