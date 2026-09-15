/* Unit tests for the hex codec and the optional zlib codec. */
#include "ncl_test.h"

#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_codec.h"

static void test_hex_round_trip(void)
{
    static const unsigned char input[] = {0x00, 0x0F, 0xA5, 0xFF, 0x7F, 0x80};
    ncl_buffer encoded;
    ncl_buffer decoded;

    NCL_TEST_CASE("hex encoding uses upper case pairs");
    NCL_CHECK_EQ_INT(ncl_codec_encode_hex(input, sizeof(input), &encoded), NCL_OK);
    NCL_CHECK_EQ_INT(encoded.len, sizeof(input) * 2);
    NCL_CHECK(memcmp(encoded.data, "000FA5FF7F80", 12) == 0);

    NCL_TEST_CASE("hex decoding is the exact inverse");
    NCL_CHECK_EQ_INT(ncl_codec_decode_hex(encoded.data, encoded.len, &decoded), NCL_OK);
    NCL_CHECK_EQ_INT(decoded.len, sizeof(input));
    NCL_CHECK(memcmp(decoded.data, input, sizeof(input)) == 0);

    ncl_buffer_free(&encoded);
    ncl_buffer_free(&decoded);

    NCL_TEST_CASE("empty input is accepted");
    NCL_CHECK_EQ_INT(ncl_codec_encode_hex(NULL, 0, &encoded), NCL_OK);
    NCL_CHECK_EQ_INT(encoded.len, 0);
    ncl_buffer_free(&encoded);
}

static void test_hex_validation(void)
{
    ncl_buffer out;

    NCL_TEST_CASE("lower case and out-of-range characters are rejected");
    NCL_CHECK_EQ_INT(ncl_codec_decode_hex((const unsigned char *)"0f", 2, &out),
                     NCL_ERR_INVALID_VALUE);
    NCL_CHECK_EQ_INT(ncl_codec_decode_hex((const unsigned char *)"0G", 2, &out),
                     NCL_ERR_INVALID_VALUE);
    NCL_CHECK_EQ_INT(ncl_codec_decode_hex((const unsigned char *)"0:", 2, &out),
                     NCL_ERR_INVALID_VALUE);

    NCL_TEST_CASE("odd length input is rejected instead of truncated");
    NCL_CHECK_EQ_INT(ncl_codec_decode_hex((const unsigned char *)"ABC", 3, &out),
                     NCL_ERR_INVALID_REQUEST);
}

static void test_compress(void)
{
    static const char payload[] =
        "{\"@id\":\"m1\",\"ids\":[{\"id\":\"/STATUS\"},{\"id\":\"/PART_COUNT\"}]}";
    ncl_buffer compressed;
    ncl_buffer restored;

    if (!ncl_codec_compress_available()) {
        NCL_TEST_CASE("zlib codec disabled at build time");
        NCL_CHECK_EQ_INT(ncl_codec_encode_compress((const unsigned char *)payload,
                                                   sizeof(payload) - 1, &compressed),
                         NCL_ERR_NOT_SUPPORTED);
        return;
    }

    NCL_TEST_CASE("deflate/inflate round trip");
    NCL_CHECK_EQ_INT(ncl_codec_encode_compress((const unsigned char *)payload,
                                               sizeof(payload) - 1, &compressed),
                     NCL_OK);
    NCL_CHECK(compressed.len > 0);
    NCL_CHECK_EQ_INT(ncl_codec_decode_compress(compressed.data, compressed.len,
                                               &restored),
                     NCL_OK);
    NCL_CHECK_EQ_INT(restored.len, sizeof(payload) - 1);
    NCL_CHECK(memcmp(restored.data, payload, sizeof(payload) - 1) == 0);
    ncl_buffer_free(&compressed);
    ncl_buffer_free(&restored);

    NCL_TEST_CASE("garbage input fails to inflate");
    NCL_CHECK_EQ_INT(ncl_codec_decode_compress((const unsigned char *)"not zlib", 8,
                                               &restored),
                     NCL_ERR_PARSE);
}

static void test_dispatch(void)
{
    static const unsigned char input[] = {1, 2, 3};
    ncl_buffer out;

    NCL_TEST_CASE("codec kind dispatch");
    NCL_CHECK_EQ_INT(ncl_codec_encode(NCL_CODEC_DEFAULT, input, sizeof(input), &out),
                     NCL_OK);
    /* ncl_buffer is not NUL terminated: compare by length. */
    NCL_CHECK_EQ_INT(out.len, 6);
    NCL_CHECK(memcmp(out.data, "010203", 6) == 0);
    ncl_buffer_free(&out);
}

NCL_TEST_MAIN_BEGIN()
    test_hex_round_trip();
    test_hex_validation();
    test_compress();
    test_dispatch();
NCL_TEST_MAIN_END()
