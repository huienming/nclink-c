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

/**
 * KrnlAPI 的桩头其实是 **16** 字节，不是 NCL_SYNTEC_FUNCTION_HEADER 那 8 字节：
 * OCAPIServer 把包头之后的全部字节当成 `Syntec.OpenCNC.MMI_Request_KrnlAPI`
 * （`{ uFuncID i4, dwCode i4, dwSizeIn i4, dwSizeOut i4, pBufferIn ptr }`，
 * 线上只发前 16 字节），于是
 *
 *   [0..3]   uFuncID   = 200（KrnlAPI 那一路的号）
 *   [4..7]   dwCode    = 0x043F / 0x0440 …（要调哪一个 Krnl API）
 *   [8..11]  dwSizeIn  = In 的字节数
 *   [12..15] dwSizeOut = Out 的字节数（连 Out 自己的 hr 一起算）
 *   [16..]   In 本体，紧跟桩头，大到几百字节也走这里
 *
 * 因此一帧 = 12 + 16 + dwSizeIn，包头的 Length 字段 = 16 + dwSizeIn。
 * `ncl_syntec_item_frame()` 造的那些短帧（dwSizeIn 4 或 8）把 In 塞在桩头最后
 * 8 个字节里，长度同样是 24，与抓到的现场帧逐字节一致。
 */
#define NCL_SYNTEC_KRML_HEAD 16u

/**
 * 应答的形状：12 字节包头 + 传输层 hr（i4）+ Out，而 **Out 的第一个字段永远是
 * 这个 Krnl API 自己的 hr**（控制器侧每个 `Out_OCK_*` 都这么定义，调用方也一律
 * 先看它）。所以
 *
 *   [12..15] 传输层 hr（Socket 层，0 = 送到了）
 *   [16..19] OCK 的 hr（0 = 控制器认了）   NCL_SYNTEC_REPLY_HR
 *   [20..]   Out 的第二个字段起           NCL_SYNTEC_REPLY_BODY
 */
#define NCL_SYNTEC_REPLY_HR 16u

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
/**
 * §11.6 写一个参数：In `{ nNo, newVal }`，A = 8 + 4，B = 参数号，
 * **flag 装的是新值**（读的那一家 flag 固定是 1）。
 */
#define NCL_SYNTEC_CODE_PARAM_PUT 0x0403u

/**
 * §11.7 刀具表（车床刀补）：`0x04C2` 问条数（`NcGetEnabledToolNumber`，`CODE(1,194)`），
 * `0x043F` 读一把（`NcGetToolCompensation`，`CODE(1,63)`，In 只有一个 `nFirst`）。
 * 一条记录 **224 字节**（从控制器侧 `JMarshal::get_SizeOfToolOffset()` 的 IL 读出来：
 * 8 + 27×sizeof(double)），布局见下。
 */
#define NCL_SYNTEC_CODE_TOOL_COUNT 0x04C2u
#define NCL_SYNTEC_CODE_TOOL_GET 0x043Fu
#define NCL_SYNTEC_CODE_TOOL_PUT 0x0440u

/**
 * 写一把（`0x0440` `NcPutToolCompensation`，`CODE(1,64)`）：In 是
 * `{ nToolNo i32, TToolOffset }` = 4 + 224 = **228** 字节（控制器侧
 * `JMarshal::SizeOfOCK_ToolOffsetArray()` = `sizeof(int) + SizeOfToolOffset()`），
 * 所以整帧 = 12（包头）+ 16（桩头）+ 228 = **256** 字节。
 *
 * 桩头那 16 字节里放不下 228 字节的 In，In 必须**跟在桩头后面**（见 `NCL_SYNTEC_KRML_HEAD`）。
 */
#define NCL_SYNTEC_TOOL_IN (4u + NCL_SYNTEC_TOOL_SIZE)
#define NCL_SYNTEC_TOOL_FRAME                                                  \
    (NCL_SYNTEC_PACKET_HEADER + NCL_SYNTEC_KRML_HEAD + NCL_SYNTEC_TOOL_IN)

/** 一条刀补的字节数。 */
#define NCL_SYNTEC_TOOL_SIZE 224u
/** 长度补偿的组数（几何与磨损各 12 组，归属由 `get_LatheToolAxisMappingID` 那张表说）。 */
#define NCL_SYNTEC_TOOL_LENGTHS 12u

