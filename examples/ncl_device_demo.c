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

/* ------------------------------------------------------------------ 模型 -- */

/*
 * 默认模型：一台数控机床（X/Y/Z/C 四轴 + 主轴 S + 数控系统），只在**首次启动**
 * 时写到 <root>/conf/model/nclink.json。之后以那个文件为准：改文件、或走 REST
 * 的 /api/setModel，下次启动就生效，这一串只是"出厂默认值"。
 *
 * 主轴单独立成一个 S 轴（number="S"、轴类型 rotary），不拿 C 轴顶替：主轴不一定
 * 就是 C 轴，借用会产生歧义。
 *
 * 模型里两个采样通道：
 *
 *   - 通道 0：sample_channel0（1 s 采样 / 1 s 上报）——机床的运行状态，八项：
 *       010307    加工计件         → /PART_COUNT
 *       010305    进给倍率         → /FEED_OVERRIDE
 *       01035409  当前加工程序名   → /CONTROLLER/PROGRAM
 *       01035413  当前刀号         → /CONTROLLER/TOOL_NUMBER
 *       01035506  主轴转速         → /AXIS@S/SPEED
 *       010302    设备状态         → /STATUS
 *       010309    加工模式         → /MACHINING_MODE
 *       01035412  报警号           → /CONTROLLER/WARNING
 *     采样项写的是模型里的**节点 id**：设备端按 id 找到节点、拿节点的路径当
 *     表头，再按路径找工具取值，所以下面 kBindings 里要注册这几条路径，
 *     否则对应列取不到值，只能是 null。
 *
 *   - 通道 1：EdgeSersors（1 ms 采样 / 100 ms 上报）——功率与振动（= 加速度）挂在
 *     AXIS 组件下，路径形如 /AXIS@<轴号>/<类型>@<传感器号>：
 *       01035004  X轴功率    → /AXIS@X/POWER@1         01035005  X轴加速度 → /AXIS@X/ACCELERATION@1
 *       01035104  Y轴功率    → /AXIS@Y/POWER@1         01035105  Y轴加速度 → /AXIS@Y/ACCELERATION@1
 *       01035204  Z轴功率    → /AXIS@Z/POWER@1         01035205  Z轴加速度 → /AXIS@Z/ACCELERATION@1
 *       01035304  C轴功率    → /AXIS@C/POWER@1         01035305  C轴加速度 → /AXIS@C/ACCELERATION@1
 *       01035504  主轴功率1  → /AXIS@S/POWER@1         01035505  主轴加速度1 → /AXIS@S/ACCELERATION@1
 *       01035507  主轴功率2  → /AXIS@S/POWER@2         01035508  主轴加速度2 → /AXIS@S/ACCELERATION@2
 *     十二项都在通道 1 里：主轴上挂了两路传感器，用数据项的 number 区分（`POWER@1`、
 *     `POWER@2`）—— 一个部件多个传感器就这么写：同 type、不同 number、不同 id，
 *     路径带上 `@<number>`，工具绑定与采样表头都按这条路径走。
 *     sampleInterval 是通道级的**槽位节奏**，各列自己的采样率靠
 *     "每槽装几个点"体现 —— 功率每槽 1 个点（1 ms 一个），振动每槽 4 个点（=
 *     0.25 ms 一个，4 kHz）。也就是 MANUAL 4.5 的亚毫秒采样：取值一次返回一批值，
 *     一列就是"槽位数组套批次数组"。消费端用 ncl_sample_item_value_at() 摊平着读，
 *     不需要关心哪列是批量、哪列是标量。
 *     采样率还能更慢（某列每槽少装点），但 0.25 ms 这种比槽位还细的周期没法写进
 *     sampleInterval（只能整数毫秒），所以细的一侧一律走"每槽多装几点"。
 *     同一条路径一套绑定（"路径 → 方法"是一对一的），所以下面给这十二条路径各配了
 *     一个取值方法，方法体是同一份样板，用宏生成。
 *
 *   - 挂在设备（MACHINE）下的数据项，路径就是 "/<TYPE>"；挂在组件
 *     （CONTROLLER）下的数据项，路径带组件名。所以同样是"报警"，挂在设备下
 *     是 "/WARNING"，挂在数控系统下才是 "/CONTROLLER/WARNING"。
 */
