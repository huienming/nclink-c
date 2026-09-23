/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - FANUC FOCAS / Fwlib32 over TCP.
 *
 * The session is the connection plus the negotiation of §2.3: a `func 1`
 * hello, then the capability probe the vendor SDK sends before any data call.
 * After that every read is one `func 0x21` frame carrying command blocks and
 * one reply carrying the same number of answer blocks - the rule that took
 * this protocol from "-17 forever" to the SDK answering rc=0.
 *
 * Every exchange goes through one mutex: one session, one request in flight.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_platform.h"
#include "nclink/ncl_socket.h"
#include "nclink/clients/focas.h"
#include "focas/ncl_focas_pdu.h"

/** The body size class of §2.2 rule 4 keeps a reply under 3470 bytes. */
#define FOCAS_MAX_BODY 4096
#define FOCAS_MAX_FRAME (NCL_FOCAS_HEADER + FOCAS_MAX_BODY)
/** 一个请求最多几个块：`cnc_rdposition` 一族实测 9 个（§2.5），留到 12。 */
#define FOCAS_MAX_CB NCL_FOCAS_ITEM_CBS

typedef struct {
    char      *host;
    unsigned   port;
    /*
     * 两条 TCP 是**分工**的，不是主备（2026-09-23 实测，01 册 §11.18）：
     *
     *   hello 计数器 **1** 那条 = **传输通道**：0x11/0x12/0x13（下行）、
     *                             0x15/0x18/0x19（上行）只能走这条；往它上面发普通
     *                             命令块（func 0x21）机床直接 RST。
     *   hello 计数器 **2** 那条 = **命令通道**：func 0x21 的读写都走这条；往它上面
     *                             发传输帧同样 RST。
     *
     * 注意分的是**hello 里那个计数器**，不是"先连上的那条"：把两边的计数器对调，
     * 角色跟着对调（同轮实测）。默认 `hello_counter = 1`，所以第一条正好是传输通道
     * —— 只不过那是同一个数的两种说法，别按"第几条"去理解。
     */
    ncl_socket *socket;   /**< 命令通道（hello 计数器 2）：所有 func 0x21 走这条 */
    ncl_socket *control;  /**< 先开的那条 TCP：只留个指针，角色见 transfer        */
    ncl_socket *transfer; /**< 传输通道（hello 计数器 1）：程序上下行那一族走这条 */
    unsigned   connect_timeout_ms;
    unsigned   timeout_ms;
    unsigned   retries;
    bool       negotiate;
    unsigned   hello_counter;
    ncl_mutex *mutex;
    uint8_t    tx[FOCAS_MAX_FRAME];
    uint8_t    rx[FOCAS_MAX_FRAME];
    size_t     last_tx_len; /**< what the audit should show */
    size_t     last_rx_len;
    /* what the hello and the probe found, reported by call("session") */
    bool       session;
    size_t     hello_records; /**< n from the `func 1` reply */
    uint16_t   hello_field2;  /**< body[2..4), the branch §2.3 documents */
    size_t     probe_blocks;  /**< blocks the machine offers */
} focas_ctx;

static unsigned json_uint(const ncl_json *object, const char *key,
                          unsigned fallback)
{
    long long value = ncl_json_obj_get_int(object, key, (long long)fallback);

    return value < 0 ? 0u : (unsigned)value;
}

/** 写上一个大端 u32（`call("payload")` 覆盖 Cb 的 d/e/arg2/arg3 时用）。 */
static void driver_put_u32be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

/* --------------------------------------------------------------- session -- */

/**
 * 一条 `func 0x02`（会话结束）：官方 SDK 在 `cnc_freelibhndl` 时对**两条**连接各发
 * 一条，机床回一条空的（§2.1 实测）。这里只当礼节，失败不报。
 */
static ncl_err focas_bye(focas_ctx *ctx, ncl_socket *socket);

/** 丢掉两条连接，不发 bye（错误路径上用：那一刻连接多半已经不通了）。 */
static void focas_drop_session(focas_ctx *ctx)
{
    if (ctx->socket != NULL) {
        ncl_socket_close(ctx->socket);
        ctx->socket = NULL;
    }
    if (ctx->transfer != NULL && ctx->transfer != ctx->control) {
        ncl_socket_close(ctx->transfer);
    }
    ctx->transfer = NULL;
    if (ctx->control != NULL) {
        ncl_socket_close(ctx->control);
        ctx->control = NULL;
    }
    ctx->session = false;
}

static void focas_close_session(focas_ctx *ctx)
{
    if (ctx->session) {
        (void)focas_bye(ctx, ctx->socket);
        (void)focas_bye(ctx, ctx->transfer);
        if (ctx->control != NULL && ctx->control != ctx->transfer &&
            ctx->control != ctx->socket) {
            (void)focas_bye(ctx, ctx->control);
        }
    }
    focas_drop_session(ctx);
}

/* -------------------------------------------------------------- exchange -- */

/**
 * One frame out, one frame in. The reply is read in two calls: 10 bytes of
 * header, which say how much body follows (§2.2), then the body itself.
 *
 * @p body_out borrows into the context's receive buffer and stays valid until
 * the next exchange.
 */
