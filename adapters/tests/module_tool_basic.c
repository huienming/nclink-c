/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * A one file adapter, written the way an adapter author writes one: a
 * connection, one dispatch function, the point table, and NCL_TOOL_MODULE as
 * the last line. Nothing here knows about drivers, ops tables or the
 * configuration's point map - the declaration is the interface.
 *
 * Built as plugins/ncl_driver_test_tool_basic.dll (or lib...so), so the host
 * test can ask for it by name exactly like it asks for "focas".
 */
#include <string.h>

#include "nclink/ncl_tool.h"

#define TEST_ERR_PROTOCOL 0x20000001

/** What a point carries as its own data - here, the "register" it reads. */
typedef struct {
    long long value;
    bool      writable;
} test_register;

static test_register k_run = {7, false};
static test_register k_mode = {1, true};
/* 参数表：不常变，所以它是"配置型数据对象"，进模型的 configs（不是 dataItems）。 */
static test_register k_param = {1234, true};

static void *test_open(const ncl_json *params, char **err)
{
    (void)params;
    (void)err;
    /* A real adapter would connect here; the fixture just hands back a marker
     * so the points can prove they share one connection. */
    return (void *)&k_run;
}

static void test_close(void *ctx)
{
    (void)ctx;
}

static ncl_err test_dispatch(void *ctx, const ncl_tool_point *self,
                            ncl_operation op, const ncl_json *params,
                            ncl_json **result, char **reason)
{
    test_register *reg = (test_register *)self->arg;

    if (ctx != (void *)&k_run || reg == NULL) {
        return ncl_tool_fail(reason, NCL_ERR_STATE,
                             "the point %s has no connection", self->path);
    }
    switch (op) {
    case NCL_OP_GET_VALUE:
        return ncl_tool_reply_int(result, reg->value);
    case NCL_OP_SET_VALUE:
    {
        const ncl_json *value = ncl_tool_param_value(params);
        long long wanted = 0;

        if (!reg->writable) {
            return ncl_tool_fail(reason, TEST_ERR_PROTOCOL,
                                 "寄存器 %s 是只读的", self->path);
        }
        if (value == NULL || !ncl_json_as_int(value, &wanted)) {
            return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG,
                                 "写入 %s 需要一个整数", self->path);
        }
        reg->value = wanted;
        return ncl_tool_reply_bool(result, true);
    }
    default:
        break;
    }
    return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED, "%s 不支持这个操作",
                         self->path);
}

NCL_TOOL_BEGIN("test_tool_basic", "夹具：一个文件的小适配器", 500, 0,
               test_open, test_close)
    NCL_POINT_SAMPLED_ARG("/MACHINE/RUN", test_dispatch, &k_run)
    NCL_POINT_RW_ARG("/MACHINE/MODE", test_dispatch, &k_mode)
    /* 声明了、但还没有帧可读的点位：模型里有它，问它答"待抓包"，自检不算失败。 */
    NCL_POINT_PENDING("/MACHINE/ALARM", "报警：待抓包（帧还没抓到）")
    /* 配置型数据：PARAMETER 在数据字典里属"不常变"，因此进 CONTROLLER 组件的 configs。 */
    NCL_POINT_ARG("/MACHINE/CONTROLLER/PARAMETER", test_dispatch, &k_param)
NCL_TOOL_END()

/* The last line of the file: who this module is. */
NCL_TOOL_MODULE("0.1.0", "夹具模块：一个 .c 文件就是一个适配器")
