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
 *
 * `--in HEX` 把出参那块 4 KiB 缓冲**先铺上初值**（十六进制字符串，可带空格）。
 * 读的那几条不需要它，**写的那几条必须有**：`cnc_wrparam` 之类是照着入参结构体
 * 发的，全 0 的时候 SDK 自己就在本地判非法（EW_NUMBER），一帧都不发。
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
/* (h, short *, short *)：cnc_rdparanum 那种"两个数量出参"的 */
typedef short (NCL_PROBE_CALL *n_n_fn)(unsigned short, short *, short *);
/* (h, short, short, short, short, short *, void *)：cnc_rdmacror / cnc_rdparar
 * 那种"给一段范围、回数量 + 一堆记录"的 */
typedef short (NCL_PROBE_CALL *s4_n_fn)(unsigned short, short, short, short, short,
                                        short *, void *);
/* (h, short, short, short *, void *)：cnc_rdgcode 那种"两个入参 + 一个数量出参" */
typedef short (NCL_PROBE_CALL *s2_n_n_fn)(unsigned short, short, short, short *,
                                          void *);
/* (h, short, short, long)：cnc_wrtofs / cnc_wrmacro 这种"值直接进参数"的写 */
typedef short (NCL_PROBE_CALL *s2_l_fn)(unsigned short, short, short, long);
/* (h, short, short, void *)：cnc_rdparam / cnc_wrparam 这种"号 + 类型 + 结构体" */
typedef short (NCL_PROBE_CALL *s2_p_fn)(unsigned short, short, short, void *);
/* (h, short, void *)：cnc_rdlife 那种"号 + 结构体" */
typedef short (NCL_PROBE_CALL *s1_p_fn)(unsigned short, short, void *);
/* (h, short, short, short, void *)：cnc_rdtofs / cnc_rdtoolrng 那种"号 + 长度" */
typedef short (NCL_PROBE_CALL *s3_p_fn)(unsigned short, short, short, short,
                                        void *);
/* (h, short, short, short, short, short, void *)：pmc_rdpmcrng 那种"五个 short + 结构体" */
typedef short (NCL_PROBE_CALL *s5_p_fn)(unsigned short, short, short, short, short,
                                        short, void *);
/* (h, short, long)：cnc_wrtofsdrctinp 那种"号 + 值" */
typedef short (NCL_PROBE_CALL *s1_l_fn)(unsigned short, short, long);
/* (h, char *)：cnc_delete / cnc_pdf_del / cnc_pdf_slctmain 的程序名 */
typedef short (NCL_PROBE_CALL *name_fn)(unsigned short, char *);
/* (h, short, char *)：cnc_dwnstart4 那种"类型 + 名字" */
typedef short (NCL_PROBE_CALL *s1_name_fn)(unsigned short, short, char *);
/* (h, short, short, short, long)：cnc_wrtofs(h, ofs_num, type, len, data) */
typedef short (NCL_PROBE_CALL *s3_l_fn)(unsigned short, short, short, short,
                                        long);
/* (h, short, short, long, short)：cnc_wrmacro(h, number, type, data, flag) */
typedef short (NCL_PROBE_CALL *s2_l1_fn)(unsigned short, short, short, long,
                                         short);
/* (h, short, short *, void *)：cnc_rdtooldata(h, type, &num, IODBTLDT *) */
typedef short (NCL_PROBE_CALL *s_np_fn)(unsigned short, short, short *, void *);
/* (h, short *, void *)：cnc_rdexecprog3(h, &num, ODBEXEPRGINFO *) */
typedef short (NCL_PROBE_CALL *np_fn)(unsigned short, short *, void *);
/* (h, short, short, short *, void *)：cnc_rdgcode(h, type, block, &num, od) */
typedef short (NCL_PROBE_CALL *s2_np_fn)(unsigned short, short, short, short *,
                                         void *);
