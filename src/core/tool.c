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
 * puts a small shim in front of each point - {context, point} - and three
 * trampolines that call the author's function with the operation that reached
 * it. The shims live in the registration the caller holds.
 */

#include "nclink/ncl_tool.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

/** The three operations a point may declare, in a fixed order. */
static const ncl_operation k_operations[3] = {NCL_OP_GET_VALUE, NCL_OP_SET_VALUE,
                                              NCL_OP_FUNC_CALL};

const char *ncl_tool_point_name(const ncl_tool_point *point)
{
    const char *slash;

    if (point == NULL || point->path == NULL) {
        return "";
    }
    /* A point may name itself when the tail of its path is ambiguous - three
     * axes declared under one tree all end in "/POSITION". */
    if (point->name != NULL && point->name[0] != '\0') {
        return point->name;
    }
    slash = strrchr(point->path, '/');
    return slash != NULL ? slash + 1 : point->path;
}

/** True when @p point declares @p op. */
static bool point_declares(const ncl_tool_point *point, ncl_operation op)
{
    switch (op) {
    case NCL_OP_GET_VALUE:
        return point->readable;
    case NCL_OP_SET_VALUE:
        return point->writable;
    case NCL_OP_FUNC_CALL:
        return point->callable;
    default:
        return false;
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
    const char *name = ncl_tool_point_name(point);

    switch (op) {
    case NCL_OP_GET_VALUE:
        (void)snprintf(buffer, size, "%s.read", name);
        break;
    case NCL_OP_SET_VALUE:
        (void)snprintf(buffer, size, "%s.write", name);
        break;
    default:
        (void)snprintf(buffer, size, "%s", name);
        break;
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
        const char *name = ncl_tool_point_name(point);
        size_t j;
        size_t op;
        size_t declared = 0;

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
        if (name[0] == '\0') {
            err_append1(err, "the point path %s ends with '/'", point->path);
            return NCL_ERR_INVALID_ARG;
        }
        if (point->sampled && !point->readable) {
            err_append1(err, "the point %s is sampled but not readable",
                        point->path);
            return NCL_ERR_INVALID_ARG;
        }
        for (op = 0; op < 3; op++) {
            if (point_declares(point, k_operations[op])) {
                declared++;
            }
        }
        if (declared == 0 && point->available) {
            err_append1(err, "the point %s declares no operation", point->path);
            return NCL_ERR_INVALID_ARG;
        }
        /* The value operations get their own suffix, so a name ending in one
         * of them could collide with another point's name. */
        if (strstr(name, ".read") != NULL || strstr(name, ".write") != NULL) {
            err_append1(err, "the point name %s must not contain .read/.write",
                        name);
            return NCL_ERR_INVALID_ARG;
        }
        for (j = 0; j < i; j++) {
            const ncl_tool_point *other = &decl->points[j];
            size_t other_op;

            if (ncl_str_is_blank(other->path) || other->path[0] != '/') {
                continue;
            }
            if (strcmp(ncl_tool_point_name(other), name) != 0) {
                continue;
            }
            /* Same name: each (path, operation) pair may be declared once. */
            for (other_op = 0; other_op < 3; other_op++) {
                ncl_operation candidate = k_operations[other_op];

                if (point_declares(other, candidate) &&
                    point_declares(point, candidate)) {
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
    ncl_json   *items;   /**< handed to the node once it is filled      */
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

        if (point->callable && !point->readable && !point->writable) {
            continue; /* a method only: it belongs in the schema, not here */
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
        ncl_json *target = items;

        if (point->callable && !point->readable && !point->writable) {
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
        if (point->writable) {
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
                snprintf(groups[g].id, sizeof(groups[g].id), "c%u",
                         (unsigned)g);
                if (groups[g].node == NULL || groups[g].items == NULL ||
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
            target = groups[g].items;
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
        if (groups[i].node != NULL && groups[i].items != NULL) {
            (void)ncl_json_obj_set(groups[i].node, "dataItems",
                                   groups[i].items);
            groups[i].items = NULL; /* the node owns it now */
        }
        ncl_json_free(groups[i].items);
    }
    ncl_mem_free(groups);
    (void)ncl_json_obj_set(node, "dataItems", items);
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
 * point that cannot be read just leaves it NULL, which the trail shows as "-".
 */
static void shim_read_old_value(const ncl_tool_shim *shim, ncl_json **old_value)
{
    if (shim->audit == NULL || shim->audit->write == NULL ||
        !shim->point->readable) {
        return;
    }
    (void)shim->point->fn(shim->ctx, shim->point, NCL_OP_GET_VALUE, NULL,
                          old_value, NULL);
}

/**
 * The answer a point that is declared but not readable yet gives (see
 * NCL_POINT_PENDING): the declaration's own summary is the reason. Nothing went
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

static ncl_err shim_read(void *instance, const ncl_json *params,
                         ncl_json **result, char **reason)
{
    const ncl_tool_shim *shim = (const ncl_tool_shim *)instance;
    int64_t started = ncl_time_monotonic_millis();
    ncl_err rc;

    if (!shim->point->available) {
        return shim_unavailable(shim, reason);
    }
    rc = shim->point->fn(shim->ctx, shim->point, NCL_OP_GET_VALUE, params, result,
                         reason);
    shim_report(shim, NCL_OP_GET_VALUE, rc,
                ncl_time_monotonic_millis() - started);
    return rc;
}

static ncl_err shim_write(void *instance, const ncl_json *params,
                          ncl_json **result, char **reason)
{
    const ncl_tool_shim *shim = (const ncl_tool_shim *)instance;
    ncl_json *old_value = NULL;
    int64_t started = ncl_time_monotonic_millis();
    ncl_err rc;

    if (!shim->point->available) {
        return shim_unavailable(shim, reason);
    }
    shim_read_old_value(shim, &old_value);
    rc = shim->point->fn(shim->ctx, shim->point, NCL_OP_SET_VALUE, params, result,
                         reason);
    if (shim->audit != NULL && shim->audit->write != NULL) {
        shim->audit->write(shim->audit->user, shim->decl->name, shim->point,
                           old_value, ncl_tool_param_value(params), rc);
    }
    ncl_json_free(old_value);
    shim_report(shim, NCL_OP_SET_VALUE, rc,
                ncl_time_monotonic_millis() - started);
    return rc;
}

static ncl_err shim_call(void *instance, const ncl_json *params,
                         ncl_json **result, char **reason)
{
    const ncl_tool_shim *shim = (const ncl_tool_shim *)instance;
    int64_t started = ncl_time_monotonic_millis();
    ncl_err rc;

    if (!shim->point->available) {
        return shim_unavailable(shim, reason);
    }
    rc = shim->point->fn(shim->ctx, shim->point, NCL_OP_FUNC_CALL, params, result,
                         reason);
    shim_report(shim, NCL_OP_FUNC_CALL, rc,
                ncl_time_monotonic_millis() - started);
    return rc;
}

static ncl_tool_fn shim_for(ncl_operation op)
{
    switch (op) {
    case NCL_OP_GET_VALUE:
        return shim_read;
    case NCL_OP_SET_VALUE:
        return shim_write;
    default:
        return shim_call;
    }
}

/** How many operations @p point declares. */
static size_t point_operation_count(const ncl_tool_point *point)
{
    size_t i;
    size_t count = 0;

    for (i = 0; i < 3; i++) {
        if (point_declares(point, k_operations[i])) {
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
        ncl_tool_method methods[3];
        ncl_tool_binding bindings[3];
        char names[3][256];
        ncl_tool_shim *shim = &registration->shims[at];
        size_t count = 0;
        size_t op;

        shim->ctx = registration->ctx;
        shim->point = point;
        shim->audit = audit;
        shim->decl = decl;
        for (op = 0; op < 3; op++) {
            ncl_operation candidate = k_operations[op];

            if (!point_declares(point, candidate)) {
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