static ncl_err focas_exchange_on(focas_ctx *ctx, ncl_socket *socket, uint8_t func,
                                 const uint8_t *body, size_t body_len,
                                 ncl_focas_pdu *pdu, const uint8_t **body_out,
                                 size_t *body_out_len)
{
    size_t frame_len;
    size_t total = 0;
    unsigned attempt = 0;
    ncl_err err = NCL_OK;

    if (socket == NULL) {
        return NCL_DRV_ERR_TRANSPORT(0x90);
    }
    frame_len = ncl_focas_build(ctx->tx, sizeof(ctx->tx), func,
                                NCL_FOCAS_DIR_REQ, body, body_len);
    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    ctx->last_tx_len = frame_len;
    ctx->last_rx_len = 0;
    for (attempt = 0;; attempt++) {
        if (ncl_socket_send(socket, ctx->tx, frame_len) != NCL_OK) {
            err = NCL_DRV_ERR_TRANSPORT(0x90);
        } else if (ncl_socket_recv_exact(socket, ctx->rx, NCL_FOCAS_HEADER,
                                        ctx->timeout_ms) != NCL_OK) {
            err = NCL_DRV_ERR_TRANSPORT(0x91);
        } else {
            err = ncl_focas_split(ctx->rx, NCL_FOCAS_HEADER, pdu, &total);
            if (err == NCL_ERR_RANGE) {
                /* the header was fine and pinned the frame's total length */
                if (total > sizeof(ctx->rx)) {
                    err = NCL_FOCAS_ERR_LENGTH;
                } else if (ncl_socket_recv_exact(socket,
                                                 ctx->rx + NCL_FOCAS_HEADER,
                                                 total - NCL_FOCAS_HEADER,
                                                 ctx->timeout_ms) != NCL_OK) {
                    err = NCL_DRV_ERR_TRANSPORT(0x92);
                } else {
                    err = ncl_focas_split(ctx->rx, total, pdu, NULL);
                }
            }
        }
        if (err == NCL_OK) {
            break;
        }
        if (err != NCL_DRV_ERR_TRANSPORT(0x90) && err != NCL_DRV_ERR_TRANSPORT(0x91) &&
            err != NCL_DRV_ERR_TRANSPORT(0x92)) {
            break; /* a protocol complaint will not get better by retrying */
        }
        if (attempt >= ctx->retries) {
            break;
        }
    }
    if (err != NCL_OK) {
        if (ncl_driver_error_tier(err) == 1) {
            /* 一条断了就两条一起丢：会话在数据通道上，控制通道跟着重开 */
            ncl_socket_shutdown(socket);
            if (ctx->socket != NULL && ctx->socket != socket) {
                ncl_socket_shutdown(ctx->socket);
            }
            if (ctx->transfer != NULL && ctx->transfer != socket &&
                ctx->transfer != ctx->socket) {
                ncl_socket_shutdown(ctx->transfer);
            }
            if (ctx->control != NULL && ctx->control != socket &&
                ctx->control != ctx->transfer && ctx->control != ctx->socket) {
                ncl_socket_shutdown(ctx->control);
            }
            ctx->session = false;
        }
        return err;
    }
    ctx->last_rx_len = NCL_FOCAS_HEADER + pdu->length;
    /* §2.2 rule 2: the reply's func must be the one we asked for. */
    if (pdu->func != func) {
        return NCL_FOCAS_ERR_HEADER;
    }
    /*
     * §2.2 rule 3：应答的方向是 1..4，SDK 的请求拿到的常态是 2。**方向 3 是机床在说
     * "没有这个数"**（真机实测：宏变量/刀补/参数里不存在的号就是这条），把它翻成一个
     * 明确的码，别当成协议错。
     */
    if (pdu->dir == 3u) {
        return NCL_FOCAS_ERR_NO_DATA;
    }
    if (pdu->dir != NCL_FOCAS_DIR_RESP) {
        return NCL_FOCAS_ERR_HEADER;
    }
    if (body_out != NULL) {
        *body_out = ctx->rx + NCL_FOCAS_HEADER;
    }
    if (body_out_len != NULL) {
        *body_out_len = pdu->length;
    }
    return NCL_OK;
}

/** The same exchange on the data channel - what every command uses. */
static ncl_err focas_exchange(focas_ctx *ctx, uint8_t func, const uint8_t *body,
                              size_t body_len, ncl_focas_pdu *pdu,
                              const uint8_t **body_out, size_t *body_out_len)
{
    return focas_exchange_on(ctx, ctx->socket, func, body, body_len, pdu,
                             body_out, body_out_len);
}

/**
 * 传输这一族（0x10..0x1f）该走哪条连接：**hello 计数器 1** 那条（§11.18）。
 *
 * 2026-09-23 在同一台 NCGuide 0i-MF 上核对官方 Fwlib64：它的 `cnc_dwnstart4` /
 * `cnc_download4` / `cnc_dwnend4`（还有收尾那条 `func 0x02`）全发在 hello 计数器 1
 * 的那条 TCP 上，而连接期那条 `func 0x21` 探针跟日常读写一样走计数器 2 那条。照发
 * 才通；发到命令通道上机床连应答都不给，直接把连接 RST 掉 —— 这就是 §11.14.4 里
 * "帧一模一样却不收"的根子。
 */
static ncl_socket *focas_transfer_socket(focas_ctx *ctx)
{
    if (ctx->transfer != NULL) {
        return ctx->transfer;
    }
    return ctx->control != NULL ? ctx->control : ctx->socket;
}

/**
 * 程序上/下行的三件套专用交换：**应答方向 2 或者 3 都算数**。
 *
 * 方向 3 的体前 4 字节是机床的返回码（大端）：0 = 这一步成了、非 0 = 机床不收
 * （`00000005` = EW_ATTRIB）。普通调用里方向 3 是"没有这个数"，在
 * `focas_exchange_on()` 那头就翻掉了 —— 传输这一族得单独认，才能把"机床为什么
 * 不收"带到上层（01 册 §11.14 记了 SDK 这两条帧）。
 */
static ncl_err focas_transfer_exchange(focas_ctx *ctx, uint8_t func,
                                       const uint8_t *body, size_t body_len,
                                       const uint8_t **reply, size_t *reply_len)
{
    ncl_focas_pdu pdu;
    const uint8_t *body_out = NULL;
    size_t body_out_len = 0;
    ncl_err err;

    memset(&pdu, 0, sizeof(pdu));
    err = focas_exchange_on(ctx, focas_transfer_socket(ctx), func, body, body_len,
                            &pdu, &body_out, &body_out_len);
    if (err == NCL_FOCAS_ERR_NO_DATA) {
        const uint8_t *rx = ctx->rx + NCL_FOCAS_HEADER;
        int code;
        int detail = 0;

        if (ctx->last_rx_len < NCL_FOCAS_HEADER + 4u) {
            return err;
        }
        code = (rx[0] << 24) | (rx[1] << 16) | (rx[2] << 8) | rx[3];
        if (code == 0) {
            return NCL_OK; /* 方向 3、码 0：这一步机床认了 */
        }
        /* 体 8 字节：`[返回码 4][细码 2][err_dtno 2]`（§11.14 实测）。细码那两字节
         * 就是 `cnc_getdtailerr` 的 `ODBERR.err_no`，含义按帧查 spec —— 带上它，
         * 上层才分得清"号重复"和"目录写错"（§11.18）。 */
        if (ctx->last_rx_len >= NCL_FOCAS_HEADER + 6u) {
            detail = (rx[4] << 8) | rx[5];
        }
        return NCL_FOCAS_ERR_TRANSFER2(code, detail);
    }
    if (err != NCL_OK) {
        return err;
    }
    if (reply != NULL) {
        *reply = body_out;
    }
    if (reply_len != NULL) {
        *reply_len = body_out_len;
    }
    return NCL_OK;
}

