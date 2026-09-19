/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/* Drive a site plugin directly: dlopen + create/call/destroy, the same three C
 * entry points the box's host uses. The plugin then talks to a "machine" we
 * control, which is where we get to see the device-side protocol. */
#include <dlfcn.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

typedef void *(*create_fn)(nlohmann::json *, void *);
typedef int (*call_fn)(void *, const char *, const char *, std::string *);
typedef void (*destroy_fn)(void *);
/* The C++ ctor takes the parameters as JSON *text* (old-ABI std::string by
 * value), so it can be called without ever handing our nlohmann object to the
 * plugin's own, differently-versioned nlohmann. */
typedef void *(*ctor_fn)(void *logger, std::string params);

/* A stand-in for spdlog::logger. The plugins are built with their own copy of
 * spdlog (header only), so we cannot hand them a logger built from our headers.
 * What they do with it is log through it, so a zeroed object whose vtable is a
 * table of no-ops is enough: should_log() reads level_ (0 = trace, so logging
 * runs), the sink loop walks a zeroed vector (size 0, so nothing happens), and
 * every virtual call lands on a no-op. */
static void fake_logger_noop(void)
{
}

struct fake_logger {
    void **vtable;
    unsigned char rest[512];
};

static void *g_noop_vtable[64];

/* The plugin's own json code (is_string, operator[]) lives in libbase.so — the
 * plugins import it. Call it on our object to see whether the vendor's layout
 * agrees with ours before blaming the plugin's parameter checks. */
static void vendor_json_selfcheck(nlohmann::json *params)
{
    typedef void *(*index_fn)(void *, const char *);
    typedef int (*is_string_fn)(const void *);
    void *libbase = dlopen("libbase.so", RTLD_NOW | RTLD_GLOBAL);
    void *idx, *iss;

    if (libbase == NULL) {
        printf("libbase: %s\n", dlerror());
        return;
    }
    idx = dlsym(libbase, "_ZN8nlohmann10basic_jsonISt3mapSt6vectorSsbxydSaNS_"
                        "14adl_serializerES2_IhSaIhEEEixIKcEERS6_PT_");
    iss = dlsym(libbase, "_ZNK8nlohmann10basic_jsonISt3mapSt6vectorSsbxydSaNS_"
                         "14adl_serializerES2_IhSaIhEEE9is_stringEv");
    printf("vendor idx=%p is_string=%p\n", idx, iss);
    if (idx != NULL && iss != NULL) {
        void *v = ((index_fn)idx)(params, "server");

        printf("vendor view: params[\"server\"]=%p is_string=%d\n", v,
               v != NULL ? ((is_string_fn)iss)(v) : -1);
        printf("our view:    params[\"server\"].is_string()=%d\n",
               (int)params->operator[]("server").is_string());
    }
}

static void *fake_logger_make(void)
{
    static fake_logger logger;
    size_t i;

    for (i = 0; i < sizeof(g_noop_vtable) / sizeof(g_noop_vtable[0]); i++) {
        g_noop_vtable[i] = (void *)fake_logger_noop;
    }
    logger.vtable = g_noop_vtable;
    return &logger;
}

int main(int argc, char **argv)
{
    const char *libname = argc > 1 ? argv[1] : "libgsk-http.so";
    const char *host = argc > 2 ? argv[2] : "127.0.0.1";
    int port = argc > 3 ? atoi(argv[3]) : 6000;
    const char *method = argc > 4 ? argv[4] : "/GSK/CNC/Open/TCP";
    const char *ctor_name = argc > 5 ? argv[5] : NULL;
    const char *call_params = argc > 6 ? argv[6] : "{}";
    void *lib, *obj;
    create_fn create;
    call_fn call;
    destroy_fn destroy;
    nlohmann::json params;
    std::string out;
    int rc;

    /* Unbuffered: if a plugin aborts we still want to see how far we got. */
    setvbuf(stdout, NULL, _IONBF, 0);
    /* "-" means "no ctor": PowerShell drops empty arguments, so the shell
     * scripts pass a sentinel instead. */
    if (ctor_name != NULL &&
        (ctor_name[0] == '\0' || strcmp(ctor_name, "-") == 0)) {
        ctor_name = NULL;
    }

    /* The plugin stack has to be preloaded in dependency order: the vendor libs
     * are C++ and do not declare libstdc++ in NEEDED, libbase.so needs
     * libbase64.so's symbols, and every plugin needs libbase.so's. On the box
     * the host links them in this order; here we do it by hand. */
    {
        static const char *preload[] = {
            "libstdc++.so.6", "libbase64.so", "libLogApi.so", "libbase.so"
        };
        size_t i;

        for (i = 0; i < sizeof(preload) / sizeof(preload[0]); i++) {
            if (dlopen(preload[i], RTLD_NOW | RTLD_GLOBAL) == NULL) {
                printf("preload %s: %s\n", preload[i], dlerror());
            }
        }
    }
    lib = dlopen(libname, RTLD_NOW | RTLD_GLOBAL);
    if (lib == NULL) {
        fprintf(stderr, "dlopen(%s): %s\n", libname, dlerror());
        return 1;
    }
    create = (create_fn)dlsym(lib, "create");
    call = (call_fn)dlsym(lib, "call");
    destroy = (destroy_fn)dlsym(lib, "destroy");
    printf("create=%p call=%p destroy=%p\n", (void *)create, (void *)call,
           (void *)destroy);
    if (create == NULL || call == NULL) {
        return 1;
    }

    params["ipAddress"] = host;
    params["port"] = port;
    params["timeout"] = 5;
    params["server"] = "http://127.0.0.1:33123";
    params["path"] = "//CNC_MEM/USER/PATH1";
    /* The plugin's create() rejects the parameters it does not understand, and
     * we do not know which keys it insists on: hand it every plausible one. */
    params["script"] = "";
    params["module"] = "";
    params["name"] = "probe";
    params["type"] = "probe";
    params["id"] = "probe";
    params["connectionId"] = "probe";
    params["sn"] = "PROBEBOX";
    params["lua"] = "";
    vendor_json_selfcheck(&params);

    if (ctor_name != NULL) {
        ctor_fn ctor = (ctor_fn)dlsym(lib, ctor_name);

        printf("ctor(%s) = %p\n", ctor_name, (void *)ctor);
        obj = ctor != NULL ? ctor(fake_logger_make(), params.dump()) : NULL;
        if (obj != NULL) {
            printf("ctor -> %p (params %s)\n", obj, params.dump().c_str());
        }
    } else {
        obj = create(&params, fake_logger_make());
    }
    printf("create -> %p\n", obj);
    if (obj == NULL) {
        return 0;
    }

    rc = call(obj, method, call_params, &out);
    printf("call(%s, %s) -> rc=%d out=%s\n", method, call_params, rc,
           out.c_str());

    if (destroy != NULL) {
        destroy(obj);
    }
    return 0;
}
