/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * 探针（Windows 版）：把 FANUC 自己的 Fwlib 接到假机床上，看它把什么发上线上。
 *
 * 和 Linux 版 focas_probe.c 是同一个思路（一次进程只跑一个 SDK 函数，好让假机床
 * 的日志一对一），差别只在两处：
 *
 *   - 用 LoadLibrary / GetProcAddress 拿符号，所以**不需要 Fwlib64.lib**，也不需要
 *     把 Fwlib64.dll 放到 exe 旁边（默认从 --dll 给的路径加载，缺省是当前目录）；
 *   - **不 include Fwlib64.h、也不抄它的结构体**：出参一律给一块 4 KiB 的零缓冲，
 *     跑完把这块缓冲按 u16/u32/int32 小端打出来。字段布局靠假机床铺的"斜坡载荷"
 *     （tools/site-probe/focas_sdk_mock.py）反推 —— 斜坡里每个字节都不一样，哪个
 *     字段落在哪个偏移一眼就看出来。
 *
 * 用法：
 *     focas_sdk_probe.exe [--dll Fwlib64.dll] <host> <port> <函数名> [整数参数...]
 *
 * 例：
 *     focas_sdk_probe.exe 127.0.0.1 8193 cnc_absolute 1
 *     focas_sdk_probe.exe 127.0.0.1 8193 cnc_rdalmmsg2 0
 *     focas_sdk_probe.exe 127.0.0.1 8193 cnc_statinfo
 *
 * 参数形状（`kind`）写死在这张表里，抄的是 Fwlib64.h 的声明：
 *   void         (h, out)                         cnc_statinfo / cnc_rdprgnum …
 *   s1           (h, short, out)                  cnc_rdcount / cnc_rdlife …
 *   s2           (h, short, short len, out)       cnc_absolute / cnc_machine …（第二
 *                                                 个 short 是**数据块长度**，不是轴号；
 *                                                 FOCAS 对"长度"查得很严，给 0 直接
 *                                                 回 EW_ATTRIB，一个字节都不发）
 *   s3           (h, short, short, short, out)    cnc_rdtofs / cnc_rdparam
 *   s1_n         (h, short, short *num, out)      cnc_rdposition / cnc_rdalmmsg2
 *   n            (h, short *num, out)             cnc_rdsvmeter
 *   s1_n_s1_n    (h, short, short *, short, short *, out)   cnc_rdaxisdata
 *   s2_n         (h, short, long *num, short *len, out)     cnc_rdprogdir3
 *   exec         (h, unsigned short *, short *, char *)      cnc_rdexecprog
 *   dwn4 / up4   程序上下行的三件套，一次进程里连着跑：
 *                  cnc_dwnstart4(h,0,"") → cnc_download4(h,&len,buf) → cnc_dwnend4(h)
 *                  cnc_upstart4(h,0,"")  → cnc_upload4(h,&len,buf)   → cnc_upend4(h)
 *                （这两个必须成对：不先 start，download4/upload4 回 EW_FUNC=1）
 *
 * `--count N` 给"进/出参数"里那个数量（short *data_num）：FOCAS 把 0 当长度错
 * （EW_LENGTH=2），所以要给个 sane 值（缺省 8）。
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define NCL_PROBE_BUF 4096

/*
 * 32 位下 SDK 的导出是 WINAPI（__stdcall），函数指针不标调用约定会把栈弹坏
 * （症状：STATUS_STACK_BUFFER_OVERRUN / 0xC0000409）。x64 只有一个约定，标了也无害。
 */
#if defined(_WIN32)
#define NCL_PROBE_CALL __stdcall
#else
#define NCL_PROBE_CALL
#endif

static void *g_lib;

static void *sym(const char *name)
{
    void *p;
#if defined(_WIN32)
    p = (void *)GetProcAddress((HMODULE)g_lib, name);
#else
    p = dlsym(g_lib, name);
#endif
    if (p == NULL) {
        fprintf(stderr, "  (no symbol %s)\n", name);
    }
    return p;
}

typedef short (NCL_PROBE_CALL *allclibhndl3_fn)(const char *, unsigned short,
                                                long, unsigned short *);
typedef short (NCL_PROBE_CALL *setdefnode_fn)(short);
typedef short (NCL_PROBE_CALL *allclibhndl_fn)(unsigned short *);
typedef short (NCL_PROBE_CALL *freelibhndl_fn)(unsigned short);

