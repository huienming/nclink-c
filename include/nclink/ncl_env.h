/*
 * NC-Link core - runtime environment, configuration paths and serial number.
 *
 * All path accessors return pointers into static storage owned by the module;
 * they stay valid until the next ncl_env_set_root() call or until
 * ncl_env_shutdown().
 */
#ifndef NCL_ENV_H
#define NCL_ENV_H

#include <stdbool.h>

#include "nclink/ncl_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Override the installation root. Passing NULL restores the current working
 *  directory. */
void ncl_env_set_root(const char *path);

/** Installation root; defaults to the current working directory. */
const char *ncl_env_root(void);

const char *ncl_env_conf_path(void);
const char *ncl_env_run_path(void);
const char *ncl_env_driver_path(void);
const char *ncl_env_log_path(void);
const char *ncl_env_log_file(void);
const char *ncl_env_mqtt_cfg_file(void);
const char *ncl_env_model_file(void);
const char *ncl_env_driver_cfg_file(void);
const char *ncl_env_sn_file(void);

/** Known NC-Link servers. */
void  ncl_env_set_server_list(const char *const *servers, size_t count);
size_t ncl_env_server_count(void);
const char *ncl_env_server_at(size_t index);

/** Release memory held by the module (call once at shutdown). */
void ncl_env_shutdown(void);

/* ------------------------------------------------------------- MqttConfig -- */

typedef struct {
    char *url;
    char *username;
    char *password;
} ncl_mqtt_config;

/**
 * Read <conf>/mqtt.cfg. When the file is missing it is created with the built
 * in defaults and those defaults are returned. Returns NCL_OK on success.
 */
ncl_err ncl_mqtt_config_read(ncl_mqtt_config *out);
void    ncl_mqtt_config_free(ncl_mqtt_config *cfg);

/* ------------------------------------------------------- serial number ---- */

/**
 * Read <root>/bin/sn.txt, generating and persisting a serial number with
 * ncl_sn_generate() when the file does not exist.
 * Returns a heap string (free with free()) or NULL on I/O failure.
 */
char *ncl_sn_read(void);

/**
 * Generate a serial number: "V2" followed by nine upper-case hexadecimal
 * digits derived from random bytes.
 * Returns a heap string or NULL.
 */
char *ncl_sn_generate(void);

/* ------------------------------------------------------------------- misc -- */

/** True when a file or directory exists. */
bool ncl_path_exists(const char *path);

/** Create @p path and any missing parents. Returns NCL_OK on success. */
ncl_err ncl_mkdir_p(const char *path);

/** Read a whole file into memory (NUL terminated). *out receives a heap
 *  buffer; @p out_len may be NULL. */
ncl_err ncl_file_read_all(const char *path, char **out, size_t *out_len);

/** Write @p data to @p path, creating parents as needed. */
ncl_err ncl_file_write_all(const char *path, const void *data, size_t len);

/** Append @p data to @p path, creating it (and its parents) when missing. */
ncl_err ncl_file_append(const char *path, const void *data, size_t len);

/** Copy @p src to @p dst, creating the parent directory of @p dst. */
ncl_err ncl_file_copy(const char *src, const char *dst);

/** Size of @p path in bytes, or -1 when it cannot be read. */
long long ncl_file_size(const char *path);

/** Last modification time of @p path in epoch milliseconds, 0 when unknown. */
int64_t ncl_file_mtime_ms(const char *path);

/** True when @p path exists and names a directory. */
bool ncl_path_is_dir(const char *path);

/** Remove @p path; directories are removed recursively. Missing paths are OK. */
ncl_err ncl_path_remove(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* NCL_ENV_H */