static const char *kDefaultModelJson =
    "{\"id\":\"01\",\"type\":\"NC_LINK_ROOT\",\"name\":\"机床模型文件\",\"version\":\"1.1.0\",\"dev"
    "ices\":[{\"type\":\"MACHINE\",\"id\":\"0103\",\"name\":\"数控机床\",\"description\":\"数控机床\""
    ",\"version\":\"1.0\",\"configs\":[{\"id\":\"sample_channel0\",\"type\":\"SAMPLE_CHANNEL"
    "\",\"name\":\"机床运行状态\",\"sampleInterval\":1000,\"uploadInterval\":1000,"
    "\"ids\":[{\"id\":\"010307\"},{\"id\":\"010305\"},{\"id\":\"01035409\"},{\"id\":\"01035413\"}"
    ",{\"id\":\"01035506\"},{\"id\":\"010302\"},{\"id\":\"010309\"},{\"id\":\"01035412\"}]}"
    ",{\"id\":\"Edge"

    "Sersors\",\"type\":\"SAMPLE_CHANNEL\",\"name\":\"边缘传感器\",\"sampleInterval\":1,"
    "\"uploadInterval\":100,\"ids\":[{\"id\":\"01035004\"},{\"id\":\"01035005\"},{\"id\":\""
    "01035104\"},{\"id\":\"01035105\"},{\"id\":\"01035204\"},{\"id\":\"01035205\"},{\"id\":"
    "\"01035304\"},{\"id\":\"01035305\"},{\"id\":\"01035504\"},{\"id\":\"01035505\"},{\"id\""
    ":\"01035507\"},{\"id\":\"01035508\"}]}]"
    ",\"dataItems\""
    ":[{\"id\":\"010302\",\"name\":\"设备状态\",\"type\":\"STATUS\"},{\"id\":\"010303\",\"name\":"
    "\"进给速度\",\"type\":\"FEED_SPEED\"},{\"id\":\"010305\",\"name\":\"进给倍率\",\"type\":\"FEED_O"
    "VERRIDE\"},{\"id\":\"010306\",\"name\":\"主轴倍率\",\"type\":\"SPINDLE_OVERRIDE\"},{\"id\":"
    "\"010307\",\"name\":\"加工计件\",\"type\":\"PART_COUNT\"},{\"id\":\"010309\",\"name\":\"加工"
    "模式\",\"type\":\"MACHINING_MODE\"}],\"components\":[{\"type\":\"AXIS"

    "\",\"number\":\"X\",\"id\":\"010350\",\"name\":\"X轴\",\"description\":\"\",\"configs\":["
    "{\"id\":\"01035001\",\"name\":\"轴名\",\"type\":\"NAME\",\"value\":\"X\"},{\"id\":\"010350"
    "02\",\"name\":\"轴号\",\"type\":\"NUMBER\",\"value\":0},{\"id\":\"01035003\",\"name\":\"轴类"
    "型\",\"type\":\"TYPE\",\"value\":\"linear\"}],\"components\":[{\"type\":\"SERVO_DRIVER\","
    "\"id\":\"01035020\",\"name\":\"驱动器\",\"description\":\"\",\"dataItems\":[{\"id\":\"01035"
    "02001\",\"name\":\"指令位置\",\"type\":\"POSITION\"},{\"id\":\"0103502003\",\"name\":\"指令速度"
    "\",\"type\":\"SPEED\"}]},{\"type\":\"MOTOR\",\"id\":\"01035021\",\"name\":\"电机\",\"descr"
    "iption\":\"\",\"dataItems\":[{\"id\":\"0103502101\",\"name\":\"负载电流\",\"type\":\"CURRENT"
    "\"}]},{\"type\":\"SCREW\",\"id\":\"01035022\",\"name\":\"丝杠\",\"description\":\"\",\"dat"
    "aItems\":[{\"id\":\"0103502201\",\"name\":\"实际位置\",\"type\":\"POSITION\"},{\"id\":\"0103"
    "502202\",\"name\":\"实际速度\",\"type\":\"SPEED\"}]}]"
    ",\"dataItems\":[{\"id\":\"01035004\",\"name\":\"功率\",\"type\":\"POWER\",\"number\":\"1\""
    "},{\"id\":\"01035005\",\"name\":\"加速度\",\"type\":\"ACCELERATION\",\"number\":\"1\"}]"
    "},{\"type\":\"AXIS\",\"number\":\"Y\","
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
    "\"实际速度\",\"type\":\"SPEED\"}]"
    "}],\"dataItems\":[{\"id\":\"01035104\",\"name\":\"功率\",\"type\":\"POWER\",\"number\":\"1\""
    "},{\"id\":\"01035105\",\"name\":\"加速度\",\"type\":\"ACCELERATION\",\"number\":\"1\""
    "}]},{\"type\":\"AXIS\",\"number\":\"Z\",\"id\":\"010352\","
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
    "\"SPEED\"}]}]"
    ",\"dataItems\":[{\"id\":\"01035204\",\"name\":\"功率\",\"type\":\"POWER\",\"number\":\"1\""
    "},{\"id\":\"01035205\",\"name\":\"加速度\",\"type\":\"ACCELERATION\",\"number\":\"1\"}]"
    "},{\"type\":\"AXIS\",\"number\":\"C\",\"id\":\"010353\",\"name\":\"C轴\",\"d"
    "escription\":\"\",\"configs\":[{\"id\":\"01035301\",\"name\":\"轴名\",\"type\":\"NAME\",\""
    "value\":\"C\"},{\"id\":\"01035302\",\"name\":\"轴号\",\"type\":\"NUMBER\",\"value\":5},{\""
    "id\":\"01035303\",\"name\":\"轴类型\",\"type\":\"TYPE\",\"value\":\"rotary\"}],\"components"
    "\":[{\"type\":\"SERVO_DRIVER\",\"id\":\"01035320\",\"name\":\"C轴驱动器\",\"description\":\""
    "\",\"dataItems\":[{\"id\":\"0103532001\",\"name\":\"指令位置\",\"type\":\"POSITION\"},{\"id"
    "\":\"0103532002\",\"name\":\"指令速度\",\"type\":\"SPEED\"}]},{\"type\":\"MOTOR\",\"id\":\"0"
    "1035321\",\"name\":\"C轴电机\",\"description\":\"\",\"dataItems\":[{\"id\":\"0103532101\","
    "\"name\":\"实际位置\",\"type\":\"POSITION\"},{\"id\":\"0103532102\",\"name\":\"实际速度\",\"type"
    "\":\"SPEED\"},{\"id\":\"0103532103\",\"name\":\"负载电流\",\"type\":\"CURRENT\"}]"
    "}],\"dataItems\":[{\"id\":\"01035304\",\"name\":\"功率\",\"type\":\"POWER\",\"number\":\"1\""
    "},{\"id\":\"01035305\",\"name\":\"加速度\",\"type\":\"ACCELERATION\",\"number\":\"1\""
    "}]},{\"type"
    "\":\""
    "AXIS\",\"number\":\"S\",\"id\":\"010355\",\"name\":\"主轴\",\"description\":\"\",\"configs\":[{\"id\":\"0103"
    "5501\",\"name\":\"轴名\",\"type\":\"NAME\",\"value\":\"S\"},{\"id\":\"01035502\",\"name\":\"轴号\",\"type\":\"NU"
    "MBER\",\"value\":6},{\"id\":\"01035503\",\"name\":\"轴类型\",\"type\":\"TYPE\",\"value\":\"rotary\"}],\"dat"
    "aItems\":[{\"id\":\"01035504\",\"name\":\"功率\",\"type\":\"POWER\",\"number\":\"1\"},"
    "{\"id\":\"01035505\",\"name\":\"加速度\",\"type\":\"ACCELERATION\",\"number\":\"1\"},"
    "{\"id\":\"01035506\",\"name\":\"转速\",\"type\":\"SPEED\"},"
    "{\"id\":\"01035507\",\"name\":\"功率\",\"type\":\"POWER\",\"number\":\"2\"},"
    "{\"id\":\"01035508\",\"name\":\"加速度\",\"type\":\"ACCELERATION\",\"number\":\"2\"}],"
    "\"components\":["
    "{\"type\":\"SERVO_DRIVER\",\"id\":\"01035520\",\"name\":"
    "\"主轴驱动器\",\"description\":\"\",\"dataItems\":[{\"id\":\"0103552001\",\"name\":\"指令位置\",\"type\":\"POSIT"
    "ION\"},{\"id\":\"0103552002\",\"name\":\"指令速度\",\"type\":\"SPEED\"}]},{\"type\":\"MOTOR\",\"id\":\"01035"
    "521\",\"name\":\"主轴电机\",\"description\":\"\",\"dataItems\":[{\"id\":\"0103552101\",\"name\":\"实际位置\",\"t"
    "ype\":\"POSITION\"},{\"id\":\"0103552102\",\"name\":\"实际速度\",\"type\":\"SPEED\"},{\"id\":\"0103552103\""
    ",\"name\":\"负载电流\",\"type\":\"CURRENT\"}]}]},{\"type\":\""
    "CONTROLLER\",\"id\":\"010354\",\"name\":\"数控系统\",\"description\":\"\",\"configs\":["
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
#define DEMO_SENSOR_MAX 2

