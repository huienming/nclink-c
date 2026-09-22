/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - SYNTEC RemoteCNC, the packet layer.
 *
 * Everything here comes from protocal/docs/10-SYNTEC-新代-RemoteCNC.md §10,
 * which is itself read out of the delivered assemblies: the controller side
 * (OCAPIServer_WinCE.exe) marshals its request structures straight onto the
 * wire (`ByteArrayToStructure` / `StructureToByteArray`), so the packet is a
 * C structure laid out with the .NET default rules.
 *
 *   packet   12 bytes : Length u4 | CmdID u2 | (2 pad) | Reserved u4
 *   function  8 bytes : uFuncID u2 | uSerial u1 | Reserved u1 | IHeader u4
 *   body     N bytes  : the function's own request structure
 *
 * `Length` counts the bytes after the 12 byte header - `ReceivePackets` in the
 * server reads the header with a 12 byte `BeginReceive` and then continues for
 * `Length - received` more (§10.4). All fields are little endian (the assem-
 * blies run on x86 / WinCE ARM and use BitConverter).
 *
 * The two CmdID ranges that are known so far (§10.6, §10.7):
 *   file transfer 1..17, Dipole 178/179/180 and 200 (the KrnlAPI one).
 * A CmdID the table has no name for still travels: the point map can name it
 * in the raw "178" form, which is how an undocumented command is tried.
 */
#ifndef NCL_SYNTEC_H
#define NCL_SYNTEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_json.h"
#include "nclink/ncl_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================== packets == */

#define NCL_SYNTEC_PACKET_HEADER 12u
#define NCL_SYNTEC_FUNCTION_HEADER 8u

/** The 12 byte packet header, as the server's CTCPCMD_PacketStart. */
typedef struct {
    uint32_t length;   /**< content bytes, header excluded */
    uint16_t cmd_id;   /**< command number (§10.7: one space for the whole box) */
    uint32_t reserved; /**< written as 0, with 2 pad bytes before it */
} ncl_syntec_packet;

/** The 8 byte function header, as CTCPFunctionCmdSend_Header. */
typedef struct {
    uint16_t func_id;  /**< uFuncID                                       */
    uint8_t  serial;   /**< uSerial, echoed back                          */
    uint32_t header;   /**< IHeader, written as 0 by the delivered client */
} ncl_syntec_function;

/**
 * Build one packet: header + function header + @p body.
 * Returns the frame length, or 0 when it does not fit.
 */
size_t ncl_syntec_build(uint8_t *out, size_t cap, uint16_t cmd_id,
                        const ncl_syntec_function *function, const void *body,
                        size_t body_len);

/** A parsed packet: the headers plus a borrowed view of the body. */
typedef struct {
    ncl_syntec_packet   packet;
    ncl_syntec_function function;
    const uint8_t      *body; /**< borrowed, may be empty */
    size_t              body_len;
} ncl_syntec_view;

/**
 * Check and split one packet. Answers NCL_ERR_RANGE while the frame is still
 * arriving (the caller reads 12 bytes, then `Length` more).
 */
ncl_err ncl_syntec_split(const uint8_t *frame, size_t len, ncl_syntec_view *out,
                         size_t *frame_len);

/* ============================================================= commands == */

/* The CmdIDs whose values are known (§10.6, §10.7). */
#define NCL_SYNTEC_CMD_FILE_FIRST 1u   /**< FileTransferCmd 1..17   */
#define NCL_SYNTEC_CMD_DIPOLE_FIRST 178u
#define NCL_SYNTEC_CMD_KRML_API 200u   /**< carries MMI_Request_KrnlAPI */

/**
 * Look a command name up: "KrnlAPI", "FileExist", "DirCreate", ... or the raw
 * decimal form ("178", "200") for a command the table does not name.
 */
bool ncl_syntec_cmd_lookup(const char *name, uint16_t *cmd_id,
                           const char **canonical);
/** Name of a known CmdID, or NULL. */
const char *ncl_syntec_cmd_name(uint16_t cmd_id);

/** The service a CmdID belongs to, for logs ("FileTransfer", "Dipole", ...). */
const char *ncl_syntec_cmd_service(uint16_t cmd_id);

