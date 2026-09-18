/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * FTP transport tests: the C server and the C client exercised against each
 * other in both active and passive mode, plus the protocol details the file
 * tool depends on (detect/disconnect, STOR/RETR/LIST/MKD/RMD).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ncl_test.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_ftp.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"

#define FTP_TEST_ROOT "ncl_ftp_test_root"

static ncl_ftp_server *start_server(unsigned port)
{
    ncl_ftp_server_options options;
    ncl_ftp_server *server;

    memset(&options, 0, sizeof(options));
    options.port = port;
    options.root = FTP_TEST_ROOT;
    options.user = "admin";
    options.password = "123456";
    options.allow_write = true;
    options.idle_timeout_ms = 5000;

    server = ncl_ftp_server_create_ex(&options);
    NCL_CHECK(server != NULL);
    if (server == NULL) {
        return NULL;
    }
    NCL_CHECK_EQ_INT(ncl_ftp_server_start(server), NCL_OK);
    NCL_CHECK(ncl_ftp_server_port(server) != 0);
    return server;
}

static ncl_ftp_client *connect_client(unsigned port, bool passive)
{
    ncl_ftp_client_options options;
    ncl_ftp_client *client;

    memset(&options, 0, sizeof(options));
    options.passive = passive;
    options.connect_timeout_ms = 3000;
    options.io_timeout_ms = 5000;
    client = ncl_ftp_client_create_ex("127.0.0.1", port, "admin", "123456",
                                      &options);
    NCL_CHECK(client != NULL);
    if (client == NULL) {
        return NULL;
    }
    NCL_CHECK(ncl_ftp_client_detect(client));
    NCL_CHECK(ncl_ftp_client_is_connected(client));
    /* detect() ends with TYPE I, so the last reply is that 200, exactly like
     * detect() finishing on a binary TYPE. */
    NCL_CHECK_EQ_INT(ncl_ftp_client_reply_code(client), 200);
    return client;
}

