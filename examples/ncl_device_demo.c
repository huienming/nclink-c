/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * 设备端示例（NC-Link Server）
 *
 * 演示一台数控机床接入 NC-Link 的完整流程：
 *   1. 指定安装根目录（不存在就建好）、初始化日志
 *   2. 首次启动自举：bin/sn.txt 没有就生成一个 SN（"V2" + 9 位十六进制）；
 *      conf/model/nclink.json 没有就写入默认模型；conf/mqtt.cfg 没有就写入
 *      本机 broker（tcp://127.0.0.1:1883、匿名登录）
 *   3. 按 conf/mqtt.cfg 连接 broker（设备端用 SN 做 clientId）
 *   4. 装载模型（读 conf/model/nclink.json，改文件即换模型），注册工具方法与
 *      `<operation>#<path>` 绑定（可带参数 JSON Schema）
 *   5. 注册文件工具并启动 FTP 端点
 *   6. 启动模型里声明的采样通道
 *   7. 开放 HTTP 接口（OpenAPI 3.0 文档 + /swagger-ui + 配置接口）
 *   8. 运行期间发布事件，直到收到 Ctrl+C（给了秒数就跑那么久），
 *      退出时按相反顺序释放资源
 *
 * 用法：ncl_device_demo [安装根目录] [运行秒数]
 *       两个参数都可省略（默认 "." 与"一直运行"）。**省略秒数就一直跑到 Ctrl+C**，
 *       装到现场就是这么用的；给了正数则跑完自动退出，脚本/冒烟里用（下面这个
 *       60 秒的跑法只为让输出有个确定的长度）：
 *           ncl_device_demo D:\sim4 60
 *       根目录可以指向一个还不存在的空目录：示例会把 conf/、bin/、log/ 连同
 *       SN、模型、mqtt.cfg 一次备齐，适合"清空目录重跑一遍"。
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_client.h"
#include "nclink/ncl_config.h"
#include "nclink/ncl_env.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_general.h"
#include "nclink/ncl_http.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_platform.h"
#include "nclink/ncl_rest.h"
#include "nclink/ncl_server.h"

#include "device_model.h"

/* ------------------------------------------------------------------ 模型 -- */

/*
 * 设备模型：**所有语言的设备端示例共用同一份**，而且编译在代码里 —— 就是
 * examples/device_model.c 的 ncl_demo_device_model()（发布包里也带着它）。
 * 分发时不需要任何外部模型文件。
 *
 * 运行时：<root>/conf/model/nclink.json（安装根目录里那份）存在就以它为准
 * （改文件、或走 REST 的 /api/setModel 都行）；没有就把编译进来的那份写进去
 * （首次启动自举）。C# / Java / Python / Go 的示例走同一份源码（垫片/绑定里
 * 也 #include 了它）。
 *
 * 模型要点（细节见文件本身）：
 *   - 一台数控机床：X/Y/Z/C 四个进给轴 + 主轴 S + 数控系统（CONTROLLER）；
 *   - 每个轴一个功率（/AXIS@<轴>/POWER@1）与**三个加速度**
 *     （/AXIS@<轴>/ACCELERATION@X|Y|Z）—— 振动信号在 X/Y/Z 三个方向上的分量，
 *     方向写在数据项的 number 里，路径就是 /AXIS@<轴>/ACCELERATION@<方向>；
 *   - 两个采样通道：sample_channel0（1 s / 1 s，机床运行状态八项）与
 *     EdgeSersors（1 ms / 100 ms：功率一槽 1 点，振动一槽 4 点 = 0.25 ms 一位，
 *     即 MANUAL 4.5 的亚毫秒采样；共 5 + 15 = 20 列）。
 */
/**
 * 取设备模型文本（堆字符串，调用方 free）。
 *
 * 模型编译在 examples/device_model.c 里（**唯一出处**，五个语言的示例共用：
 * ncl_demo_device_model()）。<root>/conf/model/nclink.json 存在就优先用现场那份
 * （改文件、或走 REST 的 /api/setModel 都行）；没有就把编译进来的那份写进去。
 * 分发时只带可执行文件与库即可，不需要任何外部模型文件。
 */
static char *demo_load_model(void)
{
    char *text = NULL;
    char *dir = NULL;

    if (ncl_file_read_all(ncl_env_model_file(), &text, NULL) == NCL_OK) {
        return text;
    }
    if (ncl_asprintf(&dir, "%s%cmodel", ncl_env_conf_path(), NCL_PATH_SEP) == NCL_OK) {
        ncl_mkdir_p(dir);
        ncl_free_safe(dir);
    }
    text = ncl_strdup(ncl_demo_device_model());
    if (text == NULL) {
        return NULL;
    }
    if (ncl_file_write_all(ncl_env_model_file(), text, strlen(text)) == NCL_OK) {
        ncl_log_info("首次启动：写入设备模型（%s，编译进去的那份）",
                     ncl_env_model_file());
    } else {
        ncl_log_warn("模型写不进去（%s），本次只用内存里那份", ncl_env_model_file());
    }
    return text;
}

/* ------------------------------------------------------------------ 工具 -- */

/* 轴槽位：顺序与默认模型里 X/Y/Z/C/S 五个 AXIS 组件一致。 */
enum {
    DEMO_AXIS_X = 0,
    DEMO_AXIS_Y,
    DEMO_AXIS_Z,
    DEMO_AXIS_C,
    DEMO_AXIS_S,
    DEMO_AXIS_COUNT
};

