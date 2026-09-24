/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link tool layer - 取值/置值函数绑定的实现（见 include/nclink/ncl_tool.h）。
 *
 * 每个族一个包装，把 client 的语义函数接成点位函数：读 → 调取值函数 → 出参转 JSON
 * 应答；写 → 从请求参数里取新值 → 调置值函数 → 回写成功的值。实例就是 open() 返回
 * 的那个 client 指针，这里对它不做任何类型假设（宏把函数指针按族放进点位的 arg）。
 *
 * 失败一律变成 code=NG + 理由：理由里带路径、错误名（ncl_err_name）和函数类别，
 * 现场看一眼日志就知道是"取值失败"还是"这台机器不支持写"。
 */
#include "nclink/ncl_tool.h"

#include "nclink/ncl_common.h"
#include "nclink/ncl_json.h"

/** 文本出参的缓冲：够放一个程序名/报警文本；实现自己截断，我们只保证 NUL 结尾。 */
#define NCL_VALUE_TEXT_CAP 256

static const ncl_tool_value_spec *bind_spec(const ncl_tool_point *self)
{
    return self != NULL ? (const ncl_tool_value_spec *)self->arg : NULL;
}

/** 点位没绑到这个操作上（只读点被写、没绑方法…）。 */
static ncl_err bind_refuse(char **reason, const ncl_tool_point *self,
                           const char *what)
{
    return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED, "%s 没有%s函数（绑定）",
                         self != NULL ? self->path : "?", what);
}

/**
 * 语义函数自己报了错：把操作、路径和错误名交出去。
 *
 * NCL_ERR_UNAVAILABLE 是例外，它不是"读失败"：这一份 client 还没有那个协议调用
 * （见 ncl_common.h），所以照实说"还读不了"，工具层也据此把这条点位从轮询与 §6
 * 审计里摘出去。要带上 client 自己的一句话（"帧待抓包（cnc_rdalmmsg2）"），就照
 * 常自己写函数、用 ncl_tool_fail() 把 ncl_focas_last_error() 带上，那条答复一样
 * 会被当"还读不了"（工具层看的是码）。
 */
static ncl_err bind_failed(char **reason, const ncl_tool_point *self,
                           const char *what, ncl_err code)
{
    if (code == NCL_ERR_UNAVAILABLE) {
        return ncl_tool_fail(reason, code, "%s：还读不了（%s）",
                             self != NULL ? self->path : "?",
                             ncl_err_name(code));
    }
    return ncl_tool_fail(reason, code, "%s %s失败：%s",
                         self != NULL ? self->path : "?", what,
                         ncl_err_name(code));
}

/* 取值：整数 ------------------------------------------------------------- */

static ncl_err bind_get_i64(void *ctx, const ncl_tool_point *self,
                            const ncl_tool_value_spec *spec, ncl_json **result,
                            char **reason)
{
    ncl_tool_get_i64_fn get = (ncl_tool_get_i64_fn)spec->get;
    long long value = 0;
    ncl_err rc;

    if (get == NULL) {
        return bind_refuse(reason, self, "取值");
    }
    rc = get(ctx, &value);
    if (rc != NCL_OK) {
        return bind_failed(reason, self, "取值", rc);
    }
    return ncl_tool_reply_int(result, value);
}

static ncl_err bind_set_i64(void *ctx, const ncl_tool_point *self,
                            const ncl_tool_value_spec *spec,
                            const ncl_json *params, ncl_json **result,
                            char **reason)
{
    ncl_tool_set_i64_fn set = (ncl_tool_set_i64_fn)spec->set;
    long long wanted = 0;
    ncl_err rc;

    if (set == NULL) {
        return bind_refuse(reason, self, "写值");
    }
    if (!ncl_json_as_int(ncl_tool_param_value(params), &wanted)) {
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "%s 的新值不是整数",
                             self->path);
    }
    rc = set(ctx, wanted);
    if (rc != NCL_OK) {
        return bind_failed(reason, self, "写值", rc);
    }
    return ncl_tool_reply_int(result, wanted);
}

ncl_err ncl_tool_value_i64(void *ctx, const ncl_tool_point *self,
                          ncl_operation op, const ncl_json *params,
                          ncl_json **result, char **reason)
{
    const ncl_tool_value_spec *spec = bind_spec(self);

    if (spec == NULL) {
        return bind_refuse(reason, self, "取值");
    }
    if (op == NCL_OP_SET_VALUE) {
        return bind_set_i64(ctx, self, spec, params, result, reason);
    }
    return bind_get_i64(ctx, self, spec, result, reason);
}

ncl_err ncl_tool_value_i64_arg(void *ctx, const ncl_tool_point *self,
                              ncl_operation op, const ncl_json *params,
                              ncl_json **result, char **reason)
{
    const ncl_tool_value_spec *spec = bind_spec(self);
    ncl_tool_get_i64_arg_fn get;
    long long value = 0;
    ncl_err rc;

    (void)op;
    (void)params;
    if (spec == NULL || spec->get == NULL) {
        return bind_refuse(reason, self, "取值");
    }
    get = (ncl_tool_get_i64_arg_fn)spec->get;
    rc = get(ctx, spec->arg, &value);
    if (rc != NCL_OK) {
        return bind_failed(reason, self, "取值", rc);
    }
    return ncl_tool_reply_int(result, value);
}