/**
 * 一条刀补（线上 224 字节）：`[0..3]` 刀尖号 i32、`[4..7]` 留白、
 * `[8]` 半径几何、`[16]` 半径磨损、`[24..119]` 长度几何 ×12、
 * `[120..215]` 长度磨损 ×12、**`[216]` 刀尖角（也是 double，排在最后）**。
 */
typedef struct {
    int32_t tool_nose;
    double  radius_geometry;
    double  radius_wear;
    double  length_geometry[NCL_SYNTEC_TOOL_LENGTHS];
    double  length_wear[NCL_SYNTEC_TOOL_LENGTHS];
    double  tool_angle;
} ncl_syntec_tool;

/**
 * 把一条刀补编成线上那 224 字节（`ncl_syntec_tool_decode()` 的反函数）。
 * @p record 至少 NCL_SYNTEC_TOOL_SIZE 字节，多出来的位置填 0。
 */
bool ncl_syntec_tool_encode(const ncl_syntec_tool *tool, uint8_t *record,
                            size_t cap);

/**
 * 写一把刀：请求 `0x0440`，帧长 NCL_SYNTEC_TOOL_FRAME（256）字节，
 * In = `{ nToolNo, TToolOffset }`。@p index 是**刀号（从 1 起）**，
 * 和读用的是同一个号，也就和 `/CONTROLLER/TOOL` 的 key 一致。
 */
size_t ncl_syntec_tool_put_frame(uint8_t *out, size_t cap,
                                 const ncl_syntec_tool *tool, unsigned index,
                                 uint8_t serial);
#define NCL_SYNTEC_CODE_STATE_GET 0x0407u

/** Build one parameter read: request 0x0404, A = 8, B = the parameter number. */
size_t ncl_syntec_param_frame(uint8_t *out, size_t cap, unsigned param,
                              uint8_t serial);

/** One parameter write: request 0x0403, A = 12, B = the number, flag = the value. */
size_t ncl_syntec_param_put_frame(uint8_t *out, size_t cap, unsigned param,
                                  int32_t value, uint8_t serial);

/** The parameter table's capacity: request 0x0401, In empty, A = 8. */
size_t ncl_syntec_param_capacity_frame(uint8_t *out, size_t cap,
                                       uint8_t serial);
/**
 * Dump @p count parameter records: request 0x0402, In `{ nLength }` (4 bytes),
 * so A = 4 + count * NCL_SYNTEC_PARAM_SPEC_SIZE, B = count.
 */
size_t ncl_syntec_param_schema_frame(uint8_t *out, size_t cap, size_t count,
                                     uint8_t serial);

/** The i32 a parameter answer carries ([20..23], little endian). */
bool ncl_syntec_reply_i32(const uint8_t *frame, size_t len, int32_t *value);

/**
 * The Krnl API's own `hr` ([16..19], little endian): 0 means the controller
 * accepted the call. `ncl_syntec_reply_i32()` reads [20..23], which is the
 * Out structure's *second* field - right for a read (the value follows the hr)
 * and meaningless for a write (the Out of a write is just `{ hr }`).
 */
bool ncl_syntec_reply_hr(const uint8_t *frame, size_t len, int32_t *hr);

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

/**
 * §11.3.4 指令位置（`SERVO_DRIVER/POSITION` 那一格）：**控制器里还没找到这一项**。
 * 交付的官方客户端只有 机械 / 绝对 / 相对 / 剩余 四个坐标 getter——既没有"指令位置"，
 * 也没有跟随误差；所以这里照实回 NCL_ERR_UNAVAILABLE（"待抓包"），
 * 不拿"实际 + 剩余距离"去凑一个出来。
 */
ncl_err ncl_syntec_command_position(ncl_syntec *syntec, double *value);

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
 * 写一个系统参数（KrnlAPI 0x0403，§11.6）。应答是一个 i32 的 `hr`，0 = 成功，
 * 非 0 回 NCL_ERR_IO 并把 hr 写进 ncl_syntec_last_error()。
 *
 * **写是持久化的**：控制器侧会落到 `OpenCNC/Data/param.dat`（实测：写完文件
 * mtime 变了，逐字节比只有文件头的时间戳与那个参数的值不同）。**权限、白名单、
 * 二次确认都在外面控制**——这里只负责把值写下去，不做判断。
 */
ncl_err ncl_syntec_param_put(ncl_syntec *syntec, unsigned param, int32_t value);