typedef short (NCL_PROBE_CALL *void_fn)(unsigned short, void *);
typedef short (NCL_PROBE_CALL *s1_fn)(unsigned short, short, void *);
typedef short (NCL_PROBE_CALL *s2_fn)(unsigned short, short, short, void *);
typedef short (NCL_PROBE_CALL *s3_fn)(unsigned short, short, short, short,
                                      void *);
typedef short (NCL_PROBE_CALL *s1_n_fn)(unsigned short, short, short *, void *);
typedef short (NCL_PROBE_CALL *n_fn)(unsigned short, short *, void *);
typedef short (NCL_PROBE_CALL *s1_n_s1_n_fn)(unsigned short, short, short *,
                                             short, short *, void *);
typedef short (NCL_PROBE_CALL *s2_n_fn)(unsigned short, short, long *, short *,
                                        void *);
typedef short (NCL_PROBE_CALL *exec_fn)(unsigned short, unsigned short *,
                                        short *, char *);
/* (h, short, short, short *, void *)：cnc_rdgcode 那种"两个入参 + 一个数量出参" */
typedef short (NCL_PROBE_CALL *s2_n_n_fn)(unsigned short, short, short, short *,
                                          void *);

static const struct {
    const char *name;
    const char *kind;
    int         len;   /**< "length" 参数（数据块字节数），0 = 不需要 */
} kCalls[] = {
    /* 连接与系统 */
    { "cnc_sysinfo", "void", 0 }, { "cnc_rdmodel", "void", 0 },
    { "cnc_statinfo", "void", 0 }, { "cnc_rdopmode", "void", 0 },
    /* 轴/坐标：feedrate 与主轴转速是"每轴/每主轴一个 float" */
    { "cnc_actf", "void", 0 }, { "cnc_acts", "void", 0 },
    /* ODBAXIS 单轴：dummy(2) + type(2) + data[0](4) = 8；全轴 = 4 + 4n */
    { "cnc_absolute", "s2", 8 }, { "cnc_machine", "s2", 8 },
    { "cnc_relative", "s2", 8 }, { "cnc_distance", "s2", 8 },
    { "cnc_rdposition", "s1_n", 8 }, { "cnc_rdaxisdata", "s1_n_s1_n", 4 },
    { "cnc_rdaxisname", "n", 0 },
    /* 主轴/伺服负载 */
    { "cnc_rdspmeter", "s1_n", 8 }, { "cnc_rdsvmeter", "n", 4 },
    { "cnc_rdspdata", "s1_n", 8 }, { "cnc_rdspcss", "s1", 0 },
    /* 状态/报警 */
    { "cnc_alarm", "void", 0 }, { "cnc_alarm2", "void", 0 },
    { "cnc_rdalmmsg2", "s1_n", 0 }, { "cnc_rdopmsg3", "s1_n", 0 },
    { "cnc_rdalmhisno3", "void", 0 },
    /* 程序 */
    { "cnc_rdprgnum", "void", 0 }, { "cnc_rdseqnum", "void", 0 },
    { "cnc_rdblkcount", "void", 0 }, { "cnc_exeprgname2", "void", 0 },
    { "cnc_rdprogdir3", "s2_n", 0 }, { "cnc_rdexecprog", "exec", 0 },
    { "cnc_rdproginfo", "s1", 0 },
    /* 计数/时间 */
    { "cnc_rdcount", "s1", 0 }, { "cnc_rdtimer", "s1", 0 },
    /* 跟踪误差那一族：伺服延迟量 / 诊断数据（0i/30i 上跟踪误差在诊断号 300 一族） */
    { "cnc_srvdelay", "s2", 8 }, { "cnc_diagnoss", "s3", 8 },
    /*
     * 两连调用：先 `cnc_rdaxisname` 再 `cnc_srvdelay`（同一条连接）。
     * 官方库的"控制轴数"是 **问过一次轴表之后** 才有的（fwlibe64.dll 1800316de 发
     * Cb 0xa4、180031783 把应答载荷的第一个字当轴数），而 ODBAXIS 那一族
     * （cnc_srvdelay / cnc_absolute）的 `axis > 轴数` 闸门查的就是它。分开跑两次探针
     * 是两个进程两条连接，缓存不共享 —— 所以单独跑 srvdelay 永远 rc=4。
     */
    { "seq:axis_then_srvdelay", "seq", 0 },
    { "cnc_rddiagnum", "void", 0 }, { "cnc_rddiaginfo", "s2", 0 },
    { "cnc_rdngrp", "void", 0 }, { "cnc_rdlife", "s1", 8 },
    { "cnc_rdtofsinfo", "void", 0 }, { "cnc_rdmacroinfo", "void", 0 },
    /* 刀补/参数/宏变量（帧抓到了、字段还没核） */
    { "cnc_rdtofs", "s3", 0 }, { "cnc_rdparam", "s3", 0 },
    { "cnc_rdmacro", "s2", 12 }, { "cnc_rddt", "s1", 0 },
    /* 工件坐标/模态 */
    { "cnc_rdgcode", "s2_n_n", 0 }, { "cnc_rdwkcdshft", "s2", 0 },
    /* 动态数据（速度/倍率一条全有）与主轴负载 */
    { "cnc_rddynamic2", "s2", 4 }, { "cnc_loadtorq", "s3", 12 },
    /*
     * 操作面板信号（IODBSGNL）：进给倍率 `feed_ovrd` / 主轴倍率 `spdl_ovrd` 就在这
     * 里面 —— `cnc_rddynamic2` 的 ODBDY2 **没有倍率字段**（官方头里查过），
     * client 早先那两条 not_yet 的注记写错了地方。
     */
    { "cnc_rdopnlsgnl", "s1", 0 },
    /* 程序上下行（三件套，探针里连着跑） */
    { "cnc_dwnstart4", "dwn4", 0 }, { "cnc_upstart4", "up4", 0 },
    { "cnc_download4", "dwn4", 0 }, { "cnc_upload4", "up4", 0 },
    { "cnc_dwnend4", "dwn4", 0 }, { "cnc_upend4", "up4", 0 },
};

