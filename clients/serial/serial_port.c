/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - the serial port, Windows (COM) and POSIX (tty).
 *
 * Both sides end up with the same behaviour: a read returns as soon as the
 * line has been quiet for inter_frame_ms, which is how the RTU protocols find
 * a frame boundary without knowing its length up front.
 */

#include "serial/serial_port.h"

#include <stdio.h>
#include <string.h>

#include "nclink/ncl_platform.h"

#if defined(NCL_OS_WINDOWS)
#else
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#endif

#define SERIAL_DEFAULT_INTER_FRAME_MS 20u

struct ncl_serial {
#if defined(NCL_OS_WINDOWS)
    HANDLE handle;
#else
    int fd;
    bool open;
#endif
    unsigned inter_frame_ms;
};

void ncl_serial_options_default(ncl_serial_options *options)
{
    if (options == NULL) {
        return;
    }
    memset(options, 0, sizeof(*options));
    options->baud = 9600;
    options->parity = 'N';
    options->data_bits = 8;
    options->stop_bits = 1;
    options->inter_frame_ms = SERIAL_DEFAULT_INTER_FRAME_MS;
}

#if defined(NCL_OS_WINDOWS)

/** Device names above COM9 need the "\\\\.\\" prefix. */
static void serial_windows_name(const char *device, char *out, size_t out_len)
{
    if (ncl_str_starts_with(device, "\\\\.\\")) {
        snprintf(out, out_len, "%s", device);
        return;
    }
    snprintf(out, out_len, "\\\\.\\%s", device);
}

ncl_serial *ncl_serial_open(const ncl_serial_options *options, char *err,
                            size_t err_len)
{
    ncl_serial *port;
    char name[512];
    DCB dcb;
    COMMTIMEOUTS timeouts;

    if (options == NULL || ncl_str_is_blank(options->device)) {
        if (err != NULL) {
            snprintf(err, err_len, "no serial device given");
        }
        return NULL;
    }
    serial_windows_name(options->device, name, sizeof(name));
    port = (ncl_serial *)ncl_mem_calloc(1, sizeof(*port));
    if (port == NULL) {
        return NULL;
    }
    port->handle = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, 0, NULL);
    if (port->handle == INVALID_HANDLE_VALUE) {
        if (err != NULL) {
            snprintf(err, err_len, "cannot open %s (error %lu)", name,
                     (unsigned long)GetLastError());
        }
        ncl_mem_free(port);
        return NULL;
    }
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(port->handle, &dcb)) {
        goto fail;
    }
    dcb.BaudRate = options->baud != 0 ? options->baud : 9600;
    dcb.ByteSize = (BYTE)(options->data_bits != 0 ? options->data_bits : 8);
    dcb.StopBits = options->stop_bits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    switch (options->parity) {
    case 'E':
    case 'e':
        dcb.Parity = EVENPARITY;
        dcb.fParity = TRUE;
        break;
    case 'O':
    case 'o':
        dcb.Parity = ODDPARITY;
        dcb.fParity = TRUE;
        break;
    default:
        dcb.Parity = NOPARITY;
        dcb.fParity = FALSE;
        break;
    }
    dcb.fBinary = TRUE;
    if (!SetCommState(port->handle, &dcb)) {
        goto fail;
    }
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout =
        options->inter_frame_ms != 0 ? options->inter_frame_ms
                                     : SERIAL_DEFAULT_INTER_FRAME_MS;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1000;
    if (!SetCommTimeouts(port->handle, &timeouts)) {
        goto fail;
    }
    port->inter_frame_ms = options->inter_frame_ms != 0
                               ? options->inter_frame_ms
                               : SERIAL_DEFAULT_INTER_FRAME_MS;
    PurgeComm(port->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return port;

fail:
    if (err != NULL) {
        snprintf(err, err_len, "cannot configure %s (error %lu)", name,
                 (unsigned long)GetLastError());
    }
    CloseHandle(port->handle);
    ncl_mem_free(port);
    return NULL;
}

void ncl_serial_close(ncl_serial *port)
{
    if (port == NULL) {
        return;
    }
    if (port->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(port->handle);
    }
    ncl_mem_free(port);
}

bool ncl_serial_is_open(const ncl_serial *port)
{
    return port != NULL && port->handle != INVALID_HANDLE_VALUE;
}

