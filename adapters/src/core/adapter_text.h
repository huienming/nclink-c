/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - text file helpers.
 *
 * Configuration files are edited by hand on Windows as often as on Linux, so
 * a UTF-8 byte order mark must not stop the adapter from starting (RFC 8259
 * lets a parser skip it; the core's parser is strict on purpose and stays so).
 */
#ifndef NCL_ADAPTER_TEXT_H
#define NCL_ADAPTER_TEXT_H

#include "nclink/ncl_common.h"
#include "nclink/ncl_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parse the JSON document in @p path, skipping a leading UTF-8 BOM.
 * Returns NULL on failure with a diagnostic appended to @p err.
 */
ncl_json *ncl_adapter_json_from_file(const char *path, ncl_strbuf *err);

#ifdef __cplusplus
}
#endif

#endif /* NCL_ADAPTER_TEXT_H */
