/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 huienming */

/*
 * 设备端示例（NC-Link Server）
 *
 * 演示一台数控机床接入 NC-Link 的完整流程：
 *   1. 指定安装根目录（不存在就建好）、初始化日志
 *   2. 首次启动自举：bin/sn.txt 没有就随机生成一个 9 位 SN 存下来；
 *      conf/model/nclink.json 没有就写入默认模型；conf/mqtt.cfg 没有就写入
 *      本机 broker（tcp://127.0.0.1:1883、匿名登录）
 *   3. 按 conf/mqtt.cfg 连接 broker（设备端用 SN 做 clientId）
 *   4. 装载模型（读 conf/model/nclink.json，改文件即换模型），注册工具方法与
 *      `<operation>#<path>` 绑定（可带参数 JSON Schema）
 *   5. 注册文件工具并启动 FTP 端点
 *   6. 启动模型里声明的采样通道
 *   7. 开放 HTTP 接口（OpenAPI 3.0 文档 + /swagger-ui + 配置接口）
 *   8. 运行期间发布事件，退出时按相反顺序释放资源
 *
 * 用法：ncl_device_demo [安装根目录] [运行秒数]
 *       两个参数都可省略（默认 "." 与 5 秒；真实设备应一直运行到 Ctrl+C）。
 *       根目录可以指向一个还不存在的空目录：示例会把 conf/、bin/、log/ 连同
 *       SN、模型、mqtt.cfg 一次备齐，适合"清空目录重跑一遍"：
 *           ncl_device_demo D:\sim4 60
 */
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

/* ------------------------------------------------------------------ 模型 -- */

/*
 * 默认模型：一台数控机床（X/Y/Z/C 四轴 + 数控系统），只在**首次启动**时写到
 * <root>/conf/model/nclink.json。之后以那个文件为准：改文件、或走 REST 的
 * /api/setModel，下次启动就生效，这一串只是"出厂默认值"。
 *
 * 和示例代码直接相关的两处：
 *
 *   - 采样通道 sample_channel0 声明了四个采样项（10 ms 采样 / 2 s 上报）：
 *       010302    机床状态     → /STATUS
 *       010307    加工件数     → /PART_COUNT
 *       01035412  报警号       → /CONTROLLER/WARNING
 *       01035409  主程序号     → /CONTROLLER/PROGRAM
 *     采样项写的是模型里的**节点 id**：设备端按 id 找到节点、拿节点的路径当
 *     表头，再按路径找工具取值，所以下面 kBindings 里要注册这几条路径，
 *     否则对应列取不到值，只能是 null。
 *
 *   - 挂在设备（MACHINE）下的数据项，路径就是 "/<TYPE>"；挂在组件
 *     （CONTROLLER）下的数据项，路径带组件名。所以同样是"报警"，挂在设备下
 *     是 "/WARNING"，挂在数控系统下才是 "/CONTROLLER/WARNING"。
 */
