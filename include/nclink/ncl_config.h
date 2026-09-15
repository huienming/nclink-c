/*
 * NC-Link core - device configuration.
 *
 * Every function returns the value or NCL_OK on success and NULL/error on
 * failure. The REST layer wraps them with ncl_result_success() /
 * ncl_result_failed() so the HTTP envelope is uniform.
 *
 * Files (all relative to ncl_env_root()):
 *   bin/sn.txt                  serial number
 *   conf/model/nclink.json      device model
 *   conf/driver/driver.json     driver configuration
 *   conf/driver/server.json     driver servers to start (setServer)
 *   conf/mqtt.cfg               "url=...\nusername=...\npassword=..."
 */
#ifndef NCL_CONFIG_H
#define NCL_CONFIG_H

#include "nclink/ncl_common.h"
#include "nclink/ncl_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Largest configuration file accepted. */
#define NCL_CONFIG_MAX_FILE_BYTES (1024 * 1024)
#define NCL_CONFIG_MAX_MQTT_BYTES 1024

/**
 * Create bin/, conf/ and log/ under the install root and write bin/sn.txt,
 * generating a serial number when @p sn is NULL or empty.
 * On success *out_sn (optional) receives the serial number in use.
 */
ncl_err ncl_config_init(const char *sn, char **out_sn);

/** The serial number, or NULL when bin/sn.txt is missing. */
char *ncl_config_get_sn(void);

/* --------------------------------------------------------------- model ---- */

/**
 * The parsed model document. Returns NULL when the file is missing, empty,
 * oversized or not valid JSON.
 */
ncl_json *ncl_config_get_model(void);

/** Write @p json to conf/model/nclink.json. */
ncl_err ncl_config_set_model(const char *json);

/* -------------------------------------------------------------- driver ---- */

/** The parsed driver configuration. */
ncl_json *ncl_config_get_driver(void);

/** Write @p json to conf/driver/driver.json. */
ncl_err ncl_config_set_driver(const char *json);

/* ---------------------------------------------------------- server list --- */

/**
 * The known servers. Reads conf/driver/server.json when present, otherwise
 * falls back to the in-memory list set through ncl_env_set_server_list().
 */
ncl_json *ncl_config_get_server_list(void);

/** Store the server list (JSON array or object). */
ncl_err ncl_config_set_server_list(const char *json);

/* ------------------------------------------------------------- mqtt.cfg --- */

/**
 * {"url":..,"username":..,"password":..} read from conf/mqtt.cfg, or NULL when
 * the file is missing, oversized or empty.
 */
ncl_json *ncl_config_get_mqtt(void);

/** Write the three fields back to conf/mqtt.cfg, one per line. */
ncl_err ncl_config_set_mqtt(const ncl_json *config);

/* ----------------------------------------------------------- ipConf.json -- */

/** The parsed conf/ipConf.json (same rules as ncl_config_get_model()). */
ncl_json *ncl_config_get_ip_conf(void);

/** Write @p json to conf/ipConf.json. */
ncl_err ncl_config_set_ip_conf(const char *json);

#ifdef __cplusplus
}
#endif

#endif /* NCL_CONFIG_H */