/**
 * §10.10: the data codes of the client's `EDataType` and `EDevice_Type` enums
 * ("DT_PART_COUNT" 43, "DT_CNC_STATUS" 41, "DT_MACHINEPOS" 0, ...). A KrnlAPI
 * read carries one of these as its `dwCode`.
 */
bool ncl_syntec_data_code(const char *name, int32_t *code);
/** Name of a data code, or NULL. The first match wins (the two enums overlap). */
const char *ncl_syntec_data_code_name(int32_t code);

/**
 * §10.12: one *named reading* of the delivered client. The client's 150 APIs
 * are thin shells over worker stubs that load a single constant, and those
 * numbers are a third numbering space of their own (1000s are counts and
 * times, 700s are the spindle) - not `EDataType` and not `EDevice_Type`.
 *
 * A named reading is therefore "KrnlAPI + this code", so a point can be
 * configured as `{"addr": "part_count", "length": 4, "dtype": "int32"}`
 * instead of writing the command number and the code by hand. The code goes
 * into `dwCode`: the doc notes that whether it is `dwCode` or a device number
 * inside `pBufferIn` needs one capture on a real controller to settle, and
 * that `pBufferIn` placement is unconfirmed anyway (§10.8).
 */
typedef struct {
    const char *name; /**< canonical name, "part_count"                        */
    int32_t     code; /**< the constant the client's worker stub loads        */
    uint16_t    cmd_id; /**< the command it travels in (KrnlAPI for all of them) */
} ncl_syntec_reading;

/**
 * Look a reading up by name. Case and underscores are ignored and the client's
 * own spelling is accepted with or without its `READ_` prefix, so "part_count",
 * "partCount" and "READ_part_count" all find the same entry. NULL when there is
 * no such reading.
 */
const ncl_syntec_reading *ncl_syntec_reading_lookup(const char *name);

/* ================================================================ items == */

/*
 * §3.1/§3.2: the controller's own **item service** - the nine items whose
 * request frames were captured off the box's gateway (tools/site-probe/
 * syntec_probe.sh) and whose answers were then probed (syntec_reply_probe.sh).
 * One request is a fixed **36 byte** frame:
 *
 *   [0..3]   Length = 24 (the content: 8 byte function header + 16 byte body)
 *   [4..5]   CmdID  = 16                      the item service
 *   [6..7]   per item flags  (0x0000; PROGRAM 0x05f1, WARNING 0x0101)
 *   [8..9]   constant 200                     the KrnlAPI command number
 *   [10..11] per item code   (0x0700; PROGRAM 0x071e, WARNING 0x0701)
 *   [12..13] uFuncID = 200
 *   [14..15] uSerial, reserved (0)
 *   [16..19] request number  (0x0407 / 0x041a / 0x048c / 0x0428)
 *   [20..23] type = 4
 *   [24..27] parameter A
 *   [28..31] parameter B     the register / state number
 *   [32..35] flag
 *
 * The answer repeats the 20 byte header and carries the value in what follows:
 * a little endian u16 at [20..21] for the numeric items, the whole body as text
 * for PROGRAM, and "no body at all" is an empty list for WARNING. FEED_SPEED is
 * not a single value: it asks the register 700 and then the states 12 and 76
 * (a state answer's first two bytes are the state itself) and combines them.
 */

#define NCL_SYNTEC_CMD_ITEM 16u /**< the item service's CmdID               */
#define NCL_SYNTEC_ITEM_BODY 16u /**< type | param A | param B | flag       */
#define NCL_SYNTEC_ITEM_FRAME                                                  \
    (NCL_SYNTEC_PACKET_HEADER + NCL_SYNTEC_FUNCTION_HEADER + NCL_SYNTEC_ITEM_BODY)
/** The answer's value starts here: the 12 byte packet + 8 byte function header. */
#define NCL_SYNTEC_REPLY_BODY                                                  \
    (NCL_SYNTEC_PACKET_HEADER + NCL_SYNTEC_FUNCTION_HEADER)