/** One `func 0x21` exchange whose reply must be a usable command list. */
static ncl_err focas_command(focas_ctx *ctx, const uint8_t *body, size_t body_len,
                             const uint8_t **reply, size_t *reply_len)
{
    ncl_focas_pdu pdu;

    memset(&pdu, 0, sizeof(pdu));
    return focas_exchange(ctx, NCL_FOCAS_FUNC_CMD, body, body_len, &pdu, reply,
                          reply_len);
}

/**
 * 一帧发出去、**不等应答**。程序下行的数据帧就是这种（func 0x12、dir 4）：官方
 * SDK 发完立刻发下一块，机床不回；回了反而把它带歪（§2.4 实测）。
 */
static ncl_err focas_send_only(focas_ctx *ctx, uint8_t func, uint8_t dir,
                              const uint8_t *body, size_t body_len)
{
    size_t frame_len = ncl_focas_build(ctx->tx, sizeof(ctx->tx), func, dir, body,
                                       body_len);

    if (frame_len == 0) {
        return NCL_ERR_RANGE;
    }
    ctx->last_tx_len = frame_len;
    ctx->last_rx_len = 0;
    /* 数据帧跟 start/end 一条道：传输通道（第一条） */
    if (ncl_socket_send(focas_transfer_socket(ctx), ctx->tx, frame_len) != NCL_OK) {
        ncl_socket_shutdown(focas_transfer_socket(ctx));
        ctx->session = false;
        return NCL_DRV_ERR_TRANSPORT(0x90);
    }
    return NCL_OK;
}

/**
 * 程序上/下行的 start 帧体（§2.4，516 字节定长）：`[1]` 是数据种类（0 NC 程序、
 * 1 刀补、2 参数…），`[4..6)` 固定 `"N:"`，`[6..)` 是目录名/文件名。
 */
static ncl_err focas_transfer_start_body(uint8_t *body, short type,
                                         const char *name)
{
    size_t len = name != NULL ? strlen(name) : 0;

    if (len > NCL_FOCAS_TRANSFER_BODY - 6u || type < 0 || type > 0xFF) {
        return NCL_ERR_RANGE;
    }
    memset(body, 0, NCL_FOCAS_TRANSFER_BODY);
    body[0] = 0x00;
    body[1] = (uint8_t)type;
    body[2] = 0x00;
    body[3] = 0x01;
    body[4] = 'N';
    body[5] = ':';
    if (len > 0) {
        memcpy(body + 6, name, len);
    }
    return NCL_OK;
}

/**
 * 程序下行（PC → CNC）：`cnc_dwnstart4` → 分块 `cnc_download4` → `cnc_dwnend4`。
 * 参数：`type`（数据种类，缺省 0 = NC 程序）、`dir`（目标目录/程序名，可省）、
 * `data`（程序文本）。
 *
 * 与官方库一致的几点：数据帧发完不等应答；一块 1400 字节以内；**错误在 end 帧
 * 才回**（`EW_DATA`/`EW_OVRFLOW` 一类），所以 end 没成功就是整条没落地。
 */