NCL_TEST_MAIN_BEGIN()

    ncl_ftp_server *server;
    unsigned port;
    ncl_ftp_client *client;
    int mode;

    ncl_path_remove(FTP_TEST_ROOT);
    NCL_CHECK_EQ_INT(ncl_mkdir_p(FTP_TEST_ROOT), NCL_OK);

    NCL_TEST_CASE("server lifecycle");
    server = start_server(0);
    if (server == NULL) {
        printf("cannot start the FTP server\n");
        return 1;
    }
    port = ncl_ftp_server_port(server);
    NCL_CHECK(ncl_ftp_server_is_running(server));
    NCL_CHECK_EQ_STR(ncl_ftp_server_root(server), FTP_TEST_ROOT);

    NCL_TEST_CASE("bad credentials are rejected");
    {
        ncl_ftp_client *bad = ncl_ftp_client_create("127.0.0.1", port, "admin",
                                                    "wrong");
        ncl_ftp_client_options options;

        memset(&options, 0, sizeof(options));
        options.connect_timeout_ms = 3000;
        options.io_timeout_ms = 3000;
        NCL_CHECK(bad != NULL);
        NCL_CHECK(!ncl_ftp_client_detect(bad));
        NCL_CHECK(!ncl_ftp_client_is_connected(bad));
        ncl_ftp_client_free(bad);
    }

    /* Exercise both transfer modes with the same script. */
    for (mode = 0; mode < 2; mode++) {
        bool passive = mode == 1;

        NCL_TEST_CASE(passive ? "passive transfers" : "active transfers");
        client = connect_client(port, passive);
        if (client == NULL) {
            continue;
        }

        {
            char cwd[256];
            NCL_CHECK_EQ_INT(ncl_ftp_client_pwd(client, cwd, sizeof(cwd)),
                             NCL_OK);
            NCL_CHECK_EQ_STR(cwd, "/");
        }
        NCL_CHECK_EQ_INT(ncl_ftp_client_mkdir(client, "/data"), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_ftp_client_chdir(client, "/data"), NCL_OK);
        {
            char cwd[256];
            NCL_CHECK_EQ_INT(ncl_ftp_client_pwd(client, cwd, sizeof(cwd)),
                             NCL_OK);
            NCL_CHECK_EQ_STR(cwd, "/data");
        }

        NCL_TEST_CASE("STOR / RETR round trip");
        {
            char payload[9000];
            ncl_strbuf back;
            size_t i;
            long long size = 0;

            for (i = 0; i < sizeof(payload); i++) {
                payload[i] = (char)((i * 31 + 7) & 0xFF);
            }
            payload[0] = '\0'; /* binary payloads may contain NUL bytes */
            NCL_CHECK_EQ_INT(
                ncl_ftp_client_store(client, "blob.bin", payload,
                                     sizeof(payload)),
                NCL_OK);
            NCL_CHECK_EQ_INT(ncl_ftp_client_size(client, "blob.bin", &size),
                             NCL_OK);
            NCL_CHECK_EQ_INT(size, (long long)sizeof(payload));

            ncl_strbuf_init(&back);
            NCL_CHECK_EQ_INT(
                ncl_ftp_client_retrieve(client, "blob.bin", &back), NCL_OK);
            NCL_CHECK_EQ_INT(back.len, sizeof(payload));
            NCL_CHECK(back.len == sizeof(payload) &&
                      memcmp(back.data, payload, sizeof(payload)) == 0);
            ncl_strbuf_free(&back);
        }

        NCL_TEST_CASE("LIST and NLST");
        {
            ncl_ptrvec entries;
            ncl_strvec names;
            bool saw_blob = false;
            size_t i;

            ncl_ptrvec_init(&entries, ncl_ftp_entry_free);
            NCL_CHECK_EQ_INT(ncl_ftp_client_list(client, "/data", &entries),
                             NCL_OK);
            for (i = 0; i < ncl_ptrvec_len(&entries); i++) {
                const ncl_ftp_entry *e =
                    (const ncl_ftp_entry *)ncl_ptrvec_at(&entries, i);
                if (e != NULL && e->name != NULL &&
                    strcmp(e->name, "blob.bin") == 0) {
                    saw_blob = true;
                    NCL_CHECK_EQ_INT(e->type, 0);
                    NCL_CHECK_EQ_INT(e->size, 9000);
                    NCL_CHECK(e->modify_time > 0);
                }
            }
            NCL_CHECK(saw_blob);
            ncl_ptrvec_free(&entries);

            ncl_strvec_init(&names);
            NCL_CHECK_EQ_INT(ncl_ftp_client_nlst(client, "/data", &names),
                             NCL_OK);
            NCL_CHECK(ncl_strvec_contains(&names, "blob.bin"));
            ncl_strvec_free(&names);
        }

        NCL_TEST_CASE("directory listing reports folders");
        {
            ncl_ptrvec entries;
            size_t i;
            bool saw_dir = false;

            NCL_CHECK_EQ_INT(ncl_ftp_client_mkdir(client, "nested"), NCL_OK);
            ncl_ptrvec_init(&entries, ncl_ftp_entry_free);
            NCL_CHECK_EQ_INT(ncl_ftp_client_list(client, ".", &entries),
                             NCL_OK);
            for (i = 0; i < ncl_ptrvec_len(&entries); i++) {
                const ncl_ftp_entry *e =
                    (const ncl_ftp_entry *)ncl_ptrvec_at(&entries, i);
                if (e != NULL && e->name != NULL &&
                    strcmp(e->name, "nested") == 0) {
                    saw_dir = true;
                    NCL_CHECK(ncl_ftp_entry_is_dir(e));
                }
            }
            NCL_CHECK(saw_dir);
            ncl_ptrvec_free(&entries);
        }

        NCL_TEST_CASE("MDTM");
        {
            int64_t when = 0;
            NCL_CHECK_EQ_INT(ncl_ftp_client_mdtm(client, "blob.bin", &when),
                             NCL_OK);
            NCL_CHECK(when > 1600000000000LL);
        }

        NCL_TEST_CASE("rename");
        NCL_CHECK_EQ_INT(ncl_ftp_client_rename(client, "blob.bin", "moved.bin"),
                         NCL_OK);
        NCL_CHECK(ncl_ftp_client_size(client, "moved.bin", NULL) == NCL_OK);
        NCL_CHECK(ncl_ftp_client_size(client, "blob.bin", NULL) != NCL_OK);

        NCL_TEST_CASE("delete and remove directory");
        NCL_CHECK_EQ_INT(ncl_ftp_client_delete(client, "moved.bin"), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_ftp_client_rmdir(client, "nested"), NCL_OK);
        NCL_CHECK_EQ_INT(ncl_ftp_client_rmdir(client, "/data"), NCL_OK);

        NCL_TEST_CASE("paths cannot escape the login root");
        NCL_CHECK_EQ_INT(ncl_ftp_client_chdir(client, "/../.."), NCL_OK);
        {
            char cwd[256];
            NCL_CHECK_EQ_INT(ncl_ftp_client_pwd(client, cwd, sizeof(cwd)),
                             NCL_OK);
            NCL_CHECK_EQ_STR(cwd, "/");
        }
        {
            char outside[512];
            snprintf(outside, sizeof(outside), "/../../%s", FTP_TEST_ROOT);
            NCL_CHECK(ncl_ftp_client_size(client, outside, NULL) != NCL_OK);
        }

        NCL_TEST_CASE("missing files report an error");
        {
            ncl_strbuf out;
            ncl_strbuf_init(&out);
            NCL_CHECK(ncl_ftp_client_retrieve(client, "nope.bin", &out) !=
                      NCL_OK);
            NCL_CHECK_EQ_INT(out.len, 0);
            ncl_strbuf_free(&out);
            NCL_CHECK(ncl_ftp_client_chdir(client, "/no-such-dir") != NCL_OK);
        }

        NCL_TEST_CASE("NOOP keeps the session alive");
        NCL_CHECK(ncl_ftp_client_noop(client));

        ncl_ftp_client_free(client);
    }

    NCL_TEST_CASE("stop closes every session");
    {
        int i;
        for (i = 0; i < 50 && ncl_ftp_server_session_count(server) != 0; i++) {
            ncl_sleep_millis(100);
        }
        NCL_CHECK_EQ_INT(ncl_ftp_server_session_count(server), 0);
    }
    NCL_CHECK(ncl_ftp_server_command_count(server) > 20);
    ncl_ftp_server_stop(server);
    NCL_CHECK(!ncl_ftp_server_is_running(server));
    ncl_ftp_server_free(server);

    NCL_TEST_CASE("the login root is never deleted");
    NCL_CHECK(ncl_path_is_dir(FTP_TEST_ROOT));
    NCL_CHECK_EQ_INT(ncl_path_remove(FTP_TEST_ROOT), NCL_OK);
    NCL_CHECK(!ncl_path_exists(FTP_TEST_ROOT));

    NCL_TEST_CASE("interface enumeration");
    {
        char *map = ncl_net_ip_map_json();
        NCL_CHECK(map != NULL);
        if (map != NULL) {
            NCL_CHECK(map[0] == '{');
            NCL_CHECK(map[strlen(map) - 1] == '}');
            ncl_free_safe(map);
        }
    }

NCL_TEST_MAIN_END()
