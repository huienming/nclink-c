/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* Probe: drive FANUC's own FOCAS client library against a fake machine and see
 * what it puts on the wire. No headers needed: everything goes through dlsym,
 * so we can call the exact functions libfocas.so imports. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef short (*allclibhndl3_fn)(const char *, unsigned short, long,
                                 unsigned short *);
typedef short (*focas_sh_uh_fn)(unsigned short, void *);
typedef short (*focas_sh_uh_i_fn)(unsigned short, short, void *);
typedef short (*freelibhndl_fn)(unsigned short);

static void *g_lib;

static void *sym(const char *name)
{
    void *p = dlsym(g_lib, name);
    if (p == NULL) {
        fprintf(stderr, "  (no symbol %s)\n", name);
    }
    return p;
}

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    unsigned short port = argc > 2 ? (unsigned short)atoi(argv[2]) : 8193;
    /* argv[3] 只跑这一个 SDK 函数（不传就跑全部）——这样 mock.log 里的报文
     * 能一对一地对上是哪个调用发的。 */
    const char *filter = argc > 3 ? argv[3] : NULL;
    unsigned char buf[4096];
    unsigned short handle = 0;
    allclibhndl3_fn allclibhndl3;
    short rc;
    int i;

    /* The functions to drive, and whether they take a leading short. */
    static const struct {
        const char *name;
        int with_short;
    } calls[] = {
        { "cnc_statinfo", 0 },        /* cnc_statinfo(h, ODBST*)          */
        { "cnc_machine", 0 },         /* cnc_machine(h, ODBM*)            */
        { "cnc_rdcount", 1 },         /* cnc_rdcount(h, num, IODBCOUNTER*)*/
        { "cnc_actf", 0 },            /* cnc_actf(h, ODBACT*)             */
        { "cnc_acts", 0 },            /* cnc_acts(h, ODBACT*)             */
        { "cnc_rdaxisdata", 1 },      /* (h, s, ...) — shape probed       */
        { "cnc_rdparam", 1 },
        { "cnc_rdmacro", 1 },
        { "cnc_rdalmmsg2", 1 },
        { "cnc_rdtofs", 1 },
        { "cnc_rdprogdir3", 1 },
        { "cnc_rdexecprog", 1 },
        { "cnc_exeprgname2", 0 },
        { "cnc_rdblkcount", 0 },
        { "cnc_rdlife", 1 },
    };

    /* The vendor library is C++ but does not declare libstdc++ as a
     * dependency: on the box it lives in the host process. Pull it in first so
     * the RTTI symbols libfwlib32.so expects are resolvable. */
    if (dlopen("libstdc++.so.6", RTLD_NOW | RTLD_GLOBAL) == NULL) {
        fprintf(stderr, "libstdc++ preload failed: %s\n", dlerror());
    }
    g_lib = dlopen("libfwlib32.so", RTLD_NOW);
    if (g_lib == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    allclibhndl3 = (allclibhndl3_fn)sym("cnc_allclibhndl3");
    if (allclibhndl3 == NULL) {
        return 1;
    }

    printf("== cnc_allclibhndl3(%s, %u, 3)\n", host, port);
    rc = allclibhndl3(host, port, 3L, &handle);
    printf("   rc=%d handle=%u\n", rc, handle);
    if (rc != 0) {
        return 0;
    }

    for (i = 0; i < (int)(sizeof(calls) / sizeof(calls[0])); i++) {
        void *fp;
        if (filter != NULL && strcmp(filter, calls[i].name) != 0) {
            continue;
        }
        fp = sym(calls[i].name);
        if (fp == NULL) {
            continue;
        }
        memset(buf, 0, sizeof(buf));
        if (calls[i].with_short) {
            rc = ((focas_sh_uh_i_fn)fp)(handle, 1, buf);
        } else {
            rc = ((focas_sh_uh_fn)fp)(handle, buf);
        }
        printf("== %s -> rc=%d\n", calls[i].name, rc);
    }

    {
        freelibhndl_fn free_handle = (freelibhndl_fn)sym("cnc_freelibhndl");
        if (free_handle != NULL) {
            printf("== cnc_freelibhndl -> rc=%d\n", free_handle(handle));
        }
    }
    dlclose(g_lib);
    return 0;
}
