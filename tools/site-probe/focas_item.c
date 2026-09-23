/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 huienming
 *
 * 裸读一个 item：把请求的 `d`/`e`/`arg2`/`arg3` 直接指定，打印 rc、应答长度与原始字节。
 * 试"这个 item 要发什么参数、机床回多少"用（01 册 §11.11.1 里几条口径就是这么核的：
 * ACTF 只回一条记录、RDPARAM 的号要 d=e=号、宏变量 500/501 是 vacant …）。
 *
 * 用法：
 *     focas_item.exe <host> [port] <item> [d] [e] [arg2] [arg3]
 *     focas_item.exe 127.0.0.1 8193 RDPARAM 6711 6711
 *     focas_item.exe 127.0.0.1 8193 ACTF 0 3
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/clients/focas.h"
#include "nclink/ncl_platform.h"

int main(int argc, char **argv)
{
    ncl_focas_config config;
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    unsigned port = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 8193u;
    const char *item = argc > 3 ? argv[3] : "ACTF";
    ncl_json *params = NULL;
    ncl_json *answer = NULL;
    ncl_json *bytes = NULL;
    ncl_focas *f = NULL;
    char *err = NULL;
    ncl_err rc;
    size_t i, n;

    ncl_focas_config_default(&config);
    config.host = host;
    config.port = (unsigned short)port;
    config.negotiate = true;
    config.timeout_ms = 15000;
    f = ncl_focas_open(&config, &err);
    if (f == NULL) {
        printf("open 失败: %s\n", err != NULL ? err : "?");
        return 1;
    }
    params = ncl_json_new_object();
    (void)ncl_json_obj_set_string(params, "item", item);
    (void)ncl_json_obj_set_int(params, "block", 0);
    if (argc > 4) {
        (void)ncl_json_obj_set_int(params, "d", atoll(argv[4]));
    }
    if (argc > 5) {
        (void)ncl_json_obj_set_int(params, "e", atoll(argv[5]));
    }
    if (argc > 6) {
        (void)ncl_json_obj_set_int(params, "arg2", atoll(argv[6]));
    }
    if (argc > 7) {
        (void)ncl_json_obj_set_int(params, "arg3", atoll(argv[7]));
    }
    rc = ncl_focas_call(f, "payload", params, &answer);
    printf("%s d=%s e=%s a2=%s a3=%s -> rc=%d %s\n", item,
           argc > 4 ? argv[4] : "-", argc > 5 ? argv[5] : "-",
           argc > 6 ? argv[6] : "-", argc > 7 ? argv[7] : "-", rc,
           ncl_focas_last_error(f));
    if (rc == NCL_OK) {
        bytes = ncl_json_obj_get(answer, "bytes");
        n = bytes != NULL ? ncl_json_arr_len(bytes) : 0;
        printf("   长度 %u：", (unsigned)n);
        for (i = 0; i < n && i < 64; i++) {
            long long v = 0;

            (void)ncl_json_as_int(ncl_json_arr_get(bytes, i), &v);
            printf("%02x", (unsigned)(v & 0xFF));
        }
        printf("%s\n", n > 64 ? " ..." : "");
    }
    ncl_json_free(params);
    ncl_json_free(answer);
    ncl_focas_close(f);
    return 0;
}