static ncl_err focas_program_download(focas_ctx *ctx, const ncl_json *params,
                                      ncl_json **result)
{
    ncl_focas_pdu pdu;
    uint8_t body[NCL_FOCAS_TRANSFER_BODY];
    const char *text = ncl_json_obj_get_string(params, "data");
    const char *dir = ncl_json_obj_get_string(params, "dir");
    long long type = ncl_json_obj_get_int(params, "type", 0);
    size_t total = text != NULL ? strlen(text) : 0;
    size_t sent = 0;
    ncl_err err;

    if (text == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    err = focas_transfer_start_body(body, (short)type, dir);
    if (err != NCL_OK) {
        return err;
    }
    memset(&pdu, 0, sizeof(pdu));
    err = focas_transfer_exchange(ctx, NCL_FOCAS_FUNC_DWN_START, body,
                                  sizeof(body), NULL, NULL);
    if (err != NCL_OK) {
        return err;
    }
    while (sent < total) {
        size_t chunk = total - sent;

        if (chunk > NCL_FOCAS_TRANSFER_CHUNK) {
            chunk = NCL_FOCAS_TRANSFER_CHUNK;
        }
        err = focas_send_only(ctx, NCL_FOCAS_FUNC_DWN_DATA, NCL_FOCAS_DIR_DATA,
                              (const uint8_t *)text + sent, chunk);
        if (err != NCL_OK) {
            return err;
        }
        sent += chunk;
    }
    memset(&pdu, 0, sizeof(pdu));
    /* 下行的错都在这条上回：应答方向 3 + 体前 4 字节的返回码。 */
    err = focas_transfer_exchange(ctx, NCL_FOCAS_FUNC_DWN_END, NULL, 0, NULL,
                                  NULL);
    if (err != NCL_OK) {
        return err; /* 下载的错都在这条上回 */
    }
    if (result != NULL) {
        ncl_json *object = ncl_json_new_object();

        if (object == NULL) {
            return NCL_ERR_NOMEM;
        }
        (void)ncl_json_obj_set_int(object, "type", type);
        (void)ncl_json_obj_set_int(object, "bytes", (long long)total);
        if (dir != NULL) {
            (void)ncl_json_obj_set_string(object, "dir", dir);
        }
        *result = object;
    }
    return NCL_OK;
}

/* ------------------------------------------------------------- handshake -- */

/** 一条 TCP 连上去（会话是两条，见下面的 focas_handshake）。 */
static ncl_err focas_connect(focas_ctx *ctx, ncl_socket **out)
{
    char err[256];

    err[0] = '\0';
    *out = ncl_socket_connect(ctx->host, ctx->port, ctx->connect_timeout_ms, err,
                              sizeof(err));
    return *out != NULL ? NCL_OK : NCL_DRV_ERR_TRANSPORT(0x93);
}

/**
 * 一条连接上的 `hello`（`func 1` + 2 字节计数器），把应答的形状记下来。
 * `records`/`field2` 只是"机床自己怎么数"，会话怎么建不看它们（§2.3）。
 */
static ncl_err focas_hello(focas_ctx *ctx, ncl_socket *socket, unsigned counter,
                           size_t *records, uint16_t *field2)
{
    uint8_t hello[2];
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    ncl_focas_pdu pdu;
    ncl_err err;
    size_t n = 0;

    hello[0] = 0;
    hello[1] = (uint8_t)(counter & 0xFFu);
    memset(&pdu, 0, sizeof(pdu));
    err = focas_exchange_on(ctx, socket, NCL_FOCAS_FUNC_HELLO, hello,
                            sizeof(hello), &pdu, &reply, &reply_len);
    if (err != NCL_OK) {
        return err;
    }
    err = ncl_focas_hello_reply(reply, reply_len, &n);
    if (err != NCL_OK) {
        return err;
    }
    if (records != NULL) {
        *records = n;
    }
    if (field2 != NULL) {
        *field2 = ncl_focas_hello_field(reply, reply_len, 2);
    }
    return NCL_OK;
}

static ncl_err focas_bye(focas_ctx *ctx, ncl_socket *socket)
{
    ncl_focas_pdu pdu;

    if (socket == NULL) {
        return NCL_OK;
    }
    memset(&pdu, 0, sizeof(pdu));
    return focas_exchange_on(ctx, socket, NCL_FOCAS_FUNC_BYE, NULL, 0, &pdu, NULL,
                             NULL);
}

/**
 * 会话探针：`func 0x21` **一个** `code 24` 的块，应答载荷就是 `cnc_sysinfo` 的
 * ODBSYS（真机实测 18 字节）。官方 SDK 在数据通道 hello 之后紧接着就发这一条。
 *
 * 它**不是**会话的必要条件（真机上不发它照样能读数据，同轮实测），所以失败只记在
 * `probe_blocks` 里，不算会话没建起来 —— 但发出去，形状与官方库一致。
 */
static ncl_err focas_probe_system(focas_ctx *ctx)
{
    uint8_t body[NCL_FOCAS_CB_SIZE + 2u];
    ncl_focas_cb cb;
    size_t used;
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    ncl_err err;

    used = ncl_focas_body_begin(body, sizeof(body));
    if (used == 0) {
        return NCL_ERR_RANGE;
    }
    ncl_focas_cb_init(&cb, NCL_FOCAS_CODE_SYSINFO);
    used = ncl_focas_body_add(body, sizeof(body), used, &cb);
    if (used == 0) {
        return NCL_ERR_RANGE;
    }
    err = focas_command(ctx, body, used, &reply, &reply_len);
    if (err != NCL_OK) {
        return err;
    }
    ctx->probe_blocks = ncl_focas_block_count(reply, reply_len);
    return ncl_focas_check_blocks(reply, reply_len, NULL);
}

/**
 * 一条会话 = **两条 TCP**（§2.1，2026-09 真机实测；官方 SDK 就是这么开的）：
 *
 *   ```
 *   hello(计数器 1) → 应答（真机 360 字节）              ← 传输通道
 *   hello(计数器 2) → 应答 → func 0x21 一个 code 24 的块（ODBSYS）← 命令通道
 *   ```
 *
 * 分工写在 **hello 里那个计数器**上（§11.18）：计数器 1 那条收传输帧、计数器 2 那条
 * 收命令帧，发错道机床直接断连接。所以这里把两条都 hello 完，再**按计数器**把角色
 * 落到 `ctx->socket`（命令）/`ctx->transfer`（传输）上 —— 默认 `hello_counter = 1`，
 * 也就是"先连的那条收传输"，但按"第几条"去理解会错（对调计数器，角色跟着对调）。
 *
 * 原来把第二条连接当成"没应答时 SDK 的重试"，于是单连接、还按握手应答里的记录发
 * 一串 `code 24` 的探针 —— 真机上第一帧就被拒，会话建不起来。
 */
static ncl_err focas_handshake(focas_ctx *ctx)
{
    ncl_socket *first = NULL;
    ncl_socket *second = NULL;
    ncl_err err;

    ctx->hello_records = 0;
    ctx->hello_field2 = 0;
    ctx->probe_blocks = 0;

    err = focas_connect(ctx, &first);
    if (err != NCL_OK) {
        return err;
    }
    ctx->control = first;
    err = focas_hello(ctx, first, ctx->hello_counter, NULL, NULL);
    if (err != NCL_OK) {
        return err;
    }

    err = focas_connect(ctx, &second);
    if (err != NCL_OK) {
        return err;
    }
    err = focas_hello(ctx, second, ctx->hello_counter + 1u, &ctx->hello_records,
                      &ctx->hello_field2);
    if (err != NCL_OK) {
        return err;
    }
    if ((ctx->hello_counter & 1u) != 0u) {
        ctx->transfer = first;  /* 奇数计数器 = 传输通道 */
        ctx->socket = second;
    } else {
        ctx->transfer = second;
        ctx->socket = first;
    }

    if (ctx->negotiate) {
        (void)focas_probe_system(ctx); /* 形状与官方库一致；失败不算事 */
    }
    ctx->session = true;
    return NCL_OK;
}

/** Open on demand: the two connections and the hello, then the probe (§2.1). */
static ncl_err focas_ensure_session(focas_ctx *ctx)
{
    ncl_err err;

    if (ctx->session) {
        return NCL_OK;
    }
    focas_drop_session(ctx); /* 半开的那一对先丢掉，重新来一遍 */
    err = focas_handshake(ctx);
    if (err != NCL_OK) {
        focas_drop_session(ctx);
    }
    return err;
}

/* ----------------------------------------------------------------- reads -- */

/** Longest item name a point may write (the table's names are much shorter). */
#define FOCAS_MAX_ITEM_NAME 64

/**
 * Split a point's `area` into the item name and the byte offset inside the
 * reply block's payload: `"STATINFO@12"` is ODBST's `alarm` short, `"ACTF@4"`
 * the second axis of `cnc_actf`.
 *
 * The offset has to live in the name because the generic address model reads
 * `bit` as a *bit* index (ncl_address_from_json turns any `bit` into dtype
 * BIT), and a FOCAS payload offset is a byte count of a whole 16/32 bit field.
 * A name without '@', or one whose tail is not a decimal number, is taken
 * whole - the lookup then fails with the same error as any unknown item.
 *
 * Returns @p name, with *byte_offset set to 0 when the name carried none.
 */
static const char *focas_item_name(const char *area, char *name, size_t cap,
                                   size_t *byte_offset)
{
    const char *at;
    size_t len;

    *byte_offset = 0;
    at = area != NULL ? strrchr(area, '@') : NULL;
    if (at == NULL || at == area || at[1] == '\0') {
        return area;
    }
    {
        const char *p = at + 1;
        size_t offset = 0;

        while (*p >= '0' && *p <= '9') {
            offset = offset * 10u + (size_t)(*p - '0');
            p++;
        }
        if (*p != '\0' || offset > 0xFFFFu) {
            return area; /* "AXIS@0" style names stay whole */
        }
        *byte_offset = offset;
    }
    len = (size_t)(at - area);
    if (len + 1u > cap) {
        return area;
    }
    memcpy(name, area, len);
    name[len] = '\0';
    return name;
}

/**
 * Build the request body for one point: one block per command code of the
 * item. A name the table does not know is taken as a bare code, with the
 * arguments left zero - which is how a block the capture has no entry for is
 * still tried.
 */
/**
 * A FOCAS item name that ends in a digit - cnc_exeprgname2, cnc_rdprogdir3 -
 * arrives split, because the generic address parser reads a trailing run of
 * digits as an offset: "EXEPRGNAME2" becomes area "EXEPRGNAME" at offset 2.
 * Which one the site meant is decided by the item table, not by the spelling:
 * when the name as given is not one of ours, put the digits back on.
 *
 * @return @p item, or @p buffer holding the rejoined name.
 */
static const char *focas_rejoin_item(const char *item, long long offset,
                                     char *buffer, size_t cap)
{
    if (offset <= 0 || ncl_focas_item_lookup(item) != NULL) {
        return item;
    }
    if ((size_t)snprintf(buffer, cap, "%s%lld", item, offset) >= cap) {
        return item;
    }
    return ncl_focas_item_lookup(buffer) != NULL ? buffer : item;
}

static ncl_err focas_build_item(const char *area, uint8_t *body, size_t cap,
                                size_t *used)
{
    const ncl_focas_item *item = ncl_focas_item_lookup(area);
    ncl_focas_item raw;
    size_t i;
    size_t offset;

    if (item == NULL) {
        uint16_t code = 0;

        if (!ncl_focas_parse_code(area, &code)) {
            return NCL_ERR_INVALID_DATA_NAME;
        }
        memset(&raw, 0, sizeof(raw));
        raw.name = area;
        raw.cbs[0] = code;
        raw.cb_count = 1;
        item = &raw;
    }
    offset = ncl_focas_body_begin(body, cap);
    if (offset == 0) {
        return NCL_ERR_RANGE;
    }
    for (i = 0; i < item->cb_count; i++) {
        ncl_focas_cb cb;

        ncl_focas_cb_init(&cb, item->cbs[i]);
        cb.arg0 = item->arg0[i];
        cb.arg1 = item->arg1[i];
        cb.arg2 = item->arg2[i];
        cb.arg3 = item->arg3[i];
        offset = ncl_focas_body_add(body, cap, offset, &cb);
        if (offset == 0) {
            return NCL_ERR_RANGE;
        }
    }
    *used = offset;
    return NCL_OK;
}

static ncl_err focas_read_one(focas_ctx *ctx, const ncl_address *address,
                              ncl_json **value)
{
    char name[FOCAS_MAX_ITEM_NAME];
    char rejoined[FOCAS_MAX_ITEM_NAME];
    uint8_t body[FOCAS_MAX_CB * NCL_FOCAS_CB_SIZE + 2u];
    const uint8_t *reply = NULL;
    const uint8_t *block = NULL;
    const uint8_t *payload;
    const char *item;
    size_t reply_len = 0;
    size_t block_len = 0;
    size_t payload_len = 0;
    size_t used = 0;
    size_t named_offset = 0;
    size_t payload_offset;
    size_t width;
    long long block_index;
    ncl_err err;

    if (address->area == NULL || address->length < 1) {
        return NCL_ERR_INVALID_ARG;
    }
    block_index = address->offset;
    item = focas_item_name(address->area, name, sizeof(name), &named_offset);
    if (named_offset == 0) {
        const char *joined = focas_rejoin_item(item, block_index, rejoined,
                                               sizeof(rejoined));

        if (joined != item) {
            /* The digits were part of the item's name, not a block index. */
            item = joined;
            block_index = 0;
        }
    }
    err = focas_build_item(item, body, sizeof(body), &used);
    if (err != NCL_OK) {
        return err;
    }
    err = focas_ensure_session(ctx);
    if (err != NCL_OK) {
        return err;
    }
    err = focas_command(ctx, body, used, &reply, &reply_len);
    if (err != NCL_OK) {
        return err;
    }
    /* §2.3 rule 1: the reply must carry a block per request block, and each of
     * them must say "OK" - that is what the SDK's getRb enforces. */
    err = ncl_focas_check_blocks(reply, reply_len, NULL);
    if (err != NCL_OK) {
        return err;
    }
    if (block_index < 0 ||
        (size_t)block_index >= ncl_focas_block_count(reply, reply_len)) {
        return NCL_FOCAS_ERR_RB_MISSING;
    }
    err = ncl_focas_block_at(reply, reply_len, (size_t)block_index, &block,
                             &block_len);
    if (err != NCL_OK) {
        return err;
    }
    payload = ncl_focas_block_payload(block, block_len, &payload_len);
    if (payload == NULL) {
        return NCL_FOCAS_ERR_LENGTH;
    }
    {
        uint16_t declared = ncl_focas_block_payload_len(block, block_len);

        if (declared > 0 && (size_t)declared < payload_len) {
            payload_len = declared; /* believe the machine's own count */
        }
    }
    /* `bit` is the byte offset inside the block's payload; -1 means zero.
     * A point that named one (`"RDCOUNT@4"`) keeps its `bit` free, because the
     * generic address model reserves `bit` for a bit index. */
    payload_offset = address->bit > 0 ? (size_t)address->bit : named_offset;
    if (payload_offset > payload_len) {
        return NCL_ERR_RANGE;
    }
    payload += payload_offset;
    payload_len -= payload_offset;
    switch (address->dtype) {
    case NCL_DTYPE_BIT:
    case NCL_DTYPE_BYTE:
        width = 1;
        break;
    case NCL_DTYPE_INT16:
        width = 2;
        break;
    case NCL_DTYPE_INT32:
    case NCL_DTYPE_FLOAT32:
        width = 4;
        break;
    case NCL_DTYPE_FLOAT64:
        width = 8;
        break;
    case NCL_DTYPE_STRING:
        width = 1;
        break;
    default:
        return NCL_ERR_INVALID_DATA_TYPE;
    }
    if (payload_len < width * (size_t)address->length) {
        return NCL_FOCAS_ERR_LENGTH;
    }
    return ncl_focas_decode(payload, payload_len, address->dtype,
                            (size_t)address->length, value);
}

static ncl_err focas_read_batch(ncl_driver *self, const ncl_address *addresses,
                                size_t count, ncl_json **values)
{
    focas_ctx *ctx = (focas_ctx *)self->ctx;
    ncl_json *array;
    ncl_err err = NCL_OK;
    size_t i;

    if (addresses == NULL || values == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    array = ncl_json_new_array();
    if (array == NULL) {
        return NCL_ERR_NOMEM;
    }
    ncl_mutex_lock(ctx->mutex);
    for (i = 0; i < count; i++) {
        ncl_json *value = NULL;

        err = focas_read_one(ctx, &addresses[i], &value);
        if (err != NCL_OK) {
            break;
        }
        if (ncl_json_arr_push(array, value != NULL ? value : ncl_json_new_null()) !=
            NCL_OK) {
            err = NCL_ERR_NOMEM;
            break;
        }
    }
    ncl_mutex_unlock(ctx->mutex);
    if (err != NCL_OK) {
        ncl_json_free(array);
        return err;
    }
    *values = array;
    return NCL_OK;
}

/* --------------------------------------------------------------- the rest -- */

static ncl_err focas_raw(ncl_driver *self, const void *frame, size_t frame_len,
                         ncl_driver_result *out)
{
    static const char kDigits[] = "0123456789abcdef";
    focas_ctx *ctx = (focas_ctx *)self->ctx;
    const uint8_t *bytes = (const uint8_t *)frame;
    const uint8_t *reply = NULL;
    size_t reply_len = 0;
    ncl_err err;
    char *hex;
    size_t i;

    if (frame == NULL || frame_len < 1u || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    ncl_mutex_lock(ctx->mutex);
    err = focas_ensure_session(ctx);
    if (err == NCL_OK) {
        err = focas_command(ctx, bytes + 1, frame_len - 1, &reply, &reply_len);
    }
    ncl_mutex_unlock(ctx->mutex);
    if (err != NCL_OK) {
        return err;
    }
    ncl_driver_result_set_raw(out, reply, reply_len);
    hex = (char *)ncl_mem_alloc(reply_len * 2u + 1u);
    if (hex == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < reply_len; i++) {
        hex[i * 2] = kDigits[(reply[i] >> 4) & 0xF];
        hex[i * 2 + 1] = kDigits[reply[i] & 0xF];
    }
    hex[reply_len * 2] = '\0';
    ncl_driver_result_ok(out, ncl_json_new_string(hex));
    ncl_free_safe(hex);
    return NCL_OK;
}

/** Session operations: what the table knows, and what the hello found. */
static ncl_err focas_call(ncl_driver *self, const char *operation,
                          const ncl_json *params, ncl_json **result)
{
    focas_ctx *ctx = (focas_ctx *)self->ctx;

    (void)params;
    if (result != NULL) {
        *result = NULL;
    }
    if (ncl_streq_ignore_case(operation, "items")) {
        static const char *const kNames[] = {
            "STATINFO", "ACTF",       "ACTS",  "RDCOUNT", "RDLIFE",
            "RDMACRO",  "RDPARAM",    "RDTOFS", "RDPROGDIR3", "EXEPRGNAME2",
        };
        ncl_json *array = ncl_json_new_array();
        size_t i;

        if (array == NULL) {
            return NCL_ERR_NOMEM;
        }
        for (i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
            const ncl_focas_item *item = ncl_focas_item_lookup(kNames[i]);
            ncl_json *entry = ncl_json_new_object();

            if (entry == NULL) {
                ncl_json_free(array);
                return NCL_ERR_NOMEM;
            }
            (void)ncl_json_obj_set_string(entry, "name", kNames[i]);
            (void)ncl_json_obj_set_int(entry, "blocks", item != NULL ? item->cb_count : 0);
            if (item != NULL && item->cb_count > 0) {
                (void)ncl_json_obj_set_int(entry, "code0", item->cbs[0]);
            }
            if (ncl_json_arr_push(array, entry) != NCL_OK) {
                ncl_json_free(array);
                return NCL_ERR_NOMEM;
            }
        }
        if (result != NULL) {
            *result = array;
        } else {
            ncl_json_free(array);
        }
        return NCL_OK;
    }
    if (ncl_streq_ignore_case(operation, "session")) {
        ncl_json *object = ncl_json_new_object();

        if (object == NULL) {
            return NCL_ERR_NOMEM;
        }
        (void)ncl_json_obj_set_string(object, "host", ctx->host != NULL ? ctx->host : "");
        (void)ncl_json_obj_set_int(object, "port", ctx->port);
        (void)ncl_json_obj_set_bool(object, "negotiated", ctx->session);
        /* §2.1 / §11.18：会话是两条 TCP —— hello 1 那条收传输帧、hello 2 那条收命令帧 */
        (void)ncl_json_obj_set_int(object, "channels", 2);
        (void)ncl_json_obj_set_int(object, "helloTransfer",
                                   (long long)ctx->hello_counter);
        (void)ncl_json_obj_set_int(object, "helloCommand",
                                   (long long)ctx->hello_counter + 1);
        (void)ncl_json_obj_set_bool(object, "transferOpen", ctx->transfer != NULL);
        (void)ncl_json_obj_set_bool(object, "commandOpen", ctx->socket != NULL);
        (void)ncl_json_obj_set_int(object, "helloRecords",
                                   (long long)ctx->hello_records);
        (void)ncl_json_obj_set_int(object, "helloField2", ctx->hello_field2);
        (void)ncl_json_obj_set_int(object, "probeBlocks",
                                   (long long)ctx->probe_blocks);
        if (result != NULL) {
            *result = object;
        } else {
            ncl_json_free(object);
        }
        return NCL_OK;
    }
    /*
     * "payload"：把一个 item 的某一块载荷**原样**取回来（不管它多长）。
     * params: {"item":"ALMMSG","block":0}，结果: {"block":0,"length":n,"bytes":[...]}。
     * 可选的 "d"/"e"/"arg2"/"arg3" 会**覆盖**那个 item 表里第 1 个块的对应格子 ——
     * "同一个 item、每次问不同的号"那一族（刀补 / 宏变量 / 参数 / 位置）靠它。
     *
     * 为什么需要它：普通的读取要报"读几个元素"，而有些应答的长度是**机床决定的**
     * —— 例如 `cnc_rdalmmsg2` 在没报警时载荷就是 **0 字节**，报长度会撞上
     * "载荷不够"（NCL_FOCAS_ERR_LENGTH），分不清"没有报警"和"读错了"。语义层靠它
     * 先看长度、再按记录切。
     */
    if (ncl_streq_ignore_case(operation, "payload")) {
        const char *item = ncl_json_obj_get_string(params, "item");
        long long block_index = ncl_json_obj_get_int(params, "block", 0);
        bool override_d = ncl_json_obj_has(params, "d");
        bool override_e = ncl_json_obj_has(params, "e");
        bool override_2 = ncl_json_obj_has(params, "arg2");
        bool override_3 = ncl_json_obj_has(params, "arg3");
        long long value_d = ncl_json_obj_get_int(params, "d", 0);
        long long value_e = ncl_json_obj_get_int(params, "e", 0);
        long long value_2 = ncl_json_obj_get_int(params, "arg2", 0);
        long long value_3 = ncl_json_obj_get_int(params, "arg3", 0);
        uint8_t body[FOCAS_MAX_CB * NCL_FOCAS_CB_SIZE + 2u];
        const uint8_t *reply = NULL;
        const uint8_t *block = NULL;
        const uint8_t *payload = NULL;
        size_t reply_len = 0;
        size_t block_len = 0;
        size_t payload_len = 0;
        size_t used = 0;
        ncl_json *object = NULL;
        ncl_json *bytes = NULL;
        ncl_err err;

        if (ncl_str_is_blank(item)) {
            return NCL_ERR_INVALID_ARG;
        }
        err = focas_build_item(item, body, sizeof(body), &used);
        if (err != NCL_OK) {
            return err;
        }
        if (used >= 2u + NCL_FOCAS_CB_SIZE && (override_d || override_e ||
                                               override_2 || override_3)) {
            /* 第 1 个块在体里从偏移 2 开始：code@[6..8)、d@[8..12)、e@[12..16)、
             * arg2@[16..20)、arg3@[20..24)。 */
            uint8_t *cb = body + 2u;

            if (override_d) {
                driver_put_u32be(cb + 8u, (uint32_t)value_d);
            }
            if (override_e) {
                driver_put_u32be(cb + 12u, (uint32_t)value_e);
            }
            if (override_2) {
                driver_put_u32be(cb + 16u, (uint32_t)value_2);
            }
            if (override_3) {
                driver_put_u32be(cb + 20u, (uint32_t)value_3);
            }
        }
        /*
         * 可选 "data"：**写**这一侧要送的值（字节数组），接在命令块后面。
         *
         * 形状按 2026-09 拿官方 SDK 对 NCGuide 0i-MF Plus 抓的那一帧抄（01 册
         * §11.13）：**块自己的长度格**（[0..2)）要带上载荷（写刀补 = 0x1c + 8 =
         * 0x24），`tag0`/`tag1` 保持 **0** —— 上一轮把长度写进 tag0 正是被机床
         * 拒的原因。块返回码由下面那句 check_blocks 兜着：机床不收就是模块错，
         * 不会假装成功。
         */
        if (ncl_json_obj_has(params, "data")) {
            const ncl_json *data = ncl_json_obj_get(params, "data");
            size_t n = ncl_json_type_of(data) == NCL_JSON_ARRAY
                           ? ncl_json_arr_len(data)
                           : 0u;
            uint8_t write_bytes[FOCAS_MAX_CB * NCL_FOCAS_CB_SIZE + 2u];
            size_t i;

            if (n == 0u || used + n > sizeof(body)) {
                return NCL_ERR_RANGE;
            }
            if (n > sizeof(write_bytes)) {
                return NCL_ERR_RANGE;
            }
            for (i = 0; i < n; i++) {
                long long byte = 0;

                if (!ncl_json_as_int(ncl_json_arr_get(data, i), &byte)) {
                    return NCL_ERR_INVALID_ARG;
                }
                write_bytes[i] = (uint8_t)byte;
            }
            used = ncl_focas_body_add_payload(body, sizeof(body), used,
                                              write_bytes, n);
            if (used == 0) {
                return NCL_ERR_RANGE;
            }
        }
        ncl_mutex_lock(ctx->mutex);
        err = focas_ensure_session(ctx);
        if (err == NCL_OK) {
            err = focas_command(ctx, body, used, &reply, &reply_len);
        }
        if (err == NCL_OK) {
            err = ncl_focas_check_blocks(reply, reply_len, NULL);
        }
        if (err == NCL_OK && (block_index < 0 ||
                              (size_t)block_index >=
                                  ncl_focas_block_count(reply, reply_len))) {
            err = NCL_FOCAS_ERR_RB_MISSING;
        }
        if (err == NCL_OK) {
            err = ncl_focas_block_at(reply, reply_len, (size_t)block_index,
                                     &block, &block_len);
        }
        if (err == NCL_OK) {
            payload = ncl_focas_block_payload(block, block_len, &payload_len);
            if (payload == NULL) {
                err = NCL_FOCAS_ERR_LENGTH;
            } else {
                uint16_t declared = ncl_focas_block_payload_len(block, block_len);

                if (declared > 0 && (size_t)declared < payload_len) {
                    payload_len = declared; /* 信机床自己报的数 */
                }
            }
        }
        ncl_mutex_unlock(ctx->mutex);
        if (err != NCL_OK) {
            return err;
        }
        bytes = ncl_json_new_array();
        object = ncl_json_new_object();
        if (bytes == NULL || object == NULL) {
            ncl_json_free(bytes);
            ncl_json_free(object);
            return NCL_ERR_NOMEM;
        }
        {
            size_t i;

            for (i = 0; i < payload_len; i++) {
                ncl_json *byte = ncl_json_new_int(payload[i]);

                if (byte == NULL || ncl_json_arr_push(bytes, byte) != NCL_OK) {
                    ncl_json_free(byte);
                    ncl_json_free(bytes);
                    ncl_json_free(object);
                    return NCL_ERR_NOMEM;
                }
            }
        }
        (void)ncl_json_obj_set_int(object, "block", block_index);
        (void)ncl_json_obj_set_int(object, "length", (long long)payload_len);
        (void)ncl_json_obj_set(object, "bytes", bytes);
        if (result != NULL) {
            *result = object;
        } else {
            ncl_json_free(object);
        }
        return NCL_OK;
    }
    /* 程序下行（PC → CNC）：cnc_dwnstart4 → 分块 cnc_download4 → cnc_dwnend4。
     * 帧与体长按官方 SDK 实测（01 册 §2.4），语义层只是把参数转过来。 */
    if (ncl_streq_ignore_case(operation, "download")) {
        ncl_err err;

        ncl_mutex_lock(ctx->mutex);
        err = focas_ensure_session(ctx);
        if (err == NCL_OK) {
            err = focas_program_download(ctx, params, result);
        }
        ncl_mutex_unlock(ctx->mutex);
        return err;
    }
    /* 程序上行（CNC → PC）：cnc_upstart4 → cnc_upload4 → cnc_upend4。请求码已核
     * （0x15 / 0x18），但**应答里程序文本的切法还没核**（SDK 里在 0x14fe70 里解，
     * 2026-09 反汇编到这一层没再往下），所以这里明确回"还没有"。 */
    if (ncl_streq_ignore_case(operation, "upload")) {
        return NCL_ERR_UNAVAILABLE;
    }
    return NCL_DRV_ERR_PROTOCOL(0x94); /* no such operation */
}

static ncl_err focas_create(ncl_driver *self, const ncl_json *parameters)
{
    focas_ctx *ctx = (focas_ctx *)self->ctx;
    const char *text;

    if (parameters == NULL) {
        return NCL_OK;
    }
    text = ncl_json_obj_get_string(parameters, "host");
    if (text != NULL) {
        ncl_free_safe(ctx->host);
        ctx->host = ncl_strdup(text);
        if (ctx->host == NULL) {
            return NCL_ERR_NOMEM;
        }
    }
    ctx->port = json_uint(parameters, "port", ctx->port);
    ctx->connect_timeout_ms = json_uint(parameters, "connectTimeoutMs",
                                        ctx->connect_timeout_ms);
    ctx->timeout_ms = json_uint(parameters, "timeoutMs", ctx->timeout_ms);
    ctx->retries = json_uint(parameters, "retries", ctx->retries);
    ctx->negotiate = ncl_json_obj_get_bool(parameters, "negotiate", ctx->negotiate);
    ctx->hello_counter = json_uint(parameters, "helloCounter", ctx->hello_counter);
    if (ctx->host == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    return NCL_OK;
}

static ncl_err focas_open(ncl_driver *self)
{
    focas_ctx *ctx = (focas_ctx *)self->ctx;
    ncl_err err;

    ncl_mutex_lock(ctx->mutex);
    err = focas_ensure_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
    return err;
}

static void focas_close(ncl_driver *self)
{
    focas_ctx *ctx = (focas_ctx *)self->ctx;

    ncl_mutex_lock(ctx->mutex);
    focas_close_session(ctx);
    ncl_mutex_unlock(ctx->mutex);
}

static bool focas_is_connected(const ncl_driver *self)
{
    const focas_ctx *ctx = (const focas_ctx *)self->ctx;

    return ctx->socket != NULL && ctx->session;
}

static void focas_attach_event(ncl_driver *self, ncl_driver_event_fn fn,
                               void *user)
{
    (void)self;
    (void)fn;
    (void)user;
}

static void focas_last_raw(const ncl_driver *self, ncl_driver_raw *out)
{
    const focas_ctx *ctx = (const focas_ctx *)self->ctx;

    out->request = ctx->last_tx_len > 0 ? ctx->tx : NULL;
    out->request_len = ctx->last_tx_len;
    out->reply = ctx->last_rx_len > 0 ? ctx->rx : NULL;
    out->reply_len = ctx->last_rx_len;
}

static void focas_destroy(ncl_driver *self)
{
    focas_ctx *ctx;

    if (self == NULL) {
        return;
    }
    ctx = (focas_ctx *)self->ctx;
    if (ctx != NULL) {
        focas_close_session(ctx);
        ncl_free_safe(ctx->host);
        if (ctx->mutex != NULL) {
            ncl_mutex_destroy(ctx->mutex);
        }
        ncl_free_safe(ctx);
    }
    ncl_free_safe(self);
}

static const ncl_driver_ops kFocasOps = {
    "focas",                focas_create,
    focas_open,             focas_close,
    focas_is_connected,     focas_read_batch,
    NULL,                   focas_raw, /* 写：没抓到写帧；骨架对 NULL 回 NOT_SUPPORTED */
    focas_raw,              focas_call,
    focas_attach_event,     focas_destroy,
    focas_last_raw,
};

ncl_driver *ncl_focas_create(void)
{
    focas_ctx *ctx = (focas_ctx *)ncl_mem_calloc(1, sizeof(*ctx));
    ncl_driver *driver;

    if (ctx == NULL) {
        return NULL;
    }
    ctx->port = 8193; /* §2.1: FOCAS over Ethernet listens here */
    ctx->connect_timeout_ms = 3000;
    ctx->timeout_ms = 3000;
    ctx->retries = 0;
    ctx->negotiate = true;
    ctx->hello_counter = NCL_FOCAS_HELLO_TRANSFER; /* 1：官方库默认就是这个 */
    ctx->mutex = ncl_mutex_create();
    if (ctx->mutex == NULL) {
        ncl_free_safe(ctx);
        return NULL;
    }
    driver = ncl_driver_new(&kFocasOps, ctx);
    if (driver == NULL) {
        ncl_mutex_destroy(ctx->mutex);
        ncl_free_safe(ctx);
        return NULL;
    }
    return driver;
}