static const char *kDefaultModelJson =
    "{\"id\":\"01\",\"type\":\"NC_LINK_ROOT\",\"name\":\"机床模型文件\",\"version\":\"1.1.0\",\"dev"
    "ices\":[{\"type\":\"MACHINE\",\"id\":\"0103\",\"name\":\"数控机床\",\"description\":\"数控机床\""
    ",\"version\":\"1.0\",\"configs\":[{\"id\":\"sample_channel0\",\"type\":\"SAMPLE_CHANNEL"
    "\",\"name\":\"采样通道\",\"sampleInterval\":10,\"uploadInterval\":2000,\"ids\":[{\"id\":\"01"
    "0302\"},{\"id\":\"010307\"},{\"id\":\"01035412\"},{\"id\":\"01035409\"}]}],\"dataItems\""
    ":[{\"id\":\"010302\",\"name\":\"机床状态\",\"type\":\"STATUS\"},{\"id\":\"010303\",\"name\":"
    "\"进给速度\",\"type\":\"FEED_SPEED\"},{\"id\":\"010305\",\"name\":\"进给倍率\",\"type\":\"FEED_O"
    "VERRIDE\"},{\"id\":\"010306\",\"name\":\"主轴倍率\",\"type\":\"SPINDLE_OVERRIDE\"},{\"id\":"
    "\"010307\",\"name\":\"加工件数\",\"type\":\"PART_COUNT\"}],\"components\":[{\"type\":\"AXIS"
    "\",\"number\":\"0\",\"id\":\"010350\",\"name\":\"X轴\",\"description\":\"\",\"configs\":["
    "{\"id\":\"01035001\",\"name\":\"轴名\",\"type\":\"NAME\",\"value\":\"X\"},{\"id\":\"010350"
    "02\",\"name\":\"轴号\",\"type\":\"NUMBER\",\"value\":0},{\"id\":\"01035003\",\"name\":\"轴类"
    "型\",\"type\":\"TYPE\",\"value\":\"linear\"}],\"components\":[{\"type\":\"SERVO_DRIVER\","
    "\"id\":\"01035020\",\"name\":\"驱动器\",\"description\":\"\",\"dataItems\":[{\"id\":\"01035"
    "02001\",\"name\":\"指令位置\",\"type\":\"POSITION\"},{\"id\":\"0103502003\",\"name\":\"指令速度"
    "\",\"type\":\"SPEED\"}]},{\"type\":\"MOTOR\",\"id\":\"01035021\",\"name\":\"电机\",\"descr"
    "iption\":\"\",\"dataItems\":[{\"id\":\"0103502101\",\"name\":\"负载电流\",\"type\":\"CURRENT"
    "\"}]},{\"type\":\"SCREW\",\"id\":\"01035022\",\"name\":\"丝杠\",\"description\":\"\",\"dat"
    "aItems\":[{\"id\":\"0103502201\",\"name\":\"实际位置\",\"type\":\"POSITION\"},{\"id\":\"0103"
    "502202\",\"name\":\"实际速度\",\"type\":\"SPEED\"}]}]},{\"type\":\"AXIS\",\"number\":\"1\","
    "\"id\":\"010351\",\"name\":\"Y轴\",\"description\":\"\",\"configs\":[{\"id\":\"01035101\""
    ",\"name\":\"轴名\",\"type\":\"NAME\",\"value\":\"Y\"},{\"id\":\"01035102\",\"name\":\"轴号\""
    ",\"type\":\"NUMBER\",\"value\":1},{\"id\":\"01035103\",\"name\":\"轴类型\",\"type\":\"TYPE"
    "\",\"value\":\"linear\"}],\"components\":[{\"type\":\"SERVO_DRIVER\",\"id\":\"01035120\""
    ",\"name\":\"驱动器\",\"description\":\"\",\"dataItems\":[{\"id\":\"0103512001\",\"name\":\""
    "指令位置\",\"type\":\"POSITION\"},{\"id\":\"0103512003\",\"name\":\"指令速度\",\"type\":\"SPEED"
    "\"}]},{\"type\":\"MOTOR\",\"id\":\"01035121\",\"name\":\"电机\",\"description\":\"\",\"dat"
    "aItems\":[{\"id\":\"0103512101\",\"name\":\"负载电流\",\"type\":\"CURRENT\"}]},{\"type\":\"S"
    "CREW\",\"id\":\"01035122\",\"name\":\"丝杠\",\"description\":\"\",\"dataItems\":[{\"id\":"
    "\"0103512201\",\"name\":\"实际位置\",\"type\":\"POSITION\"},{\"id\":\"0103512202\",\"name\":"
    "\"实际速度\",\"type\":\"SPEED\"}]}]},{\"type\":\"AXIS\",\"number\":\"2\",\"id\":\"010352\","
    "\"name\":\"Z轴\",\"description\":\"\",\"configs\":[{\"id\":\"01035201\",\"name\":\"轴名\","
    "\"type\":\"NAME\",\"value\":\"Z\"},{\"id\":\"01035202\",\"name\":\"轴号\",\"type\":\"NUMBE"
    "R\",\"value\":2},{\"id\":\"01035203\",\"name\":\"轴类型\",\"type\":\"TYPE\",\"value\":\"lin"
    "ear\"}],\"components\":[{\"type\":\"SERVO_DRIVER\",\"id\":\"01035220\",\"name\":\"驱动器\","
    "\"description\":\"\",\"dataItems\":[{\"id\":\"0103522001\",\"name\":\"指令位置\",\"type\":\""
    "POSITION\"},{\"id\":\"0103522003\",\"name\":\"指令速度\",\"type\":\"SPEED\"}]},{\"type\":\"M"
    "OTOR\",\"id\":\"01035221\",\"name\":\"电机\",\"description\":\"\",\"dataItems\":[{\"id\":"
    "\"0103522101\",\"name\":\"负载电流\",\"type\":\"CURRENT\"}]},{\"type\":\"SCREW\",\"id\":\"01"
    "035222\",\"name\":\"丝杠\",\"description\":\"\",\"dataItems\":[{\"id\":\"0103522201\",\"na"
    "me\":\"实际位置\",\"type\":\"POSITION\"},{\"id\":\"0103522202\",\"name\":\"实际速度\",\"type\":"
    "\"SPEED\"}]}]},{\"type\":\"AXIS\",\"number\":\"5\",\"id\":\"010353\",\"name\":\"C轴\",\"d"
    "escription\":\"\",\"configs\":[{\"id\":\"01035301\",\"name\":\"轴名\",\"type\":\"NAME\",\""
    "value\":\"C\"},{\"id\":\"01035302\",\"name\":\"轴号\",\"type\":\"NUMBER\",\"value\":5},{\""
    "id\":\"01035303\",\"name\":\"轴类型\",\"type\":\"TYPE\",\"value\":\"rotary\"}],\"components"
    "\":[{\"type\":\"SERVO_DRIVER\",\"id\":\"01035320\",\"name\":\"C轴驱动器\",\"description\":\""
    "\",\"dataItems\":[{\"id\":\"0103532001\",\"name\":\"指令位置\",\"type\":\"POSITION\"},{\"id"
    "\":\"0103532002\",\"name\":\"指令速度\",\"type\":\"SPEED\"}]},{\"type\":\"MOTOR\",\"id\":\"0"
    "1035321\",\"name\":\"C轴电机\",\"description\":\"\",\"dataItems\":[{\"id\":\"0103532101\","
    "\"name\":\"实际位置\",\"type\":\"POSITION\"},{\"id\":\"0103532102\",\"name\":\"实际速度\",\"type"
    "\":\"SPEED\"},{\"id\":\"0103532103\",\"name\":\"负载电流\",\"type\":\"CURRENT\"}]}]},{\"type"
    "\":\"CONTROLLER\",\"id\":\"010354\",\"name\":\"数控系统\",\"description\":\"\",\"configs\":["
    "{\"id\":\"01035404\",\"type\":\"TOOL_PARAM\",\"name\":\"刀具参数\",\"dataType\":\"LIST\",\"s"
    "ettable\":true},{\"id\":\"01035405\",\"type\":\"COORDINATE\",\"name\":\"坐标系\",\"dataType"
    "\":\"LIST\",\"settable\":true},{\"id\":\"01035406\",\"type\":\"CONSOLE\",\"name\":\"指令\""
    ",\"settable\":true},{\"id\":\"01035407\",\"type\":\"PARAMETER\",\"name\":\"参数\",\"dataTy"
    "pe\":\"LIST\",\"settable\":true},{\"id\":\"01035408\",\"type\":\"FILE\",\"name\":\"G代码文件"
    "\",\"dataType\":\"HASH\",\"settable\":true}],\"dataItems\":[{\"id\":\"01035409\",\"type"
    "\":\"PROGRAM\",\"name\":\"主程序名\"},{\"id\":\"01035410\",\"type\":\"SUBPROGRAM\",\"name\":"
    "\"子程序名\"},{\"id\":\"01035411\",\"type\":\"LINE_NUMBER\",\"name\":\"指令行号\"},{\"id\":\"010"
    "35412\",\"type\":\"WARNING\",\"name\":\"报警\"},{\"id\":\"01035413\",\"type\":\"TOOL_NUMBE"
    "R\",\"name\":\"刀具号\"},{\"id\":\"01035414\",\"type\":\"PROGRAM_NUMBER\",\"name\":\"程序号\"}"
    ",{\"id\":\"01035415\",\"type\":\"VARIABLE\",\"number\":\"PROGID_MAP\",\"name\":\"程序ID映射表"
    "\"},{\"id\":\"01035420\",\"type\":\"VARIABLE\",\"number\":\"EVENT\",\"name\":\"事件\"},{\""
    "id\":\"01035430\",\"type\":\"VARIABLE\",\"number\":\"REG_X\",\"name\":\"寄存器X\",\"dataTyp"
    "e\":\"LIST\",\"settable\":true},{\"id\":\"01035431\",\"type\":\"VARIABLE\",\"number\":\""
    "REG_Y\",\"name\":\"寄存器Y\",\"dataType\":\"LIST\",\"settable\":true},{\"id\":\"01035432\","
    "\"type\":\"VARIABLE\",\"number\":\"REG_F\",\"name\":\"寄存器F\",\"dataType\":\"LIST\",\"set"
    "table\":true},{\"id\":\"01035433\",\"type\":\"VARIABLE\",\"number\":\"REG_G\",\"name\":"
    "\"寄存器G\",\"dataType\":\"LIST\",\"settable\":true},{\"id\":\"01035434\",\"type\":\"VARIAB"
    "LE\",\"number\":\"REG_R\",\"name\":\"寄存器R\",\"dataType\":\"LIST\",\"settable\":true},{\""
    "id\":\"01035435\",\"type\":\"VARIABLE\",\"number\":\"REG_W\",\"name\":\"寄存器W\",\"dataTyp"
    "e\":\"LIST\",\"settable\":true},{\"id\":\"01035436\",\"type\":\"VARIABLE\",\"number\":\""
    "REG_D\",\"name\":\"寄存器D\",\"dataType\":\"LIST\",\"settable\":true},{\"id\":\"01035437\","
    "\"type\":\"VARIABLE\",\"number\":\"REG_B\",\"name\":\"寄存器B\",\"dataType\":\"LIST\",\"set"
    "table\":true},{\"id\":\"01035438\",\"type\":\"VARIABLE\",\"number\":\"REG_P\",\"name\":"
    "\"寄存器P\",\"dataType\":\"LIST\",\"settable\":true},{\"id\":\"01035439\",\"type\":\"VARIAB"
    "LE\",\"number\":\"REG_I\",\"name\":\"寄存器I\",\"dataType\":\"LIST\",\"settable\":true},{\""
    "id\":\"01035440\",\"type\":\"VARIABLE\",\"number\":\"REG_Q\",\"name\":\"寄存器Q\",\"dataTyp"
    "e\":\"LIST\",\"settable\":true},{\"id\":\"01035441\",\"type\":\"VARIABLE\",\"number\":\""
    "REG_K\",\"name\":\"寄存器K\",\"dataType\":\"LIST\",\"settable\":true},{\"id\":\"01035442\","
    "\"type\":\"VARIABLE\",\"number\":\"REG_T\",\"name\":\"寄存器T\",\"dataType\":\"LIST\",\"set"
    "table\":true},{\"id\":\"01035443\",\"type\":\"VARIABLE\",\"number\":\"REG_C\",\"name\":"
    "\"寄存器C\",\"dataType\":\"LIST\",\"settable\":true},{\"id\":\"01035450\",\"type\":\"VARIAB"
    "LE\",\"number\":\"CHAN_0\",\"name\":\"通道0数据\",\"dataType\":\"LIST\",\"settable\":true},{"
    "\"id\":\"01035455\",\"type\":\"VARIABLE\",\"number\":\"AXIS_0\",\"name\":\"轴0数据\",\"data"
    "Type\":\"LIST\"},{\"id\":\"01035456\",\"type\":\"VARIABLE\",\"number\":\"AXIS_1\",\"name"
    "\":\"轴1数据\",\"dataType\":\"LIST\"},{\"id\":\"01035457\",\"type\":\"VARIABLE\",\"number\""
    ":\"AXIS_2\",\"name\":\"轴2数据\",\"dataType\":\"LIST\"},{\"id\":\"01035460\",\"type\":\"VAR"
    "IABLE\",\"number\":\"AXIS_5\",\"name\":\"轴5数据\",\"dataType\":\"LIST\"},{\"id\":\"0103547"
    "0\",\"type\":\"VARIABLE\",\"number\":\"SYS\",\"name\":\"系统数据\",\"dataType\":\"LIST\"},{"
    "\"id\":\"01035471\",\"type\":\"VARIABLE\",\"number\":\"MACRO\",\"name\":\"宏变量\",\"dataTy"
    "pe\":\"LIST\",\"settable\":true},{\"id\":\"01035472\",\"type\":\"VARIABLE\",\"number\":"
    "\"VAR_AXIS\",\"name\":\"轴变量\",\"dataType\":\"LIST\",\"settable\":true},{\"id\":\"0103547"
    "8\",\"type\":\"VARIABLE\",\"number\":\"VAR_CHAN_0\",\"name\":\"通道变量\",\"dataType\":\"LIS"
    "T\",\"settable\":true},{\"id\":\"01035482\",\"type\":\"VARIABLE\",\"number\":\"VAR_SYS\""
    ",\"name\":\"系统变量\",\"dataType\":\"LIST\",\"settable\":true},{\"id\":\"01035483\",\"type"
    "\":\"VARIABLE\",\"number\":\"VAR_SYSF\",\"name\":\"浮点系统变量\",\"dataType\":\"LIST\",\"sett"
    "able\":true}]}]}]}";