/** The register FEED_SPEED reads first, and the two states it combines with it. */
#define NCL_SYNTEC_FEED_SPEED_REG 700u
#define NCL_SYNTEC_FEED_SPEED_UNIT_STATE 12u
#define NCL_SYNTEC_FEED_SPEED_MODE_STATE 76u
/** State 76 answers this when register 700 may be used as it is (§3.2). */
#define NCL_SYNTEC_FEED_SPEED_DIRECT 70u

/** One item of the item service: everything the 36 byte frame is built from. */
typedef struct {
    const char *name;    /**< canonical name, "PART_COUNT"                  */
    uint16_t    flags;   /**< frame [6..7]                                  */
    uint16_t    code;    /**< frame [10..11]                                */
    uint32_t    request; /**< frame [16..19]                                */
    uint32_t    param_a; /**< frame [24..27]                                */
    uint32_t    param_b; /**< frame [28..31], the register / state number   */
    uint32_t    flag;    /**< frame [32..35]                                */
} ncl_syntec_item;

/**
 * Look an item up by name, with the same leniency as the readings: case and
 * underscores are ignored and a "READ_" prefix is accepted, so "STATUS",
 * "status" and "READ_status" all find the same entry. NULL when there is none.
 */
const ncl_syntec_item *ncl_syntec_item_lookup(const char *name);
/** The item at @p index (the captured order), or NULL. */
const ncl_syntec_item *ncl_syntec_item_at(size_t index);
size_t                 ncl_syntec_item_count(void);

/**
 * Build the 36 byte request of @p item. @p param_b overrides the item's own
 * number - that is how FEED_SPEED asks its three questions (700, then 12, then
 * 76) with one item table entry - and @p serial goes into the function header.
 * Returns the frame length (36), or 0 when it does not fit.
 */
size_t ncl_syntec_item_frame(uint8_t *out, size_t cap,
                             const ncl_syntec_item *item, uint32_t param_b,
                             uint8_t serial);

/** The u16 a numeric item answers with ([20..21], little endian). */
bool ncl_syntec_item_u16(const uint8_t *frame, size_t len, uint16_t *value);
/** The @p index'th i16 of a **state zone** answer ([20 + 2*index]). */
bool ncl_syntec_item_i16(const uint8_t *frame, size_t len, size_t index,
                         int16_t *value);
/** Copy the answer's body (PROGRAM) into @p out, NUL terminated and trimmed. */
bool ncl_syntec_item_text(const uint8_t *frame, size_t len, char *out,
                          size_t cap);
/** True when the answer carries no body at all (WARNING with no alarm). */
bool ncl_syntec_item_empty(const uint8_t *frame, size_t len);

/**
 * Build a **state zone** request: the §3.1 frame shape with `param A` carrying
 * `4 + 2 * count` (the answer's byte count including its 4 byte word) and
 * `param B` the zone number. `ncl_syntec_read_zone()` uses this.
 */
size_t ncl_syntec_zone_frame(uint8_t *out, size_t cap, unsigned zone,
                             size_t count, uint8_t serial);

/**
 * §11.4：系统参数区是**另一族 KrnlAPI**，帧形状与状态区一模一样，只有请求号
 * 不同。请求号出自控制器里的 `OCK_CODE::CODE(type, id) = (type << 10) | id`：
 *
 *   0x0401 CncParamGetCapacity   In { }         Out { hr, nValue }
 *   0x0402 CncParamDump          In { nLength } Out { hr, TParamSpec[nLength] }
 *   0x0404 CncParamGetValue      In { nNo }     Out { hr, nValue:i32 }
 *   0x0407 NcStateGetValue       In { nNo }     Out { hr, nValue:i16[] }
 *
 * 参数值与状态区**不同宽**：一个参数是一个 **i32**，所以 A = 4 + 4。轴名就在这一
 * 区（321 + 槽），见 `ncl_syntec_axis_name()`。
 */
#define NCL_SYNTEC_CODE_PARAM_CAPACITY 0x0401u
#define NCL_SYNTEC_CODE_PARAM_SCHEMA 0x0402u
#define NCL_SYNTEC_CODE_PARAM_GET 0x0404u
#define NCL_SYNTEC_CODE_STATE_GET 0x0407u