/** 工具实例：真实设备这里放句柄（串口、PLC 连接等）。 */
typedef struct {
    int status;          /**< /STATUS 设备状态（示例用 0 空闲、1 运行、2 报警） */
    long long part_count; /**< /PART_COUNT 加工计件 */
    int warning;         /**< /CONTROLLER/WARNING 报警号，0 表示无报警 */
    int program;         /**< /CONTROLLER/PROGRAM 当前加工程序名 */
    int tool_number;     /**< /CONTROLLER/TOOL_NUMBER 当前刀号 */
    int feed_override;   /**< /FEED_OVERRIDE 进给倍率（%） */
    int spindle_speed;   /**< /AXIS@S/SPEED 主轴转速（r/min） */
    int mode;            /**< /MACHINING_MODE 加工模式（0 手动 / 1 录入 / 2 自动） */
} demo_device;

/**
 * 示例的"传感器寄存器"：每次被取值就往前推进（真机是硬件按自己的节拍刷新，设备端
 * 只是读寄存器）。按查询次数推进，采样调度快慢都不影响曲线的连续性。
 */
/*
 * 一根轴上可以挂多路传感器（模型里就是同 type、不同 number 的几个数据项）：示例里
 * 主轴 S 挂两路（number 1/2），其余各一路。所以寄存器按 [轴][传感器] 两维开，
 * sensor 从 0 开始数，模型里的 number 就是 sensor + 1。
 */
static ncl_err tool_get_status(void *instance, const ncl_json *params,
                               ncl_json **result, char **reason)
{
    demo_device *device = (demo_device *)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_int(device->status);
    return NCL_OK;
}

static ncl_err tool_set_status(void *instance, const ncl_json *params,
                               ncl_json **result, char **reason)
{
    demo_device *device = (demo_device *)instance;
    long long value = 0;

    if (!ncl_json_as_int(ncl_json_obj_get(params, "value"), &value)) {
        if (reason != NULL) {
            *reason = ncl_strdup("value 必须是整数");
        }
        return NCL_ERR_INVALID_VALUE;
    }
    device->status = (int)value;
    ncl_log_info("STATUS 被设置为 %d", device->status);
    /* 返回 NULL 会让应答 code=NG；这里返回 true 表示写入成功。 */
    *result = ncl_json_new_bool(true);
    return NCL_OK;
}

static ncl_err tool_get_part_count(void *instance, const ncl_json *params,
                                   ncl_json **result, char **reason)
{
    demo_device *device = (demo_device *)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_int(device->part_count);
    return NCL_OK;
}

static ncl_err tool_get_warning(void *instance, const ncl_json *params,
                                ncl_json **result, char **reason)
{
    demo_device *device = (demo_device *)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_int(device->warning);
    return NCL_OK;
}

static ncl_err tool_get_program(void *instance, const ncl_json *params,
                                ncl_json **result, char **reason)
{
    demo_device *device = (demo_device *)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_int(device->program);
    return NCL_OK;
}

/*
 * 通道 0 的另外几项：刀号、进给倍率、主轴转速、加工模式。都是读一眼就知道的标量
 * （真机从 PLC / 数控系统读），示例里由 100 ms 的主循环推着走（见 main 的模拟循环）。
 *
 * 主轴转速就是 S 轴的 SPEED 数据项（/AXIS@S/SPEED）："轴上转多快"这类量挂在轴上，
 * 与轴的功率、加速度同一层写法；S 轴的中文名是"主轴"，所以 SPEED 在这里读作
 * "主轴转速"。
 */
static ncl_err tool_get_tool_number(void *instance, const ncl_json *params,
                                    ncl_json **result, char **reason)
{
    demo_device *device = (demo_device *)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_int(device->tool_number);
    return NCL_OK;
}

static ncl_err tool_get_feed_override(void *instance, const ncl_json *params,
                                      ncl_json **result, char **reason)
{
    demo_device *device = (demo_device *)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_int(device->feed_override);
    return NCL_OK;
}

static ncl_err tool_get_speed_s(void *instance, const ncl_json *params,
                                ncl_json **result, char **reason)
{
    demo_device *device = (demo_device *)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_int(device->spindle_speed);
    return NCL_OK;
}

static ncl_err tool_get_machining_mode(void *instance, const ncl_json *params,
                                       ncl_json **result, char **reason)
{
    demo_device *device = (demo_device *)instance;
    (void)params;
    (void)reason;
    *result = ncl_json_new_int(device->mode);
    return NCL_OK;
}


/*
 * 示例的"传感器寄存器"：每次被取值就往前推进（真机是硬件按自己的节拍刷新，设备端
 * 只是读寄存器）。按查询次数推进，采样调度快慢都不影响曲线的连续性。
 *
 * 功率每轴一格；振动**每轴每方向**一格 —— X/Y/Z 三条曲线各走各的相位。
 */
enum { DEMO_DIR_X = 0, DEMO_DIR_Y, DEMO_DIR_Z, DEMO_DIR_COUNT };

static long long demo_power_tick[DEMO_AXIS_COUNT];
static long long demo_vibration_tick[DEMO_AXIS_COUNT][DEMO_DIR_COUNT];

/** 功率（W）：每轴一个基值，再叠 0~300 W 的缓升，25 格一个锯齿。 */
static double demo_axis_power(int axis)
{
    long long tick = demo_power_tick[axis]++;

    return 800.0 + (double)axis * 250.0 +
           (double)((tick + (long long)axis * 7) % 25) * 12.5;
}

/*
 * 振动：一次查询给 4 个子采样，打包成一个数组。通道周期最小只能写整数毫秒
 * （1 ms），1 ms 里给 4 个就等效 0.25 ms 一个（4 kHz），即 MANUAL 4.5 的
 * "亚毫秒采样"：一列允许是数组，库里的 ncl_sample_item_value_count()/value_at()
 * 会把它摊平，客户端照样按一列读。
 */
#define DEMO_VIBRATION_PER_QUERY 4