static void dump(const unsigned char *buf, size_t len)
{
    size_t i;

    for (i = 0; i < len; i += 16) {
        size_t j;

        printf("  %04x  ", (unsigned)i);
        for (j = 0; j < 16; j++) {
            if (i + j < len) {
                printf("%02x ", buf[i + j]);
            } else {
                printf("   ");
            }
        }
        printf(" | ");
        for (j = 0; j < 16; j++) {
            if (i + j < len) {
                unsigned char c = buf[i + j];

                printf("%c", c >= 32 && c < 127 ? (char)c : '.');
            }
        }
        printf("\n");
    }
    /* 结构体头几个字段基本是 short/long，照着两三种宽度打一遍最省事。 */
    for (i = 0; i + 2 <= len && i < 32; i += 2) {
        printf("  u16[%2u] = 0x%04x (%u)\n", (unsigned)(i / 2),
               (unsigned)(buf[i] | (buf[i + 1] << 8)),
               (unsigned)(buf[i] | (buf[i + 1] << 8)));
    }
    for (i = 0; i + 4 <= len && i < 32; i += 4) {
        long v = (long)(buf[i] | (buf[i + 1] << 8) | (buf[i + 2] << 16) |
                        ((unsigned long)buf[i + 3] << 24));

        printf("  i32[%2u] = %ld\n", (unsigned)(i / 4), v);
    }
}

static int arg_i(int argc, char **argv, int index, int fallback)
{
    return index < argc ? (int)strtol(argv[index], NULL, 0) : fallback;
}