/** 刀具表的条数（0x04C2，21A 的车床答 96）。 */
ncl_err ncl_syntec_tool_count(ncl_syntec *syntec, size_t *count);
/** 读第 @p index 把刀的刀补（0x043F，一条 224 字节）。 */
ncl_err ncl_syntec_tool_get(ncl_syntec *syntec, unsigned index,
                            ncl_syntec_tool *out);

/**
 * 写一把刀（KrnlAPI 0x0440）。@p index 是**刀号，从 1 起** —— 和读用的是同一个号，
 * 也就是 /CONTROLLER/TOOL 的 key。
 *
 * 一次把整条 224 字节记录写下去：控制器侧自己的
 * `NcPutToolCompensation(nToolNo, TToolOffset)` 收的就是完整记录
 * （它的 `SetToolOffsetData()` 把字段分成几组反复写，但每次发的都是整条）。
 * 控制器回 hr，非 0 返 NCL_ERR_IO，原文在 ncl_syntec_last_error()。
 */
ncl_err ncl_syntec_tool_put(ncl_syntec *syntec, unsigned index,
                            const ncl_syntec_tool *tool);

/** 把那两个读法的帧拼出来（A = 4 + 正文，B = 索引，与状态区同一个形状）。 */
size_t ncl_syntec_tool_count_frame(uint8_t *out, size_t cap, uint8_t serial);
size_t ncl_syntec_tool_frame(uint8_t *out, size_t cap, unsigned index,
                             uint8_t serial);
/** 把一条刀补的应答解成结构（正文 224 字节，小端）。 */
bool ncl_syntec_tool_decode(const uint8_t *frame, size_t len,
                            ncl_syntec_tool *out);

/**
 * §11.4 的参数表：线上一条 `TParamSpec` **268 字节**——
 * `u16 No` + `u16 留白` + `wchar Title[128]`（UTF-16LE，定长、NUL 填充）+
 * `u32 字段 A`（语义未定，像类型/范围位）+ `u32 出厂默认值`。
 */
#define NCL_SYNTEC_PARAM_SPEC_SIZE 268u
/** 标题字段的字节数：一个 UTF-16 单元 2 字节 × 128 个字符。 */
#define NCL_SYNTEC_PARAM_TITLE_BYTES 256u
/** 转成 UTF-8 后标题的落点大小（够放 128 字节 ASCII 标题或几十个汉字）。 */
#define NCL_SYNTEC_PARAM_TITLE_MAX 192u

/** 一条参数表记录，转成 C 的写法。 */
typedef struct {
    int32_t no;       /**< 参数号（表里的 `No`）                          */
    int32_t flags;    /**< 第 4 个字段：含义未定，原样给出                 */
    int32_t fallback; /**< 第 5 个字段：出厂默认值（轴名这里是 100 = 'X'） */
    char    title[NCL_SYNTEC_PARAM_TITLE_MAX]; /**< 标题，UTF-8                 */
} ncl_syntec_param_spec;

/** 参数表的容量（0x0401）：21A 答 3784。 */
ncl_err ncl_syntec_param_capacity(ncl_syntec *syntec, size_t *count);

/**
 * 参数表的一段（0x0402，`[first, first + count)`）：标题、参数号、默认值。
 *
 * 整表**一次读回来**（21A 3784 条 ≈ 1 MB）并缓存在会话里，所以翻页很便宜——
 * 线上那条命令没有偏移，只能整表拿，分页由这里做。@p out_count 回填实际条数，
 * @p total 非空时回填整表条数。@p count 超过剩余条数时就给到末尾。
 */
ncl_err ncl_syntec_param_table(ncl_syntec *syntec, size_t first, size_t count,
                               ncl_syntec_param_spec *out, size_t *out_count,
                               size_t *total);

/**
 * 按**参数号**找它在表里的位置（表里的 `No` 列；顺序大致升序，但有跳号，所以
 * 下标 ≠ 参数号）。找到回 NCL_OK 并写出下标，找不到回 NCL_ERR_NOT_FOUND。
 */
ncl_err ncl_syntec_param_find(ncl_syntec *syntec, unsigned no, size_t *index);

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

/* ======================================================== PLC / variables == */

