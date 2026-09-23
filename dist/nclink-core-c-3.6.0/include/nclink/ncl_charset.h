/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * 字符集：GB2312 → UTF-8。
 *
 * 为什么库里有这个东西：机床的**文本量是机床自己的字符集** —— FANUC 的中文报警消息
 * 就是 GB2312（一条 80 字节记录里那 4 个字节 = "保护"），而 NC-Link 的 JSON 是 UTF-8。
 * 直接把那些字节塞进 JSON 就是无效 UTF-8（上层看到乱码），所以这里给一张纯查表的
 * 转换（7445 个码位，无第三方依赖、无 locale 依赖，Windows/Linux 一个行为）。
 *
 * 用法：
 *
 *     char *text = NULL;
 *     if (ncl_gb2312_to_utf8(raw, raw_len, &text, NULL) == NCL_OK) {
 *         ... 把 text 当 UTF-8 用 ...
 *         ncl_mem_free(text);
 *     }
 *
 * 约定：ASCII（< 0x80）原样过去（GB2312 的 ASCII 段与 UTF-8 一致）；GB 双字节按码表
 * 转；**不合法的字节变成 U+FFFD（替换字符）**，不报错 —— 机床的文本里常有半截的
 * 汉字或控制字符，让整条报警读不出来更糟。
 */
#ifndef NCL_CHARSET_H
#define NCL_CHARSET_H

#include <stddef.h>
#include <stdint.h>

#include "nclink/ncl_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 一个 GB2312 双字节（@p hi 在前、@p lo 在后）对应的 Unicode 码点。
 * 不是合法 GB2312 就回 0（注意 U+0000 本身不是 GB2312 里的字，所以 0 可以当"没有"）。
 */
uint32_t ncl_gb2312_codepoint(unsigned char hi, unsigned char lo);

/**
 * 把 @p len 个字节的 GB2312（或纯 ASCII）文本转成 UTF-8。
 *
 * @param in       输入（不按 NUL 截断，按 @p len 走，可以是中间的一段）
 * @param len      输入字节数
 * @param out      收下**新分配**的、NUL 结尾的 UTF-8 字符串（ncl_free 释放）
 * @param out_len  非空时收下输出的字节数（不含结尾的 NUL）
 */
ncl_err ncl_gb2312_to_utf8(const char *in, size_t len, char **out,
                           size_t *out_len);

/**
 * 把 @p len 个字节的 **UTF-16LE** 文本（新代控制器的一些文本字段就是这个，
 * 比如系统参数表的标题 `wchar_t[128]`）转成 UTF-8。
 *
 * 约定：
 *   - 遇到 `0x0000` 就当文本结束（定长字段里剩下的都是 NUL 填充）。
 *   - 代理对（U+10000 以上）照样拼；落单的代理项写 U+FFFD，不抛错。
 *   - 奇数个字节时最后一个字节忽略。
 *
 * @param out     产出**新分配**的、NUL 结尾的 UTF-8 字符串；ncl_free 释放
 * @param out_len 非空时写出字节数（不含结尾 NUL）
 */
ncl_err ncl_utf16le_to_utf8(const uint8_t *in, size_t len, char **out,
                            size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* NCL_CHARSET_H */