int main(int argc, char **argv)
{
    const char *host;
    unsigned short port;
    const char *fn_name;
    const char *kind = NULL;
    const char *dll = "Fwlib64.dll";
    const char *name_arg = "";
    bool        hssb = false;  /* --hssb：走 cnc_allclibhndl（节点号，NCGuide 用 9） */
    int         node = 9;
    const char *positional[8];
    int npos = 0;
    int block_len = 0;
    unsigned short handle = 0;
    unsigned char buf[NCL_PROBE_BUF];
    short num = 8;
    short num2 = 8;
    short len = 8;  /* s2_n 的第 3 个 short 是 `*num`（给 0 会被回 EW_LENGTH） */
    long lnum = 0;
    size_t i;
    int a0 = 0;
    int a1 = 0;
    int a2 = 0;
    short rc;
    allclibhndl3_fn allclibhndl3;
    freelibhndl_fn freelibhndl;
    int base = 1;

    /* 选项可以出现在任何位置（探针的调用行是 "<函数名> <整数参数...>"，选项混在
     * 里面比放在前面好写），先摘出来，剩下的按 host / port / 函数 / 整数参数 排。 */
    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--dll") == 0 && i + 1 < (size_t)argc) {
            dll = argv[++i];
        } else if (strcmp(argv[i], "--len") == 0 && i + 1 < (size_t)argc) {
            block_len = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < (size_t)argc) {
            num = (short)strtol(argv[++i], NULL, 0);
            num2 = num;
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < (size_t)argc) {
            name_arg = argv[++i]; /* 程序上下行的目录名/文件名（start4 的那个 char *） */
        } else if (strcmp(argv[i], "--hssb") == 0) {
            hssb = true;
        } else if (strcmp(argv[i], "--node") == 0 && i + 1 < (size_t)argc) {
            node = (int)strtol(argv[++i], NULL, 0);
        } else if (npos < (int)(sizeof(positional) / sizeof(positional[0]))) {
            positional[npos++] = argv[i];
        }
    }
    base = 0;
    if (npos < 3) {
        fprintf(stderr,
                "usage: %s [--dll Fwlib64.dll] [--len N] [--count N] "
                "<host> <port> <function> [ints...]\n",
                argv[0]);
        return 2;
    }
    host = positional[0];
    port = (unsigned short)strtol(positional[1], NULL, 0);
    fn_name = positional[2];
    for (i = 0; i < sizeof(kCalls) / sizeof(kCalls[0]); i++) {
        if (strcmp(kCalls[i].name, fn_name) == 0) {
            kind = kCalls[i].kind;
            if (block_len == 0) {
                block_len = kCalls[i].len; /* --len 优先，表里的只是缺省 */
            }
        }
    }
    if (kind == NULL) {
        fprintf(stderr, "  (unknown function %s - add it to kCalls)\n",
                fn_name);
        return 2;
    }
    a0 = npos > 3 ? (int)strtol(positional[3], NULL, 0) : 0;
    a1 = npos > 4 ? (int)strtol(positional[4], NULL, 0) : 0;
    a2 = npos > 5 ? (int)strtol(positional[5], NULL, 0) : 0;

#if defined(_WIN32)
    g_lib = (void *)LoadLibraryA(dll);
#else
    g_lib = dlopen(dll, RTLD_NOW);