/* ------------------------------------------------------------------ 工具 -- */

/** 工具实例：真实设备这里放句柄（串口、PLC 连接等）。 */
typedef struct {
    int status;      /**< /STATUS 机床状态（示例用 0 空闲、1 运行、2 报警） */
    int part_count;  /**< /PART_COUNT 加工件数 */
    int warning;     /**< /CONTROLLER/WARNING 报警号，0 表示无报警 */
    int program;     /**< /CONTROLLER/PROGRAM 当前主程序号 */
} demo_device;

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
 * 参数 JSON Schema：只在收到 check=true 的 MethodCall 时使用，
 * 用于在不执行任何动作的前提下校验参数。
 */
#define SET_STATUS_SCHEMA                                                      \
    "{\"type\":\"object\",\"properties\":{"                                    \
    "\"value\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":65535}},"       \
    "\"required\":[\"value\"]}"

static const ncl_tool_method kMethods[] = {
    {"getValue", tool_get_status, NULL},
    {"setValue", tool_set_status, SET_STATUS_SCHEMA},
    {"getCount", tool_get_part_count, NULL},
    {"getWarning", tool_get_warning, NULL},
    {"getProgram", tool_get_program, NULL}};

/*
 * 绑定：<方法>#<模型路径>。路径要和默认模型里数据项的路径一致（设备端收到
 * 的请求项是节点 id，服务端先按 id 找到节点、再取它的路径来匹配绑定）。
 * 模型里 sample_channel0 的四个采样项就是按这些路径取值的。
 */