/** Build one parameter read: request 0x0404, A = 8, B = the parameter number. */
size_t ncl_syntec_param_frame(uint8_t *out, size_t cap, unsigned param,
                              uint8_t serial);

/** The i32 a parameter answer carries ([20..23], little endian). */
bool ncl_syntec_reply_i32(const uint8_t *frame, size_t len, int32_t *value);

/* ============================================================ state zones == */

/*
 * Positions are **state zones** in the controller (read out of the controller
 * side assembly `Syntec.RemoteCNC.Win32.dll`, v10.116.54):
 *
 *   float[] get_MachineCoordinate() {
 *       float[] r = new float[EnableAxes];
 *       if (!RemoteCnc.State.TCPClientLink.Dump(101, MaxUsedAxisID + 1)) ...
 *       for (i...) r[i] = (float)data[EnableAxisMappingID[i]];  // i2 → i4 → r4
 *   }
 *
 * A zone is read with the zone number in `param B` and `4 + 2 * count` in
 * `param A`; the answer is `count` **int16** values, scaled by 10^DecPoint
 * (zone 261 answers the decimal places - the 21A lathe says 3, and its screen
 * shows `0.000`).
 */
#define NCL_SYNTEC_ZONE_MACHINE 101u  /**< 机械坐标 - the actual position      */
#define NCL_SYNTEC_ZONE_RELATIVE 141u /**< 相对坐标                            */
#define NCL_SYNTEC_ZONE_ABSOLUTE 181u /**< 绝对坐标                            */
#define NCL_SYNTEC_ZONE_DISTANCE 221u /**< 剩余距离                            */
#define NCL_SYNTEC_ZONE_DECIMALS 261u /**< 轴小数位（缩放用 10^dec）           */
/**
 * Most axes one position read handles (the frame's A is 4 + 2*count). 参数表里
 * 一共 16 个轴槽（`*Nth axis ...`），状态区的一项就有这么长，所以按 16 留。
 */
#define NCL_SYNTEC_POSITION_MAX_AXES 16u

/* ============================================================== session == */

/** How to reach one controller (the adapter fills this from its parameters). */
typedef struct {
    const char *host;              /**< required                             */
    unsigned    port;              /**< 8000 (the OCAPIServer's port)        */
    unsigned    connect_timeout_ms;/**< 3000                                 */
    unsigned    timeout_ms;        /**< 3000                                 */
    unsigned    retries;           /**< 0: re-send after a transport failure  */
} ncl_syntec_config;

void ncl_syntec_config_default(ncl_syntec_config *config);

/**
 * One session: the TCP connection plus the `uSerial` counter the controller
 * echoes back (§10.4). The connection is made lazily and re-made after a
 * transport failure, so open() only fails when the configuration is unusable.
 */
typedef struct ncl_syntec ncl_syntec;

ncl_syntec *ncl_syntec_open(const ncl_syntec_config *config, char **err);
void        ncl_syntec_close(ncl_syntec *syntec);
bool        ncl_syntec_is_open(const ncl_syntec *syntec);
/** One line about the last failure; "" when the last call succeeded. */
const char *ncl_syntec_last_error(const ncl_syntec *syntec);

/* State zones (positions) - see the NCL_SYNTEC_ZONE_* numbers above. */

/** Read @p count int16 values of a state zone. */
ncl_err ncl_syntec_read_zone(ncl_syntec *syntec, unsigned zone, size_t count,
                             int16_t *out);
/** The axis decimal places (state zone 261). */
ncl_err ncl_syntec_decimals(ncl_syntec *syntec, int *decimals);
/**
 * One coordinate group, per axis: @p count values as real numbers
 * (`raw / 10^decimals`). A stationary machine reads 0.0 - which is exactly what
 * the 21A lathe answers on all four groups.
 */
ncl_err ncl_syntec_position(ncl_syntec *syntec, unsigned zone, size_t count,
                            double *out);

/* ============================================================== 参数区 == */