#endif
    if (g_lib == NULL) {
        fprintf(stderr, "  (cannot load %s)\n", dll);
        return 2;
    }
    freelibhndl = (freelibhndl_fn)sym("cnc_freelibhndl");
    if (freelibhndl == NULL) {
        return 2;
    }
    if (hssb) {
        /* NCGuide 那条路（手册 §4.4）：节点号 9，用 FwlibNCG 那套处理库。 */
        setdefnode_fn setdefnode = (setdefnode_fn)sym("cnc_setdefnode");
        allclibhndl_fn allclibhndl = (allclibhndl_fn)sym("cnc_allclibhndl");

        if (setdefnode != NULL) {
            (void)setdefnode((short)node);
        }
        if (allclibhndl == NULL) {
            fprintf(stderr, "  (no cnc_allclibhndl)\n");
            return 2;
        }
        rc = allclibhndl(&handle);
        printf("cnc_allclibhndl(node %d) = %d, handle=%u\n", node, (int)rc,
               (unsigned)handle);
    } else {
        allclibhndl3 = (allclibhndl3_fn)sym("cnc_allclibhndl3");
        if (allclibhndl3 == NULL) {
            return 2;
        }
        rc = allclibhndl3(host, port, 3, &handle);
        printf("cnc_allclibhndl3(%s:%u) = %d, handle=%u\n", host, port, (int)rc,
               (unsigned)handle);
    }
    if (rc != 0) {
        return 1;
    }

    memset(buf, 0, sizeof(buf));
    printf("%s", fn_name);
    for (i = 3; i < (size_t)npos; i++) {
        printf(" %s", positional[i]);
    }
    printf("\n");

    if (strcmp(kind, "seq") == 0) {
        n_fn rdaxisname_fn = (n_fn)sym("cnc_rdaxisname");
        s2_fn srvdelay_fn = (s2_fn)sym("cnc_srvdelay");
        short num = 8;

        if (rdaxisname_fn == NULL || srvdelay_fn == NULL) {
            return 2;
        }
        rc = rdaxisname_fn(handle, &num, buf);
        printf("  cnc_rdaxisname rc = %d, num = %d\n", (int)rc, (int)num);
        memset(buf, 0, sizeof(buf));
        rc = srvdelay_fn(handle, 1, 8, buf);
        printf("  cnc_srvdelay 1 (len 8) rc = %d\n", (int)rc);
        dump(buf, 64);
        freelibhndl(handle);
        return 0;
    } else if (strcmp(kind, "void") == 0) {
        rc = ((void_fn)sym(fn_name))(handle, buf);
    } else if (strcmp(kind, "s1") == 0) {
        rc = ((s1_fn)sym(fn_name))(handle, (short)a0, buf);
    } else if (strcmp(kind, "s2") == 0) {
        /* s2 的第二个 short 是数据块长度（cnc_absolute 一族）：不给就按 8 字节
         * 单轴算，FOCAS 对长度查得严，给 0 会直接回 EW_ATTRIB 而不发帧。 */
        rc = ((s2_fn)sym(fn_name))(handle, (short)a0,
                                   (short)(block_len > 0 ? block_len : 8), buf);
    } else if (strcmp(kind, "s3") == 0) {
        rc = ((s3_fn)sym(fn_name))(handle, (short)a0, (short)a1, (short)a2, buf);
    } else if (strcmp(kind, "s1_n") == 0) {
        rc = ((s1_n_fn)sym(fn_name))(handle, (short)a0, &num, buf);
    } else if (strcmp(kind, "n") == 0) {
        rc = ((n_fn)sym(fn_name))(handle, &num, buf);
    } else if (strcmp(kind, "s1_n_s1_n") == 0) {
        /*
         * cnc_rdaxisdata(h, cls, short *type, short num, short *len, ODBAXDT*)：
         * 第 2 个 short 是**指针**（就地读写的类型数组），早先这里把 a1 当指针
         * 传了，等于让它去写 a1 那个地址。这里老老实实给一块短数组。
         */
        short ty[16];
        int  k;

        for (k = 0; k < 16; k++) {
            ty[k] = (short)a1;
        }
        rc = ((s1_n_s1_n_fn)sym(fn_name))(handle, (short)a0, ty, num, &len,
                                          buf);
    } else if (strcmp(kind, "s2_n_n") == 0) {
        /* cnc_rdgcode(h, short type, short block, short *num, ODBGCD*) */
        rc = ((s2_n_n_fn)sym(fn_name))(handle, (short)a0, (short)a1, &num,
                                       buf);
    } else if (strcmp(kind, "s2_n") == 0) {
        rc = ((s2_n_fn)sym(fn_name))(handle, (short)a0, &lnum, &len, buf);
    } else if (strcmp(kind, "exec") == 0) {
        rc = ((exec_fn)sym(fn_name))(handle, &handle, &num, (char *)buf);
    } else if (strcmp(kind, "dwn4") == 0 || strcmp(kind, "up4") == 0) {
        /* 三件套：start → 一块 → end。块大小按官方建议的 1024-1400 取 1024
         * （以太网单帧上限 1460）。每一步都打 rc，好看出"哪一步被拒"。 */
        typedef short (*start4_fn)(unsigned short, short, char *);
        typedef short (*xfer4_fn)(unsigned short, long *, char *);
        typedef short (*end4_fn)(unsigned short);
        long want = 1024;
        short rc2 = 0;
        short rc3 = 0;

        memset(buf, 'A', sizeof(buf));
        rc = ((start4_fn)sym(strcmp(kind, "dwn4") == 0 ? "cnc_dwnstart4"
                                                       : "cnc_upstart4"))(
            handle, (short)a0, (char *)name_arg);
        printf("  start4 rc = %d\n", (int)rc);
        rc2 = ((xfer4_fn)sym(strcmp(kind, "dwn4") == 0 ? "cnc_download4"
                                                       : "cnc_upload4"))(
            handle, &want, (char *)buf);
        printf("  %s rc = %d, want = %ld, first bytes: %.16s\n",
               strcmp(kind, "dwn4") == 0 ? "download4" : "upload4", (int)rc2,
               want, buf);
        rc3 = ((end4_fn)sym(strcmp(kind, "dwn4") == 0 ? "cnc_dwnend4"
                                                      : "cnc_upend4"))(handle);
        printf("  end4 rc = %d\n", (int)rc3);
        rc = rc2 != 0 ? rc2 : rc3;
    } else {
        rc = -999;
    }
    printf("  rc = %d\n", (int)rc);
    printf("  count/num = %d %d, len = %d, lnum = %ld\n", (int)num, (int)num2,
           (int)len, lnum);
    dump(buf, 64);
    freelibhndl(handle);
    return 0;
}