static const int demo_axis_sensors[DEMO_AXIS_COUNT] = {1, 1, 1, 1, 2};

static long long demo_power_tick[DEMO_AXIS_COUNT][DEMO_SENSOR_MAX];      /* 功率：一格一查 */
static long long demo_vibration_tick[DEMO_AXIS_COUNT][DEMO_SENSOR_MAX];  /* 振动：一次 4 格 */

/** 这一路传感器在整机里的序号：让每路的基值互不相同（X..C 各占 1，主轴占 2）。 */
static int demo_sensor_slot(int axis, int sensor)
{
    static const int first_slot[DEMO_AXIS_COUNT] = {0, 1, 2, 3, 4};

    return first_slot[axis] + sensor;
}

/** 一路传感器的功率（W）：每路一个基值，再叠 0~300 W 的缓升，25 格一个锯齿。 */
static double demo_axis_power(int axis, int sensor)
{
    int slot = demo_sensor_slot(axis, sensor);
    long long tick = demo_power_tick[axis][sensor]++;

    return 800.0 + (double)slot * 250.0 +
           (double)((tick + slot * 7) % 25) * 12.5;
}

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
 * 轴的功率与振动：服务端按"路径 → 方法"取值，而方法签名里拿不到路径，
 * 所以每条路径要一套绑定、一个方法（主轴两路传感器就是两条路径、两个方法）。
 * 方法体是同一份样板，用宏生成。
 *
 * 振动一次查询给 4 个值：通道周期最小只能写整数毫秒（1 ms），1 ms 里给 4 个就
 * 等效 0.25 ms 一个（4 kHz），就是 MANUAL 4.5 的"亚毫秒采样"：一列允许是数组，
 * 库里的 ncl_sample_item_value_count()/value_at() 会把它摊平，客户端照样按一列看。
 */
