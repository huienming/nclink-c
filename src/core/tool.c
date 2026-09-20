/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * NC-Link core - the declaration seam between an adapter author and the host.
 *
 * nclink/ncl_tool.h is what an author writes; this file is the other half: the
 * helpers a point function uses, plus the two steps the host runs on a
 * declaration - ncl_tool_model() (declaration -> model document) and
 * ncl_tool_register() (declaration -> open once + methods and bindings).
 *
 * The host's tool API hands every method one shared `instance` pointer and a
 * callback of its own shape (ncl_tool_fn: no place to say *which* point is
 * being served, no place for the point's own data). So the declaration layer
 * puts a small shim in front of each point - {context, point} - and one
 * trampoline per operation, so the author's function gets told which
 * operation reached it. The shims live in the registration the caller holds.
 */

#include "nclink/ncl_tool.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_logger.h"
#include "nclink/ncl_platform.h"

/* ------------------------------------------------------------- diagnostics -- */

static void err_append(ncl_strbuf *err, const char *text)
{
    if (err == NULL) {
        return;
    }
    if (err->len > 0) {
        (void)ncl_strbuf_puts(err, "; ");
    }
    (void)ncl_strbuf_puts(err, text);
}

static void err_append1(ncl_strbuf *err, const char *fmt, const char *arg)
{
    if (err == NULL) {
        return;
    }
    if (err->len > 0) {
        (void)ncl_strbuf_puts(err, "; ");
    }
    (void)ncl_strbuf_printf(err, fmt, arg);
}

static void err_appendf(ncl_strbuf *err, const char *fmt, ...)
{
    char buffer[256];
    va_list args;

    if (err == NULL) {
        return;
    }
    va_start(args, fmt);
    (void)vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (err->len > 0) {
        (void)ncl_strbuf_puts(err, "; ");
    }
    (void)ncl_strbuf_puts(err, buffer);
}

/* ----------------------------------------------------------- point helpers -- */

bool ncl_tool_param_bool(const ncl_json *params, const char *key, bool fallback)
{
    if (params == NULL || key == NULL) {
        return fallback;
    }
    return ncl_json_obj_get_bool(params, key, fallback);
}

long long ncl_tool_param_int(const ncl_json *params, const char *key,
                             long long fallback)
{
    if (params == NULL || key == NULL) {
        return fallback;
    }
    return ncl_json_obj_get_int(params, key, fallback);
}

const char *ncl_tool_param_str(const ncl_json *params, const char *key,
                               const char *fallback)
{
    const char *value;

    if (params == NULL || key == NULL) {
        return fallback;
    }
    value = ncl_json_obj_get_string(params, key);
    return value != NULL ? value : fallback;
}

const ncl_json *ncl_tool_param_value(const ncl_json *params)
{
    const ncl_json *value;

    if (params == NULL) {
        return NULL;
    }
    value = ncl_json_obj_get(params, "value");
    return value != NULL ? value : params;
}

ncl_err ncl_tool_reply_int(ncl_json **result, long long value)
{
    ncl_json *json;

    if (result == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    json = ncl_json_new_int(value);
    if (json == NULL) {
        return NCL_ERR_NOMEM;
    }
    *result = json;
    return NCL_OK;
}

ncl_err ncl_tool_reply_double(ncl_json **result, double value)
{
    ncl_json *json;

    if (result == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    json = ncl_json_new_double(value);
    if (json == NULL) {
        return NCL_ERR_NOMEM;
    }
    *result = json;
    return NCL_OK;
}

ncl_err ncl_tool_reply_bool(ncl_json **result, bool value)
{
    ncl_json *json;

    if (result == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    json = ncl_json_new_bool(value);
    if (json == NULL) {
        return NCL_ERR_NOMEM;
    }
    *result = json;
    return NCL_OK;
}

ncl_err ncl_tool_reply_text(ncl_json **result, const char *text)
{
    ncl_json *json;

    if (result == NULL) {
        return NCL_ERR_INVALID_ARG;
    }
    json = ncl_json_new_string(text != NULL ? text : "");
    if (json == NULL) {
        return NCL_ERR_NOMEM;
    }
    *result = json;
    return NCL_OK;
}

ncl_err ncl_tool_fail(char **reason, ncl_err code, const char *fmt, ...)
{
    char buffer[512];
    va_list args;

    if (reason != NULL && fmt != NULL) {
        va_start(args, fmt);
        (void)vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        *reason = ncl_strdup(buffer);
    }
    return code;
}

/* ------------------------------------------------------------- declaration -- */

/**
 * Every operation a point may declare, in a fixed order: the standard's Query
 * and Set operations and the call family (册 5 §5.2.7/§5.2.8), in the enum's
 * order so a trail line reads the way the spec's tables do.
 */
static const ncl_operation k_operations[NCL_OP_COUNT] = {
    NCL_OP_GET_VALUE, NCL_OP_GET_LENGTH, NCL_OP_GET_KEYS, NCL_OP_GET_ATTRIBUTES,
    NCL_OP_SET_VALUE, NCL_OP_ADD, NCL_OP_DELETE, NCL_OP_FUNC_CALL,
    NCL_OP_FUNC_STATUS, NCL_OP_FUNC_RESULT, NCL_OP_FUNC_CANCEL};

/*
 * 数据对象的第二种分法（第 3 部分 5.3/5.4/5.5）：同样是数据对象，
 *
 *   dataItems  —— "可以采集的数据"：物理量（表 4）与从设备感知的实时量，
 *                 采样通道只能引用它们；
 *   configs    —— "配置信息"：参数、坐标系、刀具表这类**不常变**的数据，
 *                 应用系统可以查询或修改，但表 1 注 b 明说"配置中的数据对象
 *                 不得作为采样数据源"。
 *
 * 名字来自数据字典，所以属于哪一类也由 type 决定 —— 声明里不用再写一个开关，
 * 写错的组合（比如把这些点声明成 sampled）会被校验直接拒掉。
 */
static bool tool_is_config_type(const char *type)
{
    static const char *const k_config_types[] = {
        /* 表 6：对象自己的元信息（型号、编号、版本、厂商、创建者…） */
        "CREATE_TIME", "CREATOR", "MANUFACTURER", "MODEL", "NAME", "NUMBER",
        "PARAMETER", "VERSION",
        /* 表 7：成表、成结构、不常变的那几项 */
        "COORDINATE", "FILE", "SHELF_UNIT", "TOOL", "TOOLPARAM", "TYPE",
        "VARIABLE",
    };
    size_t i;

    if (type == NULL) {
        return false;
    }
    for (i = 0; i < sizeof(k_config_types) / sizeof(k_config_types[0]); i++) {
        if (strcmp(k_config_types[i], type) == 0) {
            return true;
        }
    }
    return false;
}

char *ncl_tool_point_name(const ncl_tool_point *point, char *buf, size_t cap)
{
    const char *p;
    size_t used = 0;

    if (buf == NULL || cap == 0) {
        return buf;
    }
    buf[0] = '\0';
    if (point == NULL || point->path == NULL) {
        return buf;
    }
    /* 名字 = 路径去掉设备段，'@' 换 '_'、'/' 换 '.'：
     *   /MACHINE/STATUS             -> STATUS
     *   /MACHINE/AXIS@X/POSITION@REAL -> AXIS_X.POSITION_REAL
     * 路径唯一，名字就唯一，所以不用手写名字。 */
    p = point->path;
    if (p[0] == '/') {
        const char *slash = strchr(p + 1, '/');

        p = slash != NULL ? slash + 1 : p + 1;
    }
    for (; *p != '\0' && used + 1 < cap; p++) {
        buf[used++] = *p == '@' ? '_' : (*p == '/' ? '.' : *p);
    }
    buf[used] = '\0';
    return buf;
}

/**
 * The suffix an operation adds to a point's name, or NULL for the bare call.
 * The value operations are addressed by model path, so their method names are
 * suffixed to stay unique: "<point>.read", "<point>.write", "<point>.get_keys".
 */
static const char *point_op_suffix(ncl_operation op)
{
    switch (op) {
    case NCL_OP_FUNC_CALL:
        return NULL;
    case NCL_OP_GET_VALUE:
        return "read";
    case NCL_OP_SET_VALUE:
        return "write";
    default:
        return ncl_operation_to_string(op);
    }
}

/**
 * The name a point's operation answers to inside its tool. A call keeps the
 * bare name - "<tool>/RESET" is what a client writes - while the value
 * operations are suffixed, because they are addressed by model path rather
 * than by name and the suffix keeps every name unique.
 */
static void point_method_name(const ncl_tool_point *point, ncl_operation op,
                              char *buffer, size_t size)
{
    char name[256];
    const char *suffix;

    (void)ncl_tool_point_name(point, name, sizeof(name));
    suffix = point_op_suffix(op);
    if (suffix == NULL) {
        (void)snprintf(buffer, size, "%s", name);
    } else {
        (void)snprintf(buffer, size, "%s.%s", name, suffix);
    }
}

ncl_err ncl_tool_validate(const ncl_tool_decl *decl, ncl_strbuf *err)
{
    size_t i;

    if (decl == NULL) {
        err_append(err, "the tool declaration is missing");
        return NCL_ERR_INVALID_ARG;
    }
    if (ncl_str_is_blank(decl->name)) {
        err_append(err, "the tool has no name");
        return NCL_ERR_INVALID_ARG;
    }
    if (decl->open == NULL) {
        err_append1(err, "the tool %s has no open function", decl->name);
        return NCL_ERR_INVALID_ARG;
    }
    if (decl->points == NULL || decl->point_count == 0) {
        err_append1(err, "the tool %s declares no point", decl->name);
        return NCL_ERR_INVALID_ARG;
    }
    if (decl->sample_ms < 0 || decl->upload_ms < 0) {
        err_append1(err, "the tool %s has a negative period", decl->name);
        return NCL_ERR_INVALID_ARG;
    }
    for (i = 0; i < decl->point_count; i++) {
        const ncl_tool_point *point = &decl->points[i];
        char name[256];
        size_t j;
        size_t op;

        (void)ncl_tool_point_name(point, name, sizeof(name));

        if (ncl_str_is_blank(point->path)) {
            err_append1(err, "a point of the tool %s has no path", decl->name);
            return NCL_ERR_INVALID_ARG;
        }
        if (point->path[0] != '/') {
            err_append1(err, "the point path %s must start with '/'",
                        point->path);
            return NCL_ERR_INVALID_ARG;
        }
        if (!point->available) {
            /* Declared but not readable yet: there is no function and there may
             * be nothing to call, so the only thing it has to carry is the
             * reason the site will be shown ("frame not captured yet"). */
            if (ncl_str_is_blank(point->summary)) {
                err_append1(err, "the pending point %s needs a summary",
                            point->path);
                return NCL_ERR_INVALID_ARG;
            }
        } else if (point->fn == NULL) {
            err_append1(err, "the point %s has no function", point->path);
            return NCL_ERR_INVALID_ARG;
        }
        if (ncl_str_is_blank(point->path) == false &&
            point->path[strlen(point->path) - 1] == '/') {
            err_append1(err, "the point path %s ends with '/'", point->path);
            return NCL_ERR_INVALID_ARG;
        }
        if (point->sampled &&
            !ncl_tool_point_handles(point, NCL_OP_GET_VALUE)) {
            err_append1(err, "the point %s is sampled but not readable",
                        point->path);
            return NCL_ERR_INVALID_ARG;
        }
        if ((point->ops & NCL_OP_WRITE_MASK) != 0 &&
            !ncl_tool_point_handles(point, NCL_OP_GET_VALUE)) {
            /* A write only point is not a thing here: the trail records the
             * value a write replaces, and what cannot be read back cannot
             * be confirmed. A device that only takes commands - a password,
             * a reset pulse - gets a method instead. */
            err_append1(err,
                        "the point %s can be written but not read"
                        "（只写点位不许进模型：真只写的东西写成 NCL_METHOD，"
                        "值走方法调用的参数）",
                        point->path);
            return NCL_ERR_INVALID_ARG;
        }
        if (point->config && point->sampled) {
            /* 配置型数据对象（参数、坐标系、刀具表…）进模型的 configs，
             * 而采样通道只能引用 dataItems（册 3 表 1 注 b）。NCL_CONFIG_*
             * 一族没有 SAMPLED 形式，这里拦的是手写这张表的情况。 */
            err_append1(err,
                        "the point %s is a config and cannot be sampled"
                        "（配置型数据不得作为采样数据源，册 3 表 1 注 b）",
                        point->path);
            return NCL_ERR_INVALID_ARG;
        }
        if (point->ops == 0 && point->available) {
            err_append1(err, "the point %s declares no operation", point->path);
            return NCL_ERR_INVALID_ARG;
        }
        /* Every operation suffixes the point's name ("<point>.read",
         * "<point>.get_attributes", ...), so a name carrying one of those
         * suffixes could collide with the method of another point. */
        for (op = 0; op < NCL_OP_COUNT; op++) {
            const char *suffix = point_op_suffix(k_operations[op]);
            char pattern[64];

            if (suffix == NULL) {
                continue; /* a call keeps the bare name */
            }
            (void)snprintf(pattern, sizeof(pattern), ".%s", suffix);
            if (strstr(name, pattern) != NULL) {
                err_appendf(err, "the point name %s contains the operation "
                                 "suffix %s (.read/.write/...)",
                            name, pattern);
                return NCL_ERR_INVALID_ARG;
            }
        }
        for (j = 0; j < i; j++) {
            const ncl_tool_point *other = &decl->points[j];
            char other_name[256];
            size_t other_op;

            if (ncl_str_is_blank(other->path) || other->path[0] != '/') {
                continue;
            }
            (void)ncl_tool_point_name(other, other_name, sizeof(other_name));
            if (strcmp(other_name, name) != 0) {
                continue;
            }
            /* Same name: each (path, operation) pair may be declared once. */
            for (other_op = 0; other_op < NCL_OP_COUNT; other_op++) {
                ncl_operation candidate = k_operations[other_op];

                if (ncl_tool_point_handles(other, candidate) &&
                    ncl_tool_point_handles(point, candidate)) {
                    err_appendf(err, "the points %s and %s both declare %s",
                                other->path, point->path,
                                ncl_operation_to_string(candidate));
                    return NCL_ERR_INVALID_ARG;
                }
            }
            if (strcmp(other->path, point->path) == 0) {
                err_append1(err, "the point %s is declared twice", point->path);
                return NCL_ERR_INVALID_ARG;
            }
            err_appendf(err, "the points %s and %s share the name %s",
                        other->path, point->path, name);
            return NCL_ERR_INVALID_ARG;
        }
    }
    return NCL_OK;
}

/* ----------------------------------------------------------------- model -- */

/**
 * "/MACHINE/STATUS@RUN" -> type "STATUS", number "RUN". Same rule the adapter host
 * applies to a configured point name, so both model writers agree.
 */
static void split_type_number(const char *text, char **type_out,
                              char **number_out)
{
    const char *at = strchr(text, '@');

    if (at == NULL) {
        *type_out = ncl_strdup(text);
        *number_out = NULL;
        return;
    }
    *type_out = ncl_strndup(text, (size_t)(at - text));
    *number_out = ncl_strdup(at + 1);
}

/*
 * 数据项/组件的**可读名**：第 4 部分那张表里的中文含义（表 4 物理量、表 6 通用、
 * 表 7 专用），组件另有表 2。名字只写给人和界面看 —— 路径是 type(+number) 拼出来的，
 * 跟名字没有关系，所以名字怎么改都不会动路径。
 */
typedef struct {
    const char *type;
    const char *label;
} tool_label;

static const tool_label k_labels[] = {
    /* 表 4 物理量数据项（默认单位见 32 册） */
    {"ACCELERATION", "加速度"},
    {"ANGLE", "角位置"},
    {"ANGULAR_ACCELERATION", "角加速度"},
    {"ANGULAR_VELOCITY", "角速度"},
    {"CONCENTRATION", "浓度"},
    {"CONDUCTIVITY", "导电率"},
    {"CURRENT", "电流"},
    {"DISPLACEMENT", "位移"},
    {"ENERGY", "功耗"},
    {"FLOW", "瞬时流量"},
    {"FREQUENCY", "频率"},
    {"LENGTH", "长度"},
    {"MASS", "质量"},
    {"POSITION", "位置"},
    {"POWER", "功率"},
    {"POWER_FACTOR", "功率因数"},
    {"PRESSURE", "压强"},
    {"RESISTANCE", "电阻"},
    {"SPEED", "速度"},
    {"TEMPERATURE", "温度"},
    {"TORQUE", "扭矩"},
    {"VISCOSITY", "粘度"},
    {"VOLT_AMPERE", "视在功率"},
    /* 表 6 通用数据项 */
    {"CREATE_TIME", "创建时间"},
    {"CREATOR", "创建者"},
    {"MANUFACTURER", "厂商"},
    {"MODEL", "型号"},
    {"NAME", "名称"},
    {"NUMBER", "编号"},
    {"PARAMETER", "参数"},
    {"STATUS", "运行状态"},
    {"VERSION", "版本"},
    {"WARNING", "报警信息"},
    /* 表 7 专用数据项 */
    {"CONSOLE", "控制台"},
    {"COORDINATE", "坐标系"},
    {"FEED_OVERRIDE", "进给倍率"},
    {"FEED_SPEED", "进给速度"},
    {"FILE", "文件"},
    {"LINE_NUMBER", "程序行号"},
    {"PATH_LEFT_LENGTH", "剩余进给"},
    {"PART", "工件"},
    {"PART_COUNT", "加工件数"},
    {"PROGRAM", "主程序名"},
    {"PROGRAM_NUMBER", "当前程序号"},
    {"SHELF_UNIT", "仓位"},
    {"SITE", "站点"},
    {"SPINDLE_OVERRIDE", "主轴倍率"},
    {"SUBPROGRAM", "子程序名"},
    {"TOOL", "刀具"},
    {"TOOLPARAM", "刀具参数"},
    {"TOOL_NUMBER", "当前刀号"},
    {"TYPE", "类型"},
    {"VARIABLE", "运行变量"},
    {"WORK_MODE", "工作模式"},
    /* 表 2 组件类型 */
    {"AXIS", "轴"},
    {"CONTROLLER", "控制器"},
    {"MOTOR", "电机"},
    {"SCREW", "丝杠"},
    {"SHELF", "货架系统"},
    {"SERVO", "伺服"},
    {"TOOL_MAGAZINE", "刀库"},
};

/** number 是我们自己的约定后缀，能译就译成可读的词（表 8 里没有它）。 */
static const char *tool_label_of(const char *type)
{
    size_t i;

    if (type == NULL) {
        return NULL;
    }
    for (i = 0; i < sizeof(k_labels) / sizeof(k_labels[0]); i++) {
        if (strcmp(k_labels[i].type, type) == 0) {
            return k_labels[i].label;
        }
    }
    return type; /* 表里没有的（厂商自定的 type）照原名，至少不是空的 */
}

static const char *tool_number_label(const char *number)
{
    if (number == NULL) {
        return NULL;
    }
    if (strcmp(number, "REAL") == 0) {
        return "实际";
    }
    if (strcmp(number, "CMD") == 0) {
        return "目标";
    }
    return number;
}

/*
 * 数据对象的**取值形状**（第 4 部分那张表的"类型"列）：名单里的 type 其值是一个集合
 * （dict / JSON 对象 → "HASH"，list → "LIST"），标量（string / number）不写这一项 ——
 * 手写的那份模型也是这么做的。这一列只用来给模型写 dataType，不影响归置（归置看
 * tool_is_config_type()）。
 */
static const struct {
    const char *type;
    const char *data_type;
} k_data_types[] = {
    /* 表 6 通用数据项 */
    {"PARAMETER", "HASH"},  /* dict：参数 */
    {"WARNING", "HASH"},    /* JSON 对象：报警（number / text / key_value） */
    /* 表 7 专用数据项 */
    {"COORDINATE", "LIST"}, /* 一张表：各坐标系的点（跟刀具表一样） */
    {"FILE", "HASH"},       /* dict：文件 */
    {"PART", "HASH"},       /* JSON 对象：工件 */
    {"SHELF_UNIT", "LIST"}, /* list：仓位 */
    {"TOOL", "LIST"},       /* list：刀具（刀具列表） */
    {"TOOLPARAM", "HASH"},  /* JSON 对象：刀具参数 */
    {"VARIABLE", "LIST"},   /* list：运行变量 */
};

/** The "类型" of @p type as the model writes it ("HASH"/"LIST"), NULL for scalars. */
static const char *tool_data_type_of(const char *type)
{
    size_t i;

    if (type == NULL) {
        return NULL;
    }
    for (i = 0; i < sizeof(k_data_types) / sizeof(k_data_types[0]); i++) {
        if (strcmp(k_data_types[i].type, type) == 0) {
            return k_data_types[i].data_type;
        }
    }
    return NULL;
}

/** 数据项的可读名："运行状态"、"位置（实际）"、"功率（1）"。 */
static char *tool_item_label(const char *type, const char *number)
{
    char *name = NULL;

    if (number == NULL) {
        return ncl_strdup(tool_label_of(type));
    }
    if (ncl_asprintf(&name, "%s（%s）", tool_label_of(type),
                     tool_number_label(number)) != NCL_OK) {
        return NULL;
    }
    return name;
}

/** 组件的可读名："控制器"、"X 轴"（轴号在前，读起来才知道是哪根轴）。 */
static char *tool_component_label(const char *type, const char *number)
{
    char *name = NULL;

    if (number == NULL) {
        return ncl_strdup(tool_label_of(type));
    }
    if (strcmp(type, "AXIS") == 0) {
        if (ncl_asprintf(&name, "%s %s", number, tool_label_of(type)) != NCL_OK) {
            return NULL;
        }
        return name;
    }
    if (ncl_asprintf(&name, "%s（%s）", tool_label_of(type), number) != NCL_OK) {
        return NULL;
    }
    return name;
}

/**
 * Build the model of a declaration.
 *
 * A declared path is <device>/[<component>/]<item>:
 *
 *   "/MACHINE/STATUS"                a data item of the device
 *   "/MACHINE/CONTROLLER/PROGRAM"    a data item of the CONTROLLER component
 *   "/MACHINE/AXIS@X/POSITION@REAL"  a data item of the AXIS component named X
 *
 * The middle segment becomes a component node of its own (册 32 表 2/表 3), so the
 * tree is the standard's device -> component -> data object instead of a flat
 * list, and the device node answers on the segment its points live under.
 */
/** One component of the model: the middle segment of a declared path. */
typedef struct {
    const char *segment; /**< borrowed from the path: "CONTROLLER", "AXIS@X" */
    size_t      length;
    char        id[32];
    ncl_json   *node;    /**< owned by the device's components array */
    ncl_json   *items;   /**< dataItems: handed to the node when filled */
    ncl_json   *configs; /**< configs: the same for the slow changing ones */
} tool_group;

ncl_json *ncl_tool_model(const ncl_tool_decl *decl, const ncl_json *device,
                         ncl_strbuf *err)
{
    const char *device_type =
        device != NULL ? ncl_json_obj_get_string(device, "type") : NULL;
    const char *device_id =
        device != NULL ? ncl_json_obj_get_string(device, "id") : NULL;
    const char *device_name =
        device != NULL ? ncl_json_obj_get_string(device, "name") : NULL;
    ncl_json *root = NULL;
    ncl_json *devices = NULL;
    ncl_json *node = NULL;
    ncl_json *items = NULL;
    ncl_json *components = NULL;
    ncl_json *configs = NULL;
    ncl_json *channel = NULL;
    ncl_json *ids = NULL;
    tool_group *groups = NULL;
    size_t group_count = 0;
    const char *prefix = NULL; /**< the device segment, e.g. "MACHINE" */
    size_t prefix_len = 0;
    size_t i;

    if (ncl_tool_validate(decl, err) != NCL_OK) {
        return NULL;
    }
    for (i = 0; i < decl->point_count; i++) {
        const ncl_tool_point *point = &decl->points[i];
        const char *slash;

        if (ncl_tool_point_is_method(point)) {
            continue; /* a method: it belongs in the schema, not here */
        }
        slash = strchr(point->path + 1, '/');
        prefix = point->path + 1;
        prefix_len = slash != NULL ? (size_t)(slash - (point->path + 1))
                                   : strlen(point->path + 1);
        break;
    }

    root = ncl_json_new_object();
    devices = ncl_json_new_array();
    node = ncl_json_new_object();
    items = ncl_json_new_array();
    components = ncl_json_new_array();
    configs = ncl_json_new_array();
    channel = ncl_json_new_object();
    ids = ncl_json_new_array();
    groups = (tool_group *)ncl_mem_calloc(decl->point_count, sizeof(*groups));
    if (root == NULL || devices == NULL || node == NULL || items == NULL ||
        components == NULL || configs == NULL || channel == NULL ||
        ids == NULL || groups == NULL) {
        goto fail;
    }
    (void)ncl_json_obj_set_string(root, "id", "01");
    (void)ncl_json_obj_set_string(root, "type", NCL_NODE_TYPE_ROOT);
    (void)ncl_json_obj_set_string(root, "name", "适配器设备模型");
    (void)ncl_json_obj_set_string(root, "version", "1.1.0");

    /* The device node's type is the first segment of every declared path, so
     * the model tree walks to exactly the path the declaration wrote. A device
     * type that disagrees with that segment would give two different paths for
     * one point (the tree says one thing, source/config another), so it is
     * refused by name instead of being papered over with a "source". */
    if (prefix != NULL && !ncl_str_is_blank(device_type) &&
        (strlen(device_type) != prefix_len ||
         strncmp(device_type, prefix, prefix_len) != 0)) {
        err_appendf(err,
                    "点位路径的设备段 /%.*s 与配置里的 device.type（%s）不一致："
                    "两者必须是同一个名字，否则模型里的路径和点位路径对不上",
                    (int)prefix_len, prefix, device_type);
        goto fail;
    }
    {
        char type_buf[128];

        if (!ncl_str_is_blank(device_type)) {
            (void)ncl_json_obj_set_string(node, "type", device_type);
        } else if (prefix != NULL && prefix_len < sizeof(type_buf)) {
            /* No device type in the configuration: the declaration's first
             * segment is what the points are already addressed by. */
            memcpy(type_buf, prefix, prefix_len);
            type_buf[prefix_len] = '\0';
            (void)ncl_json_obj_set_string(node, "type", type_buf);
        } else {
            (void)ncl_json_obj_set_string(node, "type", "MACHINE");
        }
    }
    (void)ncl_json_obj_set_string(node, "id",
                                  ncl_str_is_blank(device_id) ? "01"
                                                              : device_id);
    (void)ncl_json_obj_set_string(node, "name",
                                  ncl_str_is_blank(device_name) ? "适配器设备"
                                                                : device_name);
    (void)ncl_json_obj_set_string(node, "version", "1.0");
    /* No "source" anywhere in this model: the tree says it all. The device is
     * the segment its points live under (a device of type MACHINE answers on
     * /MACHINE), each component is a segment of its own, and walking the
     * parents gives exactly the path the declaration asked for - so the two
     * ways of getting a path cannot drift apart (model.c checks that too). */

    for (i = 0; i < decl->point_count; i++) {
        const ncl_tool_point *point = &decl->points[i];
        const char *last = strrchr(point->path, '/');
        const char *tail = last != NULL ? last + 1 : point->path;
        const char *component = NULL;
        size_t component_len = 0;
        char *type = NULL;
        char *number = NULL;
        char id[32];
        ncl_json *item;
        ncl_json *target = items; /* decided below: dataItems or configs */
        bool config_kind;

        if (ncl_tool_point_is_method(point)) {
            continue;
        }
        item = ncl_json_new_object();
        if (item == NULL) {
            goto fail;
        }
        /* "/MACHINE/AXIS@X/POSITION@REAL": the slash after the device segment is
         * not the last one, so what sits between them is a component. */
        if (prefix != NULL && last != NULL) {
            const char *after = point->path + 1 + prefix_len;

            if (*after == '/' && after != last) {
                component = after + 1;
                component_len = (size_t)(last - component);
            }
        }
        split_type_number(tail, &type, &number);
        config_kind = point->config;
        /* 归类由声明说了算（NCL_CONFIG_* / NCL_DATAITEM_*），字典那张表只用来
         * 核对：把 PARAMETER 这类配置型名字声明成 dataItem 通常是写错了。 */
        if (!config_kind && tool_is_config_type(type)) {
            ncl_log_warn("点位 %s 的类型 %s 在数据字典里属配置型，却被声明成 "
                         "dataItem（会进采样候选）——确认一下",
                         point->path, type);
        }
        if (component == NULL) {
            /* 设备自己那两层：dataItems 放感知量，configs 放配置型数据。 */
            target = config_kind ? configs : items;
        }
        snprintf(id, sizeof(id), "p%u", (unsigned)i);
        (void)ncl_json_obj_set_string(item, "id", id);
        {
            /* name 给人看（"运行状态"、"位置（实际）"），不是路径：路径由 type 与
             * number 在模型树里拼出来，名字改了也不会动路径。 */
            char *label = tool_item_label(type, number);

            if (label != NULL) {
                (void)ncl_json_obj_set_string(item, "name", label);
                ncl_free_safe(label);
            }
        }
        (void)ncl_json_obj_set_string(item, "type",
                                      type != NULL ? type : point->path);
        if (number != NULL) {
            (void)ncl_json_obj_set_string(item, "number", number);
        }
        if (point->summary != NULL) {
            (void)ncl_json_obj_set_string(item, "description", point->summary);
        }
        /* 取值形状跟着字典的 type 走（FILE → HASH、TOOL → LIST…）：标量不写，
         * 手写的那份模型也是这样。`number` 在它前面（第 4 部分的字段顺序）。 */
        {
            const char *data_type = tool_data_type_of(type);

            if (data_type != NULL) {
                (void)ncl_json_obj_set_string(item, "dataType", data_type);
            }
        }
        if ((point->ops & NCL_OP_WRITE_MASK) != 0) {
            (void)ncl_json_obj_set_bool(item, "settable", true);
        }
        ncl_free_safe(type);
        ncl_free_safe(number);

        if (component != NULL) {
            size_t g;

            for (g = 0; g < group_count; g++) {
                if (groups[g].length == component_len &&
                    strncmp(groups[g].segment, component, component_len) == 0) {
                    break;
                }
            }
            if (g == group_count) {
                char *name = ncl_strndup(component, component_len);
                char *ctype = NULL;
                char *cnumber = NULL;
                const char *at = (const char *)memchr(component, '@',
                                                      component_len);

                if (name == NULL) {
                    ncl_json_free(item);
                    goto fail;
                }
                if (at != NULL) {
                    ctype = ncl_strndup(component,
                                        (size_t)(at - component));
                    cnumber = ncl_strndup(at + 1,
                                          component_len -
                                              (size_t)(at - component) - 1);
                } else {
                    ctype = ncl_strndup(component, component_len);
                }
                groups[g].segment = component;
                groups[g].length = component_len;
                groups[g].node = ncl_json_new_object();
                groups[g].items = ncl_json_new_array();
                groups[g].configs = ncl_json_new_array();
                snprintf(groups[g].id, sizeof(groups[g].id), "c%u",
                         (unsigned)g);
                if (groups[g].node == NULL || groups[g].items == NULL ||
                    groups[g].configs == NULL ||
                    name == NULL || ctype == NULL) {
                    ncl_free_safe(name);
                    ncl_free_safe(ctype);
                    ncl_free_safe(cnumber);
                    ncl_json_free(item);
                    group_count++; /* so the fail path frees what exists */
                    goto fail;
                }
                (void)ncl_json_obj_set_string(groups[g].node, "id",
                                              groups[g].id);
                {
                    /* 组件的 name 也给人看："控制器"、"X 轴"（路径由 type 与
                     * number 决定，与 name 无关）。 */
                    char *label = tool_component_label(ctype, cnumber);
                    const char *shown = label != NULL ? label : name;

                    (void)ncl_json_obj_set_string(groups[g].node, "name", shown);
                    ncl_free_safe(label);
                }
                (void)ncl_json_obj_set_string(groups[g].node, "type", ctype);
                if (cnumber != NULL) {
                    (void)ncl_json_obj_set_string(groups[g].node, "number",
                                                  cnumber);
                }
                (void)ncl_json_arr_push(components, groups[g].node);
                ncl_free_safe(name);
                ncl_free_safe(ctype);
                ncl_free_safe(cnumber);
                group_count++;
            }
            /* 组件下的数据对象同样分两类（第 3 部分是"组件也各有 configs"）。 */
            target = config_kind ? groups[g].configs : groups[g].items;
        }
        (void)ncl_json_arr_push(target, item);

        if (point->sampled) {
            ncl_json *ref = ncl_json_new_object();

            if (ref == NULL) {
                goto fail;
            }
            (void)ncl_json_obj_set_string(ref, "id", id);
            (void)ncl_json_arr_push(ids, ref);
        }
    }

    /* No period means "this declaration asks for no sample channel"; a site can
     * still add one to the model file, which is where the running period is
     * kept. */
    if (decl->sample_ms > 0 && ncl_json_arr_len(ids) > 0) {
        long long upload = decl->upload_ms > 0 ? decl->upload_ms
                                              : decl->sample_ms;

        (void)ncl_json_obj_set_string(channel, "id", decl->name);
        (void)ncl_json_obj_set_string(channel, "type",
                                      NCL_NODE_TYPE_SAMPLE_CHANNEL);
        (void)ncl_json_obj_set_string(
            channel, "name",
            ncl_str_is_blank(decl->description) ? decl->name
                                                : decl->description);
        (void)ncl_json_obj_set_int(channel, "sampleInterval", decl->sample_ms);
        (void)ncl_json_obj_set_int(channel, "uploadInterval", upload);
        (void)ncl_json_obj_set(channel, "ids", ids);
        (void)ncl_json_arr_push(configs, channel);
        ids = NULL;
        channel = NULL;
    }
    for (i = 0; i < group_count; i++) {
        if (groups[i].node != NULL && groups[i].items != NULL &&
            ncl_json_arr_len(groups[i].items) > 0) {
            (void)ncl_json_obj_set(groups[i].node, "dataItems",
                                   groups[i].items);
            groups[i].items = NULL; /* the node owns it now */
        }
        if (groups[i].node != NULL && groups[i].configs != NULL &&
            ncl_json_arr_len(groups[i].configs) > 0) {
            (void)ncl_json_obj_set(groups[i].node, "configs",
                                   groups[i].configs);
            groups[i].configs = NULL; /* the node owns it now */
        }
        ncl_json_free(groups[i].items);
        ncl_json_free(groups[i].configs);
    }
    ncl_mem_free(groups);
    /* 两个数组都是可选的（册 3 表 2）：只有真有内容时才写出来，空的不占地方。 */
    if (ncl_json_arr_len(items) > 0) {
        (void)ncl_json_obj_set(node, "dataItems", items);
        items = NULL;
    }
    ncl_json_free(items);
    (void)ncl_json_obj_set(node, "components", components);
    (void)ncl_json_obj_set(node, "configs", configs);
    (void)ncl_json_arr_push(devices, node);
    (void)ncl_json_obj_set(root, "devices", devices);
    return root;

fail:
    if (err != NULL && err->len == 0) {
        err_append(err, "cannot build the model of the declared tool");
    }
    if (groups != NULL) {
        for (i = 0; i < group_count; i++) {
            ncl_json_free(groups[i].items);
            ncl_json_free(groups[i].configs);
        }
        ncl_mem_free(groups);
    }
    ncl_json_free(root);
    ncl_json_free(devices);
    ncl_json_free(node);
    ncl_json_free(items);
    ncl_json_free(components);
    ncl_json_free(configs);
    ncl_json_free(channel);
    ncl_json_free(ids);
    return NULL;
}

/* --------------------------------------------------------- several tools -- */

/**
 * One id-bearing node of a device: a data object, a component, or one entry of
 * a sample channel's "ids" list. Visiting them all is what renumbering (and
 * counting) the ids of a merged document needs.
 */
static void model_walk_ids(ncl_json *device,
                           void (*visit)(ncl_json *owner, void *user),
                           void *user)
{
    static const char *const k_arrays[] = {"dataItems", "configs"};
    ncl_json *components = ncl_json_obj_get(device, "components");
    size_t c;
    size_t k;

    for (k = 0; k < sizeof(k_arrays) / sizeof(k_arrays[0]); k++) {
        ncl_json *array = ncl_json_obj_get(device, k_arrays[k]);
        size_t i;

        for (i = 0; i < ncl_json_arr_len(array); i++) {
            ncl_json *item = ncl_json_arr_get(array, i);
            ncl_json *refs = ncl_json_obj_get(item, "ids");
            size_t r;

            visit(item, user);
            for (r = 0; r < ncl_json_arr_len(refs); r++) {
                visit(ncl_json_arr_get(refs, r), user);
            }
        }
    }
    for (c = 0; c < ncl_json_arr_len(components); c++) {
        ncl_json *component = ncl_json_arr_get(components, c);

        visit(component, user);
        for (k = 0; k < sizeof(k_arrays) / sizeof(k_arrays[0]); k++) {
            ncl_json *array = ncl_json_obj_get(component, k_arrays[k]);
            size_t i;

            for (i = 0; i < ncl_json_arr_len(array); i++) {
                ncl_json *item = ncl_json_arr_get(array, i);
                ncl_json *refs = ncl_json_obj_get(item, "ids");
                size_t r;

                visit(item, user);
                for (r = 0; r < ncl_json_arr_len(refs); r++) {
                    visit(ncl_json_arr_get(refs, r), user);
                }
            }
        }
    }
}

/** The number in an id like "p12" / "c3", @p missing when it is not one. */
static size_t model_id_index(const char *id, char letter, size_t missing)
{
    unsigned long value;
    char *end = NULL;

    if (id == NULL || id[0] != letter || id[1] == '\0') {
        return missing;
    }
    value = strtoul(id + 1, &end, 10);
    if (end == id + 1 || *end != '\0') {
        return missing;
    }
    return (size_t)value;
}

/** One past the highest "p<n>" and "c<n>" of a document. */
typedef struct {
    size_t points;
    size_t components;
} model_offsets;

static void model_note_id(ncl_json *owner, void *user)
{
    model_offsets *offsets = (model_offsets *)user;
    const char *id = ncl_json_obj_get_string(owner, "id");
    size_t n;

    n = model_id_index(id, 'p', (size_t)-1);
    if (n != (size_t)-1 && n + 1 > offsets->points) {
        offsets->points = n + 1;
    }
    n = model_id_index(id, 'c', (size_t)-1);
    if (n != (size_t)-1 && n + 1 > offsets->components) {
        offsets->components = n + 1;
    }
}

static void model_shift_id(ncl_json *owner, void *user)
{
    const model_offsets *offsets = (const model_offsets *)user;
    const char *id = ncl_json_obj_get_string(owner, "id");
    char buffer[32];
    size_t n;

    n = model_id_index(id, 'p', (size_t)-1);
    if (n != (size_t)-1) {
        (void)snprintf(buffer, sizeof(buffer), "p%u",
                       (unsigned)(n + offsets->points));
    } else {
        n = model_id_index(id, 'c', (size_t)-1);
        if (n == (size_t)-1) {
            return; /* an id this builder did not write ("01"): leave it */
        }
        (void)snprintf(buffer, sizeof(buffer), "c%u",
                       (unsigned)(n + offsets->components));
    }
    (void)ncl_json_obj_set_string(owner, "id", buffer);
}

/** Move every element of @p source's @p key array to the end of @p target's. */
static void model_merge_array(ncl_json *target, ncl_json *source, const char *key)
{
    ncl_json *from = ncl_json_obj_get(source, key);
    ncl_json *to = ncl_json_obj_get(target, key);

    if (ncl_json_arr_len(from) == 0) {
        return;
    }
    if (to == NULL) {
        to = ncl_json_new_array();
        if (to == NULL) {
            return; /* the document stays valid, just short of this tool */
        }
        (void)ncl_json_obj_set(target, key, to);
    }
    while (ncl_json_arr_len(from) > 0) {
        (void)ncl_json_arr_push(to, ncl_json_arr_take(from, 0));
    }
}

/** The component of @p components that carries the same type (and number). */
static ncl_json *model_find_component(ncl_json *components, const ncl_json *like)
{
    const char *type = ncl_json_obj_get_string(like, "type");
    const char *number = ncl_json_obj_get_string(like, "number");
    size_t i;

    for (i = 0; i < ncl_json_arr_len(components); i++) {
        ncl_json *component = ncl_json_arr_get(components, i);
        const char *other_type = ncl_json_obj_get_string(component, "type");
        const char *other_number = ncl_json_obj_get_string(component, "number");

        if (type == NULL || other_type == NULL || strcmp(type, other_type) != 0) {
            continue;
        }
        if ((number == NULL) != (other_number == NULL)) {
            continue;
        }
        if (number == NULL || strcmp(number, other_number) == 0) {
            return component;
        }
    }
    return NULL;
}

/** A component both tools share is one component: join them, do not repeat. */
static void model_merge_components(ncl_json *target, ncl_json *source)
{
    ncl_json *theirs = ncl_json_obj_get(source, "components");
    ncl_json *ours = ncl_json_obj_get(target, "components");

    while (ncl_json_arr_len(theirs) > 0) {
        ncl_json *component = ncl_json_arr_take(theirs, 0);
        ncl_json *same = model_find_component(ours, component);

        if (same != NULL) {
            model_merge_array(same, component, "dataItems");
            model_merge_array(same, component, "configs");
            ncl_json_free(component);
            continue;
        }
        if (ours == NULL) {
            ours = ncl_json_new_array();
            if (ours == NULL) {
                ncl_json_free(component);
                continue;
            }
            (void)ncl_json_obj_set(target, "components", ours);
        }
        (void)ncl_json_arr_push(ours, component);
    }
}

ncl_json *ncl_tool_model_add(ncl_json *document, const ncl_tool_decl *decl,
                             const ncl_json *device, ncl_strbuf *err)
{
    ncl_json *added;
    ncl_json *target;
    ncl_json *source;
    model_offsets offsets;

    if (document == NULL) {
        return ncl_tool_model(decl, device, err);
    }
    added = ncl_tool_model(decl, device, err);
    if (added == NULL) {
        return NULL;
    }
    target = ncl_json_arr_get(ncl_json_obj_get(document, "devices"), 0);
    source = ncl_json_arr_get(ncl_json_obj_get(added, "devices"), 0);
    if (target == NULL || source == NULL ||
        ncl_json_obj_get_string(target, "type") == NULL ||
        ncl_json_obj_get_string(source, "type") == NULL ||
        strcmp(ncl_json_obj_get_string(target, "type"),
               ncl_json_obj_get_string(source, "type")) != 0) {
        err_append(err, "两份声明的设备不是同一台：一个模型里的工具必须挂在同一台设备上");
        ncl_json_free(added);
        return NULL;
    }
    /* Each declaration numbered its own points ("p0", "p1", ...) and its own
     * components ("c0", ...), and a model is looked up by id: shift the new ones
     * past the ones already there, sample channel references included. */
    offsets.points = 0;
    offsets.components = 0;
    model_walk_ids(target, model_note_id, &offsets);
    model_walk_ids(source, model_shift_id, &offsets);
    model_merge_array(target, source, "dataItems");
    model_merge_array(target, source, "configs");
    model_merge_components(target, source);
    ncl_json_free(added);
    return document;
}

/* --------------------------------------------------------------- register -- */

/** What the server hands back to the author's function. */
typedef struct {
    void                  *ctx;
    const ncl_tool_point  *point;
    /** Where the §6 trail goes (borrowed from the caller; may be NULL). */
    const ncl_tool_audit  *audit;
    const ncl_tool_decl   *decl;
} ncl_tool_shim;

struct ncl_tool_registration {
    const ncl_tool_decl *decl;
    void                *ctx;
    ncl_tool_shim       *shims;
    size_t               shim_count;
};

/** The frames of the last exchange, when the trail wants them (§6). */
static ncl_tool_frames shim_frames(const ncl_tool_shim *shim)
{
    ncl_tool_frames frames;

    memset(&frames, 0, sizeof(frames));
    if (shim->audit != NULL && shim->audit->wants_raw != NULL &&
        shim->audit->wants_raw(shim->audit->user) &&
        shim->decl->last_raw != NULL) {
        shim->decl->last_raw(shim->ctx, &frames);
    }
    return frames;
}

/** One line in the trail: what was asked, what it answered, how long it took. */
static void shim_report(const ncl_tool_shim *shim, ncl_operation op, ncl_err code,
                        int64_t micros)
{
    ncl_tool_frames frames;

    if (shim->audit == NULL || shim->audit->request == NULL) {
        return;
    }
    frames = shim_frames(shim);
    shim->audit->request(shim->audit->user, shim->decl->name, shim->point, op,
                         code, micros, &frames);
}

/**
 * The value a write is about to replace, for the trail (§6 asks for it). A
 * writable point is always readable - the validator refuses a write only
 * point - so a write always has an old value to show.
 */
static void shim_read_old_value(const ncl_tool_shim *shim, ncl_json **old_value)
{
    if (shim->audit == NULL || shim->audit->write == NULL) {
        return;
    }
    (void)shim->point->fn(shim->ctx, shim->point, NCL_OP_GET_VALUE, NULL,
                          old_value, NULL);
}

/**
 * The answer a point that is declared but not readable yet gives (see
 * NCL_DATAITEM_PENDING / NCL_CONFIG_PENDING): the declaration's own summary is the reason. Nothing went
 * over the wire, so the §6 trail stays empty - a request that never happened is
 * not a request.
 */
static ncl_err shim_unavailable(const ncl_tool_shim *shim, char **reason)
{
    return ncl_tool_fail(reason, NCL_ERR_NOT_SUPPORTED, "%s：%s",
                         shim->point->path,
                         shim->point->summary != NULL
                             ? shim->point->summary
                             : "还读不了（待抓包）");
}

/**
 * What every operation goes through: the availability check, the old value a
 * write replaces, the call itself, the trail. Only the writing operations
 * (NCL_OP_WRITE_MASK) read the old value - a read has nothing to replace.
 */
static ncl_err shim_run(const ncl_tool_shim *shim, ncl_operation op,
                        const ncl_json *params, ncl_json **result, char **reason)
{
    int64_t started = ncl_time_monotonic_millis();
    bool writes = (NCL_OP_BIT(op) & NCL_OP_WRITE_MASK) != 0;
    ncl_json *old_value = NULL;
    ncl_err rc;

    if (!shim->point->available) {
        return shim_unavailable(shim, reason);
    }
    if (writes) {
        shim_read_old_value(shim, &old_value);
    }
    rc = shim->point->fn(shim->ctx, shim->point, op, params, result, reason);
    if (writes && shim->audit != NULL && shim->audit->write != NULL) {
        shim->audit->write(shim->audit->user, shim->decl->name, shim->point,
                           old_value, ncl_tool_param_value(params), rc);
    }
    ncl_json_free(old_value);
    shim_report(shim, op, rc, ncl_time_monotonic_millis() - started);
    return rc;
}

/*
 * One entry point per operation. A binding holds the instance alone - it has
 * nowhere to put an operation - so the operation has to be baked into the
 * function it points at, and the point's own fn gets it back as @p op.
 */
#define NCL_TOOL_SHIM(shim_name, op_value)                                     \
    static ncl_err shim_name(void *instance, const ncl_json *params,           \
                             ncl_json **result, char **reason)                 \
    {                                                                          \
        return shim_run((const ncl_tool_shim *)instance, (op_value), params,   \
                        result, reason);                                       \
    }

NCL_TOOL_SHIM(shim_get_value, NCL_OP_GET_VALUE)
NCL_TOOL_SHIM(shim_get_length, NCL_OP_GET_LENGTH)
NCL_TOOL_SHIM(shim_get_keys, NCL_OP_GET_KEYS)
NCL_TOOL_SHIM(shim_get_attributes, NCL_OP_GET_ATTRIBUTES)
NCL_TOOL_SHIM(shim_set_value, NCL_OP_SET_VALUE)
NCL_TOOL_SHIM(shim_add, NCL_OP_ADD)
NCL_TOOL_SHIM(shim_delete, NCL_OP_DELETE)
NCL_TOOL_SHIM(shim_call, NCL_OP_FUNC_CALL)
NCL_TOOL_SHIM(shim_status, NCL_OP_FUNC_STATUS)
NCL_TOOL_SHIM(shim_result, NCL_OP_FUNC_RESULT)
NCL_TOOL_SHIM(shim_cancel, NCL_OP_FUNC_CANCEL)

#undef NCL_TOOL_SHIM

/** The shim that serves @p op. */
static ncl_tool_fn shim_for(ncl_operation op)
{
    switch (op) {
    case NCL_OP_GET_VALUE:
        return shim_get_value;
    case NCL_OP_GET_LENGTH:
        return shim_get_length;
    case NCL_OP_GET_KEYS:
        return shim_get_keys;
    case NCL_OP_GET_ATTRIBUTES:
        return shim_get_attributes;
    case NCL_OP_SET_VALUE:
        return shim_set_value;
    case NCL_OP_ADD:
        return shim_add;
    case NCL_OP_DELETE:
        return shim_delete;
    case NCL_OP_FUNC_STATUS:
        return shim_status;
    case NCL_OP_FUNC_RESULT:
        return shim_result;
    case NCL_OP_FUNC_CANCEL:
        return shim_cancel;
    case NCL_OP_FUNC_CALL:
    default:
        return shim_call;
    }
}

/** How many operations @p point declares. */
static size_t point_operation_count(const ncl_tool_point *point)
{
    size_t i;
    size_t count = 0;

    for (i = 0; i < NCL_OP_COUNT; i++) {
        if (ncl_tool_point_handles(point, k_operations[i])) {
            count++;
        }
    }
    return count;
}

ncl_err ncl_tool_register(ncl_server *server, const ncl_tool_decl *decl,
                          const ncl_json *params,
                          const ncl_tool_audit *audit,
                          ncl_tool_registration **out, ncl_strbuf *err)
{
    ncl_tool_registration *registration;
    size_t operations = 0;
    size_t at = 0;
    size_t i;

    if (out == NULL) {
        err_append(err, "ncl_tool_register() needs somewhere to put the result");
        return NCL_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (server == NULL) {
        err_append(err, "ncl_tool_register() needs a server");
        return NCL_ERR_INVALID_ARG;
    }
    if (ncl_tool_validate(decl, err) != NCL_OK) {
        return NCL_ERR_INVALID_ARG;
    }
    registration = (ncl_tool_registration *)ncl_mem_calloc(
        1, sizeof(*registration));
    if (registration == NULL) {
        return NCL_ERR_NOMEM;
    }
    for (i = 0; i < decl->point_count; i++) {
        operations += point_operation_count(&decl->points[i]);
    }
    registration->shims =
        (ncl_tool_shim *)ncl_mem_calloc(operations, sizeof(ncl_tool_shim));
    if (registration->shims == NULL) {
        ncl_mem_free(registration);
        return NCL_ERR_NOMEM;
    }
    registration->shim_count = operations;
    registration->decl = decl;

    /* Once for the whole tool: every point shares this connection. */
    registration->ctx = decl->open(params, NULL);
    if (registration->ctx == NULL) {
        err_append1(err, "the tool %s cannot open its connection", decl->name);
        ncl_mem_free(registration->shims);
        ncl_mem_free(registration);
        return NCL_ERR_CONNECT;
    }

    /* One registration per point: the host's instance pointer is per tool, and
     * a point's operations have to keep their own context and their own name.
     * The tool name is the declaration's, so a call is addressed
     * "<tool>/<point name>" whichever point it lands on. */
    for (i = 0; i < decl->point_count; i++) {
        const ncl_tool_point *point = &decl->points[i];
        ncl_tool_method methods[NCL_OP_COUNT];
        ncl_tool_binding bindings[NCL_OP_COUNT];
        char names[NCL_OP_COUNT][256];
        ncl_tool_shim *shim = &registration->shims[at];
        size_t count = 0;
        size_t op;

        shim->ctx = registration->ctx;
        shim->point = point;
        shim->audit = audit;
        shim->decl = decl;
        for (op = 0; op < NCL_OP_COUNT; op++) {
            ncl_operation candidate = k_operations[op];

            if (!ncl_tool_point_handles(point, candidate)) {
                continue;
            }
            point_method_name(point, candidate, names[count],
                              sizeof(names[count]));
            methods[count].name = names[count];
            methods[count].fn = shim_for(candidate);
            methods[count].params_schema = NULL;
            bindings[count].path = point->path;
            bindings[count].operation = candidate;
            bindings[count].method = names[count];
            bindings[count].tool = decl->name;
            count++;
        }
        if (ncl_server_register_tool(server, decl->name, shim, methods, count,
                                     bindings, count) != NCL_OK) {
            err_append1(err, "cannot bind the point %s", point->path);
            ncl_tool_unregister(decl, registration);
            return NCL_ERR_INVALID_ARG;
        }
        at++;
    }
    *out = registration;
    return NCL_OK;
}

void ncl_tool_unregister(const ncl_tool_decl *decl,
                         ncl_tool_registration *registration)
{
    if (registration == NULL) {
        return;
    }
    if (decl != NULL && decl->close != NULL) {
        decl->close(registration->ctx);
    }
    ncl_mem_free(registration->shims);
    ncl_mem_free(registration);
}