static const ncl_tool_binding kBindings[] = {
    {"/STATUS", NCL_OP_GET_VALUE, "getValue", "plc"},
    {"/STATUS", NCL_OP_SET_VALUE, "setValue", "plc"},
    {"/PART_COUNT", NCL_OP_GET_VALUE, "getCount", "plc"},
    {"/CONTROLLER/WARNING", NCL_OP_GET_VALUE, "getWarning", "plc"},
    {"/CONTROLLER/PROGRAM", NCL_OP_GET_VALUE, "getProgram", "plc"}};

/* ------------------------------------------------------------- MQTT 回调 -- */

typedef struct {
    ncl_server *server;
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

/** 随机生成一个 9 位 SN：9 个十进制数字。 */
static char *demo_generate_sn(void)
{
    unsigned char raw[9];
    char *sn = (char *)malloc(sizeof(raw) + 1);
    size_t i;

    if (sn == NULL || !ncl_random_bytes(raw, sizeof(raw))) {
        free(sn);
        return NULL;
    }
    for (i = 0; i < sizeof(raw); i++) {
        sn[i] = (char)('0' + (raw[i] % 10));
    }
    sn[sizeof(raw)] = '\0';
    return sn;
}

/**
 * 首次启动自举：安装根目录里缺什么补什么，已经存在的文件一律不动。
 *
 *   bin/sn.txt              设备 SN：随机生成的 9 位数字，之后一直沿用
 *   conf/model/nclink.json  设备模型：默认模型（kDefaultModelJson）
 *   conf/mqtt.cfg           本机 broker：tcp://127.0.0.1:1883，匿名登录
 *
 * 这三个文件就是设备身份与配置的唯一出处，所以"删掉根目录重跑"和"换一台设备"
 * 是一回事。
 *
 * 为什么这里自己写而不是全交给库：ncl_sn_read() 生成的是 "V2 + 9 位十六进制"，
 * 现场 SN 习惯用 9 位数字，所以先把 SN 生成好落盘，再让 ncl_sn_read() 去读
 * （它只在文件不存在时才自己生成）。
 */
static ncl_err demo_bootstrap(void)
{
    ncl_err rc;
    char *dir = NULL;

    ncl_mkdir_p(ncl_env_run_path());
    ncl_mkdir_p(ncl_env_conf_path());

    /* 1) SN */
    if (!ncl_path_exists(ncl_env_sn_file())) {
        char *fresh = demo_generate_sn();

        if (fresh == NULL) {
            return NCL_ERR_NOMEM;
        }
        rc = ncl_file_write_all(ncl_env_sn_file(), fresh, strlen(fresh));
        if (rc == NCL_OK) {
            ncl_log_info("首次启动：生成 SN %s（%s）", fresh, ncl_env_sn_file());
        }
        free(fresh);
        if (rc != NCL_OK) {
            return rc;
        }
    }

    /* 2) 模型：conf/model/ 目录得自己建（ncl_file_write_all 不建父目录）。 */
    if (!ncl_path_exists(ncl_env_model_file())) {
        if (ncl_asprintf(&dir, "%s%cmodel", ncl_env_conf_path(), NCL_PATH_SEP) ==
            NCL_OK) {
            ncl_mkdir_p(dir);
            free(dir);
        }
        rc = ncl_file_write_all(ncl_env_model_file(), kDefaultModelJson,
                                strlen(kDefaultModelJson));
        if (rc != NCL_OK) {
            return rc;
        }
        ncl_log_info("首次启动：写入默认模型（%s）", ncl_env_model_file());
    }

    /* 3) mqtt.cfg：本机 broker、匿名登录（用户名/密码留空）。 */
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
                free(path);
            }
        }
    }
}