/*
 * §11.4：轴名不是状态区里的东西，它在**系统参数区**（请求号 0x0404），而且
 * 每个轴一行、行号固定（表 21A 的 `*Nth axis axis name`）：
 *
 *   21  + 槽  "Port no. for Nth axis"     端口号，0 = 这一槽没接轴
 *   321 + 槽  "*Nth axis axis name"       轴名代号（下面的解码规则）
 *
 * 客户端的 `get_AllAxisName()` 就是这么拼的：`XXX... = "XYZABCUVW"`，
 * `letter = code / 100`（1 = X、2 = Y、3 = Z、4 = A …），`digit = code % 100`
 * （0 = 没有后缀）。所以 100 = "X"、102 = "X2"、323 → 300 = "Z"、901 = "W1"。
 */
#define NCL_SYNTEC_PARAM_AXIS_PORT 21u  /**< + 槽：轴控端口号            */
#define NCL_SYNTEC_PARAM_AXIS_NAME 321u /**< + 槽：轴名代号              */
/** 参数表里的轴槽数：第 1..16 槽（§11.4 抓到的 `*Nth axis ...` 到 16）。 */
#define NCL_SYNTEC_AXIS_SLOTS 16u
/** 轴名的字节数：一个字母 + 两位数字 + 终止符，8 字节有余。 */
#define NCL_SYNTEC_AXIS_NAME_MAX 8u

/** Read one system parameter (KrnlAPI 0x0404, one i32). */
ncl_err ncl_syntec_param(ncl_syntec *syntec, unsigned param, int32_t *value);

/**
 * 一个轴名代号 → 字符串（§11.4）。0 与 ≥ 10000 都是"这一槽没有名字"，回空串；
 * 这不是错误，所以照样返回 true。@p cap 至少给 8 字节
 * （NCL_SYNTEC_AXIS_NAME_MAX）。
 */
bool ncl_syntec_axis_name_decode(int32_t code, char *out, size_t cap);

/** 第 @p slot 槽（0 = 第一个）的轴名；这一槽没有名字时回空串。 */
ncl_err ncl_syntec_axis_name(ncl_syntec *syntec, unsigned slot, char *out,
                             size_t cap);

/** 一条"在用的轴"：参数槽、端口号、名字。 */
typedef struct {
    unsigned slot;                           /**< 0 = X/Y/Z 那一行        */
    int32_t  port;                           /**< 21 + slot 的端口号      */
    char     name[NCL_SYNTEC_AXIS_NAME_MAX]; /**< "X" / "X2" / "Z" …      */
} ncl_syntec_axis;

/**
 * 控制器说自己在用哪些轴，次序就是位置数组的次序（表里第 0 条 = 位置第 0 个）。
 * 判据抄自客户端的 `get_EnableAxisMappingID()`：**端口号 > 0 且 0 < 轴名代号 <
 * 10000** 的槽才算数；`get_MaxUsedAxisID()` 又是这张表的**最后一条**，而状态区
 * 一共有 `MaxUsedAxisID + 1` 项——所以轴表也顺带告诉了你位置数组有多长。
 *
 * 一条都没读到时回 NCL_ERR_UNAVAILABLE（理由写在 ncl_syntec_last_error()），
 * 而不是猜一个轴出来。@p out 是调用者的数组，@p count 回填实际条数。
 */
ncl_err ncl_syntec_axes(ncl_syntec *syntec, ncl_syntec_axis *out, size_t cap,
                        size_t *count);

/**
 * 轴名 -> 轴号（§11.4）。**轴号就是参数槽号**，也就是状态区里的下标：客户端的
 * `get_MachineCoordinate()` 是 `r[i] = data[EnableAxisMappingID[i]]`——`data` 按
 * 槽排，`r` 按"在用的轴"排。所以按路径里写死的 `X` 读值时：先在这里把 `X` 换成
 * 槽号，再去读状态区，取第槽号个值。
 *
 * @p count 回填状态区一项里有几个轴值（客户端 `get_MaxUsedAxisID() + 1`），也就
 * 是这次位置读该读几个。表带缓存（`NCL_SYNTEC_AXES_TTL_MS`）：读一次要问 32 个
 * 参数，不能每个点位都问；刷新失败时继续用上一张好表。
 *
 * 名字不在"在用的轴"里（端口 0、没名字、或压根不在表里）回 NCL_ERR_NOT_FOUND，
 * 并把控制器说在用的名字写进 ncl_syntec_last_error()——不猜。
 */
