/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - file module internals shared with the server.
 */
#ifndef NCL_FILE_INTERNAL_H
#define NCL_FILE_INTERNAL_H

#include "nclink/ncl_json.h"

struct ncl_client;

/**
 * Walk the top level values of a method call result, copy every {"@file": path}
 * value into <root>/temp/<name>, replace it with the "/temp/<name>" token and
 * append the affected keys as "fileKeys".
 * @p data is modified in place and returned.
 */
ncl_json *ncl_file_extract_file_values(ncl_json *data);

/**
 * Borrowed lease name of @p client's file channel, or NULL when it holds none.
 * Written by the channel handshake, cleared when the channel closes.
 */
const char *ncl_client_file_channel_lease(const struct ncl_client *client);

/** Borrowed FTP login the library minted for the channel, or NULL. */
const char *ncl_client_file_channel_login(const struct ncl_client *client);

/**
 * Replace the file channel state of @p client. Both strings are copied; a NULL
 * @p channel_id drops the channel (and with it the login).
 */
ncl_err ncl_client_set_file_channel_lease(struct ncl_client *client,
                                          const char *channel_id,
                                          const char *login);

#endif /* NCL_FILE_INTERNAL_H */
