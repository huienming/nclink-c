/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link adapter - LSV2 byte level (see ncl_lsv2.h).
 */

#include "nclink/clients/lsv2.h"

#include <stdio.h>
#include <string.h>

#define LSV2_MAX_FRAME 0x10000u /* the length field is four bytes, but a frame
                                 * this side of 64 KiB is what a control sends */

size_t ncl_lsv2_frame(uint8_t *out, size_t cap, const char *command,
                      const void *payload, size_t payload_len)
{
    size_t total = 8u + payload_len;

    if (out == NULL || command == NULL || strlen(command) != 4 ||
        cap < total || payload_len > LSV2_MAX_FRAME ||
        (payload_len > 0 && payload == NULL)) {
        return 0;
    }
    /* §7.1: big endian, unlike every PLC protocol in this tree. */
    out[0] = (uint8_t)(payload_len >> 24);
    out[1] = (uint8_t)(payload_len >> 16);
    out[2] = (uint8_t)(payload_len >> 8);
    out[3] = (uint8_t)payload_len;
    memcpy(out + 4, command, 4);
    if (payload_len > 0) {
        memcpy(out + 8, payload, payload_len);
    }
    return total;
}

ncl_err ncl_lsv2_split(const uint8_t *frame, size_t len, char name[5],
                       const uint8_t **payload, size_t *payload_len,
                       size_t *frame_len)
{
    size_t declared;
    size_t total;

    if (frame == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    if (len < 8) {
        return NCL_ERR_RANGE; /* the header is still arriving */
    }
    declared = ((size_t)frame[0] << 24) | ((size_t)frame[1] << 16) |
               ((size_t)frame[2] << 8) | frame[3];
    if (declared > LSV2_MAX_FRAME) {
        return NCL_DRV_ERR_PROTOCOL(0x60);
    }
    total = 8u + declared;
    if (len < total) {
        return NCL_ERR_RANGE;
    }
    if (name != NULL) {
        memcpy(name, frame + 4, 4);
        name[4] = '\0';
    }
    if (payload != NULL) {
        *payload = frame + 8;
    }
    if (payload_len != NULL) {
        *payload_len = declared;
    }
    if (frame_len != NULL) {
        *frame_len = total;
    }
    return NCL_OK;
}

size_t ncl_lsv2_string_payload(char *out, size_t cap, const char *text)
{
    size_t len;

    if (out == NULL || text == NULL) {
        return 0;
    }
    len = strlen(text);
    if (len + 1u > cap) {
        return 0;
    }
    memcpy(out, text, len);
    out[len] = '\0';
    return len + 1u; /* the NUL is part of the payload (§7.2) */
}

/* ================================================================ 表 ====== */

static const struct {
    const char *name;
    const char *text;
} kCommands[] = {
    /* §4.1 登录 */
    {"A_LG", "登录"},
    {"A_LO", "登出"},
    /* §4.2 控制 */
    {"C_CC", "设置系统命令"},
    {"C_DC", "切换工作目录"},
    {"C_DT", "设置日期时间"},
    {"C_DD", "删除目录"},
    {"C_DM", "新建目录"},
    {"C_EK", "发送按键码"},
    {"C_FA", "修改文件属性"},
    {"C_FC", "本地文件复制"},
    {"C_FD", "删除文件"},
    {"C_FL", "上传文件到控制器"},
    {"C_FR", "本地文件移动"},
    {"C_LK", "锁定/解锁键盘输入"},
    {"C_MC", "设置机器参数"},
    {"C_ST", "设置状态"},
    /* §4.3 读取 */
    {"R_CD", "字符集"},
    {"R_DI", "目录信息"},
    {"R_DP", "从数据路径读数据"},
    {"R_DR", "目录内容"},
    {"R_DT", "日期时间"},
    {"R_FI", "文件信息"},
    {"R_FL", "从控制器下载文件"},
    {"R_FO", "字体定义"},
    {"R_LB", "日志缓冲"},
    {"R_MB", "读 PLC 内存"},
    {"R_MC", "读机器参数"},
    {"R_MP", "读机器参数"},
    {"R_PD", "托盘定义"},
    {"R_PR", "读控制参数"},
    {"R_RI", "读控制器状态"},
    {"R_RS", "寄存器状态"},
    {"R_SD", "屏幕截图"},
    {"R_SE", "屏幕窗口元素信息"},
    {"R_SP", "屏幕调色板"},
    {"R_SS", "当前活动屏幕"},
    {"R_ST", "远程状态"},
    {"R_SW", "屏幕窗口信息"},
    {"R_VR", "控制器版本信息"},
    {"R_WD", "窗口定义"},
    /* §4.4 响应 */
    {"T_OK", "事务完成"},
    {"T_ER", "错误"},
    {"T_FD", "文件传输完成"},
    {"T_BD", "文件传输出错"},
    {"M_CC", "操作完成"},
    {"S_DI", "目录信息应答"},
    {"S_DR", "目录内容应答"},
    {"S_DP", "数据路径应答"},
    {"S_FI", "文件信息应答"},
    {"S_FL", "文件数据块"},
    {"S_MB", "PLC 内存应答"},
    {"S_MC", "参数读取应答"},
    {"S_PR", "参数读取应答"},
    {"S_RI", "控制器状态应答"},
    {"S_ST", "远程状态应答"},
    {"S_VR", "版本应答"},
    {"NONE", "无响应"},
    {"UNKN", "未知响应"},
};

static const char *lookup(const char *name)
{
    size_t i;

    if (name == NULL || strlen(name) != 4) {
        return NULL;
    }
    for (i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
        if (strncmp(kCommands[i].name, name, 4) == 0) {
            return kCommands[i].text;
        }
    }
    return NULL;
}

bool ncl_lsv2_command_known(const char *name)
{
    return lookup(name) != NULL;
}

const char *ncl_lsv2_command_text(const char *name)
{
    return lookup(name);
}

/* ================================================================ 错误 ==== */

static const struct {
    int         code;
    const char *text;
} kStatus[] = {
    {0, "LSV2_OK"},
    {1, "LSV2_TIMEOUT"},
    {2, "LSV2_NO_ENQ"},
    {3, "LSV2_TIMEOUT2"},
    {4, "LSV2_WRONG_CHAR"},
    {5, "LSV2_TO_LONG"},
    {6, "LSV2_WRONG_BBC"},
    {7, "LSV2_NO_EOT"},
    {12, "LSV2_TIMEOUT3"},
    {16, "LSV2_NO_MESSAGE"},
};

const char *ncl_lsv2_status_text(int code)
{
    size_t i;

    if (code == 0) {
        return "LSV2_OK";
    }
    for (i = 0; i < sizeof(kStatus) / sizeof(kStatus[0]); i++) {
        if (kStatus[i].code == code) {
            return kStatus[i].text;
        }
    }
    return "LSV2_UNKNOWN_STATUS";
}

ncl_err ncl_lsv2_check_status(int code, char *message, size_t message_len)
{
    if (code == 0) {
        return NCL_OK;
    }
    if (message != NULL) {
        snprintf(message, message_len, "LSV2 状态 %d：%s", code,
                 ncl_lsv2_status_text(code));
    }
    /* §6: the timeouts and the "no reply" family are link problems; a wrong
     * block check or a too long frame is the protocol's business; the rest is
     * the control refusing the request. */
    switch (code) {
    case 1:
    case 2:
    case 3:
    case 12:
    case 16:
        return NCL_DRV_ERR_TRANSPORT(0x60 | code);
    case 4:
    case 5:
    case 6:
    case 7:
        return NCL_DRV_ERR_PROTOCOL(0x60 | code);
    default:
        return NCL_DRV_ERR_BUSINESS(0x60 | code);
    }
}

/* ============================================================== 数据模型 = */

static const struct {
    const char *name;
    unsigned    code;
} kMemoryTypes[] = {
    {"MARKER", 1},        {"M", 1},
    {"INPUT", 2},         {"I", 2},
    {"OUTPUT", 3},        {"O", 3},
    {"COUNTER", 4},       {"TIMER", 5},
    {"BYTE", 6},          {"B", 6},
    {"WORD", 7},          {"W", 7},
    {"DWORD", 8},         {"STRING", 9},
    {"INPUT_WORD", 10},   {"OUTPUT_WORD", 11},
    {"OUTPUT_DWORD", 12}, {"INPUT_DWORD", 13},
};

bool ncl_lsv2_memory_type(const char *name, unsigned *code)
{
    size_t i;

    if (ncl_str_is_blank(name)) {
        return false;
    }
    for (i = 0; i < sizeof(kMemoryTypes) / sizeof(kMemoryTypes[0]); i++) {
        if (ncl_streq_ignore_case(name, kMemoryTypes[i].name)) {
            if (code != NULL) {
                *code = kMemoryTypes[i].code;
            }
            return true;
        }
    }
    return false;
}

const char *ncl_lsv2_exec_state(unsigned value)
{
    static const char *const kNames[] = {"MANUAL", "MDI", "PASS_REFERENCES",
                                         "SINGLE_STEP", "AUTOMATIC", "UNDEFINED"};

    return value < 6 ? kNames[value] : "UNDEFINED";
}

const char *ncl_lsv2_pgm_state(unsigned value)
{
    static const char *const kNames[] = {"STARTED", "STOPPED", "FINISHED",
                                         "CANCELLED", "INTERRUPTED", "ERROR",
                                         "ERROR_CLEARED", "IDLE", "UNDEFINED"};

    return value < 9 ? kNames[value] : "UNDEFINED";
}

size_t ncl_lsv2_read_memory_payload(uint8_t *out, size_t cap, int64_t address,
                                    unsigned count)
{
    uint32_t value;

    if (out == NULL || cap < 5 || address < 0 || address > 0x7FFFFFFF) {
        return 0;
    }
    /* §4.3: R_MB takes a four byte address and one length byte, both as the
     * point map wrote them - the memory type rides in the address itself. */
    value = (uint32_t)address;
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
    out[4] = (uint8_t)count;
    return 5;
}

ncl_err ncl_lsv2_decode_memory(const uint8_t *payload, size_t len,
                               ncl_dtype dtype, size_t text_chars, ncl_json **out)
{
    char text[512];

    if (out != NULL) {
        *out = NULL;
    }
    if (payload == NULL || out == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    switch (dtype) {
    case NCL_DTYPE_BIT:
        return len < 1 ? NCL_ERR_RANGE
                       : (*out = ncl_json_new_bool(payload[0] != 0), NCL_OK);
    case NCL_DTYPE_BYTE:
        return len < 1 ? NCL_ERR_RANGE
                       : (*out = ncl_json_new_int(payload[0]), NCL_OK);
    case NCL_DTYPE_INT16:
        return len < 2 ? NCL_ERR_RANGE
                       : (*out = ncl_json_new_int((int16_t)((payload[0] << 8) |
                                                            payload[1])),
                          NCL_OK);
    case NCL_DTYPE_INT32:
        return len < 4
                   ? NCL_ERR_RANGE
                   : (*out = ncl_json_new_int((int32_t)(((uint32_t)payload[0] << 24) |
                                                        ((uint32_t)payload[1] << 16) |
                                                        ((uint32_t)payload[2] << 8) |
                                                        payload[3])),
                      NCL_OK);
    case NCL_DTYPE_FLOAT32: {
        uint32_t raw;
        float value;

        if (len < 4) {
            return NCL_ERR_RANGE;
        }
        raw = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
              ((uint32_t)payload[2] << 8) | payload[3];
        memcpy(&value, &raw, sizeof(value));
        *out = ncl_json_new_double((double)value);
        return NCL_OK;
    }
    case NCL_DTYPE_STRING: {
        size_t chars = text_chars < sizeof(text) - 1 ? text_chars
                                                     : sizeof(text) - 1;
        size_t i;

        if (chars > len) {
            chars = len;
        }
        memcpy(text, payload, chars);
        text[chars] = '\0';
        for (i = 0; i < chars; i++) {
            if (text[i] == '\0') {
                text[i] = ' ';
            }
        }
        *out = ncl_json_new_string(text);
        return *out != NULL ? NCL_OK : NCL_ERR_NOMEM;
    }
    default:
        return NCL_ERR_INVALID_DATA_TYPE;
    }
}