ncl_err ncl_syntec_axis_index(ncl_syntec *syntec, const char *name,
                              unsigned *slot, size_t *count);

/** 轴表缓存的有效期（毫秒）。 */
#define NCL_SYNTEC_AXES_TTL_MS 5000u

/* The nine items, each one named after what it reads (§3.2). A call that the
 * captured material does not cover answers NCL_ERR_UNAVAILABLE - "还读不了" -
 * with ncl_syntec_last_error() saying which part is missing. */

/** STATUS: "running" / "free" / "holding" / "unknown" (state 4, §3.2). */
ncl_err ncl_syntec_status(ncl_syntec *syntec, char *out, size_t cap);
/** PART_COUNT: the register 1000 (u16). */
ncl_err ncl_syntec_part_count(ncl_syntec *syntec, long long *value);
/** LINE_NUMBER: the register 10 (u16). */
ncl_err ncl_syntec_line_number(ncl_syntec *syntec, long long *value);
/** PROGRAM: the whole answer body as text. */
ncl_err ncl_syntec_program(ncl_syntec *syntec, char *out, size_t cap);
/** FEED_SPEED: register 700 combined with states 12 and 76 (§3.2). */
ncl_err ncl_syntec_feed_speed(ncl_syntec *syntec, double *value);
/** FEED_OVERRIDE: the register 19 (u16). */
ncl_err ncl_syntec_feed_override(ncl_syntec *syntec, long long *value);
/** SPDL_SPEED: the register 771 (u16). */
ncl_err ncl_syntec_spindle_speed(ncl_syntec *syntec, long long *value);
/** SPDL_OVERRIDE: the register 21 (u16). */
ncl_err ncl_syntec_spindle_override(ncl_syntec *syntec, long long *value);
/**
 * WARNING: the alarm list. An empty answer is an empty list, which is what the
 * probe saw; the layout of a *populated* answer was never captured, so that
 * case answers NCL_ERR_UNAVAILABLE rather than guessing. The caller owns the
 * array.
 */
ncl_err ncl_syntec_warning(ncl_syntec *syntec, ncl_json **list);

/** The frames of the last exchange (audit): borrowed from the session. */
void ncl_syntec_last_raw(const ncl_syntec *syntec, const uint8_t **request,
                         size_t *request_len, const uint8_t **reply,
                         size_t *reply_len);

/* ================================================================= data == */

/** CRC-16 with the reversed 0xA001 polynomial, as the client library has it. */
uint16_t ncl_syntec_crc16(const uint8_t *data, size_t len);

/**
 * The MMI_Request_KrnlAPI body: uFuncID u2, dwCode i4, dwSizeIn i4,
 * dwSizeOut i4, then the input bytes.
 *
 * The pointer field of the C structure cannot travel, so the bytes it pointed
 * at follow the structure - **this placement is the one part of the packet
 * that still needs a capture to confirm** (§10.8); the header fields are read
 * straight out of the metadata.
 */
size_t ncl_syntec_krnl_body(uint8_t *out, size_t cap, uint16_t func_id,
                            int32_t code, size_t size_in, size_t size_out,
                            const void *in, size_t in_len);

/** The four fields of that body, as parsed from a reply. */
typedef struct {
    uint16_t func_id;
    int32_t  code;
    int32_t  size_in;
    int32_t  size_out;
} ncl_syntec_krnl_request;

/** Read the four fields back (the input bytes are ignored). */
ncl_err ncl_syntec_krnl_parse(const uint8_t *body, size_t len,
                              ncl_syntec_krnl_request *out);

/** A string request body: u4 length + the characters (§10.2). */
size_t ncl_syntec_path_body(uint8_t *out, size_t cap, uint16_t func_id,
                            const char *path);

#ifdef __cplusplus
}
#endif

#endif /* NCL_SYNTEC_H */