/** 振动的一个子采样：0.25 ms 一格的三角波，值都是 0.125 的整数倍。 */
static double demo_axis_vibration_step(int axis, int direction, long long step)
{
    int slot = axis * DEMO_DIR_COUNT + direction;

    return (double)(((step + (long long)slot * 3) % 16) - 8) * 0.125;
}

/** 一次查询取"某轴某方向"的振动：这一轮里的 4 个 0.25 ms 子采样。 */
static ncl_json *demo_axis_vibration_block(int axis, int direction)
{
    long long step = demo_vibration_tick[axis][direction];
    ncl_json *block = ncl_json_new_array();
    int k;

    if (block == NULL) {
        return NULL;
    }
    demo_vibration_tick[axis][direction] += DEMO_VIBRATION_PER_QUERY;
    for (k = 0; k < DEMO_VIBRATION_PER_QUERY; k++) {
        double value = demo_axis_vibration_step(axis, direction, step + k);

        if (ncl_json_arr_push(block, ncl_json_new_double(value)) != NCL_OK) {
            ncl_json_free(block);
            return NULL;
        }
    }
    return block;
}

/*
 * 服务端按"路径 → 方法"取值，而方法签名里拿不到路径，所以每条路径要一套绑定、
 * 一个方法体。样板用宏生成：功率 5 个，加速度 5 轴 × 3 方向 = 15 个。
 */
#define DEMO_DEFINE_POWER_GETTER(fn, axis_slot)                                \
    static ncl_err fn(void *instance, const ncl_json *params,                  \
                      ncl_json **result, char **reason)                        \
    {                                                                          \
        (void)instance;                                                        \
        (void)params;                                                          \
        (void)reason;                                                          \
        *result = ncl_json_new_double(demo_axis_power(axis_slot));             \
        return NCL_OK;                                                         \
    }

#define DEMO_DEFINE_VIBRATION_GETTER(fn, axis_slot, direction_idx)             \
    static ncl_err fn(void *instance, const ncl_json *params,                  \
                      ncl_json **result, char **reason)                        \
    {                                                                          \
        (void)instance;                                                        \
        (void)params;                                                          \
        (void)reason;                                                          \
        *result = demo_axis_vibration_block(axis_slot, direction_idx);         \
        return *result != NULL ? NCL_OK : NCL_ERR_NOMEM;                       \
    }

DEMO_DEFINE_POWER_GETTER(tool_get_power_x, DEMO_AXIS_X)
DEMO_DEFINE_POWER_GETTER(tool_get_power_y, DEMO_AXIS_Y)
DEMO_DEFINE_POWER_GETTER(tool_get_power_z, DEMO_AXIS_Z)
DEMO_DEFINE_POWER_GETTER(tool_get_power_c, DEMO_AXIS_C)
DEMO_DEFINE_POWER_GETTER(tool_get_power_s, DEMO_AXIS_S)

DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_xx, DEMO_AXIS_X, DEMO_DIR_X)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_xy, DEMO_AXIS_X, DEMO_DIR_Y)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_xz, DEMO_AXIS_X, DEMO_DIR_Z)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_yx, DEMO_AXIS_Y, DEMO_DIR_X)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_yy, DEMO_AXIS_Y, DEMO_DIR_Y)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_yz, DEMO_AXIS_Y, DEMO_DIR_Z)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_zx, DEMO_AXIS_Z, DEMO_DIR_X)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_zy, DEMO_AXIS_Z, DEMO_DIR_Y)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_zz, DEMO_AXIS_Z, DEMO_DIR_Z)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_cx, DEMO_AXIS_C, DEMO_DIR_X)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_cy, DEMO_AXIS_C, DEMO_DIR_Y)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_cz, DEMO_AXIS_C, DEMO_DIR_Z)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_sx, DEMO_AXIS_S, DEMO_DIR_X)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_sy, DEMO_AXIS_S, DEMO_DIR_Y)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_sz, DEMO_AXIS_S, DEMO_DIR_Z)

#undef DEMO_DEFINE_POWER_GETTER
#undef DEMO_DEFINE_VIBRATION_GETTER

/*
 * 参数 JSON Schema：只在收到 check=true 的 MethodCall 时使用，
 * 用于在不执行任何动作的前提下校验参数。
 */
#define SET_STATUS_SCHEMA                                                      \
    "{\"type\":\"object\",\"properties\":{"                                    \
    "\"value\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":65535}},"       \
    "\"required\":[\"value\"]}"

/*
 * 工具方法表：8 个状态量 + 5 个轴功率 + 15 个方向加速度 —— 每条绑定路径一个方法。
 * 方法名里的轴与方向直接对应路径 /AXIS@<轴>/ACCELERATION@<方向>
 * （getAccelerationXY = X 轴、Y 方向的振动）。
 */
static const ncl_tool_method kMethods[] = {
    {"getValue", tool_get_status, NULL},
    {"setValue", tool_set_status, SET_STATUS_SCHEMA},
    {"getCount", tool_get_part_count, NULL},
    {"getWarning", tool_get_warning, NULL},
    {"getProgram", tool_get_program, NULL},
    {"getToolNumber", tool_get_tool_number, NULL},
    {"getFeedOverride", tool_get_feed_override, NULL},
    {"getSpeedS", tool_get_speed_s, NULL},
    {"getMachiningMode", tool_get_machining_mode, NULL},
    {"getPowerX", tool_get_power_x, NULL},
    {"getPowerY", tool_get_power_y, NULL},
    {"getPowerZ", tool_get_power_z, NULL},
    {"getPowerC", tool_get_power_c, NULL},
    {"getPowerS", tool_get_power_s, NULL},
    {"getAccelerationXX", tool_get_acceleration_xx, NULL},
    {"getAccelerationXY", tool_get_acceleration_xy, NULL},
    {"getAccelerationXZ", tool_get_acceleration_xz, NULL},
    {"getAccelerationYX", tool_get_acceleration_yx, NULL},
    {"getAccelerationYY", tool_get_acceleration_yy, NULL},
    {"getAccelerationYZ", tool_get_acceleration_yz, NULL},
    {"getAccelerationZX", tool_get_acceleration_zx, NULL},
    {"getAccelerationZY", tool_get_acceleration_zy, NULL},
    {"getAccelerationZZ", tool_get_acceleration_zz, NULL},
    {"getAccelerationCX", tool_get_acceleration_cx, NULL},
    {"getAccelerationCY", tool_get_acceleration_cy, NULL},
    {"getAccelerationCZ", tool_get_acceleration_cz, NULL},
    {"getAccelerationSX", tool_get_acceleration_sx, NULL},
    {"getAccelerationSY", tool_get_acceleration_sy, NULL},
    {"getAccelerationSZ", tool_get_acceleration_sz, NULL}};