#define DEMO_VIBRATION_PER_QUERY 4

/** 振动的一个子采样：0.25 ms 一格的三角波，值都是 0.125 的整数倍。 */
static double demo_axis_vibration_step(int axis, int sensor, long long step)
{
    int slot = demo_sensor_slot(axis, sensor);

    return (double)(((step + (long long)slot * 3) % 16) - 8) * 0.125;
}

/** 一次查询取一路传感器的振动：这一轮里的 4 个 0.25 ms 子采样，打包成数组。 */
static ncl_json *demo_axis_vibration_block(int axis, int sensor)
{
    long long step = demo_vibration_tick[axis][sensor];
    ncl_json *block = ncl_json_new_array();
    int k;

    if (block == NULL) {
        return NULL;
    }
    demo_vibration_tick[axis][sensor] += DEMO_VIBRATION_PER_QUERY;
    for (k = 0; k < DEMO_VIBRATION_PER_QUERY; k++) {
        double value = demo_axis_vibration_step(axis, sensor, step + k);

        if (ncl_json_arr_push(block, ncl_json_new_double(value)) != NCL_OK) {
            ncl_json_free(block);
            return NULL;
        }
    }
    return block;
}

/* 方法名带轴与传感器号（与路径 /AXIS@<轴>/<类型>@<传感器号> 一一对应）：
 * sensor 是 0 起的下标，名字里用的是模型 number（= sensor + 1）。 */
#define DEMO_DEFINE_POWER_GETTER(fn_name, axis_slot, sensor_idx)               \
    static ncl_err fn_name(void *instance, const ncl_json *params,             \
                           ncl_json **result, char **reason)                   \
    {                                                                          \
        (void)instance;                                                        \
        (void)params;                                                          \
        (void)reason;                                                          \
        *result = ncl_json_new_double(demo_axis_power(axis_slot, sensor_idx)); \
        return NCL_OK;                                                         \
    }

#define DEMO_DEFINE_VIBRATION_GETTER(fn_name, axis_slot, sensor_idx)           \
    static ncl_err fn_name(void *instance, const ncl_json *params,             \
                           ncl_json **result, char **reason)                   \
    {                                                                          \
        (void)instance;                                                        \
        (void)params;                                                          \
        (void)reason;                                                          \
        *result = demo_axis_vibration_block(axis_slot, sensor_idx);            \
        return *result != NULL ? NCL_OK : NCL_ERR_NOMEM;                       \
    }

