/*
 * NC-Link core - file module internals shared with the server.
 */
#ifndef NCL_FILE_INTERNAL_H
#define NCL_FILE_INTERNAL_H

#include "nclink/ncl_json.h"

/**
 * Walk the top level values of a method call result, copy every {"@file": path}
 * value into <root>/temp/<name>, replace it with the "/temp/<name>" token and
 * append the affected keys as "fileKeys".
 * @p data is modified in place and returned.
 */
ncl_json *ncl_file_extract_file_values(ncl_json *data);

#endif /* NCL_FILE_INTERNAL_H */