/*
 * 绑定：<operation>#<模型路径>。路径要和统一模型里数据项的路径一致（设备端收到的
 * 请求项是节点 id，服务端先按 id 找到节点、再取路径匹配绑定）。两个采样通道里的
 * 28 项全都按这些路径取值，缺一条那一列只能是 null。
 */
static const ncl_tool_binding kBindings[] = {
    {"/STATUS", NCL_OP_GET_VALUE, "getValue", "plc"},
    {"/STATUS", NCL_OP_SET_VALUE, "setValue", "plc"},
    {"/PART_COUNT", NCL_OP_GET_VALUE, "getCount", "plc"},
    {"/CONTROLLER/WARNING", NCL_OP_GET_VALUE, "getWarning", "plc"},
    {"/CONTROLLER/PROGRAM", NCL_OP_GET_VALUE, "getProgram", "plc"},
    {"/CONTROLLER/TOOL_NUMBER", NCL_OP_GET_VALUE, "getToolNumber", "plc"},
    {"/FEED_OVERRIDE", NCL_OP_GET_VALUE, "getFeedOverride", "plc"},
    {"/MACHINING_MODE", NCL_OP_GET_VALUE, "getMachiningMode", "plc"},
    {"/AXIS@S/SPEED", NCL_OP_GET_VALUE, "getSpeedS", "plc"},
    {"/AXIS@X/POWER@1", NCL_OP_GET_VALUE, "getPowerX", "plc"},
    {"/AXIS@Y/POWER@1", NCL_OP_GET_VALUE, "getPowerY", "plc"},
    {"/AXIS@Z/POWER@1", NCL_OP_GET_VALUE, "getPowerZ", "plc"},
    {"/AXIS@C/POWER@1", NCL_OP_GET_VALUE, "getPowerC", "plc"},
    {"/AXIS@S/POWER@1", NCL_OP_GET_VALUE, "getPowerS", "plc"},
    {"/AXIS@X/ACCELERATION@X", NCL_OP_GET_VALUE, "getAccelerationXX", "plc"},
    {"/AXIS@X/ACCELERATION@Y", NCL_OP_GET_VALUE, "getAccelerationXY", "plc"},
    {"/AXIS@X/ACCELERATION@Z", NCL_OP_GET_VALUE, "getAccelerationXZ", "plc"},
    {"/AXIS@Y/ACCELERATION@X", NCL_OP_GET_VALUE, "getAccelerationYX", "plc"},
    {"/AXIS@Y/ACCELERATION@Y", NCL_OP_GET_VALUE, "getAccelerationYY", "plc"},
    {"/AXIS@Y/ACCELERATION@Z", NCL_OP_GET_VALUE, "getAccelerationYZ", "plc"},
    {"/AXIS@Z/ACCELERATION@X", NCL_OP_GET_VALUE, "getAccelerationZX", "plc"},
    {"/AXIS@Z/ACCELERATION@Y", NCL_OP_GET_VALUE, "getAccelerationZY", "plc"},
    {"/AXIS@Z/ACCELERATION@Z", NCL_OP_GET_VALUE, "getAccelerationZZ", "plc"},
    {"/AXIS@C/ACCELERATION@X", NCL_OP_GET_VALUE, "getAccelerationCX", "plc"},
    {"/AXIS@C/ACCELERATION@Y", NCL_OP_GET_VALUE, "getAccelerationCY", "plc"},
    {"/AXIS@C/ACCELERATION@Z", NCL_OP_GET_VALUE, "getAccelerationCZ", "plc"},
    {"/AXIS@S/ACCELERATION@X", NCL_OP_GET_VALUE, "getAccelerationSX", "plc"},
    {"/AXIS@S/ACCELERATION@Y", NCL_OP_GET_VALUE, "getAccelerationSY", "plc"},
    {"/AXIS@S/ACCELERATION@Z", NCL_OP_GET_VALUE, "getAccelerationSZ", "plc"}};


/* ------------------------------------------------------------- MQTT 回调 -- */

typedef struct {
    ncl_server *server;
    demo_device *device;   /**< 自研传输与自定义路由要读当前状态 */
    const char *sn;        /**< 只读：报给 /api/hello */
} device_link;

/** 每收到一条请求：按主题推断类型解析，再交给服务端处理。 */
static void on_mqtt_message(void *user, const ncl_mqtt_publish *publish)
{
    device_link *link = (device_link *)user;
    ncl_message *request;

    if (publish->topic == NULL) {
        return;
    }
    request = ncl_message_parse(publish->topic,
                                (const char *)publish->payload,
                                publish->payload_len);
    if (request == NULL) {
        ncl_log_warn("无法解析来自 %s 的报文", publish->topic);
        return;
    }
    /* 内部会转交线程池处理，因此不要在收包线程上逗留。 */
    ncl_server_on_message(link->server, publish->topic, request);
}