/* 取值：浮点 ------------------------------------------------------------- */

static ncl_err bind_get_f64(void *ctx, const ncl_tool_point *self,
                            const ncl_tool_value_spec *spec, ncl_json **result,
                            char **reason)
{
    ncl_tool_get_f64_fn get = (ncl_tool_get_f64_fn)spec->get;
    double value = 0.0;
    ncl_err rc;

    if (get == NULL) {
        return bind_refuse(reason, self, "取值");
    }
    rc = get(ctx, &value);
    if (rc != NCL_OK) {
        return bind_failed(reason, self, "取值", rc);
    }
    return ncl_tool_reply_double(result, value);
}

static ncl_err bind_set_f64(void *ctx, const ncl_tool_point *self,
                            const ncl_tool_value_spec *spec,
                            const ncl_json *params, ncl_json **result,
                            char **reason)
{
    ncl_tool_set_f64_fn set = (ncl_tool_set_f64_fn)spec->set;
    double wanted = 0.0;
    ncl_err rc;

    if (set == NULL) {
        return bind_refuse(reason, self, "写值");
    }
    if (!ncl_json_as_double(ncl_tool_param_value(params), &wanted)) {
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "%s 的新值不是数值",
                             self->path);
    }
    rc = set(ctx, wanted);
    if (rc != NCL_OK) {
        return bind_failed(reason, self, "写值", rc);
    }
    return ncl_tool_reply_double(result, wanted);
}

ncl_err ncl_tool_value_f64(void *ctx, const ncl_tool_point *self,
                          ncl_operation op, const ncl_json *params,
                          ncl_json **result, char **reason)
{
    const ncl_tool_value_spec *spec = bind_spec(self);

    if (spec == NULL) {
        return bind_refuse(reason, self, "取值");
    }
    if (op == NCL_OP_SET_VALUE) {
        return bind_set_f64(ctx, self, spec, params, result, reason);
    }
    return bind_get_f64(ctx, self, spec, result, reason);
}

ncl_err ncl_tool_value_f64_arg(void *ctx, const ncl_tool_point *self,
                              ncl_operation op, const ncl_json *params,
                              ncl_json **result, char **reason)
{
    const ncl_tool_value_spec *spec = bind_spec(self);
    ncl_tool_get_f64_arg_fn get;
    double value = 0.0;
    ncl_err rc;

    (void)op;
    (void)params;
    if (spec == NULL || spec->get == NULL) {
        return bind_refuse(reason, self, "取值");
    }
    get = (ncl_tool_get_f64_arg_fn)spec->get;
    rc = get(ctx, spec->arg, &value);
    if (rc != NCL_OK) {
        return bind_failed(reason, self, "取值", rc);
    }
    return ncl_tool_reply_double(result, value);
}

/* 取值：布尔 ------------------------------------------------------------- */

static ncl_err bind_get_bool(void *ctx, const ncl_tool_point *self,
                             const ncl_tool_value_spec *spec, ncl_json **result,
                             char **reason)
{
    ncl_tool_get_bool_fn get = (ncl_tool_get_bool_fn)spec->get;
    bool value = false;
    ncl_err rc;

    if (get == NULL) {
        return bind_refuse(reason, self, "取值");
    }
    rc = get(ctx, &value);
    if (rc != NCL_OK) {
        return bind_failed(reason, self, "取值", rc);
    }
    return ncl_tool_reply_bool(result, value);
}

ncl_err ncl_tool_value_bool(void *ctx, const ncl_tool_point *self,
                           ncl_operation op, const ncl_json *params,
                           ncl_json **result, char **reason)
{
    const ncl_tool_value_spec *spec = bind_spec(self);

    (void)op;
    (void)params;
    if (spec == NULL) {
        return bind_refuse(reason, self, "取值");
    }
    return bind_get_bool(ctx, self, spec, result, reason);
}

ncl_err ncl_tool_value_bool_arg(void *ctx, const ncl_tool_point *self,
                               ncl_operation op, const ncl_json *params,
                               ncl_json **result, char **reason)
{
    const ncl_tool_value_spec *spec = bind_spec(self);
    ncl_tool_get_bool_arg_fn get;
    bool value = false;
    ncl_err rc;

    (void)op;
    (void)params;
    if (spec == NULL || spec->get == NULL) {
        return bind_refuse(reason, self, "取值");
    }
    get = (ncl_tool_get_bool_arg_fn)spec->get;
    rc = get(ctx, spec->arg, &value);
    if (rc != NCL_OK) {
        return bind_failed(reason, self, "取值", rc);
    }
    return ncl_tool_reply_bool(result, value);
}

/* 文本（读 / 写）--------------------------------------------------------- */