/*
 * §11.9：PLC（梯形图）与变量。码表来自控制器侧 `OCK_CODE` 的 .cctor（433 个 code 全取出来了）：
 *
 *   0x0412 PlcGetIBit       In { nNo }  Out { hr, Value u8 }      I 位
 *   0x0414 PlcGetOBit       In { nNo }  Out { hr, Value u8 }      O 位
 *   0x0415 PlcGetCBit       In { nNo }  Out { hr, Value u8 }      C 位
 *   0x0417 PlcGetSBit       In { nNo }  Out { hr, Value u8 }      S 位
 *   0x0419 PlcGetABit       In { nNo }  Out { hr, Value u8 }      A 位
 *   0x041A PlcGetRRegister  In { nNo }  Out { hr, nValue u32 }    R 寄存器
 *   0x041C PlcGetTimer      In { nNo }  Out { hr, TPlcTimer }     定时器
 *   0x041D PlcGetCounter    In { nNo }  Out { hr, TPlcCounter }   计数器
 *   0x041E PlcGetCapacity   In 空       Out { hr, TPlcCapacity }  容量（8 个 u32）
 *
 *   0x0421 NcGlobalGetValue     In { nNo } Out { hr, TOcVariant }  全局变量（#号）
 *   0x0423 NcGlobalGetCapacity  In 空      Out { hr, nValue u32 }  变量表容量
 *
 * `0x041A` 就是现成的 PART_COUNT（R1000）/ SPDL_SPEED（R771）用的那个码 —— 九项里的
 * “寄存器”本来就是 PLC 寄存器读；这一节只是把它开放成按号读。
 *
 * 21A 实测（2026-09-22）：容量 = I/O/C/S/A 各 512 位、R 寄存器 65536、定时器/计数器各 256；
 * 全局变量 14096 个。R771 = 1000（正是这台机床屏幕上的主轴转速），R700 = 0。
 */
#define NCL_SYNTEC_CODE_PLC_GET_BIT 0x0412u /**< I；下面四个是另外几族（差 2 个数） */
#define NCL_SYNTEC_CODE_PLC_GET_OBIT 0x0414u
#define NCL_SYNTEC_CODE_PLC_GET_CBIT 0x0415u
#define NCL_SYNTEC_CODE_PLC_GET_SBIT 0x0417u
#define NCL_SYNTEC_CODE_PLC_GET_ABIT 0x0419u
#define NCL_SYNTEC_CODE_PLC_GET_REGISTER 0x041Au
#define NCL_SYNTEC_CODE_PLC_GET_TIMER 0x041Cu
#define NCL_SYNTEC_CODE_PLC_GET_COUNTER 0x041Du
#define NCL_SYNTEC_CODE_PLC_GET_CAPACITY 0x041Eu
#define NCL_SYNTEC_CODE_GLOBAL_GET_VALUE 0x0421u
#define NCL_SYNTEC_CODE_GLOBAL_PUT_VALUE 0x0422u
#define NCL_SYNTEC_CODE_GLOBAL_GET_CAPACITY 0x0423u

/*
 * 写这一侧（§11.9）：`0x041B` R 寄存器、`0x0413/16/18` I/C/S 位（O 位只能 Force
 * `0x0494`、A 位没有写）、`0x0422` 变量。In 都是 `{ nNo, 新值 }`，帧长 16 + In。
 */
#define NCL_SYNTEC_CODE_PLC_PUT_REGISTER 0x041Bu

/** 位族（`PlcGet?Bit` 之间就差两个数：0x0412/14/15/17/19）。 */
typedef enum {
    NCL_SYNTEC_PLC_I = 0,
    NCL_SYNTEC_PLC_O,
    NCL_SYNTEC_PLC_C,
    NCL_SYNTEC_PLC_S,
    NCL_SYNTEC_PLC_A,
} ncl_syntec_plc_kind;

/** PLC 容量（`TPlcCapacity { u32 IBits, OBits, CBits, SBits, ABits, RRegister, Timer, Counter }`）。 */
typedef struct {
    uint32_t ibits;
    uint32_t obits;
    uint32_t cbits;
    uint32_t sbits;
    uint32_t abits;
    uint32_t registers; /**< R 寄存器个数（0xFFFF 是 65536） */
    uint32_t timers;
    uint32_t counters;
} ncl_syntec_plc_slots;

/**
 * 一个变量值（`TOcVariant`，16 字节：`i16 nValType` + 6 填 + `i32|f64` 在 [8..]）。
 * `type` 就是线上那个数：0 = 空、1 = 整数、2 = 浮点（3 = 字符串；参考客户端也只会解
 * 前两种，第三种这里按“空”处理）。
 */