/* --------------------------------------------------------- 首次启动自举 -- */

/** 本机 broker：默认值，也写进首次启动生成的 conf/mqtt.cfg。 */
#define DEMO_BROKER_URL "tcp://127.0.0.1:1883"

/**
 * 首次启动自举：安装根目录里缺什么补什么，已经存在的文件一律不动。
 *
 *   bin/sn.txt              设备 SN：由 ncl_sn_read() 生成（"V2" + 9 位十六进制）
 *   conf/model/nclink.json  设备模型：从编译进去的那份自举
 *                           （见 demo_load_model()；五个语言的示例共用同一份源码）
 *   conf/mqtt.cfg           本机 broker：tcp://127.0.0.1:1883，匿名登录
 *
 * 这几个文件就是设备身份与配置的唯一出处，所以"删掉根目录重跑"和"换一台设备"
 * 是一回事。模型不在本函数里写：demo_load_model() 先看 <root>/conf/model/
 * nclink.json，没有才把仓库里那份拷进去。
 *
 * SN 不在这里生成：ncl_sn_read() 在 bin/sn.txt 缺失时自己生成并落盘（见 4.1），
 * 示例跟着库走，不另造一种格式。
 */
static ncl_err demo_bootstrap(void)
{
    ncl_err rc;
    char *dir = NULL;

    ncl_mkdir_p(ncl_env_run_path());
    ncl_mkdir_p(ncl_env_conf_path());

    /* 模型不在这里写：demo_load_model() 会在 <root>/conf/model/nclink.json
 * 缺失时用编译进去的那份（ncl_demo_device_model()）自举。 */

    /* 2) mqtt.cfg：本机 broker、匿名登录（用户名/密码留空）。 */
    if (!ncl_path_exists(ncl_env_mqtt_cfg_file())) {
        static const char kDefaultCfg[] =
            "url=" DEMO_BROKER_URL "\r\nusername=\r\npassword=\r\n";

        rc = ncl_file_write_all(ncl_env_mqtt_cfg_file(), kDefaultCfg,
                                sizeof(kDefaultCfg) - 1);
        if (rc != NCL_OK) {
            return rc;
        }
        ncl_log_info("首次启动：写入 MQTT 配置（%s）", ncl_env_mqtt_cfg_file());
    }
    return NCL_OK;
}

/**
 * 读取 conf/mqtt.cfg 的连接参数。
 *
 * 用 ncl_config_get_mqtt() 读文件原文（"key=value" 行变成 JSON），而不是
 * ncl_mqtt_config_read()：后者会把**留空的用户名**补成 admin（面向远程 broker
 * 的保守默认），而本机 broker 一般不开鉴权，要的就是空 —— 空即匿名，连接报文
 * 里干脆不带用户名/密码字段。
 *
 * 三个参数都是堆字符串，由调用方 free。
 */
static ncl_err demo_mqtt_config(char **url, char **username, char **password)
{
    ncl_json *file = ncl_config_get_mqtt();
    const char *value;

    if (file == NULL) {
        return NCL_ERR_IO;
    }
    value = ncl_json_obj_get_string(file, "url");
    *url = ncl_strdup(value != NULL && value[0] != '\0' ? value
                                                        : DEMO_BROKER_URL);
    value = ncl_json_obj_get_string(file, "username");
    *username = ncl_strdup(value != NULL ? value : "");
    value = ncl_json_obj_get_string(file, "password");
    *password = ncl_strdup(value != NULL ? value : "");
    ncl_json_free(file);

    return (*url != NULL && *username != NULL && *password != NULL)
               ? NCL_OK
               : NCL_ERR_NOMEM;
}

/**
 * 把模型里声明的采样通道与各采样项的路径打出来。
 *
 * 采样报文里某一列一直是 null 时，先看这里：路径要和 kBindings 里注册的路径
 * 对得上，工具才会被调用。
 */
static void log_sample_channels(const ncl_node *root)
{
    size_t d;
    size_t c;

    if (root == NULL) {
        return;
    }
    for (d = 0; ncl_node_device_at(root, d) != NULL; d++) {
        const ncl_node *device = ncl_node_device_at(root, d);

        ncl_log_info("设备 %s（%s %s）",
                     device->id != NULL ? device->id : "?",
                     device->node_type_name != NULL ? device->node_type_name : "?",
                     device->name != NULL ? device->name : "");
        for (c = 0; ncl_node_config_at(device, c) != NULL; c++) {
            const ncl_node *config = ncl_node_config_at(device, c);
            size_t i;

            if (!ncl_node_is_sample_node(config)) {
                continue;
            }
            ncl_log_info("采样通道 %s：%u ms 采样 / %u ms 上报，%u 项",
                         config->id != NULL ? config->id : "?",
                         (unsigned)config->sample_interval,
                         (unsigned)config->upload_interval,
                         (unsigned)ncl_node_sample_count(config));
            for (i = 0; i < ncl_node_sample_count(config); i++) {
                ncl_sample_ref *ref = ncl_node_sample_at(config, i);
                char *path = ref != NULL ? ncl_sample_ref_path(ref) : NULL;

                ncl_log_info("    [%u] %s -> %s", (unsigned)i,
                             ref != NULL && ref->id != NULL ? ref->id : "?",
                             path != NULL ? path : "(未解析)");
                ncl_free_safe(path);
            }
        }
    }
}

/* ---------------------------------------------- 轴的功率、转速与加速度 -- */