static ncl_err bind_get_str(void *ctx, const ncl_tool_point *self,
                            const ncl_tool_value_spec *spec, ncl_json **result,
                            char **reason)
{
    ncl_tool_get_str_fn get = (ncl_tool_get_str_fn)spec->get;
    char text[NCL_VALUE_TEXT_CAP];
    ncl_err rc;

    if (get == NULL) {
        return bind_refuse(reason, self, "取值");
    }
    memset(text, 0, sizeof(text));
    rc = get(ctx, text, sizeof(text));
    if (rc != NCL_OK) {
        return bind_failed(reason, self, "取值", rc);
    }
    text[sizeof(text) - 1] = '\0'; /* 实现没截断也兜住 */
    return ncl_tool_reply_text(result, text);
}

static ncl_err bind_set_str(void *ctx, const ncl_tool_point *self,
                            const ncl_tool_value_spec *spec,
                            const ncl_json *params, ncl_json **result,
                            char **reason)
{
    ncl_tool_set_str_fn set = (ncl_tool_set_str_fn)spec->set;
    const char *wanted = ncl_json_as_string(ncl_tool_param_value(params));
    ncl_err rc;

    if (set == NULL) {
        return bind_refuse(reason, self, "写值");
    }
    if (wanted == NULL) {
        return ncl_tool_fail(reason, NCL_ERR_INVALID_ARG, "%s 的新值不是文本",
                             self->path);
    }
    rc = set(ctx, wanted);
    if (rc != NCL_OK) {
        return bind_failed(reason, self, "写值", rc);
    }
    return ncl_tool_reply_text(result, wanted);
}

ncl_err ncl_tool_value_str(void *ctx, const ncl_tool_point *self,
                          ncl_operation op, const ncl_json *params,
                          ncl_json **result, char **reason)
{
    const ncl_tool_value_spec *spec = bind_spec(self);

    if (spec == NULL) {
        return bind_refuse(reason, self, "取值");
    }
    if (op == NCL_OP_SET_VALUE) {
        return bind_set_str(ctx, self, spec, params, result, reason);
    }
    return bind_get_str(ctx, self, spec, result, reason);
}

ncl_err ncl_tool_value_str_arg(void *ctx, const ncl_tool_point *self,
                              ncl_operation op, const ncl_json *params,
                              ncl_json **result, char **reason)
{
    const ncl_tool_value_spec *spec = bind_spec(self);
    ncl_tool_get_str_arg_fn get;
    char text[NCL_VALUE_TEXT_CAP];
    ncl_err rc;

    (void)op;
    (void)params;
    if (spec == NULL || spec->get == NULL) {
        return bind_refuse(reason, self, "取值");
    }
    get = (ncl_tool_get_str_arg_fn)spec->get;
    memset(text, 0, sizeof(text));
    rc = get(ctx, spec->arg, text, sizeof(text));
    if (rc != NCL_OK) {
        return bind_failed(reason, self, "取值", rc);
    }
    text[sizeof(text) - 1] = '\0';
    return ncl_tool_reply_text(result, text);
}

/* 结构化（读）------------------------------------------------------------ */

/*
 * 表型的配置（刀具表、坐标系…）：值本身就是 client 交出来的那个 JSON，宿主原样
 * 放进应答，不再拆成一个字段。出参所有权在成功时交给宿主；实现返回 NULL 视作
 * "没有值"，照样答一个 JSON null，免得应答缺一块。
 */
ncl_err ncl_tool_value_json(void *ctx, const ncl_tool_point *self,
                            ncl_operation op, const ncl_json *params,
                            ncl_json **result, char **reason)
{
    const ncl_tool_value_spec *spec = bind_spec(self);
    ncl_tool_get_json_fn get;
    ncl_json *value = NULL;
    ncl_err rc;

    (void)op;
    (void)params;
    if (spec == NULL || spec->get == NULL) {
        return bind_refuse(reason, self, "取值");
    }
    get = (ncl_tool_get_json_fn)spec->get;
    rc = get(ctx, &value);
    if (rc != NCL_OK) {
        ncl_json_free(value); /* 失败时不该留下东西，兜住实现的不小心 */
        return bind_failed(reason, self, "取值", rc);
    }
    if (value == NULL) {
        value = ncl_json_new_null();
        if (value == NULL) {
            return NCL_ERR_NOMEM;
        }
    }
    *result = value; /* 所有权交给宿主 */
    return NCL_OK;
}

/* 方法 ------------------------------------------------------------------- */

ncl_err ncl_tool_value_method(void *ctx, const ncl_tool_point *self,
                             ncl_operation op, const ncl_json *params,
                             ncl_json **result, char **reason)
{
    const ncl_tool_value_spec *spec = bind_spec(self);
    ncl_tool_method_fn fn;

    (void)op;
    if (spec == NULL || spec->get == NULL) {
        return bind_refuse(reason, self, "方法");
    }
    fn = (ncl_tool_method_fn)spec->get;
    return fn(ctx, params, result, reason);
}
