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

int main(int argc, char **argv)
{
    const char *libname = argc > 1 ? argv[1] : "libgsk-http.so";
    const char *host = argc > 2 ? argv[2] : "127.0.0.1";
    int port = argc > 3 ? atoi(argv[3]) : 6000;
    const char *method = argc > 4 ? argv[4] : "/GSK/CNC/Open/TCP";
    void *lib, *obj;
    create_fn create;
    call_fn call;
    destroy_fn destroy;
    nlohmann::json params;
    std::string out;
    int rc;

    /* Unbuffered: if a plugin aborts we still want to see how far we got. */
    setvbuf(stdout, NULL, _IONBF, 0);

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

    obj = create(&params, NULL);
    printf("create -> %p\n", obj);
    if (obj == NULL) {
        return 0;
    }

    rc = call(obj, method, "{}", &out);
    printf("call(%s) -> rc=%d out=%s\n", method, rc, out.c_str());

    if (destroy != NULL) {
        destroy(obj);
    }
    return 0;
}