DEMO_DEFINE_POWER_GETTER(tool_get_power_x1, DEMO_AXIS_X, 0)
DEMO_DEFINE_POWER_GETTER(tool_get_power_y1, DEMO_AXIS_Y, 0)
DEMO_DEFINE_POWER_GETTER(tool_get_power_z1, DEMO_AXIS_Z, 0)
DEMO_DEFINE_POWER_GETTER(tool_get_power_c1, DEMO_AXIS_C, 0)
DEMO_DEFINE_POWER_GETTER(tool_get_power_s1, DEMO_AXIS_S, 0)
DEMO_DEFINE_POWER_GETTER(tool_get_power_s2, DEMO_AXIS_S, 1)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_x1, DEMO_AXIS_X, 0)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_y1, DEMO_AXIS_Y, 0)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_z1, DEMO_AXIS_Z, 0)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_c1, DEMO_AXIS_C, 0)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_s1, DEMO_AXIS_S, 0)
DEMO_DEFINE_VIBRATION_GETTER(tool_get_acceleration_s2, DEMO_AXIS_S, 1)

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
    /* 通道 1（EdgeSersors）的十二项：每轴一路功率 + 一路加速度，主轴两路。 */
    {"getPowerX1", tool_get_power_x1, NULL},
    {"getPowerY1", tool_get_power_y1, NULL},
    {"getPowerZ1", tool_get_power_z1, NULL},
    {"getPowerC1", tool_get_power_c1, NULL},
    {"getPowerS1", tool_get_power_s1, NULL},
    {"getPowerS2", tool_get_power_s2, NULL},
    {"getAccelerationX1", tool_get_acceleration_x1, NULL},
    {"getAccelerationY1", tool_get_acceleration_y1, NULL},
    {"getAccelerationZ1", tool_get_acceleration_z1, NULL},
    {"getAccelerationC1", tool_get_acceleration_c1, NULL},
    {"getAccelerationS1", tool_get_acceleration_s1, NULL},
    {"getAccelerationS2", tool_get_acceleration_s2, NULL}};

/*
 * 绑定：<方法>#<模型路径>。路径要和默认模型里数据项的路径一致（设备端收到
 * 的请求项是节点 id，服务端先按 id 找到节点、再取它的路径来匹配绑定）。
 * 模型里 sample_channel0 的八个采样项、EdgeSersors 的十二项，都是按这些路径取值的。
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
    /* 轴上的数据项：通道 0 只取主轴转速，通道 1 取那十二项功率/振动。数据项带
     * number 时路径是 /AXIS@<轴号>/<类型>@<传感器号>；路径怎么写，采样表头就是
     * 什么。 */
    {"/AXIS@S/SPEED", NCL_OP_GET_VALUE, "getSpeedS", "plc"},
    {"/AXIS@X/POWER@1", NCL_OP_GET_VALUE, "getPowerX1", "plc"},
    {"/AXIS@Y/POWER@1", NCL_OP_GET_VALUE, "getPowerY1", "plc"},
    {"/AXIS@Z/POWER@1", NCL_OP_GET_VALUE, "getPowerZ1", "plc"},
    {"/AXIS@C/POWER@1", NCL_OP_GET_VALUE, "getPowerC1", "plc"},
    {"/AXIS@S/POWER@1", NCL_OP_GET_VALUE, "getPowerS1", "plc"},
    {"/AXIS@S/POWER@2", NCL_OP_GET_VALUE, "getPowerS2", "plc"},
    {"/AXIS@X/ACCELERATION@1", NCL_OP_GET_VALUE, "getAccelerationX1", "plc"},
    {"/AXIS@Y/ACCELERATION@1", NCL_OP_GET_VALUE, "getAccelerationY1", "plc"},
    {"/AXIS@Z/ACCELERATION@1", NCL_OP_GET_VALUE, "getAccelerationZ1", "plc"},
    {"/AXIS@C/ACCELERATION@1", NCL_OP_GET_VALUE, "getAccelerationC1", "plc"},
    {"/AXIS@S/ACCELERATION@1", NCL_OP_GET_VALUE, "getAccelerationS1", "plc"},
    {"/AXIS@S/ACCELERATION@2", NCL_OP_GET_VALUE, "getAccelerationS2", "plc"}};

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