typedef struct {
    int16_t type;
    int32_t int_value;
    double  double_value;
} ncl_syntec_variant;

/** PLC 容量。 */
ncl_err ncl_syntec_plc_capacity(ncl_syntec *syntec, ncl_syntec_plc_slots *out);
/** 读一个 R 寄存器（`0x041A`）。 */
ncl_err ncl_syntec_plc_register(ncl_syntec *syntec, unsigned no, uint32_t *value);
/** 读一个位（`0x0412/14/15/17/19`），@p kind 选 I/O/C/S/A。 */
ncl_err ncl_syntec_plc_bit(ncl_syntec *syntec, ncl_syntec_plc_kind kind,
                           unsigned no, bool *value);
/** 变量表容量（`0x0423`）。 */
ncl_err ncl_syntec_variable_capacity(ncl_syntec *syntec, size_t *count);
/** 读一个变量（`0x0421`，号就是 `#` 号）。 */
ncl_err ncl_syntec_variable(ncl_syntec *syntec, unsigned no,
                            ncl_syntec_variant *out);

/** `0x041E`：In 空，Out = { hr, TPlcCapacity }。 */
size_t ncl_syntec_plc_capacity_frame(uint8_t *out, size_t cap, uint8_t serial);
bool ncl_syntec_plc_capacity_decode(const uint8_t *frame, size_t len,
                                     ncl_syntec_plc_slots *out);
/** `0x041A`：In `{ nNo }`，Out = { hr, nValue }。 */
size_t ncl_syntec_plc_register_frame(uint8_t *out, size_t cap, unsigned no,
                                     uint8_t serial);
/** `0x0412 + 2*kind`：In `{ nNo }`，Out = { hr, Value u8 }。 */
size_t ncl_syntec_plc_bit_frame(uint8_t *out, size_t cap,
                                ncl_syntec_plc_kind kind, unsigned no,
                                uint8_t serial);
/** `0x0421`：In `{ nNo }`，Out = { hr, TOcVariant }。 */
size_t ncl_syntec_variable_frame(uint8_t *out, size_t cap, unsigned no,
                                 uint8_t serial);
/** `0x0423`：In 空，Out = { hr, nValue }。 */
size_t ncl_syntec_variable_capacity_frame(uint8_t *out, size_t cap,
                                          uint8_t serial);

/** `0x041B`：In `{ nNo, newVal }`，Out = { hr }。 */
size_t ncl_syntec_plc_register_put_frame(uint8_t *out, size_t cap, unsigned no,
                                         uint32_t value, uint8_t serial);
/** `0x0413/16/18`：I/C/S 位写（O/A 没有写，返 0）。In `{ nNo, newVal u8 }`。 */
size_t ncl_syntec_plc_bit_put_frame(uint8_t *out, size_t cap,
                                    ncl_syntec_plc_kind kind, unsigned no,
                                    bool value, uint8_t serial);
/** `0x0422`：In `{ nNo, TOcVariant }`（20 字节），Out = { hr }。 */
size_t ncl_syntec_variable_put_frame(uint8_t *out, size_t cap, unsigned no,
                                     const ncl_syntec_variant *value,
                                     uint8_t serial);

ncl_err ncl_syntec_plc_register_put(ncl_syntec *syntec, unsigned no,
                                    uint32_t value);
ncl_err ncl_syntec_plc_bit_put(ncl_syntec *syntec, ncl_syntec_plc_kind kind,
                               unsigned no, bool value);
ncl_err ncl_syntec_variable_put(ncl_syntec *syntec, unsigned no,
                                const ncl_syntec_variant *value);

/** 一个 u32 正文（应答 [20..23]，小端）。 */
bool ncl_syntec_reply_u32(const uint8_t *frame, size_t len, uint32_t *value);
/** 一个 u8 正文（应答 [20]，位读用它）。 */
bool ncl_syntec_reply_u8(const uint8_t *frame, size_t len, uint8_t *value);
/** 一个 `TOcVariant`（应答 [20..35]）。 */
bool ncl_syntec_variable_decode(const uint8_t *frame, size_t len,
                                ncl_syntec_variant *out);

#ifdef __cplusplus
}
#endif

#endif /* NCL_SYNTEC_H */