/**
 * 轴下面这几类数据项的中文含义（T/CMTBA 1008.4—2020 表4 物理量数据项）。
 *
 * 含义按"**对象 + 物理量**"的说法写，和"主轴振动"（主轴 + 振动）是同一种写法：
 *
 *     POWER        → 功率      （瓦特 W）
 *     SPEED        → 转速      （转每分 r/min）
 *     ACCELERATION → 加速度    （毫米每秒平方 mm/s²）
 *
 * 打印时再把轴补在前面，于是有"X轴功率""主轴转速""主轴加速度"。返回 NULL 表示不是
 * 本函数关心的数据项。
 */
static const char *axis_quantity_meaning_cn(const char *type)
{
    if (type == NULL) {
        return NULL;
    }
    if (strcmp(type, "POWER") == 0) {
        return "功率";
    }
    if (strcmp(type, "SPEED") == 0) {
        return "转速";
    }
    if (strcmp(type, "ACCELERATION") == 0) {
        return "加速度";
    }
    return NULL;
}

/**
 * 打印模型里**每个轴**的功率、转速与加速度，一行一条，格式为
 *
 *     路径 中文含义
 *
 * 含义是"轴 + 物理量"的说法：/AXIS@X/POWER 是"X轴功率"、/AXIS@S/SPEED 是
 * "主轴转速"、/AXIS@S/ACCELERATION 是"主轴加速度"。路径就是设备端对这几个
 * 数据项取值用的路径：工具绑定（kBindings）与采样通道的 ids 都要按这里的
 * 路径来写，否则该列只能是 null。
 */
static void log_axis_quantities(const ncl_node *root)
{
    size_t i;

    if (root == NULL) {
        return;
    }
    /* 只认组件里的 AXIS；功率/加速度是挂在轴下面的数据项。 */
    if (root->type == NCL_NODE_COMPONENT && root->node_type_name != NULL &&
        strcmp(root->node_type_name, "AXIS") == 0) {
        /* 轴的中文名（X轴 / 主轴）就是含义里的那个"对象" */
        const char *axis_name = root->name != NULL ? root->name : "轴";

        for (i = 0; ncl_node_data_item_at(root, i) != NULL; i++) {
            const ncl_node *item = ncl_node_data_item_at(root, i);
            const char *meaning = axis_quantity_meaning_cn(item->node_type_name);

            if (meaning != NULL) {
                /* 同一个部件多路传感器时，光看"主轴功率"分不清是哪一路，所以把
                 * 数据项的 number 也带上（没有 number 的项就是单路，不加后缀）。 */
                ncl_log_info("%-24s %s%s%s%s", ncl_node_path(item), axis_name,
                             meaning, item->number != NULL ? " #" : "",
                             item->number != NULL ? item->number : "");
            }
        }
    }
    for (i = 0; ncl_node_device_at(root, i) != NULL; i++) {
        log_axis_quantities(ncl_node_device_at(root, i));
    }
    for (i = 0; ncl_node_component_at(root, i) != NULL; i++) {
        log_axis_quantities(ncl_node_component_at(root, i));
    }
}

/* ------------------------------------------------------------------ main -- */

/* ------------------------------------------------------------- 退出信号 -- */

/*
 * Ctrl+C（SIGINT）只置一个标志，主循环自己看到之后走**正常的清理路径**：停采样、
 * 停 FTP/HTTP、断开 MQTT、收尾日志。信号处理函数里能做的事很少（异步信号安全），
 * 所以除了置标志什么都不做；等待是 100 ms 一跳（见主循环），响应不会迟。
 *
 * SIGTERM 是 POSIX 的"温和退出"（kill、systemd stop）。Windows 上 Ctrl+C 走的就是
 * SIGINT；把控制台窗口直接关掉（右上角 ×）另说 —— 那是 CTRL_CLOSE_EVENT，CRT 不会
 * 转成 SIGINT，进程会被直接结束。
 */
static volatile sig_atomic_t g_stop = 0;

static void demo_on_signal(int signum)
{
    (void)signum;
    g_stop = 1;
}

static void demo_install_signal_handlers(void)
{
    signal(SIGINT, demo_on_signal);
#ifdef SIGTERM
    signal(SIGTERM, demo_on_signal);
#endif
}

/* 离线模式（broker 写 "-"）的出站传输：报文打到控制台，一眼能看到设备在发什么。 */
static ncl_err demo_console_publish(void *user, const char *topic,
                                    const char *payload, size_t len)
{
    (void)user;
    ncl_log_info("out %s %.*s", topic, (int)len, payload);
    return NCL_OK;
}

/* 自定义 REST 路由：GET /api/hello → {"sn":...,"status":...,"parts":...} */
static void demo_hello_route(ncl_http_request *request, ncl_http_response *response,
                             void *user)
{
    const device_link *link = (const device_link *)user;
    ncl_json *body = ncl_json_new_object();

    (void)request;
    if (body == NULL) {
        ncl_http_reply_text(response, NCL_HTTP_INTERNAL_ERROR, "out of memory");
        return;
    }
    ncl_json_obj_set_string(body, "sn", link->sn != NULL ? link->sn : "");
    ncl_json_obj_set_int(body, "status",
                         link->device != NULL ? link->device->status : 0);
    ncl_json_obj_set_int(body, "parts",
                         link->device != NULL ? link->device->part_count : 0);
    ncl_http_reply_json(response, NCL_HTTP_OK, body);
    ncl_json_free(body);
}

