/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * GB2312 → UTF-8：码表抽查 + 边界（ASCII、混排、非法字节、空串）。
 *
 * 抽查的字都是拿 Python 的 gb2312 codec 对过口径的（同一张表生成的）：
 *
 *     "保"  = B1 A3 = U+4FDD = E4 BF 9D
 *     "护"  = BB A4 = U+62A4 = E6 8A A4
 *     "报"  = B1 A8 = U+62A5 = E6 8A A5
 *     "警"  = BE AF = U+8B66 = E8 AD A6
 *     "（）" = A3 A8 / A3 A9 = U+FF08 / U+FF09 = EF BC 88 / EF BC 89   （符号区）
 */
#include <string.h>

#include "nclink/ncl_charset.h"
#include "nclink/ncl_common.h"
#include "ncl_test.h"

/** 转一段字节，回一个堆字符串（用完 ncl_free）。 */
static char *convert(const char *bytes, size_t len)
{
    char *out = NULL;

    if (ncl_gb2312_to_utf8(bytes, len, &out, NULL) != NCL_OK) {
        return NULL;
    }
    return out;
}

static void test_codepoints(void)
{
    NCL_TEST_CASE("码表抽查：汉字与符号区");
    NCL_CHECK_EQ_INT(ncl_gb2312_codepoint(0xB1, 0xA3), 0x4FDD); /* 保 */
    NCL_CHECK_EQ_INT(ncl_gb2312_codepoint(0xBB, 0xA4), 0x62A4); /* 护 */
    NCL_CHECK_EQ_INT(ncl_gb2312_codepoint(0xBE, 0xAF), 0x8B66); /* 警 */
    NCL_CHECK_EQ_INT(ncl_gb2312_codepoint(0xA3, 0xA8), 0xFF08); /* （ */
    NCL_CHECK_EQ_INT(ncl_gb2312_codepoint(0xA1, 0xA1), 0x3000); /* 全角空格 */

    NCL_TEST_CASE("不是合法 GB2312 就回 0");
    NCL_CHECK_EQ_INT(ncl_gb2312_codepoint(0xA1, 0xA0), 0); /* 区外 */
    NCL_CHECK_EQ_INT(ncl_gb2312_codepoint(0x80, 0x80), 0); /* 高位字节在 GB2312 之外 */
    NCL_CHECK_EQ_INT(ncl_gb2312_codepoint(0xFF, 0xFF), 0);
}

static void test_convert(void)
{
    static const char kASCII[] = "SV0075 servo alarm";
    static const char kBaoHu[] = { '\xB1', '\xA3', '\xBB', '\xA4' };          /* 保护 */
    static const char kMixed[] = { 'A', '\xB1', '\xA3', 'B', '\xBB', '\xA4' }; /* A保B护 */
    static const char kBad[] = { 'X', '\x81', 'Y' }; /* 0x81 不在 GB2312 里 */
    char *text;

    NCL_TEST_CASE("纯 ASCII 原样过去");
    text = convert(kASCII, sizeof(kASCII) - 1u);
    NCL_CHECK(text != NULL);
    NCL_CHECK_EQ_STR(text, "SV0075 servo alarm");
    ncl_mem_free(text);

    NCL_TEST_CASE("\"保护\" → 6 字节 UTF-8");
    {
        size_t len = 0;

        NCL_CHECK_EQ_INT(ncl_gb2312_to_utf8(kBaoHu, sizeof(kBaoHu), &text, &len),
                         NCL_OK);
        NCL_CHECK(text != NULL);
        NCL_CHECK_EQ_INT((int)len, 6);
        NCL_CHECK_EQ_STR(text, "\xE4\xBF\x9D\xE6\x8A\xA4");
        ncl_mem_free(text);
    }

    NCL_TEST_CASE("ASCII 与汉字混排");
    text = convert(kMixed, sizeof(kMixed));
    NCL_CHECK(text != NULL);
    /* 注意：C 的 \x 转义是贪婪的，后面再跟十六进制字符就得把字符串断开 */
    NCL_CHECK_EQ_STR(text, "A\xE4\xBF\x9D" "B" "\xE6\x8A\xA4");
    ncl_mem_free(text);

    NCL_TEST_CASE("非法字节变成一个替换字符（U+FFFD），整条不丢");
    text = convert(kBad, sizeof(kBad));
    NCL_CHECK(text != NULL);
    NCL_CHECK_EQ_STR(text, "X\xEF\xBF\xBDY");
    ncl_mem_free(text);

    NCL_TEST_CASE("空串与非 NUL 结尾的片段");
    text = convert("", 0);
    NCL_CHECK(text != NULL);
    NCL_CHECK_EQ_STR(text, "");
    ncl_mem_free(text);
    /* 只转前 2 个字节（不按 NUL 截断） */
    text = convert("AB\xB1\xA3", 2);
    NCL_CHECK(text != NULL);
    NCL_CHECK_EQ_STR(text, "AB");
    ncl_mem_free(text);

    NCL_TEST_CASE("参数错");
    NCL_CHECK_EQ_INT(ncl_gb2312_to_utf8(NULL, 1, &text, NULL),
                     NCL_ERR_INVALID_ARG);
    NCL_CHECK_EQ_INT(ncl_gb2312_to_utf8("A", 1, NULL, NULL),
                     NCL_ERR_INVALID_ARG);
}

NCL_TEST_MAIN_BEGIN()
    test_codepoints();
    test_convert();
NCL_TEST_MAIN_END()