ncl_err ncl_serial_write(ncl_serial *port, const void *data, size_t len)
{
    const unsigned char *cursor = (const unsigned char *)data;
    size_t sent = 0;

    if (port == NULL || data == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    while (sent < len) {
        DWORD written = 0;

        if (!WriteFile(port->handle, cursor + sent, (DWORD)(len - sent), &written,
                       NULL)) {
            return NCL_ERR_IO;
        }
        if (written == 0) {
            return NCL_ERR_IO;
        }
        sent += written;
    }
    return NCL_OK;
}

int ncl_serial_read(ncl_serial *port, void *buf, size_t len, unsigned timeout_ms)
{
    COMMTIMEOUTS timeouts;
    DWORD got = 0;

    if (port == NULL || buf == NULL || len == 0) {
        return -1;
    }
    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = port->inter_frame_ms;
    timeouts.ReadTotalTimeoutConstant = timeout_ms;
    if (!SetCommTimeouts(port->handle, &timeouts)) {
        return -1;
    }
    if (!ReadFile(port->handle, buf, (DWORD)len, &got, NULL)) {
        return -1;
    }
    return (int)got;
}

void ncl_serial_flush(ncl_serial *port)
{
    if (port != NULL) {
        PurgeComm(port->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    }
}

#else /* POSIX */

static speed_t serial_speed(unsigned baud)
{
    switch (baud) {
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 9600:
    default: return B9600;
    }
}

ncl_serial *ncl_serial_open(const ncl_serial_options *options, char *err,
                            size_t err_len)
{
    ncl_serial *port;
    struct termios tty;
    int fd;

    if (options == NULL || ncl_str_is_blank(options->device)) {
        if (err != NULL) {
            snprintf(err, err_len, "no serial device given");
        }
        return NULL;
    }
    fd = open(options->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        if (err != NULL) {
            snprintf(err, err_len, "cannot open %s (%s)", options->device,
                     strerror(errno));
        }
        return NULL;
    }
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        if (err != NULL) {
            snprintf(err, err_len, "cannot read the port settings (%s)",
                     strerror(errno));
        }
        close(fd);
        return NULL;
    }
    /* Raw mode, spelled out: cfmakeraw() is a BSD/GNU extension that
     * -D_POSIX_C_SOURCE=200809L does not declare. */
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL |
                     IXON);
    tty.c_oflag &= ~OPOST;
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_cflag &= ~(CSIZE | PARENB);
    tty.c_cflag |= CS8;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= options->data_bits == 7 ? CS7 : CS8;
    switch (options->parity) {
    case 'E':
    case 'e':
        tty.c_cflag |= PARENB;
        tty.c_cflag &= ~PARODD;
        break;
    case 'O':
    case 'o':
        tty.c_cflag |= (PARENB | PARODD);
        break;
    default:
        tty.c_cflag &= ~PARENB;
        break;
    }
    tty.c_cflag &= ~CSTOPB;
    if (options->stop_bits == 2) {
        tty.c_cflag |= CSTOPB;
    }
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    cfsetispeed(&tty, serial_speed(options->baud));
    cfsetospeed(&tty, serial_speed(options->baud));
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        if (err != NULL) {
            snprintf(err, err_len, "cannot change the port settings (%s)",
                     strerror(errno));
        }
        close(fd);
        return NULL;
    }
    tcflush(fd, TCIOFLUSH);
    port = (ncl_serial *)ncl_mem_calloc(1, sizeof(*port));
    if (port == NULL) {
        close(fd);
        return NULL;
    }
    port->fd = fd;
    port->open = true;
    port->inter_frame_ms = options->inter_frame_ms != 0
                               ? options->inter_frame_ms
                               : SERIAL_DEFAULT_INTER_FRAME_MS;
    return port;
}

void ncl_serial_close(ncl_serial *port)
{
    if (port == NULL) {
        return;
    }
    if (port->open) {
        close(port->fd);
    }
    ncl_mem_free(port);
}

bool ncl_serial_is_open(const ncl_serial *port)
{
    return port != NULL && port->open;
}

ncl_err ncl_serial_write(ncl_serial *port, const void *data, size_t len)
{
    const unsigned char *cursor = (const unsigned char *)data;
    size_t sent = 0;

    if (port == NULL || data == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    while (sent < len) {
        ssize_t written = write(port->fd, cursor + sent, len - sent);

        if (written < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            }
            return NCL_ERR_IO;
        }
        sent += (size_t)written;
    }
    return NCL_OK;
}

int ncl_serial_read(ncl_serial *port, void *buf, size_t len, unsigned timeout_ms)
{
    fd_set set;
    struct timeval tv;
    int rc;

    if (port == NULL || buf == NULL || len == 0) {
        return -1;
    }
    FD_ZERO(&set);
    FD_SET(port->fd, &set);
    tv.tv_sec = (time_t)(timeout_ms / 1000u);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
    rc = select(port->fd + 1, &set, NULL, NULL, &tv);
    if (rc == 0) {
        return 0; /* quiet line: for RTU that is the end of the frame */
    }
    if (rc < 0) {
        return errno == EINTR ? 0 : -1;
    }
    rc = (int)read(port->fd, buf, len);
    if (rc < 0 && (errno == EAGAIN || errno == EINTR)) {
        return 0;
    }
    return rc;
}

void ncl_serial_flush(ncl_serial *port)
{
    if (port != NULL) {
        tcflush(port->fd, TCIOFLUSH);
    }
}

#endif /* NCL_OS_WINDOWS */