/* (h, short *, short, short *, short *, void *)：cnc_rdparar 那条"三个 short 指针" */
typedef short (NCL_PROBE_CALL *parar_fn)(unsigned short, short *, short, short *,
                                         short *, void *);

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
    /* 写这一侧（语义与读同族，`(h, 结构体*)`）：抓它们的帧用 */
    { "cnc_wrparam", "s1p", 0 }, { "cnc_wrparas", "s1p", 0 },
    { "cnc_wrmacro", "s2_l1", 0 }, { "cnc_wrmacror", "s1p", 0 },
    { "cnc_wrtofs", "s3_l", 0 }, { "cnc_wrtofsr", "s1p", 0 },
    { "cnc_wrwkcdshft", "s1p", 0 }, { "cnc_delprogram", "name", 0 },
    { "cnc_delete", "name", 0 }, { "cnc_pdf_del", "name", 0 },
    { "cnc_pdf_slctmain", "name", 0 },
    { "cnc_rdparanum", "void", 0 }, { "cnc_rdparar", "s4_n", 0 },
    { "cnc_rdwkcdshft", "s2p", 0 }, { "cnc_rdwkcdshft2", "s2p", 0 },
    { "cnc_rdtooldata", "s_np", 0 }, { "cnc_rdtoolrng", "s3p", 0 },
    { "cnc_rdtoollife_data", "s2_n", 8 },
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
    /*
     * PMC（FANUC 的 PLC 就是 PMC）：`pmc_rdpmcrng(h, adr_type, data_type, s_no, e_no,`
     * `length, IODBPMC*)`。adr_type：0=G、1=F、2=Y、3=X、4=A、5=R、6=T、7=K、8=C、
     * 9=D；data_type：0=字节、1=字、2=长字；**length = 8 + N×每点字节数**、
     * 而 N = `e_no - s_no`（spec 的例子：D0100 字型 `s=100 e=101 length=10` 读 1 个）。
     * 写是 `pmc_wrpmcrng(h, length, IODBPMC*)`（形状 s1p）。
     */
    { "pmc_rdpmcrng", "s5p", 0 }, { "pmc_wrpmcrng", "s1p", 0 },
    /* PMC 各族的实际号段（`pmc_rdpmcinfo(h, adr_type, ODBPMCINF*)`，-1 = 全族） */
    /* PMC 另外几族（2026-09-23 抓帧用）：
     *   pmc_rdalmmsg(h, type, short *num, short *nmsg, ODBPMCALM*) —— PMC 报警文本
     *   pmc_rdcntlgrp / pmc_rdcntl_exrelay_grp(h, short *num) —— 控制数据组数
     *   pmc_rdcntldata / pmc_rdcntlexrelay(h, type, group, num, IODBPMCCNTL*) —— 控制数据 */
    { "pmc_rdalmmsg", "alm", 0 }, { "pmc_rdcntlgrp", "np", 0 },
    { "pmc_rdcntl_exrelay_grp", "np", 0 }, { "pmc_rdcntldata", "s3p", 0 },
    { "pmc_rdcntlexrelay", "s3p", 0 },
    { "pmc_rdpmcinfo", "s1p", 0 },
    /* 定时器 / 计数器（结构体那两个，先只看帧） */
    { "pmc_rdpmctm", "s1p", 0 }, { "pmc_rdpmccnt", "s1p", 0 },
    /* 刀补/参数/宏变量（帧抓到了、字段还没核） */
    { "cnc_rdtofs", "s3p", 0 }, { "cnc_rdparam", "s3p", 0 },
    { "cnc_rdmacro", "s2", 12 }, { "cnc_rddt", "s1", 0 },
    /* 整表那几条（范围 + 数量出参） */
    { "cnc_rdparanum", "n", 0 }, { "cnc_rdparar", "s4_n", 0 },
    { "cnc_rdmacror", "s3p", 0 },
    { "cnc_rdtooldata", "s2_n", 8 }, { "cnc_rdtoolrng", "s2_n", 8 },
    /* 工件坐标/模态 */
    { "cnc_rdgcode", "s2_n_n", 0 }, { "cnc_rdwkcdshft", "s2", 0 },
    /*
     * 工件零点偏移（G54…）：`cnc_rdzofs(h, number, axis, length, IODBZOFS*)` ——
     * `number` = 偏移号（1 = G54…）、`axis` = 轴号（-1 = ALL_AXES）、`length` =
     * 出参结构的字节数。`cnc_rdwkcdshft` 那条第 3 个参数是**轴号**（不是长度），
     * 两个别混。`cnc_zofs_rnge` 问的是"这个号/轴合不合法"。
     */
    { "cnc_rdzofs", "s3p", 0 }, { "cnc_zofs_rnge", "s2p", 0 },
    /* 写工件零点偏移（"对拍"用：写一个显眼的值进去，再读回来看载荷里它是哪几个字节） */
    { "cnc_wrzofs", "s1p", 0 }, { "cnc_rdzofsr", "s1p", 0 },
    /* 一条连接内的"写 → 读"对拍（见下面 kind == "zofs" 那段） */
    { "cnc_zofs", "zofs", 0 },
    /*
     * 指令值（**当前刀号就在这条里**）：`cnc_rdcommand(h, type, block, &num, ODBCMD*)`
     * —— `type` 0..29 模态（非 G 码）、-1 全读、100..129 指令值逐条、-2 全读；
     * 应答每条 12 字节 `{adrs, num, flag(2), cmd_val(4), dec_val(4)}`，`adrs` 是
     * 字母（'T'/'M'/'S'/'F'…）。`-2` 一次把 T/M/S/F 都拿回来。
     */
    { "cnc_rdcommand", "s2_n_n", 0 },
    /* 模态（老系列那条；0i-D/F 上 G 码走 cnc_rdgcode，其余走 cnc_rdcommand） */
    { "cnc_modal", "s2p", 0 },
    /* 执行中程序的信息（含**子程序号**）：cnc_rdexecprog3(h, &num, ODBEXEPRGINFO*) */
    { "cnc_rdexecprog3", "np", 0 },
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
    /*
     * 上行的**别的代际**：`cnc_upstart`（第一代，按号）与 `cnc_upstart3`
     * （第三代，按号段）。第四代（`up4`，按文件名/目录）在这台模拟器上 start 通、
     * 但 `0x18` 数据请求机床不答 —— 换代际试试是不是另一条路能读。
     */
    { "cnc_upstart", "up1", 0 }, { "cnc_upstart3", "up3", 0 },
    /* 同一个会话里"先下行一小段、再上行" —— 试机床的上行要不要先被下行"点着"。 */
    { "cnc_up-after-dwn", "upafter", 0 },
    /* 程序文件夹/主程序那一族：上传的 0x18 很可能跟"当前文件夹"有关。 */
    { "cnc_pdf_rdmain", "pdfinfo", 0 }, { "cnc_rdpdf_curdir", "pdfinfo", 0 },
    { "cnc_wrpdf_curdir", "pdfset", 0 },
    /* 读整段程序：cnc_pdf_wractpt（指针挪到第 N 块）+ cnc_rdexecprog（读指针处的文本）循环 */
    { "cnc_pdf_rdactpt", "progread", 0 }, { "cnc_pdf_wractpt", "progread", 0 },
    { "cnc_rdexecprog", "progread", 0 },
    /* 按文件名按行读内容（手册标"以太网不支持"，但模拟器可能照答） */
    { "cnc_rdpdf_line", "rdline", 0 },
    { "cnc_rdpdf_alldir", "alldir", 0 }, { "cnc_rdpdf_inf", "rdinf", 0 },
    /* 写这一侧的文件接口：建文件 + 按行写（手册也标 HSSB 专用，但先问一句） */
    { "cnc_pdf_add", "pdfadd", 0 }, { "cnc_wrpdf_line", "wrline", 0 },
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
    const char *seed = NULL;   /* --in HEX：出参/入参那块缓冲的初值 */
    const char *shape = NULL;  /* --shape：临时改调用形状（表里的只是缺省） */
    const char *data_arg = NULL; /* --data TEXT：程序下行的正文（第一行 = 程序号） */
    const char *data_file = NULL; /* --data-file F：正文从文件读（批处理里好传） */
    bool        hssb = false;  /* --hssb：走 cnc_allclibhndl（节点号，NCGuide 用 9） */
    int         node = 9;
    const char *positional[8];
    int npos = 0;
    int block_len = 0;
    unsigned short handle = 0;
    unsigned char buf[NCL_PROBE_BUF];
    short num = 8;
    short num2 = 8;
    short num3 = 0;
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
        } else if (strcmp(argv[i], "--in") == 0 && i + 1 < (size_t)argc) {
            seed = argv[++i];
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < (size_t)argc) {
            data_arg = argv[++i];
        } else if (strcmp(argv[i], "--data-file") == 0 && i + 1 < (size_t)argc) {
            data_file = argv[++i];
        } else if (strcmp(argv[i], "--shape") == 0 && i + 1 < (size_t)argc) {
            shape = argv[++i];
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
    if (shape != NULL) {
        kind = shape;
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

    /* `--in` 铺的初值要在调用前就位（写的那几条全靠它）。 */
    memset(buf, 0, sizeof(buf));
    if (seed != NULL) {
        size_t k = 0;
        int nibble = -1;

        for (i = 0; seed[i] != '\0'; i++) {
            int hi = -1;

            if (seed[i] >= '0' && seed[i] <= '9') {
                hi = seed[i] - '0';
            } else if (seed[i] >= 'a' && seed[i] <= 'f') {
                hi = seed[i] - 'a' + 10;
            } else if (seed[i] >= 'A' && seed[i] <= 'F') {
                hi = seed[i] - 'A' + 10;
            } else {
                continue; /* 空格/冒号之类当分隔符 */
            }
            if (nibble < 0) {
                nibble = hi;
            } else {
                if (k < sizeof(buf)) {
                    buf[k] = (unsigned char)((nibble << 4) | hi);
                    k++;
                }
                nibble = -1;
            }
        }
    }
    printf("%s", fn_name);
    for (i = 3; i < (size_t)npos; i++) {
        printf(" %s", positional[i]);
    }
    printf("\n");

    if (strcmp(kind, "pdfadd") == 0) {
        /*
         * `cnc_pdf_add(h, char *name, short type, char *comment)`：
         * `type` 0 = 文件 / 1 = 文件夹（照手册），`--name` 给路径，
         * 注释用 `--data`（可省）。
         */
        typedef short (NCL_PROBE_CALL *add_fn)(unsigned short, char *, short,
                                               char *);

        rc = ((add_fn)sym("cnc_pdf_add"))(handle, (char *)name_arg, (short)a0,
                                          (char *)name_arg);
        printf("  cnc_pdf_add('%s', type=%d) rc = %d\n", name_arg, a0, (int)rc);
    } else if (strcmp(kind, "wrline") == 0) {
        /*
         * `cnc_wrpdf_line(h, char *prog_name, unsigned long line_no, char *prog_data,
         *                 unsigned long *line_len, unsigned long *data_len)`：
         * 从第 `a0` 行起把 `--in` 那段文本写进去（`--len` 是文本长度）。
         */
        typedef short (NCL_PROBE_CALL *wrline_fn)(unsigned short, char *, 
                                                  unsigned long, char *,
                                                  unsigned long *,
                                                  unsigned long *);
        unsigned long line_len = 1;
        unsigned long data_len = (unsigned long)strlen(seed != NULL ? seed : "");

        rc = ((wrline_fn)sym("cnc_wrpdf_line"))(handle, (char *)name_arg,
                                                (unsigned long)a0,
                                                (char *)(seed != NULL ? seed : ""),
                                                &line_len, &data_len);
        printf("  cnc_wrpdf_line('%s', line %d, %lu 行 / %lu 字符) rc = %d\n",
               name_arg, a0, line_len, data_len, (int)rc);
    } else if (strcmp(kind, "rdline") == 0) {
        /*
         * `cnc_rdpdf_line(h, char *prog_name, unsigned long line_no, char *prog_data,
         *                 unsigned long *line_len, unsigned long *data_len)`
         * —— **按文件名、按行读程序内容**（手册标以太网不支持，但先问一句试）。
         * `a0` = 起始行号（0 = 程序头），`a1` = 要读几行，`--len` 不用于这里，
         * 读的字符数给 `--count`（缺省 1024）。
         */
        typedef short (NCL_PROBE_CALL *rdline_fn)(unsigned short, char *,
                                                  unsigned long, char *,
                                                  unsigned long *,
                                                  unsigned long *);
        unsigned long line_len = (unsigned long)(a1 > 0 ? a1 : 32);
        unsigned long data_len = (unsigned long)(num > 0 ? num : 1024);

        memset(buf, 0, sizeof(buf));
        rc = ((rdline_fn)sym("cnc_rdpdf_line"))(handle, (char *)name_arg,
                                                (unsigned long)a0, (char *)buf,
                                                &line_len, &data_len);
        printf("  cnc_rdpdf_line('%s', line %d, %lu 行 / %lu 字符) rc = %d\n",
               name_arg, a0, line_len, data_len, (int)rc);
        printf("  读回 %lu 字符：\n%.*s\n", data_len, (int)data_len,
               (char *)buf);
    } else if (strcmp(kind, "alldir") == 0) {
        /* `cnc_rdpdf_alldir(h, short *num, IDBPDFADIR *id, ODBPDFADIR *od)` */
        typedef short (NCL_PROBE_CALL *alldir_fn)(unsigned short, short *, void *,
                                                  void *);
        short count = (short)(num > 0 ? num : 8);

        memset(buf, 0, sizeof(buf));
        rc = ((alldir_fn)sym("cnc_rdpdf_alldir"))(handle, &count, buf,
                                                  buf + 1024);
        printf("  cnc_rdpdf_alldir('%s', num=%d) rc = %d\n", name_arg, (int)count,
               (int)rc);
        dump(buf + 1024, 128);
    } else if (strcmp(kind, "rdinf") == 0) {
        /* `cnc_rdpdf_inf(h, char *name, short type, ODBPDFINF *inf)` */
        typedef short (NCL_PROBE_CALL *inf_fn)(unsigned short, char *, short,
                                               void *);

        memset(buf, 0, 256);
        rc = ((inf_fn)sym("cnc_rdpdf_inf"))(handle, (char *)name_arg  ,
                                            (short)a0, buf);
        printf("  cnc_rdpdf_inf('%s', type=%d) rc = %d\n", name_arg, a0, (int)rc);
        dump(buf, 64);
    } else if (strcmp(kind, "pdfinfo") == 0) {
        /*
         * 程序文件夹/主程序这一族（`cnc_pdf_rdmain` / `cnc_rdpdf_curdir` /
         * `cnc_rdpdf_drive`）—— 上行的 `0x18` 一直没人答，先看看机床自己认为
         * "当前文件夹 / 主程序 / 盘"是什么。
         */
        typedef short (NCL_PROBE_CALL *rdmain_fn)(unsigned short, char *);
        typedef short (NCL_PROBE_CALL *curdir_fn)(unsigned short, short, char *);
        typedef short (NCL_PROBE_CALL *drive_fn)(unsigned short, void *);
        char path[256];
        int t;

        memset(path, 0, sizeof(path));
        rc = ((rdmain_fn)sym("cnc_pdf_rdmain"))(handle, path);
        printf("  cnc_pdf_rdmain rc = %d, main = '%s'\n", (int)rc, path);
        for (t = 0; t <= 2; t++) {
            memset(path, 0, sizeof(path));
            rc = ((curdir_fn)sym("cnc_rdpdf_curdir"))(handle, (short)t, path);
            printf("  cnc_rdpdf_curdir(type=%d) rc = %d, cur = '%s'\n", t,
                   (int)rc, path);
        }
        memset(buf, 0, 128);
        rc = ((drive_fn)sym("cnc_rdpdf_drive"))(handle, buf);
        printf("  cnc_rdpdf_drive rc = %d\n", (int)rc);
        dump(buf, 64);
    } else if (strcmp(kind, "pdfset") == 0) {
        /* 把"当前文件夹"设成 //CNC_MEM/USER/PATH1/，然后立刻试上行。 */
        typedef short (NCL_PROBE_CALL *curdir_fn)(unsigned short, short, char *);
        typedef short (NCL_PROBE_CALL *start4_fn)(unsigned short, short, char *);
        typedef short (NCL_PROBE_CALL *xfer4_fn)(unsigned short, long *, char *);
        typedef short (NCL_PROBE_CALL *end4_fn)(unsigned short);
        long got = 1024;
        short r2 = 0;
        short r3 = 0;
        int t;

        for (t = 0; t <= 1; t++) {
            rc = ((curdir_fn)sym("cnc_wrpdf_curdir"))(handle, (short)t,
                                                      (char *)name_arg);
            printf("  cnc_wrpdf_curdir(type=%d, '%s') rc = %d\n", t, name_arg,
                   (int)rc);
        }
        rc = ((start4_fn)sym("cnc_upstart4"))(handle, 0, "O3001");
        printf("  upstart4('O3001') rc = %d\n", (int)rc);
        memset(buf, 0, sizeof(buf));
        r2 = ((xfer4_fn)sym("cnc_upload4"))(handle, &got, (char *)buf);
        printf("  upload4 rc = %d, got = %ld, 取回：%.120s\n", (int)r2, got,
               (char *)buf);
        r3 = ((end4_fn)sym("cnc_upend4"))(handle);
        printf("  upend4 rc = %d\n", (int)r3);
        rc = r2 != 0 ? r2 : r3;
    } else if (strcmp(kind, "upafter") == 0) {
        /*
         * 同一个会话里先做一次**下行**（写一份很小的程序），紧接着做**上行**：
         * 试机床的上行要不要先被一次成功的下行"点着"（它现在对 0x18 一声不响）。
         */
        typedef short (NCL_PROBE_CALL *start4_fn)(unsigned short, short, char *);
        typedef short (NCL_PROBE_CALL *xfer4_fn)(unsigned short, long *, char *);
        typedef short (NCL_PROBE_CALL *end4_fn)(unsigned short);
        static const char kProg[] = "\nO0009\nM30\n%";
        long len = (long)sizeof(kProg) - 1;
        long got = 1024;
        short r2 = 0;
        short r3 = 0;

        rc = ((start4_fn)sym("cnc_dwnstart4"))(handle, 0,
                                               "//CNC_MEM/USER/PATH1/");
        printf("  [先下行] start4 rc = %d\n", (int)rc);
        memcpy(buf, kProg, (size_t)len);
        r2 = ((xfer4_fn)sym("cnc_download4"))(handle, &len, (char *)buf);
        printf("  [先下行] download4 rc = %d, len = %ld\n", (int)r2, len);
        r3 = ((end4_fn)sym("cnc_dwnend4"))(handle);
        printf("  [先下行] dwnend4 rc = %d\n", (int)r3);
        memset(buf, 0, sizeof(buf));
        rc = ((start4_fn)sym("cnc_upstart4"))(handle, 0, "O0009");
        printf("  [再上行] upstart4 rc = %d\n", (int)rc);
        r2 = ((xfer4_fn)sym("cnc_upload4"))(handle, &got, (char *)buf);
        printf("  [再上行] upload4 rc = %d, got = %ld, 取回：%.120s\n", (int)r2,
               got, (char *)buf);
        r3 = ((end4_fn)sym("cnc_upend4"))(handle);
        printf("  [再上行] upend4 rc = %d\n", (int)r3);
        rc = r2 != 0 ? r2 : r3;
    } else if (strcmp(kind, "seq") == 0) {
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
    } else if (strcmp(kind, "zofs") == 0) {
        /*
         * 工件零点偏移的"对拍"：一条连接里
         *   ① `cnc_rdaxisname`（官方库的轴数闸门要问过一次轴表才放行）
         *   ② `cnc_wrzofs(h, number, IODBZOFS*)` 写三个显眼的值（0x3039/0x5BA0/0x8707）
         *   ③ `cnc_rdzofs(h, number, ALL_AXES, 136, IODBZOFS*)` 读回来
         * 两边都打印 —— 载荷里那三个值落在哪几个字节，就是客户端该切的偏移。
         * `a0` = 偏移号（1 = G54）。
         */
        n_fn axisname_fn = (n_fn)sym("cnc_rdaxisname");
        s1_p_fn wr_fn = (s1_p_fn)sym("cnc_wrzofs");
        s3_p_fn rd_fn = (s3_p_fn)sym("cnc_rdzofs");
        short anum = 8;
        short number = (short)(a0 > 0 ? a0 : 1);
        static const long kValues[3] = { 12345L, 23456L, 34567L };
        int k;

        if (axisname_fn == NULL || wr_fn == NULL || rd_fn == NULL) {
            fprintf(stderr, "  (缺函数)\n");
            return 2;
        }
        rc = axisname_fn(handle, &anum, buf);
        printf("  cnc_rdaxisname rc = %d, 轴数 = %d\n", (int)rc, (int)anum);
        /* IODBZOFS = {short datano; short type; long data[MAX_AXIS]}（0iD 那版没有 dummy） */
        memset(buf, 0, sizeof(buf));
        buf[0] = (unsigned char)number;
        /*
         * `a1` 选写的形状（官方库拒了就得试；EW_LENGTH=2 说明它自己先挡了）：
         *   0 = type=ALL_AXES、结构体不带 dummy（Fwlib64 头里 iodbzofs 这版）
         *   1 = type=1（单轴 X）
         *   2 = type=ALL_AXES、结构体带 4 字节 dummy（iodbzofs64 那版的样子）
         */
        if (a1 == 1) {
            buf[2] = 1;
            buf[3] = 0;
        } else {
            buf[2] = 0xff;
            buf[3] = 0xff;
        }
        for (k = 0; k < 3; k++) {
            memcpy(buf + (a1 == 2 ? 8 : 4) + 4 * k, &kValues[k], 4);
        }
        rc = wr_fn(handle, number, buf);
        printf("  cnc_wrzofs(%d) rc = %d\n", (int)number, (int)rc);
        memset(buf, 0, sizeof(buf));
        rc = rd_fn(handle, number, -1, 136, buf);
        printf("  cnc_rdzofs(%d, ALL_AXES, 136) rc = %d\n", (int)number, (int)rc);
        dump(buf, 48);
        /* 单个轴再读一次（同一号、axis = 1） */
        memset(buf, 0, sizeof(buf));
        rc = rd_fn(handle, number, 1, 136, buf);
        printf("  cnc_rdzofs(%d, 1, 136) rc = %d\n", (int)number, (int)rc);
        dump(buf, 32);
        rc = 0;
    } else if (strcmp(kind, "up1") == 0 || strcmp(kind, "up3") == 0) {
        /*
         * 老代际的上行：`cnc_upstart(h, type)` / `cnc_upstart3(h, type, s_no, e_no)`
         * → `cnc_upload(h, ODBUP *, unsigned short *)` / `cnc_upload3(h, long *, char *)`
         * → `cnc_upend(h)`。答出来的文本直接印前 200 字符。
         */
        typedef short (NCL_PROBE_CALL *up1_start_fn)(unsigned short, short);
        typedef short (NCL_PROBE_CALL *up1_xfer_fn)(unsigned short, void *,
                                                    unsigned short *);
        typedef short (NCL_PROBE_CALL *up3_start_fn)(unsigned short, short, long,
                                                     long);
        typedef short (NCL_PROBE_CALL *up3_xfer_fn)(unsigned short, long *,
                                                    char *);
        typedef short (NCL_PROBE_CALL *up_end_fn)(unsigned short);
        short rc2 = 0;
        short rc3 = 0;
        unsigned short olen = 0;

        if (strcmp(kind, "up1") == 0) {
            rc = ((up1_start_fn)sym("cnc_upstart"))(handle, (short)a0);
            printf("  cnc_upstart(%d) rc = %d\n", a0, (int)rc);
        } else {
            long s_no = npos > 3 ? strtol(positional[3], NULL, 0) : 0;
            long e_no = npos > 4 ? strtol(positional[4], NULL, 0) : 0;

            rc = ((up3_start_fn)sym("cnc_upstart3"))(handle, (short)a0, s_no,
                                                     e_no);
            printf("  cnc_upstart3(%d, %ld, %ld) rc = %d\n", a0, s_no, e_no,
                   (int)rc);
        }
        memset(buf, 0, sizeof(buf));
        if (strcmp(kind, "up1") == 0) {
            rc2 = ((up1_xfer_fn)sym("cnc_upload"))(handle, buf, &olen);
            printf("  cnc_upload rc = %d, len = %u\n", (int)rc2,
                   (unsigned)olen);
        } else {
            long want = (long)sizeof(buf) - 1;

            rc2 = ((up3_xfer_fn)sym("cnc_upload3"))(handle, &want, (char *)buf);
            printf("  cnc_upload3 rc = %d, want = %ld\n", (int)rc2, want);
        }
        printf("  取回的前 120 字节：%.120s\n", (char *)buf);
        rc3 = ((up_end_fn)sym(strcmp(kind, "up1") == 0 ? "cnc_upend"
                                                       : "cnc_upend3"))(handle);
        printf("  upend rc = %d\n", (int)rc3);
        rc = rc2 != 0 ? rc2 : rc3;
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
    } else if (strcmp(kind, "s4") == 0) {
        /* (h, short, short, short, short, void *)：cnc_wrmacror 这种"类型 + 号段" */
        typedef short (NCL_PROBE_CALL *s4_fn)(unsigned short, short, short, short,
                                              short, void *);
        short a3 = (short)(npos > 6 ? strtol(positional[6], NULL, 0) : 0);

        rc = ((s4_fn)sym(fn_name))(handle, (short)a0, (short)a1, (short)a2, a3,
                                   buf);
    } else if (strcmp(kind, "parar") == 0) {
        /*
         * `cnc_rdparar(h, short *s_no, short e_no, short *s_type, short *e_type,
         *              void *data)` —— 三个出/入 short 各自的来路不一样，所以
         * 这里明着摆：`--count` 给 s_no/s_type（同一个值）、`a0` 给 e_no、
         * `a1` 给 e_type。
         */
        short s_type = num;
        short e_type = (short)a1;

        rc = ((parar_fn)sym(fn_name))(handle, &num, (short)a0, &s_type,
                                      &e_type, buf);
        num2 = s_type;
        num3 = e_type;
    } else if (strcmp(kind, "parar2") == 0) {
        /* 全明着给：`a0` = s_no、`a1` = s_type、`a2` = e_no、`a3` = e_type
         * （机床那头看到的 d/e/a2/a3 就是这四个数，见 01 册 §11.13）。 */
        short s_no = (short)a0;
        short s_type = (short)a1;
        short e_no = (short)a2;
        short e_type = (short)(npos > 5 ? strtol(positional[5], NULL, 0) : 0);

        rc = ((parar_fn)sym(fn_name))(handle, &s_no, e_no, &s_type, &e_type,
                                      buf);
        num = s_no;
        num2 = s_type;
        num3 = e_type;
    } else if (strcmp(kind, "s2_l") == 0) {
        /* (h, short, short, long)：cnc_wrtofs / cnc_wrmacro，值直接进参数 */
        rc = ((s2_l_fn)sym(fn_name))(handle, (short)a0, (short)a1, (long)a2);
    } else if (strcmp(kind, "s2p") == 0) {
        rc = ((s2_p_fn)sym(fn_name))(handle, (short)a0, (short)a1, buf);
    } else if (strcmp(kind, "s3p") == 0) {
        rc = ((s3_p_fn)sym(fn_name))(handle, (short)a0, (short)a1, (short)a2,
                                     buf);
    } else if (strcmp(kind, "s5p") == 0) {
        /*
         * `pmc_rdpmcrng(h, adr_type, data_type, s_number, e_number, length, IODBPMC*)`：
         * 五个 short = `a0`(族) `a1`(字节/字/长字) `a2`(起始号) `a3`(结束号) `length`，
         * 其中 `length` 取自 `--count`（**= 8 + N×每点字节数**，见 spec）。
         */
        rc = ((s5_p_fn)sym(fn_name))(
            handle, (short)a0, (short)a1, (short)a2,
            (short)(npos > 6 ? strtol(positional[6], NULL, 0) : 0), num, buf);
    } else if (strcmp(kind, "s3_l") == 0) {
        long data = (long)(npos > 5 ? strtol(positional[5], NULL, 0) : 0);

        rc = ((s3_l_fn)sym(fn_name))(handle, (short)a0, (short)a1, (short)a2,
                                     data);
    } else if (strcmp(kind, "s2_l1") == 0) {
        long data = (long)(npos > 5 ? strtol(positional[5], NULL, 0) : 0);
        short flag = (short)(npos > 6 ? strtol(positional[6], NULL, 0) : 0);

        rc = ((s2_l1_fn)sym(fn_name))(handle, (short)a0, (short)a1, data, flag);
    } else if (strcmp(kind, "s_np") == 0) {
        rc = ((s_np_fn)sym(fn_name))(handle, (short)a0, &num, buf);
    } else if (strcmp(kind, "np") == 0) {
        rc = ((np_fn)sym(fn_name))(handle, &num, buf);
    } else if (strcmp(kind, "alm") == 0) {
        /* pmc_rdalmmsg(h, type, short *num, short *nmsg, ODBPMCALM*)：
         * num = 起始报警号（a1）、nmsg = 要几条（--count）。 */
        typedef short (NCL_PROBE_CALL *alm_fn)(unsigned short, short, short *,
                                               short *, void *);
        short start = (short)a1;
        short nmsg = num;

        rc = ((alm_fn)sym(fn_name))(handle, (short)a0, &start, &nmsg, buf);
        num2 = nmsg;
        lnum = start;
    } else if (strcmp(kind, "s2_np") == 0) {
        rc = ((s2_np_fn)sym(fn_name))(handle, (short)a0, (short)a1, &num, buf);
    } else if (strcmp(kind, "s1p") == 0) {
        rc = ((s1_p_fn)sym(fn_name))(handle, (short)a0, buf);
    } else if (strcmp(kind, "s1_l") == 0) {
        rc = ((s1_l_fn)sym(fn_name))(handle, (short)a0, (long)a1);
    } else if (strcmp(kind, "name") == 0) {
        rc = ((name_fn)sym(fn_name))(handle, (char *)name_arg);
    } else if (strcmp(kind, "s1_name") == 0) {
        rc = ((s1_name_fn)sym(fn_name))(handle, (short)a0, (char *)name_arg);
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
    } else if (strcmp(kind, "n_n") == 0) {
        /* (h, short *, short *)：cnc_rdparanum 那种"两个数量出参" */
        short second = num2;

        rc = ((n_n_fn)sym(fn_name))(handle, &num, &second);
        num2 = second;
    } else if (strcmp(kind, "s4_n") == 0) {
        /* (h, s1, e1, s2, e2, short *num, buf)：读一段（参数/宏变量） */
        rc = ((s4_n_fn)sym(fn_name))(handle, (short)a0, (short)a1, (short)a2,
                                     (short)(npos > 6 ? strtol(positional[6], NULL, 0)
                                                      : 0),
                                     &num, buf);
    } else if (strcmp(kind, "exec") == 0) {
        rc = ((exec_fn)sym(fn_name))(handle, &handle, &num, (char *)buf);
    } else if (strcmp(kind, "dwn4") == 0 || strcmp(kind, "up4") == 0) {
        /* 三件套：start → 一块 → end。块大小按官方建议的 1024-1400 取 1024
         * （以太网单帧上限 1460）。每一步都打 rc，好看出"哪一步被拒"。 */
        typedef short (*start4_fn)(unsigned short, short, char *);
        typedef short (*xfer4_fn)(unsigned short, long *, char *);
        typedef short (*end4_fn)(unsigned short);
        /* `--count N` 可以改读/写长度（上行 `*length` 按 spec 要 ≥256 且是 256 的倍数）。 */
        long want = num > 0 ? (long)num : 1024;
        short rc2 = 0;
        short rc3 = 0;

        /*
         * 下行要送的那一段：`--data TEXT` 给（**程序的第一行必须是程序号**，
         * 例如 `O0001`，这是 FANUC 的规矩 —— 目录给的是文件夹）。不给就铺 'A'
         * （只用来核帧形状，不是一份合法的程序）。
         */
        if (data_file != NULL) {
            /* 从文件读正文（二进制原样）：批处理/并行测试里传多行文本省事。 */
            FILE *fh = fopen(data_file, "rb");

            if (fh == NULL) {
                fprintf(stderr, "  (打不开 %s)\n", data_file);
                freelibhndl(handle);
                return 2;
            }
            want = (long)fread(buf, 1, sizeof(buf), fh);
            fclose(fh);
        } else if (data_arg != NULL) {
            size_t n = strlen(data_arg);

            if (n > sizeof(buf)) {
                n = sizeof(buf);
            }
            memcpy(buf, data_arg, n);
            want = (long)n;
        } else {
            memset(buf, 'A', sizeof(buf));
        }
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
        /* 细码要**紧跟着失败的那一条**问（后面的调用会把它盖掉）。 */
        {
            typedef short (NCL_PROBE_CALL *dtail_fn)(unsigned short, void *);
            dtail_fn dtail = (dtail_fn)sym("cnc_getdtailerr");

            if (dtail != NULL && rc2 != 0) {
                unsigned char e[8];
                short drc;

                memset(e, 0, sizeof(e));
                drc = dtail(handle, e);
                printf("  [紧跟在 upload4/download4 之后] getdtailerr rc = %d, err_no %d / err_dtno %d\n",
                       (int)drc, (short)((e[0] << 8) | e[1]),
                       (short)((e[2] << 8) | e[3]));
            }
        }
        rc3 = ((end4_fn)sym(strcmp(kind, "dwn4") == 0 ? "cnc_dwnend4"
                                                      : "cnc_upend4"))(handle);
        printf("  end4 rc = %d\n", (int)rc3);
        /*
         * 官方手册说这类错要问 `cnc_getdtailerr` 要细码（`ODBERR = {short err_no;
         * short err_dtno;}`）—— "程序不在指定范围里" / "NC 程序内存坏了" 之类。
         */
        {
            typedef short (NCL_PROBE_CALL *dtail_fn)(unsigned short, void *);
            dtail_fn dtail = (dtail_fn)sym("cnc_getdtailerr");

            if (dtail != NULL) {
                memset(buf, 0, 16);
                rc = dtail(handle, buf);
                {
                    int err_no = (short)((buf[0] << 8) | buf[1]);
                    int err_dtno = (short)((buf[2] << 8) | buf[3]);

                    printf("  cnc_getdtailerr rc = %d, ODBERR = err_no %d / err_dtno %d\n",
                           (int)rc, err_no, err_dtno);
                }
            }
        }
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