/**
 * 首次启动自举：安装根目录里缺什么补什么，已经存在的文件一律不动。
 *
 *   bin/sn.txt              设备 SN：由 ncl_sn_read() 生成（"V2" + 9 位十六进制）
 *   conf/model/nclink.json  设备模型：默认模型（kDefaultModelJson）
 *   conf/mqtt.cfg           本机 broker：tcp://127.0.0.1:1883，匿名登录
 *
 * 这三个文件就是设备身份与配置的唯一出处，所以"删掉根目录重跑"和"换一台设备"
 * 是一回事。
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

    /* 1) 模型：conf/model/ 目录得自己建（ncl_file_write_all 不建父目录）。 */
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
                free(path);
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

int main(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : ".";
    /* 运行秒数可省略：省略或 0 = 一直运行（现场用法），正数 = 跑完自动退出。 */
    long long seconds = argc > 2 ? atoll(argv[2]) : 0;
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

    /* 1. 安装根目录：conf/、bin/、log/、uploadFile/ 等都在它下面。目录不存在
     *    就先建好，于是"指着一个空目录启动"也是合法的首次启动。 */
    ncl_mkdir_p(root);
    ncl_env_set_root(root);
    ncl_log_init(NULL);
    demo_install_signal_handlers();   /* 早装：自举期间 Ctrl+C 也走同一条清理路径 */
    ncl_log_info("NC-Link 设备端启动，根目录 %s", ncl_env_root());

    /* 2. 首次启动自举：模型、mqtt.cfg 缺什么补什么（SN 在下一步按需生成）。
     *
     *    注意这里不要用 ncl_config_init(NULL, &sn)：那是 REST 的 /api/cfg/init，
     *    它会**无条件覆盖** bin/sn.txt，每调一次设备身份就变一次。 */
    if (demo_bootstrap() != NCL_OK) {
        ncl_log_error("初始化安装根目录失败: %s", ncl_env_root());
        exit_code = 1;
        goto cleanup;
    }

    /* 3. 设备身份：bin/sn.txt（缺失时由 ncl_sn_read() 生成并落盘，之后一直沿用）。 */
    if (!ncl_path_exists(ncl_env_sn_file())) {
        ncl_log_info("首次启动：生成 SN（%s）", ncl_env_sn_file());
    }
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
    ncl_log_info("各轴的功率、转速与加速度（路径 含义）:");
    log_axis_quantities(ncl_server_model(server));
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
    device.mode = 2;                /* 加工模式：自动 */
    device.status = 1;              /* 设备状态：运行中 */
    device.program = 1001;          /* 当前加工程序名；下面每 2 秒换一个 */
    device.tool_number = 1;         /* 当前刀号：换程序时跟着换 */
    device.feed_override = 100;     /* 进给倍率 100% */
    device.spindle_speed = 3600;    /* 主轴转速 r/min */
    if (seconds > 0) {
        ncl_log_info("运行 %lld 秒（Ctrl+C 可随时退出）", seconds);
    } else {
        ncl_log_info("一直运行，Ctrl+C 退出");
    }
    /* 主循环仍是 100 ms 一跳：功率/振动与通道 0 的那几项都在取值时按当前状态算，
     * 所以 1 ms 与 1 s 两个采样周期都拿得到新鲜值，这里不必跟着提速。
     * 退出条件：收到 Ctrl+C/SIGTERM，或者（给了秒数时）跑满那么多秒。 */
    for (i = 0; !g_stop && (seconds <= 0 || i < seconds * 10); i++) {
        /* i 是 100 ms 的计数（一直跑时也不会溢出：long long 够几亿年） */
        ncl_sleep_millis(100);
        device.part_count++;                               /* 模拟产量累加 */
        device.feed_override = 60 + (int)(i % 7) * 10;     /* 60% ~ 120% 来回 */
        device.spindle_speed = 3000 + (int)(i % 5) * 300;  /* 3000 ~ 4200 r/min */
        if (i % 20 == 19) {
            device.program++;                   /* 模拟换程序 */
            device.tool_number = 1 + (int)(device.program % 8);   /* 跟着换刀 */
            device.mode = (device.program % 2 == 0) ? 1 : 2;      /* 录入 / 自动 */
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

    if (g_stop) {
        ncl_log_info("收到退出信号（Ctrl+C），开始停止…");
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