/* ------------------------------------------------------------------ main -- */

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : ".";
    int seconds = argc > 2 ? atoi(argv[2]) : 5;
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
    int i;

    memset(&device, 0, sizeof(device));
    memset(&link, 0, sizeof(link));

    /* 1. 安装根目录：conf/、bin/、log/、uploadFile/ 等都在它下面。目录不存在
     *    就先建好，于是"指着一个空目录启动"也是合法的首次启动。 */
    ncl_mkdir_p(root);
    ncl_env_set_root(root);
    ncl_log_init(NULL);
    ncl_log_info("NC-Link 设备端启动，根目录 %s", ncl_env_root());

    /* 2. 首次启动自举：SN、模型、mqtt.cfg 缺什么补什么。
     *
     *    注意这里不要用 ncl_config_init(NULL, &sn)：那是 REST 的 /api/cfg/init，
     *    它会**无条件覆盖** bin/sn.txt，每调一次设备身份就变一次。 */
    if (demo_bootstrap() != NCL_OK) {
        ncl_log_error("初始化安装根目录失败: %s", ncl_env_root());
        exit_code = 1;
        goto cleanup;
    }

    /* 3. 设备身份：bin/sn.txt（首次启动已生成，之后一直沿用）。 */
    sn = ncl_sn_read();
    if (sn == NULL) {
        ncl_log_error("无法取得设备 SN: %s", ncl_env_sn_file());
        exit_code = 1;
        goto cleanup;
    }
    ncl_log_info("设备 SN: %s", sn);

    /* 4. broker：参数来自 conf/mqtt.cfg，连接（设备端用 SN 做 clientId）。 */
    if (demo_mqtt_config(&broker_url, &broker_user, &broker_password) != NCL_OK) {
        ncl_log_error("读取 mqtt.cfg 失败: %s", ncl_env_mqtt_cfg_file());
        exit_code = 1;
        goto cleanup;
    }
    ncl_log_info("MQTT 服务器: %s，用户名 %s", broker_url,
                 broker_user[0] != '\0' ? broker_user : "空（匿名连接）");
    ncl_mqtt_client_options_default(&mqtt_options);
    mqtt_options.url = broker_url;
    mqtt_options.client_id = sn;              /* 设备端用 SN 做 clientId */
    /* 空用户名/密码 = 匿名：传 NULL 才不会往连接报文里塞空字段。 */
    mqtt_options.username = broker_user[0] != '\0' ? broker_user : NULL;
    mqtt_options.password = broker_password[0] != '\0' ? broker_password : NULL;
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

    /* 5. 模型：conf/model/nclink.json 是唯一出处（首次启动写的是默认模型）。 */
    if (ncl_file_read_all(ncl_env_model_file(), &model_json, NULL) != NCL_OK) {
        ncl_log_error("读取模型失败: %s", ncl_env_model_file());
        exit_code = 1;
        goto cleanup;
    }

    /* 6. 服务端：装载模型、注册工具、订阅请求主题。 */
    memset(&server_options, 0, sizeof(server_options));
    server_options.sn = sn;
    server_options.mqtt = mqtt;
    server_options.model_json = model_json;
    server = ncl_server_create(&server_options);
    if (server == NULL) {
        ncl_log_error("服务端创建失败（模型是否合法？）");
        exit_code = 1;
        goto cleanup;
    }
    link.server = server;
    log_sample_channels(ncl_server_model(server));
    ncl_server_register_tool(server, "plc", &device, kMethods,
                             sizeof(kMethods) / sizeof(kMethods[0]), kBindings,
                             sizeof(kBindings) / sizeof(kBindings[0]));
    ncl_server_register_builtin_tool(server);   /* /nclinkServer/addSample 等 */
    ncl_server_register_file_tool(server);      /* /CONTROLLER/FILE */
    ncl_server_subscribe(server);

    /* 7. FTP 端点：端口与账号来自 bin/ftp.txt（缺省 admin/123456/2121）。 */
    ncl_server_start_ftp(server);

    /* 8. 采样：启动模型里声明的 SAMPLE_CHANNEL。 */
    ncl_server_init_samples(server);

    /* 9. HTTP：OpenAPI 3.0 文档、Swagger UI 与配置接口。 */
    http = ncl_http_server_create(9008);
    if (http != NULL && ncl_http_server_start(http) == NCL_OK) {
        ncl_rest_attach(http, server);
        ncl_rest_attach_config(http);
        ncl_log_info("HTTP 接口: http://localhost:9008/swagger-ui");
    }

    /* 10. 运行：真实设备这里一直循环，示例只在指定秒数内跑一会儿。 */
    device.status = 1;      /* 运行中 */
    device.program = 1001;  /* 当前主程序号；下面每 2 秒换一个 */
    ncl_log_info("运行 %d 秒（Ctrl+C 可随时退出）", seconds);
    for (i = 0; i < seconds * 10; i++) {
        ncl_sleep_millis(100);
        device.part_count++;                   /* 模拟产量累加 */
        if (i % 20 == 19) {
            device.program++;                  /* 模拟换程序 */
        }

        /* 每秒发布一条事件到 Event/<sn>：time 与 @id 会自动补齐。 */
        if (i % 10 == 9) {
            ncl_json *event = ncl_json_new_object();
            ncl_json_obj_set_string(event, "key", "PART_COUNT");
            ncl_json_obj_set_int(event, "value", device.part_count);
            ncl_json_obj_set_int(event, "oldValue", device.part_count - 1);
            ncl_server_push_event(server, "010307", event);   /* /PART_COUNT */
            ncl_json_free(event);
        }
    }

    ncl_log_info("采样上报次数: %u", (unsigned)ncl_server_sample_upload_count(server));
    ncl_log_info("已发布事件数: %u", (unsigned)ncl_server_event_count(server));

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
    free(broker_url);
    free(broker_user);
    free(broker_password);
    free(model_json);
    ncl_log_info("设备端已退出");
    ncl_log_shutdown();
    ncl_env_shutdown();
    free(sn);
    return exit_code;
}