int main(int argc, char **argv)
{
    /* 五个语言的设备端示例用同一套命令行：
     *   ncl_device_demo [broker] [sn] [seconds] [http-port]
     *     broker    tcp://… / ssl://…；写 "-" = 离线（不接 MQTT，出站报文打控制台）；
     *               省略 = 读 <root>/conf/mqtt.cfg
     *     sn        省略或写 "-" = <root>/bin/sn.txt（没有就生成 "V2" + 9 位十六进制）
     *     seconds   0 或省略 = 一直运行到 Ctrl+C
     *     http-port 省略 = 9008；写 0 = 系统分配的随机端口
     *   安装根目录用环境变量 NCL_DEVICE_ROOT 指定（省略 = 当前目录）。 */
    const char *broker_arg = argc > 1 ? argv[1] : NULL;
    const char *sn_arg = argc > 2 ? argv[2] : NULL;
    long long seconds = argc > 3 ? atoll(argv[3]) : 0;
    long long http_port = argc > 4 ? atoll(argv[4]) : 9008;
    const char *root = getenv("NCL_DEVICE_ROOT");
    bool offline = broker_arg != NULL && strcmp(broker_arg, "-") == 0;
    demo_device device;
    char *broker_url = NULL;
    char *broker_user = NULL;
    char *broker_password = NULL;
    ncl_mqtt_client_options mqtt_options;
    ncl_mqtt_client *mqtt = NULL;
    ncl_server_options server_options;
    ncl_server *server = NULL;
    ncl_http_server *http = NULL;
    device_link link;
    char *model_json = NULL;
    char *sn = NULL;
    int exit_code = 0;
    long long i;

    memset(&device, 0, sizeof(device));
    memset(&link, 0, sizeof(link));
    if (root == NULL || root[0] == '\0') {
        root = ".";
    }

    /* 1. 安装根目录：conf/、bin/、log/、uploadFile/ 都在它下面。目录不存在就先
     *    建好，于是"指着一个空目录启动"也是合法的首次启动。 */
    ncl_mkdir_p(root);
    ncl_env_set_root(root);
    ncl_log_init(NULL);
    demo_install_signal_handlers();   /* 早装：自举期间 Ctrl+C 也走同一条清理路径 */
    ncl_log_info("NC-Link 设备端启动，根目录 %s", ncl_env_root());

    /* 2. 自举 conf/ 与 mqtt.cfg（模型在下一步按需自举）。
     *
     *    注意这里不要用 ncl_config_init(NULL, &sn)：那是 REST 的 /api/cfg/init，
     *    它会**无条件覆盖** bin/sn.txt，每调一次设备身份就变一次。 */
    if (demo_bootstrap() != NCL_OK) {
        ncl_log_error("初始化安装根目录失败: %s", ncl_env_root());
        exit_code = 1;
        goto cleanup;
    }

    /* 3. 设备身份：命令行给了就用；否则 bin/sn.txt（缺失时由 ncl_sn_read()
     *    生成并落盘，之后一直沿用）。 */
    if (sn_arg != NULL && sn_arg[0] != '\0' && strcmp(sn_arg, "-") != 0) {
        sn = ncl_strdup(sn_arg);
    } else {
        if (!ncl_path_exists(ncl_env_sn_file())) {
            ncl_log_info("首次启动：生成 SN（%s）", ncl_env_sn_file());
        }
        sn = ncl_sn_read();
    }
    if (sn == NULL) {
        ncl_log_error("无法取得设备 SN: %s", ncl_env_sn_file());
        exit_code = 1;
        goto cleanup;
    }

    /* 4. broker：命令行 → conf/mqtt.cfg；"-" 表示离线。 */
    if (!offline) {
        if (broker_arg != NULL && broker_arg[0] != '\0') {
            broker_url = ncl_strdup(broker_arg);
            broker_user = ncl_strdup("");
            broker_password = ncl_strdup("");
        } else if (demo_mqtt_config(&broker_url, &broker_user,
                                    &broker_password) != NCL_OK) {
            ncl_log_error("读取 mqtt.cfg 失败: %s", ncl_env_mqtt_cfg_file());
            exit_code = 1;
            goto cleanup;
        }
        if (broker_url == NULL) {
            exit_code = 1;
            goto cleanup;
        }
        ncl_mqtt_client_options_default(&mqtt_options);
        mqtt_options.url = broker_url;
        mqtt_options.client_id = sn;              /* 设备端用 SN 做 clientId */
        /* 空用户名/密码 = 匿名：传 NULL 才不会往连接报文里塞空字段。 */
        mqtt_options.username =
            broker_user != NULL && broker_user[0] != '\0' ? broker_user : NULL;
        mqtt_options.password =
            broker_password != NULL && broker_password[0] != '\0' ? broker_password
                                                                  : NULL;
        mqtt_options.keep_alive_seconds = 60;
        mqtt_options.automatic_reconnect = true;  /* 断线自动重连并恢复订阅 */
        mqtt_options.on_message = on_mqtt_message;
        mqtt_options.user = &link;
        mqtt = ncl_mqtt_client_create(&mqtt_options);
        if (mqtt == NULL || ncl_mqtt_client_connect(mqtt) != NCL_OK) {
            ncl_log_error("MQTT 连接失败: %s",
                          mqtt != NULL ? ncl_mqtt_client_last_error(mqtt)
                                       : "内存不足");
            exit_code = 1;
            goto cleanup;
        }
    }

    /* 5. 模型：<root>/conf/model/nclink.json，没有就用共享模型自举。 */
    model_json = demo_load_model();
    if (model_json == NULL) {
        exit_code = 1;
        goto cleanup;
    }

    /* 6. 服务端：装载模型、注册工具、订阅请求主题；离线时出站报文走控制台。 */
    memset(&server_options, 0, sizeof(server_options));
    server_options.sn = sn;
    server_options.mqtt = mqtt;
    server_options.model_json = model_json;
    if (offline) {
        server_options.publish = demo_console_publish;
        server_options.publish_user = &link;
    }
    server = ncl_server_create(&server_options);
    if (server == NULL) {
        ncl_log_error("服务端创建失败（模型是否合法？）");
        exit_code = 1;
        goto cleanup;
    }
    link.server = server;
    link.device = &device;
    link.sn = sn;
    ncl_log_info("设备 SN: %s", sn);
    if (offline) {
        ncl_log_info("离线模式：不接 MQTT，出站报文打到控制台");
    } else {
        ncl_log_info("MQTT 服务器: %s，用户名 %s", broker_url,
                     broker_user != NULL && broker_user[0] != '\0' ? broker_user
                                                                  : "空（匿名连接）");
    }
    log_sample_channels(ncl_server_model(server));
    ncl_log_info("轴的功率与振动（路径 含义 #方向）:");
    log_axis_quantities(ncl_server_model(server));

    /* 7. 工具：8 个状态量 + 5 个轴功率 + 15 个方向加速度（一条路径一个方法）。 */
    if (ncl_server_register_tool(server, "plc", &device, kMethods,
                                 sizeof(kMethods) / sizeof(kMethods[0]), kBindings,
                                 sizeof(kBindings) / sizeof(kBindings[0])) != NCL_OK) {
        ncl_log_error("注册工具失败（方法名与绑定路径是否对得上？）");
        exit_code = 1;
        goto cleanup;
    }
    ncl_server_register_builtin_tool(server);   /* /nclinkServer/addSample 等 */
    ncl_server_register_file_tool(server);      /* /CONTROLLER/FILE */
    if (!offline) {
        ncl_server_subscribe(server);           /* 离线没有请求主题可订 */
    }
    ncl_server_start_ftp(server);               /* 端口与账号来自 bin/ftp.txt */
    ncl_server_init_samples(server);            /* 启动模型里的采样通道 */

    /* 8. HTTP：OpenAPI 3.0 文档、Swagger UI、配置接口 + 一条自定义路由。 */
    http = ncl_http_server_create((unsigned)(http_port > 0 ? http_port : 0));
    if (http != NULL && ncl_http_server_start(http) == NCL_OK) {
        ncl_rest_attach(http, server);
        ncl_rest_attach_config(http);
        ncl_http_server_route(http, "GET", "/api/hello", demo_hello_route, &link);
        ncl_log_info("HTTP: http://localhost:%u/swagger-ui（自定义路由 /api/hello）",
                     ncl_http_server_port(http));
    } else {
        ncl_log_warn("HTTP 端点没起来（端口 %lld 被占？）", http_port);
    }

    /* 9. 运行：100 ms 一跳。功率/振动与状态量都在取值时按当前状态算，所以 1 ms
     *    与 1 s 两个采样周期都拿得到新鲜值，这里不必跟着提速。 */
    device.mode = 2;                /* 加工模式：自动 */
    device.status = 1;              /* 设备状态：运行中 */
    device.program = 1001;          /* 当前加工程序名；下面每 2 秒换一个 */
    device.tool_number = 1;         /* 当前刀号：换程序时跟着换 */
    device.feed_override = 100;     /* 进给倍率 100% */
    device.spindle_speed = 3600;    /* 主轴转速 r/min */
    ncl_log_info("工具 %u 个操作，采样通道 %u 个",
                 (unsigned)ncl_server_operation_count(server),
                 (unsigned)ncl_server_sample_count(server));
    if (seconds > 0) {
        ncl_log_info("运行 %lld 秒（Ctrl+C 可随时退出）", seconds);
    } else {
        ncl_log_info("一直运行，Ctrl+C 退出");
    }
    for (i = 0; !g_stop && (seconds <= 0 || i < seconds * 10); i++) {
        ncl_sleep_millis(100);
        device.part_count++;                               /* 模拟产量累加 */
        device.feed_override = 60 + (int)(i % 7) * 10;     /* 60% ~ 120% 来回 */
        device.spindle_speed = 3000 + (int)(i % 5) * 300;  /* 3000 ~ 4200 r/min */
        if (i % 20 == 19) {                                /* 每 2 秒换程序 */
            device.program++;
            device.tool_number = 1 + (int)(device.program % 8);
            device.mode = (device.program % 2 == 0) ? 1 : 2;
        }
        if (i % 10 == 9) {                                 /* 每秒一条事件 */
            ncl_json *event = ncl_json_new_object();
            ncl_json_obj_set_string(event, "key", "PART_COUNT");
            ncl_json_obj_set_int(event, "value", device.part_count);
            ncl_json_obj_set_int(event, "oldValue", device.part_count - 1);
            ncl_server_push_event(server, "010307", event);   /* /PART_COUNT */
            ncl_json_free(event);
            ncl_log_info("事件 PART_COUNT=%lld；采样上报 %u 次，状态 %d，刀号 %d",
                         device.part_count,
                         (unsigned)ncl_server_sample_upload_count(server),
                         device.status, device.tool_number);
        }
    }
    if (g_stop) {
        ncl_log_info("收到退出信号（Ctrl+C），开始停止…");
    }
    ncl_log_info("设备端退出统计：采样上报 %u 次，事件 %u 条",
                 (unsigned)ncl_server_sample_upload_count(server),
                 (unsigned)ncl_server_event_count(server));

cleanup:

    /* 释放顺序：先停服务（采样线程、FTP），再拆连接，最后是配置与 SN。 */
    if (http != NULL) {
        ncl_http_server_stop(http);
        ncl_http_server_free(http);
    }
    if (server != NULL) {
        ncl_server_free(server);   /* 内部会停采样、FTP 与文件工具 */
    }
    ncl_mqtt_client_disconnect(mqtt);
    ncl_mqtt_client_destroy(mqtt);
    ncl_free_safe(broker_url);
    ncl_free_safe(broker_user);
    ncl_free_safe(broker_password);
    ncl_free_safe(model_json);
    ncl_log_info("设备端已退出");
    ncl_log_shutdown();
    ncl_env_shutdown();
    ncl_free_safe(sn);
    return exit_code;
}
