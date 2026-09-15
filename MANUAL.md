# NC-Link C 实现 · 使用手册

> 面向集成者：怎么在自己的设备/上位机工程里用这套库跑通 NC-Link 协议。
> 目录结构、构建、测试清单见 [README.md](README.md)。

## 目录

1. [这是什么](#1-这是什么)
2. [构建与集成](#2-构建与集成)
3. [十分钟上手](#3-十分钟上手)
4. [核心概念](#4-核心概念)
5. [模块手册](#5-模块手册)
6. [典型任务速查](#6-典型任务速查)
7. [常见问题与排错](#7-常见问题与排错)
8. [许可](#8-许可)
9. 附录：[A · API 索引](#附录-a--api-索引) ·
   [B · 错误码](#附录-b--错误码全表) ·
   [C · 主题](#附录-c--主题前缀一览) ·
   [D · 目录布局](#附录-d--安装根目录布局)

---

## 1. 这是什么

`nclink-core-c` 是 NC-Link 中间件（GB/T 41970-2022）的 C11 实现，覆盖协议
3.0.0。它让设备端和上位机能够：

| 能力 | 说明 |
|------|------|
| 数据模型 | 设备/组件/数据项/配置项的树，路径与节点 id 双向可查 |
| 读写数据 | `get_value` / `set_value` / `get_length` / `get_attributes`，支持索引与区间 |
| 方法调用 | `methodCall` 工具分发，支持参数 JSON Schema 校验（`check`） |
| 采样上报 | 按 `sampleInterval` 采集、按 `uploadInterval` 聚合成 `Sample` 报文 |
| 事件推送 | 设备主动发布 `Event/<sn>`，对端订阅并回调 |
| 文件传输 | MQTT 只传 `/temp/<名字>` 令牌，字节走 FTP；含自研 FTP 服务端/客户端 |
| 对外接口 | 内置 HTTP 服务、OpenAPI 3.0 文档、`/swagger-ui`、12 个配置接口 |
| 配置管理 | SN、模型、驱动、服务器列表、`conf/mqtt.cfg` 的读写 |
| 传输 | 自研 MQTT 5.0（QoS 0/1/2、保活、自动重连、订阅恢复）、TCP 套接字 |

**零第三方依赖**：JSON、线程池、TTL 缓存、MQTT、HTTP、FTP、SHA-256、JSON Schema
校验全部自带，只有启用压缩编解码时才需要 zlib。

两个角色，对应两个入口头文件：

| 角色 | 入口 | 典型场景 |
|------|------|----------|
| 设备端 | `nclink/ncl_server.h` | 机床/PLC/机器人等被访问的一方 |
| 上位机 | `nclink/ncl_client.h` | 网关、MES、调试工具等主动访问的一方 |

---

## 2. 构建与集成

### 2.1 目录结构

```
include/nclink/     公共头文件（-I 只需要指向 include）
lib/<平台>/         预编译静态库（发布包：windows-x64-msvc / linux-x86_64-gcc）
examples/           两个可运行示例：设备端 / 客户端
MANUAL.md/.docx     本手册；README/RELEASE/CHANGELOG 见同名文件

src/<模块>/         实现，共 13 个模块目录        ← 以下仅源码仓库有
tests/              19 个测试套件 + 协议黄金样本
tools/              许可头检查、文档生成与发布打包脚本
build.ps1           Windows 一键：配置 + 编译 + ctest
build-linux.sh      Linux 免 cmake 构建
```

> **发布包含头文件、两个平台的静态库、文档与示例程序**；实现源码与测试套件在
> 工程仓库里（需要自行重编时获取，见 2.5）。

### 2.2 Windows（已验证环境）

```powershell
.\build.ps1                 # 配置 + 编译 + 跑全部测试
.\build.ps1 -Clean          # 先清空 build 目录再全量编译
.\build.ps1 -NoTest         # 只编译
```

脚本会自动定位 Visual Studio 2022 Build Tools 自带的 CMake/Ninja 并调用
`vcvars64.bat`，不用先开 VS 命令行。手工构建等价于：

```bat
call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### 2.3 Linux（已验证：gcc 13.4，19/19 测试通过）

源码是 C11，套接字层有 Winsock / BSD 两套实现，两个平台都已在真机编过并跑通
全部测试（Windows 见 2.2，Linux 见下）。

```sh
./build-linux.sh                 # 不需要 cmake：产出 build-linux/libnclink_core.a 并跑全部测试
CC=clang ./build-linux.sh out    # 换编译器 / 换输出目录
```

用 CMake 也行：

```sh
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j
ctest --test-dir build-linux --output-on-failure
```

手工编译（不走上面两个脚本）时注意：

| 要点 | 原因 |
|------|------|
| `-Iinclude -Isrc` | 公共头在 `include/`，极少数内部头在 `src/` 下（如 `file/file_internal.h`） |
| `-D_POSIX_C_SOURCE=200809L` | `-std=c11` 会隐藏 `strdup`、`getaddrinfo`、`localtime_r`、`pthread_*` 等 POSIX 接口 |
| `-lpthread` | 线程、互斥量、条件变量 |
| 可加 `-Wno-format-truncation` | 库内用定长路径缓冲（4096），GCC 对此的保守告警没有意义 |

### 2.4 CMake 选项

| 选项 | 默认 | 作用 |
|------|------|------|
| `NCLINK_BUILD_TESTS` | ON | 编译并注册测试套件 |
| `NCLINK_BUILD_EXAMPLES` | ON | 编译 `examples/` 下的两个示例 |
| `NCLINK_WITH_MQTT` | ON | 编译 MQTT 传输层、客户端、服务端、文件与 FTP |
| `NCLINK_WITH_ZLIB` | OFF | 启用 zlib 压缩编解码 |

### 2.5 集成到自己的工程

最小做法：把 `include/` 与 `src/` 纳入你的构建，或先编出静态库再链接。

```cmake
add_subdirectory(nclink-c)                       # 或自行 add_library(... STATIC)
target_link_libraries(your_app PRIVATE nclink::core)
```

有三件事必须留意：

1. **字符集**：源码与字符串字面量都是 UTF-8（日志与设备描述含中文），
   MSVC 必须加 `/utf-8`。否则在非 UTF-8 代码页上会出现 C4819，甚至中文字符串
   把结尾引号“吞掉”导致 C2001。CMake 工程已对全部目标统一设置。
2. **平台库**：Windows 需要 `ws2_32`（套接字）与 `iphlpapi`（网卡枚举，供
   `ncl_net_ip_map_json()` 使用）。MSVC 下源码自带 `#pragma comment(lib, ...)`，
   MinGW/其它工具链需显式 `-lws2_32 -liphlpapi`；POSIX 需要 `-pthread`。
3. **收尾**：进程退出前可调用一次 `ncl_socket_system_release()` 显式释放网络栈。
   不调用也可以——库默认让网络栈存活到进程结束，以免误伤同进程内其它套接字。

Windows 用 MSVC 时还要注意 ABI 一致：发布包里的 `nclink_core.lib` 是
**x64 + Release + /MD（动态 CRT）**，你的工程必须用同样的运行库设置；
不一致时请用仓库里的源码重新编译（见 2.3）。

---

## 3. 十分钟上手

`examples/` 下有两个**可直接编译运行**的完整程序，本节片段都摘自它们：

```powershell
.\build.ps1
build\examples\ncl_device_demo.exe <安装根目录> <运行秒数>   # 设备端
build\examples\ncl_client_demo.exe <broker> <设备SN> <秒数>  # 客户端
```

### 3.1 设备端最小程序

```c
#include "nclink/ncl_env.h"
#include "nclink/ncl_logger.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_server.h"

/* 工具方法的签名固定：入参是请求的 params 对象，出参是 JSON 值。
 * 返回 NCL_OK 但 *result 为 NULL，会让应答 code=NG。 */
static ncl_err on_get_status(void *instance, const ncl_json *params,
                             ncl_json **result, char **reason) {
    (void)instance; (void)params; (void)reason;
    *result = ncl_json_new_int(42);
    return NCL_OK;
}

static ncl_err on_set_status(void *instance, const ncl_json *params,
                             ncl_json **result, char **reason) {
    long long value = 0;
    if (!ncl_json_as_int(ncl_json_obj_get(params, "value"), &value)) {
        *reason = ncl_strdup("value 必须是整数");
        return NCL_ERR_INVALID_VALUE;
    }
    /* ... 真正写设备 ... */
    *result = ncl_json_new_bool(true);
    return NCL_OK;
}

/* 第三个字段是参数 JSON Schema，可为 NULL；只在 check=true 时使用 */
static const ncl_tool_method methods[] = {
    {"getValue", on_get_status, NULL},
    {"setValue", on_set_status,
     "{\"type\":\"object\",\"properties\":{"
     "\"value\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":65535}},"
     "\"required\":[\"value\"]}"},
};

/* 绑定键是 "<operation>#<path>"，path 取自模型（见 4.3） */
static const ncl_tool_binding bindings[] = {
    {"/STATUS", NCL_OP_GET_VALUE, "getValue", "plc"},
    {"/STATUS", NCL_OP_SET_VALUE, "setValue", "plc"},
};

static ncl_server *g_server;

static void on_mqtt(void *user, const ncl_mqtt_publish *publish) {
    ncl_message *request = ncl_message_parse(publish->topic,
                                             (const char *)publish->payload,
                                             publish->payload_len);
    if (request != NULL) {
        ncl_server_on_message(g_server, publish->topic, request);
        /* 内部会转线程池处理：不要在 MQTT 收包线程上同步发布应答，
         * 否则会等自己还没处理的 PUBREC 而自我死锁。 */
    }
}

int main(void) {
    ncl_mqtt_config config;
    ncl_mqtt_client_options options;
    ncl_mqtt_client *mqtt;
    ncl_server_options server_options;
    char *sn;

    ncl_env_set_root(".");            /* 安装根目录：conf/bin/log 都在它下面 */
    ncl_log_init(NULL);               /* 写 <root>/log/out.txt */
    sn = ncl_sn_read();               /* bin/sn.txt 存在就沿用，没有才生成 */
    ncl_mqtt_config_read(&config);    /* url/username/password */

    ncl_mqtt_client_options_default(&options);
    options.url = config.url;
    options.client_id = sn;           /* 设备端用 SN 做 clientId */
    options.username = config.username;
    options.password = config.password;
    options.automatic_reconnect = true;
    options.on_message = on_mqtt;
    mqtt = ncl_mqtt_client_create(&options);
    ncl_mqtt_client_connect(mqtt);

    memset(&server_options, 0, sizeof(server_options));
    server_options.sn = sn;
    server_options.mqtt = mqtt;             /* 借用，不接管所有权 */
    server_options.model_json = model_json; /* conf/model/nclink.json 的内容 */
    g_server = ncl_server_create(&server_options);
    ncl_server_register_tool(g_server, "plc", device_state, methods, 2,
                             bindings, 2);
    ncl_server_register_builtin_tool(g_server);  /* addSample/removeSample */
    ncl_server_subscribe(g_server);              /* 订 6 个请求主题 */

    /* ... 一直运行 ... */

    ncl_server_free(g_server);
    ncl_mqtt_client_disconnect(mqtt);
    ncl_mqtt_client_destroy(mqtt);
    ncl_mqtt_config_free(&config);
    free(sn);
    return 0;
}
```

### 3.2 客户端最小程序

```c
#include "nclink/ncl_client.h"
#include "nclink/ncl_message.h"

int main(void) {
    ncl_client *client;
    ncl_json *value = NULL;
    ncl_message *probe = NULL;
    long long number = 0;

    /* 一个进程只需初始化一次：内部建立 MQTT 连接，并启动本机 FTP 端点(2323) */
    ncl_client_holder_init("tcp://192.168.1.10:1883", "admin", "123456");

    client = ncl_client_holder_get("V200583BC87");  /* 按 SN 取设备视图 */
    if (client == NULL) {
        return 1;
    }

    /* probe：拿设备模型。模型所有权要显式接管（见 4.7） */
    if (ncl_client_probe(client, 5000, &probe) == NCL_OK && probe != NULL) {
        ncl_client_set_root_node(client, ncl_message_take_model(probe));
        ncl_message_free(probe);
    }

    if (ncl_client_get_value(client, "/STATUS", 5000, &value) == NCL_OK) {
        ncl_json_as_int(value, &number);
        ncl_json_free(value);
    }
    ncl_client_set_value(client, "/STATUS", ncl_json_new_int(7), 5000);

    ncl_client_holder_shutdown();
    return 0;
}
```

### 3.3 示例实测输出

设备端（`ncl_device_demo.exe . 12`）与客户端（`ncl_client_demo.exe
tcp://localhost:1883 V200583BC87 4`）对跑，客户端侧输出：

```
设备模型已装载: /NC_LINK_ROOT，/STATUS 的节点 id = 030001
GET /STATUS = 0
SET /STATUS = 42 成功
check 结果: code=NG reason=[#/value: expected maximum: 65535, found 99999]
文件回传路径: D:\...\build\V200583BC87\demo.txt
  远端文件 demo.txt (14 字节)
收到采样 [Sample/V203003EA15/ch1] 通道=ch1 采样周期=1000ms 上报周期=2000ms 采样项=2
    表头 paths(2 项) = ["/STATUS","/PART_COUNT"]
    原始报文: {"paths":["/STATUS","/PART_COUNT"],"id":"ch1","beginTime":"1789450135470",
              "data":[{"data":[0,0]},{"data":[129,139]}],"interval":1000,"uploadInterval":2000}
    /STATUS          编码=raw 本轮 2 个值: [0, 0]
    /PART_COUNT      编码=raw 本轮 2 个值: [129, 139]
收到事件 [Event/V203003EA15] id=030002 key=PART_COUNT value=120
...
共收到 10 条事件、5 条采样上报
```

这六步分别验证了：模型交换、读、写、参数校验、采样上报、文件通道、事件推送。

---

## 4. 核心概念

### 4.1 SN 与主题

SN 是设备身份，也是所有主题的地址。两种来源必须分清：

| 接口 | 行为 | 用途 |
|------|------|------|
| `ncl_sn_read()` | `bin/sn.txt` 存在即沿用，不存在才生成 `V2` + 9 位十六进制 | **设备启动时用这个** |
| `ncl_config_init(sn, …)` | **无条件覆盖** `bin/sn.txt`；`sn` 为 NULL 时写入 32 位 hex | 对应 REST `/api/cfg/init`，出厂初始化用 |

 主题由 `ncl_topic_*` 构造，常用如下（完整列表见附录 C）：

| 主题 | 方向 | 说明 |
|------|------|------|
| `Query/Request/<sn>` / `Query/Response/<sn>` | 客户端 → 设备 | 读值 |
| `Set/Request/<sn>` / `Set/Response/<sn>` | 客户端 → 设备 | 写值 |
| `Probe/Query/Request/<sn>` / `.../Response/<sn>` | 客户端 → 设备 | 探测（回传模型/版本） |
| `Method/Call/Request/<sn>` / `.../Response/<sn>` | 客户端 → 设备 | 方法调用 |
| `Sample/<sn>/<通道id>` | 设备 → 订阅方 | 采样上报 |
| `Event/<sn>` | 设备 → 订阅方 | 事件推送 |
| `Ping/<sn>` / `Pong/<sn>` | 双向 | 心跳 |

设备端 `ncl_server_subscribe()` 会订阅 6 个请求主题；客户端
`ncl_client_subscribe()` 订阅 6 个响应主题，事件主题需要单独
`ncl_client_subscribe_events()`。

### 4.2 消息与消息项

一个 `ncl_message` 对应一种 NC-Link 报文（18 种），字段用 `ncl_message_set_*`
写入、`ncl_message_write_string()` 序列化，字段顺序按规范固定。

```c
ncl_message *request = ncl_message_new(NCL_MSG_QUERY_REQUEST);
ncl_query_request_item *item = ncl_query_request_item_new("/STATUS");
ncl_message_add_query_request_item(request, item);   /* 所有权转移 */
ncl_message_finalise(request);                       /* 补 @id（UUID） */
char *json = ncl_message_write_string(request);
```

解析走主题推断：

```c
ncl_message *msg = ncl_message_parse(topic, payload, payload_len);
```

### 4.3 数据模型与路径

模型就是 `conf/model/nclink.json` 那份文档：

```json
{"name":"nclink","id":"01","type":"NC_LINK_ROOT","devices":[
  {"id":"02","type":"PLC",
   "configs":[{"id":"ch1","type":"SAMPLE_CHANNEL",
               "sampleInterval":1000,"uploadInterval":5000,
               "ids":[{"id":"/STATUS"},{"id":"030002"}]}],
   "dataItems":[{"id":"030001","type":"STATUS"},
                {"id":"030002","type":"PART_COUNT"}],
   "version":"2.0"}]}
```

路径规则：

| 节点位置 | 路径 |
|----------|------|
| 根节点 | `/` + type，即 `/NC_LINK_ROOT` |
| 设备节点 | 根路径 + `/` + type，即 `/NC_LINK_ROOT/PLC` |
| 数据项/配置项，父是设备 | `/<type>[@<number>]`，即 `/STATUS` |
| 数据项/配置项，父是组件 | 父路径 + `/<type>[@<number>]` |
| 节点带 `source` 字段 | `/<source>/<type>`，**覆盖**父路径 |

客户端拿到模型后可以路径 ↔ id 互查：

```c
char *id = ncl_client_get_id(client, "/STATUS");    /* "030001" */
char *path = ncl_client_get_path(client, "030001"); /* "/STATUS" */
```

读写接口的 `path` 参数：以 `/` 开头按路径解释；否则按节点 id 查模型再换算成路径
（服务端 `ncl_server_resolve_path()` 有同样的规则）。

### 4.4 工具注册与 `<operation>#<path>` 绑定

把「模型路径 + 操作」映射到 C 函数用显式注册：

```c
ncl_server_register_tool(server, "plc", instance, methods, method_count,
                         bindings, binding_count);
```

绑定键形如 `<operation>#<path>`，例如
`get_value#/STATUS`、`set_value#/CONTROLLER/FILE`。

| operation（枚举 → 字符串） | 含义 |
|---------------------------|------|
| `NCL_OP_GET_VALUE` → `get_value` | 读值 |
| `NCL_OP_GET_LENGTH` → `get_length` | 读长度 |
| `NCL_OP_GET_KEYS` → `get_keys` | 读键列表 |
| `NCL_OP_GET_ATTRIBUTES` → `get_attributes` | 读属性 |
| `NCL_OP_SET_VALUE` → `set_value` | 写值 |
| `NCL_OP_ADD` → `add` | 新建 |
| `NCL_OP_DELETE` → `delete` | 删除 |
| `NCL_OP_FUNC_CALL` / `FUNC_STATUS` / `FUNC_RESULT` / `FUNC_CANCEL` → `call` / `status` / `result` / `cancel` | 功能调用族（本工程未使用） |

工具方法的参数名要与请求里的 `params` 键对上（如 `key`、`value`、`index`、
`offset`、`length`、`keys`、`localname`），见附录 A 里各 item 的访问器说明。

方法调用（`methodCall`）不走绑定键，而是按 `/工具名/方法名` 查找方法表：

```c
ncl_message_set_method(request, "/plc/setValue");   /* 也接受 "plc/setValue" */
```

### 4.5 采样与上报

采样通道写在模型的 `configs` 里（`type` 为 `SAMPLE_CHANNEL`）：

| 字段 | 含义 |
|------|------|
| `sampleInterval` | 采样周期（毫秒） |
| `uploadInterval` | 上报周期（毫秒），内部按「采样次数」向上取整 |
| `ids` | 采样项，可写路径（`/STATUS`）或节点 id（`030002`） |

```c
ncl_server_init_samples(server);              /* 按模型启动全部通道 */
ncl_server_add_sample(server, config_node);   /* 运行时单加一条（深拷贝） */
ncl_server_remove_sample(server, "ch1");
ncl_server_stop_all_samples(server);
```

上报报文发在 `Sample/<sn>/<通道id>`，结构见 `ncl_message` 的 `sample` 分支。

#### 采样通道的两种形态与异常处理

`ncl_server_add_sample()`（以及 `addSample` 方法调用）只接受两种形态，**其余一律返回
具体错误码且不启动任务**，不做静默降级：

| 形态 | 数据从哪来 | 表头来源 |
|------|-----------|----------|
| **1. 模型文件里有定义** | 模型里的 `SAMPLE_CHANNEL`，`ids` 写节点 id（或路径） | 由节点 id 在模型里解析出路径 |
| **2. 模型里没有定义，但给了表头** | `ids` 直接写路径 | 路径原样作为表头，走该路径上的工具绑定取值 |

```c
/* 情况 2：模型里完全没有这个通道，也无所谓 —— 表头直接给路径 */
const char *json =
    "{\"id\":\"chExt\",\"type\":\"SAMPLE_CHANNEL\","
    "\"sampleInterval\":40,\"uploadInterval\":80,"
    "\"ids\":[{\"id\":\"/EXT/A@0\"},{\"id\":\"/STATUS\"}]}";
ncl_node *config = /* ncl_node_from_json(...) */;
ncl_err rc = ncl_server_add_sample(server, config);   /* 前提：/EXT/A@0 上有工具绑定 */
```

异常（返回码 → 现象）：

| 情况 | 返回码 |
|------|--------|
| 通道 `id` 为空 | `NCL_ERR_INVALID_ARG` |
| `ids` 为空（没有采样项） | `NCL_ERR_INVALID_MODEL` |
| `sampleInterval` / `uploadInterval` 缺失或 ≤ 0 | `NCL_ERR_INVALID_ARG` |
| 采样项既不是路径，也无法在模型里按 id 找到 | `NCL_ERR_NOT_FOUND` |

经 `addSample` 方法调用时，同一个错误会变成应答 `code=NG`，原因在 `reason` 里
（`ncl_err_name()` 的文本），客户端 `ncl_client_add_sample()` 相应返回错误码。

#### 亚毫秒采样：值本身可以是数组（数组套数组）

采样周期最小仍是 1 ms（**外层槽位**）；但**一次采样可以返回一批值**——工具返回
数组时，这一列的每个槽位就是那个数组，报文自然变成"槽位数组套批次数组"。
库不配置、报文不加字段，批次长度完全由数据决定：

```
sampleInterval = 1ms（槽位）, uploadInterval = 200ms
工具每次返回 10 个值（相当于槽位内 0.1ms 分辨率）
→ 外层 200 个槽位，每槽 10 点，合计 2000 点
   "data":[ [10 个值], [10 个值], ..., [10 个值] ]
```

消费端不必自己判断两层结构，用助手即可（标量列同样适用）：

```c
const ncl_sample_item *item = ncl_message_item_at(msg, 0);
if (ncl_sample_item_is_nested(item)) {                 /* 是批量列 */
    size_t slots  = ncl_json_arr_len(item->data);      /* 槽位数 */
    size_t points = ncl_sample_item_value_count(item); /* 总点数 */
    const ncl_json *first = ncl_sample_item_value_at(item, 0);   /* 扁平取值 */
}
size_t total = ncl_message_sample_point_count(msg);    /* 整条报文点数 */
```

#### 消费端拿到的报文一定是完整的（内外层都要对齐）

设备端发布前会校验 `ncl_message_sample_is_complete()`，**内外层都必须对齐**，
一条报文只允许一种统一形状：

| 层次 | 要求 |
|------|------|
| 外层 | 表头（`paths`）非空；表头项数 == 数据块列数；**每列槽位数相同**且 ≥ 1 |
| 内层 | 要么所有位置都是标量/null；要么所有位置都是数组，且**所有内层长度完全相同** |

判为"标量混批量"或"内层长度不一致"的窗口会被**丢弃**并记
`采样报文不完整，已丢弃: 通道 xxx`，绝不发出没法按行列对读的数据。
消费端可以再兜一层：

```c
if (!ncl_message_sample_is_complete(msg)) {
    ncl_log_warn("采样报文不完整，已忽略: %s", topic);
    return;
}
```

（某一列某次取值失败时，该值是 `null`：列还在、表头对得上，属于"完整但含空值"，
不是残缺报文。）

注意：`ncl_message_is_valid()` 只检查外层（各列槽位数一致），内层对齐是
`ncl_message_sample_is_complete()` 额外做的检查；因此它仍可能对一条"外层齐、
内层不齐"的报文返回 true，判断采样报文能否消费请一律用后者。

报文内容（也是客户端回调里拿到的字段）：

| 字段 | 含义 |
|------|------|
| 主题末段 / `sample->as.sample.id` | 采样通道 id |
| `sample->as.sample.paths` | **表头**：本次采集的数据项路径**数组**，与 `data` 按下标一一对应 |
| `sample->as.sample.data[i]` | 第 i 项的数据：`{encoding, data:[本轮各次采样值]}`，未编码时 `encoding` 为空 |
| `sample->as.sample.interval` | **采样周期**，取模型里该通道的 `sampleInterval`；注意报文键名是 `interval`，不是 `sampleInterval` |
| `sample->as.sample.upload_interval` | 上报周期，取模型里的 `uploadInterval` |
| `sample->as.sample.begin_time` | 本窗口起始时刻（epoch 毫秒，字符串形式） |

一条真实报文（字段顺序按规范固定）：

```json
{"paths":["/STATUS","/PART_COUNT"],"id":"ch1","beginTime":"1789450135470",
 "data":[{"data":[0,0]},{"data":[129,139]}],"interval":1000,"uploadInterval":2000}
```

即：`paths` 是表头（采集了哪些数据项），`data[i].data` 是第 i 项在本窗口内按时间
先后采到的值；`interval` = 模型 `sampleInterval`，`uploadInterval` = 模型
`uploadInterval`。若上层要一行字符串形式的表头（日志/CSV），用
`ncl_message_sample_header(msg, ";")` 把同一个数组拼起来即可。

两点容易误解的地方：

| 事项 | 说明 |
|------|------|
| 路径分隔符 | 组件/数据项的编号用 `@`：`/AXIS@0/POSITION` |
| `params` 后缀 | 采样项若在 `ids` 里带了 `params`（LIST 的 `indexes` / HASH 的 `keys`），模型层 `ncl_sample_ref_path()` 会给出带后缀的形式（`/AXIS@0/TRACE$LIST-0`、`/AXIS@0/PARAM$HASH-speed`，多个索引时是 `$LIST-[0, 1]`）。但**设备端上报的 `paths` 用的是数据项本身的路径**，不带后缀 |

对端怎么收这些报文见 5.5 的「采样」与 6.6。

### 4.6 错误码

所有会失败的接口返回 `ncl_err`（`int`）。`NCL_OK == 0`，其余为负值：

| 区间 | 含义 |
|------|------|
| `0` | 成功 |
| `-1 … -13` | 基础设施错误（`NCL_ERR_NOMEM` `-2`、`NCL_ERR_PARSE` `-3`、`NCL_ERR_TIMEOUT` `-4`、`NCL_ERR_IO` `-5`、`NCL_ERR_NOT_FOUND` `-6`、`NCL_ERR_INVALID_ARG` `-9`、`NCL_ERR_STATE` `-10`、`NCL_ERR_CONNECT` `-12`…） |
| `-100 … -117` | 协议域校验错误，19 条校验规则各一个（如 `NCL_ERR_INVALID_VALUE` `-116`） |

`ncl_err_name(err)` 返回稳定的文本名称（如 `"InvalidValueException"`），
便于日志与对端比对。完整表格见附录 B。

**注意**：返回 `ncl_err` 的函数在出错时**不一定**会写满出参，调用前把出参置空是
好习惯（库内部在失败路径上也会尽量置空）。

### 4.7 所有权与内存约定

C 没有 GC，规则统一为「谁申请谁负责，转移要显式」：

| 场景 | 规则 |
|------|------|
| `ncl_*_new()` / `cln_strdup()` / `ncl_message_write_string()` … | 返回堆内存，**调用方 `free()`** |
| 集合元素 | `ncl_ptrvec` / `ncl_strvec` 带析构回调，`*_free()` 时自动释放元素 |
| `ncl_message_add_*_item(msg, item)` | 成功后**所有权转移**给消息 |
| `ncl_client_query/set/method_call(client, request, …)` | 接管 `request`，响应放 `*out`，**调用方** `ncl_message_free()` |
| `ncl_server_invoke_*()` | 返回新消息，调用方负责释放 |
| `ncl_client_on_message(client, topic, msg)` | **任何情况下**接管 `msg` |
| 事件回调里的 `ncl_message *` | **借用**，回调返回后立即释放，不能保存指针 |
| `ncl_ptrvec_at()` / `ncl_client_root_node()` / `ncl_message_get_data()` | 借用，不要释放 |
| JSON 上的 `ncl_json_obj_get()` / `ncl_json_arr_get()` | 借用；`ncl_json_arr_take()` 才是脱开所有权 |
| `ncl_message_take_model(probe)` | 从消息里摘出模型，调用方接管（配合 `ncl_client_set_root_node()`） |
| `ncl_cache_take()` / `ncl_ptrvec_take()` | 摘除且不释放，所有权交给调用方 |

### 4.8 线程模型与线程安全

| 组件 | 线程 | 说明 |
|------|------|------|
| `ncl_mqtt_client` | 1 个收包线程 | 回调 `on_message` **在收包线程上**执行，不要阻塞 |
| 服务端 | 共享线程池（5/10/100 + CallerRuns） | `ncl_server_on_message()` 提交后立即返回；工具方法在工作线程执行，可放心阻塞 |
| 采样 | 每通道 1 个线程 | 与请求处理并发 |
| HTTP 服务 | 1 个连接线程 + 监听线程 | 请求处理在监听线程上串行 |
| FTP 服务 | 1 个接受线程 + 每会话 1 个线程 | |
| 客户端 | 无自有线程 | `ncl_client_*` 请求会阻塞等待响应，可在任意线程调用；响应由收包线程写入缓存 |

| 对象 | 并发使用 |
|------|----------|
| `ncl_message` / `ncl_json` / `ncl_node` | **不**线程安全，按线程私有使用 |
| `ncl_client` | 请求/响应缓存有锁，可从多线程调用；同一时刻的并发请求各自按 `@id` 关联 |
| `ncl_client_holder` | 线程安全（内部互斥），`get()` 可随时调用 |
| `ncl_server` | 内部状态有锁；注册工具请在 `subscribe()` 之前完成 |
| `ncl_logger` | 线程安全 |
| `ncl_ftp_server` / `ncl_http_server` | 启停与运行线程安全，`stop()` 会 join 全部线程 |

日志函数（`ncl_log_info` 等）可在任意线程调用。

---

## 5. 模块手册

### 5.1 基础层

#### ncl_common.h —— 错误码、字符串、容器

```c
char *s = ncl_strdup("abc");            /* 堆字符串，调用方 free */
char *t = ncl_str_trim_dup(" a ");      /* 去首尾空白的新副本 */
bool  b = ncl_str_starts_with(s, "ab");
char  uuid[37];  ncl_uuid4(uuid, sizeof(uuid));   /* 小写 UUID v4 */

/* 字符串缓冲：拼 JSON、拼路径、攒报文都靠它 */
ncl_strbuf sb;
ncl_strbuf_init(&sb);
ncl_strbuf_puts(&sb, "id=");
ncl_strbuf_printf(&sb, "%d", 7);
char *text = ncl_strbuf_detach(&sb);    /* 取走内容，sb 复位 */
free(text);
ncl_strbuf_free(&sb);

/* 指针容器：带析构回调，free 时自动释放元素 */
ncl_ptrvec v;
ncl_ptrvec_init(&v, ncl_file_attribute_release);
ncl_ptrvec_push(&v, attribute);          /* 所有权转移 */
ncl_ptrvec_free(&v);
```

#### ncl_platform.h —— 时间、随机数、线程、互斥、条件变量

```c
int64_t ms  = ncl_time_millis();             /* 墙上时钟 */
int64_t now = ncl_time_monotonic_millis();   /* 单调时钟，算间隔用这个 */
ncl_sleep_millis(100);
ncl_random_bytes(buf, sizeof(buf));

ncl_mutex *m = ncl_mutex_create();
ncl_cond  *c = ncl_cond_create();
ncl_mutex_lock(m);
ncl_cond_wait_timeout(c, m, 1000);           /* 唤醒或超时都返回 */
ncl_mutex_unlock(m);

ncl_thread *th = ncl_thread_start(worker, arg);
ncl_thread_join(th);                          /* join 会释放句柄 */
```

#### ncl_logger.h —— 日志

```c
ncl_log_init(NULL);              /* 写 <root>/log/out.txt，10 MB 轮转；同时输出控制台 */
ncl_log_set_level(NCL_LOG_DEBUG);/* DEBUG/INFO/WARN/ERROR/NONE，默认 INFO */
ncl_log_set_console(false);      /* 关掉控制台镜像 */
ncl_log_info("采样任务已启动: %s", id);   /* 打印中文没问题（UTF-8） */
ncl_log_error("连接失败: %s", reason);
ncl_log_shutdown();
```

#### ncl_env.h —— 运行环境、路径、SN、mqtt.cfg

安装根目录决定所有路径，启动时设定一次：

```c
ncl_env_set_root("/opt/nclink");        /* 传 NULL 恢复为当前工作目录 */
ncl_env_root();        /* /opt/nclink */
ncl_env_conf_path();   /* /opt/nclink/conf */
ncl_env_run_path();    /* /opt/nclink/bin */
ncl_env_log_path();    /* /opt/nclink/log */
ncl_env_log_file();    /* /opt/nclink/log/out.txt */
ncl_env_mqtt_cfg_file();
ncl_env_model_file();
ncl_env_sn_file();     /* /opt/nclink/bin/sn.txt */
```

文件小工具。注意 `ncl_file_write_all()` / `ncl_file_append()` **不会**创建父目录
（直接用 `fopen`），路径上层的目录请先 `ncl_mkdir_p()`；而 `ncl_file_copy()`
会为副本建好父目录。

```c
ncl_mkdir_p(ncl_env_conf_path());
char *text; size_t len;
ncl_file_read_all("in.json", &text, &len);      /* text 需 free */
ncl_file_write_all("out.json", text, len);
ncl_file_append("out.log", "more\n", 5);
ncl_file_copy("a.txt", "b.txt");                /* 会自动建 b 的父目录 */
ncl_path_exists(p); ncl_path_is_dir(p);
ncl_file_size(p); ncl_file_mtime_ms(p);
ncl_path_remove(p);                             /* 目录则递归删除 */
```

#### ncl_thread.h —— 线程池与 TTL 缓存

```c
/* 全局单例线程池（默认：核心 5 / 最大 10 / 队列 100，队满时在调用线程执行） */
ncl_thread_pool_submit(ncl_thread_service(), work_fn, arg);
ncl_thread_service_shutdown();

/* 自带线程池 */
ncl_thread_pool_options opt;
ncl_thread_pool_options_default(&opt);
opt.core_threads = 2;
ncl_thread_pool *pool = ncl_thread_pool_create(&opt);

/* TTL 缓存：客户端响应缓存、设备客户端表都用它 */
ncl_cache *cache = ncl_cache_create(5 * 60 * 1000, false, free_fn);
ncl_cache_put(cache, key, value);
void *v  = ncl_cache_get(cache, key);     /* 借用 */
void *t  = ncl_cache_take(cache, key);    /* 摘除且不释放，所有权归你 */
ncl_cache_free(cache);
```

### 5.2 ncl_json.h —— JSON DOM

紧凑输出、忽略未知字段、空值省略由调用方显式控制、数字保留原始字面量。

```c
/* 构造 */
ncl_json *obj = ncl_json_new_object();
ncl_json_obj_set_string(obj, "name", "plc");
ncl_json_obj_set_int(obj, "size", 3);
ncl_json_obj_set_bool(obj, "ok", true);
ncl_json *arr = ncl_json_new_array();
ncl_json_arr_push(arr, ncl_json_new_int(1));      /* 所有权转移 */
ncl_json_obj_set(obj, "items", arr);              /* 所有权转移 */

/* 访问（全部是借用指针） */
const char *name = ncl_json_obj_get_string(obj, "name");
long long size   = ncl_json_obj_get_int(obj, "size", 0);
ncl_json *items  = ncl_json_obj_get(obj, "items");
size_t n         = ncl_json_arr_len(items);
ncl_json *first  = ncl_json_arr_get(items, 0);
long long v;  ncl_json_as_int(first, &v);         /* 类型不符返回 false */

/* 解析与序列化 */
ncl_json *doc = ncl_json_parse(text, len, NULL);  /* NULL 处可传 ncl_strbuf 收错误 */
char *out = ncl_json_write_string(doc);           /* 调用方 free */
ncl_json_free(doc);
```

### 5.3 协议层

#### ncl_general.h —— 常量、枚举、校验

```c
ncl_operation_to_string(NCL_OP_GET_VALUE);        /* "get_value" */
ncl_operation_parse("set_value", &op);
ncl_code_to_string(NCL_CODE_NG);                  /* "NG" */
ncl_check_is_code_ok(code); ncl_check_is_code_ng(code);
NCL_PATH_SEPARATOR  NCL_OPERATION_SEPARATOR  NCL_DATA_TYPE_HASH
```

#### ncl_topic.h —— 主题构造

```c
char *t = ncl_topic_query_request(sn, NULL);      /* Query/Request/<sn>；需 free */
char *e = ncl_topic_event(sn, NULL);              /* Event/<sn> */
char *s = ncl_topic_sample(sn, "ch1");            /* Sample/<sn>/ch1 */
char *x = ncl_topic_extract_sn("Query/Request/V203243111F");  /* 反解 SN */
```

第二个参数是「客户端 id」：传 NULL 表示不带，某些部署会拼成 `<主题>/<clientId>`。

#### ncl_message.h —— 报文

见 4.2。常用补充：

```c
ncl_msg_type type = ncl_msg_type_from_topic(topic);
bool valid = ncl_message_is_valid(msg);           /* 合法性校验 */
bool match = ncl_message_matches(response, request);
ncl_json *data = ncl_message_get_data(msg);       /* 仅 QUERY_RESPONSE 用这个 */
```

#### ncl_model.h —— 数据模型

多数字段直接读结构体（`ncl_node` 是压平后的联合体，带 `type` 判别字段）：

```c
ncl_node *root = ncl_root_node_parse(json_text);   /* 解析 + 后构造（算路径） */
ncl_node *device = ncl_node_device_at(root, 0);
ncl_node *item = ncl_node_data_item_at(device, 0);
const char *path = ncl_node_path(item);            /* "/STATUS" */
ncl_node *found = ncl_node_find_by_id(root, "030001");
ncl_node *copy = ncl_node_clone(item);             /* 深拷贝 */
ncl_node_free(root);
```

#### ncl_codec.h —— 十六进制与压缩

```c
ncl_buffer out;
ncl_codec_encode_hex(src, len, &out);      /* out.data / out.len，用完 ncl_buffer_free */
ncl_codec_decode_hex(src, len, &out);
if (ncl_codec_compress_available()) {      /* 需要 NCLINK_WITH_ZLIB=ON */
    ncl_codec_encode_compress(src, len, &out);
}
```

### 5.4 传输层

#### ncl_socket.h —— TCP

```c
ncl_socket *s = ncl_socket_connect("127.0.0.1", 1883, 5000, err, sizeof(err));
ncl_socket_send(s, data, len);
int n = ncl_socket_recv(s, buf, sizeof(buf), 1000);  /* NCL_SOCKET_TIMEOUT(-2) 表示超时 */
ncl_socket_recv_exact(s, buf, len, 1000);

ncl_socket *l = ncl_socket_listen(9008, err, sizeof(err));   /* 端口 0 = 随机 */
unsigned port = ncl_socket_local_port(l);
ncl_socket *c = ncl_socket_accept(l, 500);

ncl_socket_shutdown(s);   /* 只关句柄、幂等：用来唤醒阻塞中的读 */
ncl_socket_close(s);      /* 释放对象 */

char host[64];
ncl_socket_local_ip(s, host, sizeof(host));
ncl_socket_peer_ip(s, host, sizeof(host));
char *map = ncl_net_ip_map_json();   /* {"eth0":"192.168.1.5"}，需 free */
```

#### ncl_mqtt.h —— MQTT 5.0

报文编解码层（`ncl_mqtt_packet_*`）一般不需要直接使用；常用的是客户端：

```c
ncl_mqtt_client_options opt;
ncl_mqtt_client_options_default(&opt);
opt.url = "tcp://broker:1883";       /* ssl:// 未实现，返回 NCL_ERR_NOT_SUPPORTED */
opt.client_id = sn;
opt.username = user;  opt.password = pass;
opt.keep_alive_seconds = 60;
opt.connect_timeout_ms = 10000;
opt.clean_start = true;
opt.automatic_reconnect = true;
opt.on_message = on_message;         /* 收包线程回调 */
opt.on_disconnect = on_disconnect;   /* 断线回调，可带 reason_code */
opt.user = your_context;

ncl_mqtt_client *c = ncl_mqtt_client_create(&opt);
ncl_mqtt_client_connect(c);
ncl_mqtt_client_subscribe(c, "Query/Response/V1", 2);
/* 发布时自动附带 version=2.0 用户属性 */
ncl_mqtt_client_publish(c, topic, payload, len, 1, NULL, 5000);

const char *err = ncl_mqtt_client_last_error(c);
ncl_mqtt_client_is_connected(c);
ncl_mqtt_client_disconnect(c);
ncl_mqtt_client_destroy(c);
```

自动重连成功后，客户端会自动恢复之前订阅的主题。

### 5.5 ncl_client.h —— 客户端

#### 三层结构

```
ncl_message_channel   传输接口（函数指针表）。客户端管理器是 MQTT 实现；
                      测试或嵌入场景可自己实现，把请求直接交给 ncl_server
ncl_client            单台设备的协议视图：请求/响应关联、5 分钟响应缓存
ncl_client_holder     进程级单例：MQTT 连接 + 按 SN 的客户端表（30 分钟空闲过期）
```

#### 请求接口

```c
ncl_client *client = ncl_client_holder_get("V203243111F");

/* 便捷读值 */
ncl_json *value = NULL;
ncl_client_get_value(client, "/STATUS", 5000, &value);
ncl_client_get_value_range(client, "/PART_COUNT", 0, 9, 5000, &value);
long long length;
ncl_client_get_length(client, "/STATUS", 5000, &length);

/* 写值：返回 NCL_OK 表示设备应答 OK；NG 返回 NCL_ERR */
ncl_client_set_value(client, "/STATUS", ncl_json_new_int(7), 5000);
ncl_client_set_value_index(client, "/LIST", ncl_json_new_string("x"), 2, 5000);

/* 探测 / 心跳 */
ncl_message *probe = NULL;
ncl_client_probe(client, 5000, &probe);          /* 回传设备模型（见 4.7 接管） */
ncl_client_ping(client, 5000, &pong);

/* 方法调用：request 的所有权交给库，response 由你释放 */
ncl_message *request = ncl_message_new(NCL_MSG_METHOD_CALL_REQUEST);
ncl_message_set_method(request, "/plc/setValue");
ncl_message_set_params(request, params_json);    /* 所有权转移 */
ncl_message_set_check(request, true);            /* 只校验不执行 */
ncl_message *response = NULL;
ncl_client_method_call(client, request, 5000, &response);
```

`ncl.client` 的每个请求都会阻塞到响应或超时；超时返回 `NCL_ERR_TIMEOUT`。

#### 独立使用（不用客户端管理器）

```c
/* 自己实现通道，把发布/订阅接到任意传输上 */
static ncl_err my_publish(ncl_message_channel *self, const char *topic,
                          const ncl_message *msg, int qos,
                          const ncl_mqtt_properties *props) { /* ... */ }

ncl_message_channel channel = { my_publish, my_subscribe, my_unsubscribe };
ncl_client *client = ncl_client_create("V203243111F", &channel);
ncl_client_subscribe(client);
/* 收到响应时把它交给客户端（库会按 @id 关联并唤醒等待者） */
ncl_client_on_message(client, topic, message);
```

#### 事件

```c
static void on_event(ncl_client *c, const char *topic,
                     const ncl_message *msg, void *user) {
    const char *key = ncl_json_obj_get_string(msg->as.event.event, "key");
    /* msg 是借用的，回调返回后即被释放 */
}
ncl_client_set_event_handler(client, on_event, NULL);
ncl_client_subscribe_events(client, 2);
ncl_client_unsubscribe_events(client);
size_t n = ncl_client_event_count(client);
```

#### 运行期更换 broker

```c
/* 改完 conf/mqtt.cfg（例如经 REST /api/setMqttUrl）后，让连接按新配置重来 */
ncl_client_holder_restart();
```

> 配置写接口只落盘、不动运行中的连接；要让新配置生效就调用这个函数。

#### 采样

设备按 `uploadInterval` 把窗口内的采样值聚合成一条 `Sample` 报文，发在
`Sample/<sn>/<通道id>`。客户端订阅一次通配主题即可覆盖该设备的所有通道：

```c
/* 回调：msg 是借用的，返回后立即释放，不要保存指针 */
static void on_sample(ncl_client *client, const char *topic,
                      const ncl_message *msg, void *user) {
    size_t i, items = ncl_message_item_count(msg);   /* = 采样项个数 */
    /* 表头：本次采集了哪些数据项。线上是数组 "paths":[...] */
    ncl_json *header = ncl_strvec_to_json(&msg->as.sample.paths);
    char *header_text = header != NULL ? ncl_json_write_string(header) : NULL;
    ncl_log_info("通道 %s，采样 %lldms（=模型 sampleInterval），上报 %lldms，表头 %s",
                 msg->as.sample.id, msg->as.sample.interval,
                 msg->as.sample.upload_interval,
                 header_text != NULL ? header_text : "[]");
    free(header_text);
    ncl_json_free(header);

    for (i = 0; i < items; i++) {
        const ncl_sample_item *item = ncl_message_item_at(msg, i);
        const char *path = ncl_strvec_at(&msg->as.sample.paths, i);  /* 表头第 i 项 */
        /* item->data 是本轮（uploadInterval 内）按时间先后采集到的值数组 */
        ncl_log_info("  %s: %u 个值，第一个=%s", path,
                     (unsigned)ncl_json_arr_len(item->data),
                     ncl_json_number_raw(ncl_json_arr_get(item->data, 0)));
    }
}

ncl_client_set_sample_handler(client, on_sample, NULL);
ncl_client_subscribe_samples(client, 0);       /* 订 "Sample/<sn>/#" */
ncl_log_info("已订阅 %s", ncl_client_sample_topic(client));

/* ... 运行期间回调会不断被触发 ... */

ncl_client_unsubscribe_samples(client);
ncl_client_set_sample_handler(client, NULL, NULL);
size_t got = ncl_client_sample_count(client);   /* 收到过多少条上报 */
```

几点说明：

| 事项 | 说明 |
|------|------|
| 订阅范围 | `ncl_client_subscribe_samples()` 订的是 `Sample/<sn>/#`，一台设备的所有通道一次覆盖；只想收某个通道就自己订 `Sample/<sn>/<通道id>`（见 6.6） |
| QoS | 示例用 0；需要断线补发就用 1（设备侧上报固定按 QoS 0 发，见 `Server` 采样段） |
| 触发时机 | 上报由设备侧驱动，`uploadInterval` 一到就发；客户端只是被动接收 |
| 采样项对应 | `paths[i]` 与 `data[i]` 一一对应，顺序与设备模型里 `ids` 的顺序一致 |
| 表头 | `paths` 是数组（不是拼接字符串）；要一行字符串用 `ncl_message_sample_header(msg, ";")` |
| `interval` | 就是模型里的 `sampleInterval`（报文键名是 `interval`） |
| 时间戳 | 报文里带 `beginTime`（窗口起点，epoch 毫秒字符串），需要严格时间对齐时用它 |
| 完整性 | 设备端只发完整报文（表头与数据块对齐）；消费端可用 `ncl_message_sample_is_complete()` 复核 |
| 亚毫秒采样 | 值本身可以是数组（一个槽位一批数据）；读它用 `ncl_sample_item_is_nested()` / `_value_count()` / `_value_at()` |

### 5.6 ncl_server.h —— 服务端

#### 创建与订阅

```c
ncl_server_options opt;
memset(&opt, 0, sizeof(opt));
opt.sn = sn;
opt.mqtt = mqtt;                        /* 借用；也可换成下面的 publish 钩子 */
opt.model_json = model_text;            /* 可为 NULL，之后再 load_model */
opt.publish = my_publish;               /* 可选：自定义出站通道（无 broker 场景） */
opt.publish_user = my_ctx;

ncl_server *server = ncl_server_create(&opt);
ncl_server_load_model(server, text);    /* 或 ncl_server_set_model(server, root) */
ncl_server_save_model(server);          /* 写回 conf/model/nclink.json */
ncl_server_subscribe(server);           /* 订 6 个请求主题 */
```

#### 工具注册

```c
ncl_server_register_tool(server, "file", instance, methods, n_methods,
                         bindings, n_bindings);
ncl_server_register_builtin_tool(server);   /* /nclinkServer/addSample、removeSample */
ncl_server_register_file_tool(server);      /* /CONTROLLER/FILE 的 5 个方法 */
```

#### 离线调用（不起 MQTT 也能测）

```c
ncl_message *response = ncl_server_invoke_query(server, request);
ncl_message *response = ncl_server_invoke_set(server, request);
ncl_message *response = ncl_server_invoke_method_call(server, request);
ncl_message *response = ncl_server_check_method_call(server, request);  /* 只校验 */
ncl_message *response = ncl_server_dispatch(server, topic, request);    /* 按类型分发 */
```

#### 出站钩子

没有 MQTT 也要跑（嵌入式、单元测试、自建传输）时，注册一个 publish 回调即可，
服务端会把「主题 + 已序列化的报文体」交给你：

```c
static ncl_err my_publish(void *user, const char *topic,
                          const char *payload, size_t len) {
    return ncl_socket_send((ncl_socket *)user, payload, len) == NCL_OK
               ? NCL_OK : NCL_ERR_IO;
}
ncl_server_set_publish_sink(server, my_publish, my_socket);
```

#### 事件与统计

```c
ncl_json *event = ncl_json_new_object();
ncl_json_obj_set_string(event, "key", "PART_COUNT");
ncl_json_obj_set_int(event, "value", 12);
ncl_server_push_event(server, "030002", event);       /* 发到 Event/<sn> */
ncl_server_push_event_ex(server, "030002", event, 1700000000000LL, "evt-1");
ncl_json_free(event);

size_t uploads = ncl_server_sample_upload_count(server);
size_t events  = ncl_server_event_count(server);
size_t binds   = ncl_server_binding_count(server);
```

#### 其它

```c
ncl_json *schema = ncl_server_openapi_schema(server, "http://host:9008/api");
char *schema_json = ncl_server_openapi_schema_json(server, base_url);
ncl_node *model = ncl_server_model(server);
const char *sn = ncl_server_sn(server);
ncl_server_set_user_data(server, my_state, my_cleanup);   /* 挂载自有数据 */
```

### 5.7 ncl_http.h / ncl_rest.h —— HTTP 与 REST

```c
ncl_http_server *http = ncl_http_server_create(9008);

/* 自定路由：路径是**精确匹配**（含前导 '/'），method 传 "GET"/"POST"/...，
 * 传 NULL 或 "*" 表示任意方法；handler 跑在受理连接的那个线程上。 */
ncl_http_server_route(http, "GET", "/api/hello", hello_handler, user);
ncl_http_server_route(http, "*", "/api/health", health_handler, user);

/* 内置挂载：/api/schema + /swagger-ui，以及 12 个配置接口 */
ncl_rest_attach(http, server);
ncl_rest_attach_config(http);

ncl_http_server_set_cors(http, true);
ncl_http_server_start(http);
/* ... */
ncl_http_server_stop(http);
ncl_http_server_free(http);
```

handler 里读请求、写应答：

```c
static void hello_handler(ncl_http_request *req, ncl_http_response *res,
                          void *user) {
    const char *name = ncl_http_query(req, "name");     /* 查询参数 */
    const char *body = ncl_http_body(req);              /* 正文（NUL 结尾） */
    size_t len       = ncl_http_body_len(req);
    ncl_json *json   = ncl_http_json_body(req);         /* 解析 JSON 正文，需释放 */
    const char *auth = ncl_http_header(req, "Authorization");

    /* Result 封装：{"status":true,"data":...} / {"status":false,"data":"原因"} */
    ncl_rest_reply(res, 200, ncl_result_success_string("ok"));
    /* 直接写 JSON 值（内部负责序列化） */
    ncl_json *doc = ncl_json_new_object();
    ncl_json_obj_set_string(doc, "hello", "world");
    ncl_http_reply_json(res, 200, doc);          /* 所有权转移 */
    /* 或者返回纯文本 */
    ncl_http_reply_text(res, 404, "not found");
}
```

`ncl_http_url_encode/decode()` 处理百分号编码；`ncl_http_form_field()` 解析
`application/x-www-form-urlencoded` 正文。

#### 通过 HTTP 调用工具方法

`ncl_rest_attach()` 除了挂 `/api/schema` 与 `/swagger-ui`，还会挂一条**兜底路由**：

```
POST /api/<工具名>/<方法名>
Content-Type: application/json
{"…":"请求参数"}                 ← 直接作为 methodCall 的 params
```

应答统一是 `Result` 封装，**工具返回 NG 时原因会原样回给调用方**：

```json
{"status":true,"data":{…}}                      // code=OK
{"status":false,"data":"NotFoundException"}     // code=NG，data 就是 reason
```

| 请求 | 结果 |
|------|------|
| `POST /api/nclinkServer/addSample` | 启动采样通道；失败时 `data` 是错误名（如 `NotFoundException`） |
| `POST /api/nclinkServer/removeSample` | 停掉通道 |
| `POST /api/<你的工具>/<你的方法>` | 调用注册表里的任意方法 |
| 非 POST，或路径不是 `/api/<工具>/<方法>`（段数不对） | `404` |
| 工具/方法不存在 | `{"status":false,"data":"未找到方法"}` |

要点：

| 事项 | 说明 |
|------|------|
| 参数校验 | 想让设备只校验不执行，正文里加 `"check": true`（该字段会随 params 传给方法调用） |
| 路由优先级 | 精确路由优先于 `/api` 兜底路由，所以 `/api/schema`、12 个配置接口不会被兜底吃掉；**先挂哪个都行** |
| 工具返回文件 | 服务端把 `{"@file":…}` 换成 `/temp/<名字>` 令牌并在 `data.fileKeys` 里列键名，HTTP 调用方再按文件通道取字节（同 MQTT 流程） |
| multipart | **不支持**；带 `Content-Type: multipart/form-data` 会被拒绝并提示改用 JSON，文件传输请走文件通道 |

```bash
curl -X POST http://127.0.0.1:9008/api/nclinkServer/addSample \
     -H 'Content-Type: application/json' \
     -d '{"request":{"id":"ch1","type":"SAMPLE_CHANNEL",
                    "sampleInterval":1000,"uploadInterval":2000,
                    "ids":[{"id":"/STATUS"},{"id":"030002"}]}}'
# → {"status":true,"data":true}
```

### 5.8 ncl_config.h —— 设备配置

每个函数都有等价的 REST 接口（见 5.7 的配置接口表）。

```c
/* 初始化：建 conf/bin/log 目录并写 SN（覆盖式，见 4.1） */
char *sn = NULL;
ncl_config_init(NULL, &sn);          /* 不传则生成 32 位 hex */
ncl_config_init("V2TEST00001", &sn); /* 指定 SN */

/* 读取 */
char     *sn2   = ncl_config_get_sn();      /* 需 free（内部走 ncl_sn_read） */
ncl_json *model = ncl_config_get_model();   /* 文件缺失返回 NULL */
ncl_json *drv   = ncl_config_get_driver();
ncl_json *list  = ncl_config_get_server_list();
ncl_json *mqtt  = ncl_config_get_mqtt();    /* {"url","username","password"} */
ncl_json *ip    = ncl_config_get_ip_conf();

/* 写入 */
ncl_config_set_model(json_text);
ncl_config_set_driver(json_text);
ncl_config_set_server_list(json_text);
ncl_config_set_mqtt(json);                  /* 借用 */
ncl_config_set_ip_conf(json_text);
```

| 文件 | 内容 |
|------|------|
| `bin/sn.txt` | 设备序列号 |
| `conf/model/nclink.json` | 数据模型 |
| `conf/driver/*.json` | 驱动配置 |
| `conf/mqtt.cfg` | `url=` / `username=` / `password=` 三行 |
| `conf/ipConf.json` | 网络配置 |
| `conf/server.json` | 服务器列表 |
| `bin/ftp.txt` | FTP 端口与账号（见 5.9） |

### 5.9 ncl_ftp.h —— FTP 服务端与客户端

FTP 两端都是本库自带实现（RFC 959/2389 子集），无第三方依赖。

#### 服务端

```c
ncl_ftp_server_options opt;
memset(&opt, 0, sizeof(opt));
opt.port = 2323;            /* 0 表示随机端口，用 ncl_ftp_server_port() 查 */
opt.root = "/srv/share";    /* 登录根：路径规范化后无法用 ".." 越出 */
opt.user = "admin";
opt.password = "123456";
opt.allow_write = true;     /* 允许 STOR/DELE/MKD/RMD */
opt.idle_timeout_ms = 300000;

ncl_ftp_server *ftp = ncl_ftp_server_create_ex(&opt);
ncl_ftp_server_start(ftp);
/* ... */
ncl_ftp_server_stop(ftp);
ncl_ftp_server_free(ftp);   /* 不删除 root 目录 */
```

支持：`USER PASS SYST FEAT OPTS PWD CWD CDUP TYPE MODE STRU PASV EPSV PORT EPRT
LIST NLST RETR STOR APPE DELE RMD MKD RNFR RNTO SIZE MDTM REST ABOR NOOP STAT
ALLO HELP QUIT`，主动与被动两种数据连接都支持。

#### 客户端

```c
ncl_ftp_client_options opt;
memset(&opt, 0, sizeof(opt));
opt.passive = false;        /* 默认主动模式；跨 NAT 时改 true 走被动 */
opt.connect_timeout_ms = 5000;
opt.io_timeout_ms = 30000;

ncl_ftp_client *c = ncl_ftp_client_create_ex("127.0.0.1", 2323, "admin",
                                             "123456", &opt);
if (ncl_ftp_client_detect(c)) {               /* 连接 + 登录 + TYPE I，带 NOOP 存活判定 */
    ncl_ftp_client_mkdir(c, "/data");
    ncl_ftp_client_chdir(c, "/data");
    ncl_ftp_client_store_file(c, "a.txt", "local/a.txt");   /* 上传 */
    ncl_ftp_client_retrieve_file(c, "a.txt", "local/b.txt");/* 下载 */

    ncl_ptrvec entries;
    ncl_ptrvec_init(&entries, ncl_ftp_entry_free);
    ncl_ftp_client_list(c, ".", &entries);    /* 每个元素 ncl_ftp_entry */
    ncl_ptrvec_free(&entries);

    ncl_ftp_client_delete(c, "a.txt");
    ncl_ftp_client_rmdir(c, ".");
}
ncl_ftp_client_disconnect(c);
ncl_ftp_client_free(c);
```

`ncl_ftp_client_noop()` 可做存活探测；`ncl_ftp_client_reply_code/text()` 拿到最近
一次应答，便于排错。

### 5.10 ncl_file.h —— 文件传输

#### 设备端（收文件）

```c
ncl_server_register_file_tool(server);   /* 注册 file 工具 + 5 条绑定 */
ncl_server_start_ftp(server);            /* 读 bin/ftp.txt 起 FTP 端点，默认 2121 */
```

注册后，`/CONTROLLER/FILE` 上就有 `write/read/ll/mkdir/delete` 五个方法，
协议路径约定：

| 侧 | 路径基准 |
|----|----------|
| 设备（ncl_server_file_tool） | `<root>/uploadFile/<相对路径>`，对端目录树为 `/<sn>/<相对路径>` |
| 客户端（ncl_file_client_tool） | `<cwd>/<sn>/<相对路径>` |

即：**同一个相对路径**（如 `/demo.txt`）在两边各自落到上面两个位置。

#### 客户端（传文件）

```c
#include "nclink/ncl_file.h"

/* 上传：文件必须先放到 <cwd>/<sn>/<相对路径>，再用同样的相对路径调用 */
ncl_mkdir_p(sn);
ncl_file_write_all("V200583BC87/demo.txt", text, strlen(text));
ncl_client_write(client, "/demo.txt");        /* 设备侧落到 uploadFile/demo.txt */

/* 下载：返回本地绝对路径（堆字符串），失败返回 NULL */
char *local = ncl_client_read(client, "/demo.txt");
free(local);

/* 目录列举 */
ncl_ptrvec files;
ncl_ptrvec_init(&files, ncl_file_attribute_release);
ncl_client_ll(client, "/", &files);            /* 相对 uploadFile/ */
for (size_t i = 0; i < ncl_ptrvec_len(&files); i++) {
    const ncl_file_attribute *a = ncl_ptrvec_at(&files, i);
    printf("%s %lld %d\n", a->file_name, a->file_size, a->total_chunks);
}
ncl_ptrvec_free(&files);

ncl_file_client_tool_mkdir(ncl_client_file_tool(client), "/docs");
ncl_file_client_tool_delete(ncl_client_file_tool(client), "/demo.txt");
```

#### 方法调用里的文件参数

协议报文里无法直接携带文件，方法参数与返回值用**标记对象**
`{"@file":"<本地路径>"}` 表示：

```c
/* 工具返回文件：返回标记对象，服务端会自动复制到 <root>/temp/<名字>，
 * 把结果替换成 "/temp/<名字>"，并把键名放进 "fileKeys" */
ncl_json *marker = ncl_json_new_object();
ncl_json_obj_set_string(marker, NCL_FILE_MARKER, "/path/to/local.bin");
*result = ncl_json_new_object();
ncl_json_obj_set(*result, "copy", marker);

/* 客户端发送带文件的调用：keys 与 paths 一一对应 */
const char *keys[]  = { "key" };
const char *paths[] = { "local.bin" };
ncl_client_method_call_file(client, request, keys, paths, 1, 5000, &response);
/* 返回后 data["copy"] 已被替换成本地路径 */
```

#### 其它工具

```c
bool  need = ncl_file_need_compression("a.txt");   /* 文本类为 true */
int   n    = ncl_file_total_chunks(size);          /* 256 KB 一片 */
char *hex  = NULL;
ncl_file_checksum("a.bin", &hex);                  /* SHA-256 十六进制，需 free */
ncl_file_attribute *attr = NULL;
ncl_file_attribute_of("a.bin", "/data", &attr);
ncl_json *json = ncl_file_attribute_to_json(attr); /* 字段顺序按规范固定 */
ncl_file_attribute_free(attr);
```

### 5.11 ncl_schema.h —— 参数校验

实现 JSON Schema **draft-07 子集**：`type`（含数组形式）、`enum`、`const`、
`required`、`properties`、`additionalProperties`、`items`（单 schema 与元组）、
`minItems`/`maxItems`/`uniqueItems`、`minLength`/`maxLength`/`pattern`、
`minimum`/`maximum`/`exclusiveMinimum`/`exclusiveMaximum`/`multipleOf`、
`allOf`/`anyOf`/`oneOf`/`not`/`if-then-else`、内部 `$ref`、常见 `format`。

```c
/* 一次性校验：文档与 schema 都是 JSON 文本 */
ncl_strvec errors;
ncl_strvec_init(&errors);
ncl_json_schema_validate(json_text, schema_text, &errors);
if (errors.len > 0) {
    char *joined = ncl_schema_join_errors(&errors);   /* "[\"#/x: ...\", ...]" */
    ncl_log_error("参数不合法: %s", joined);
    free(joined);
}
ncl_strvec_free(&errors);

/* 复用同一份 schema 多次校验（注册工具时内部就是这么做的） */
char *error = NULL;
ncl_schema *schema = ncl_schema_compile_text(schema_text, strlen(schema_text),
                                             &error);
ncl_schema_validate(schema, value_json, &errors);
ncl_schema_free(schema);

/* 单独使用正则引擎（ECMA 子集，回溯实现） */
ncl_regex *re = ncl_regex_compile("^[A-Za-z_][A-Za-z0-9_]*$", NULL);
bool ok = ncl_regex_search(re, "abc_1", 5);
ncl_regex_free(re);
```

**限制**（有意为之）：不支持 `patternProperties`、`dependencies`、
`propertyNames`、`unevaluatedProperties`、外部 `$ref`，正则不支持反向引用与
前后向断言，惰性量词按贪婪处理。错误文本的措辞不属于协议契约。

---

## 6. 典型任务速查

### 6.1 增加一个数据点

1. **模型**里加一个数据项（`conf/model/nclink.json`）：
   ```json
   {"id":"030003","type":"TEMPERATURE","number":1,"dataType":"FLOAT"}
   ```
   路径随之变为 `/TEMPERATURE@1`（父是设备时前缀清空）。
2. **工具**里实现读写函数，并在 `bindings[]` 里绑定：
   ```c
   {"/TEMPERATURE@1", NCL_OP_GET_VALUE, "readTemp", "plc"},
   {"/TEMPERATURE@1", NCL_OP_SET_VALUE, "writeTemp", "plc"},
   ```
3. 重新 `ncl_server_load_model()` + `ncl_server_register_tool()` + `subscribe()`。

### 6.2 增加一个工具方法（供上位机 methodCall）

```c
static ncl_err my_method(void *inst, const ncl_json *params,
                         ncl_json **result, char **reason) {
    long long speed = 0;
    if (!ncl_json_as_int(ncl_json_obj_get(params, "speed"), &speed)) {
        *reason = ncl_strdup("speed 必须存在且为整数");   /* 进应答 reason */
        return NCL_ERR_INVALID_VALUE;                    /* 应答 code=NG */
    }
    *result = ncl_json_new_string("accepted");
    return NCL_OK;
}

static const ncl_tool_method methods[] = {
    {"setSpeed", my_method,
     "{\"type\":\"object\",\"properties\":{"
     "\"speed\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100}},"
     "\"required\":[\"speed\"]}"},
};
ncl_server_register_tool(server, "motion", inst, methods, 1, NULL, 0);
```

上位机调用 `/motion/setSpeed`；带 `check: true` 时设备只校验参数。

### 6.3 让 HTTP 接口支持文件上传

```c
/* 1. 设备端注册文件工具，客户端上传的文件落在 <root>/uploadFile/<相对路径> */
ncl_server_register_file_tool(server);

/* 2. 自己的 HTTP 路由里转交给 file 工具，或用 /api/schema 里生成的接口 */
static void upload_handler(ncl_http_request *req, ncl_http_response *res,
                           void *user) {
    const char *name = ncl_http_form_field(req, "name");
    if (name == NULL) { ncl_rest_reply_error(res, 400, "缺少 name"); return; }
    ncl_rest_reply(res, 200, ncl_result_success_string("ok"));
}
```

### 6.4 设备主动上报（不用采样通道）

```c
ncl_json *event = ncl_json_new_object();
ncl_json_obj_set_string(event, "key", "ALARM");
ncl_json_obj_set_string(event, "value", "tool_wear");
ncl_server_push_event(server, "/ALARM@1", event);
ncl_json_free(event);
```

### 6.5 让日志更像现场

```c
ncl_log_set_console(true);
ncl_log_set_level(NCL_LOG_DEBUG);   /* 排查时打开 */
```

设备现场建议把 `<root>/log/out.txt` 纳入日志轮转/上报。

### 6.6 接收采样数据

**常规做法**（用库封装，见 5.5「采样」）：订阅通配主题 + 注册回调。

```c
ncl_client_set_sample_handler(client, on_sample, NULL);
ncl_client_subscribe_samples(client, 0);     /* Sample/<sn>/# */
```

**读表头**：报文里的 `paths` 是**数组**，列出这次采集了哪些数据项（与 `data` 按下标对应）：

```c
const ncl_strvec *paths = &msg->as.sample.paths;
for (size_t i = 0; i < ncl_strvec_len(paths); i++) {
    ncl_log_info("第 %u 列: %s", (unsigned)i, ncl_strvec_at(paths, i));
}
/* 需要一行字符串（日志/CSV 表头）时： */
char *line = ncl_message_sample_header(msg, ";");   /* "/STATUS;/AXIS@0/POSITION" */
free(line);
```

**只要某一个通道**：自己订更精确的主题，回调仍是同一个（按主题里的通道 id 分发）。

```c
char *topic = ncl_topic_sample(sn, "ch1");       /* Sample/<sn>/ch1 */
ncl_channel_subscribe(client 对应的 channel, topic, 0);
```

**不用库封装、直接吃 MQTT**：适合一个连接管多台设备、或要把采样转发给别的系统。
此时自己收包、用 `ncl_message_parse()` 解析，主题以 `Sample/` 开头的就是上报：

```c
static void on_mqtt(void *user, const ncl_mqtt_publish *publish) {
    ncl_message *msg;
    if (strncmp(publish->topic, "Sample/", 7) != 0) {
        return;                                  /* 其它主题交给客户端处理 */
    }
    msg = ncl_message_parse(publish->topic, (const char *)publish->payload,
                            publish->payload_len);
    if (msg != NULL && msg->type == NCL_MSG_SAMPLE) {
        forward_to_my_database(publish->topic, &msg->as.sample);
    }
    ncl_message_free(msg);
}

ncl_mqtt_client_subscribe(mqtt, "Sample/+/ch1", 0);   /* 所有设备的 ch1 通道 */
```

**补采/回溯**：采样上报是"推"的模式，历史数据不在协议里。要历史值就自己去
`getValue(path, start, end)` 读区间（见 5.5 请求接口）。

**排错**：

| 现象 | 检查 |
|------|------|
| 一条都收不到 | 设备端 `ncl_server_init_samples()` 是否执行；`uploadInterval` 是否比你的等待时间长（示例设备 2 s）；是否真的订上了（`ncl_client_sample_topic()`） |
| 只收到一部分通道 | 订阅主题是否被写死成单通道（应该用 `Sample/<sn>/#`） |
| 值是 null | 采样项的路径或节点 id 在设备模型里能否解析（设备侧取值失败会填 null） |
| 收不到但事件能收到 | 说明 MQTT 通、采样没起来：看设备日志有没有"采样任务已启动: ch1 -> Sample/..." |

### 6.7 用 HTTP 管理采样通道

设备端的 HTTP 服务（见 5.7）可以直接增删采样通道，返回体就是 `Result`：

```bash
# 起一个通道：模型里没有定义也行，ids 直接给路径（表头）
curl -X POST http://<设备IP>:9008/api/nclinkServer/addSample \
     -H 'Content-Type: application/json' \
     -d '{"request":{"id":"ch1","type":"SAMPLE_CHANNEL",
                    "sampleInterval":1000,"uploadInterval":2000,
                    "ids":[{"id":"/STATUS"},{"id":"/AXIS@0/POSITION"}]}}'

# 停掉
curl -X POST http://<设备IP>:9008/api/nclinkServer/removeSample \
     -H 'Content-Type: application/json' -d '{"id":"ch1"}'

# 只想校验参数、不想真的起通道
curl -X POST http://<设备IP>:9008/api/nclinkServer/addSample \
     -H 'Content-Type: application/json' \
     -d '{"check":true,"request":{…}}'
```

参数不合法时返回 `{"status":false,"data":"NotFoundException"}` 之类的错误名
（错误码表见附录 B），不会留下半启动的通道。

---

## 7. 常见问题与排错

| 现象 | 原因与处理 |
|------|-----------|
| MQTT 连不上，`ncl_mqtt_client_last_error()` 提示等待 CONNACK 超时 | broker 地址/端口/账号不对；`ssl://` 未实现（返回 `NCL_ERR_NOT_SUPPORTED`）；防火墙 |
| 客户端请求全部超时 | 设备端是否 `ncl_server_subscribe()`；SN 是否一致（主题里带 SN）；设备端工具是否已注册（未注册应答 `NG 未找到`） |
| 设备端收不到请求 | 收包回调里是否调用了 `ncl_server_on_message()`；不要自己在收包线程上同步发布应答（会自我死锁，库内部已转线程池） |
| `SET` 返回失败 | 工具方法返回 NCL_OK 但 `*result` 为 NULL 时，应答 code=NG |
| 客户端 `setValue` 返回错误 | 说明设备应答里有 `code=NG` 的项（写失败被拒绝），报文 `reason` 里有原因 |
| 文件上传报 `Error` | 检查三件事：① 本地文件是否放在 `<cwd>/<sn>/<相对路径>`；② 设备端是否起了 FTP（`ncl_server_start_ftp`，端口见 `bin/ftp.txt`）且可达；③ 客户端本机 2323 端口是否被占用 |
| FTP 主动模式连不上 | 默认走主动模式：服务端要能反向连到客户端的监听端口。跨 NAT 时改用被动：`ncl_ftp_client_set_passive(c, true)` |
| `ncl_sn_read()` 每次启动都变 | 根目录是否可写、`bin/sn.txt` 是否被 `/api/cfg/init` 覆盖过（该接口无条件重写，见 4.1） |
| 中文字符串编译报 C4819/C2001 | MSVC 没加 `/utf-8` |
| 端口被占用导致启动失败 | HTTP 9008 / 设备 FTP 2121 / 客户端 FTP 2323；同一进程内不能起两个同端口服务 |
| ASan 报 `use-after-free` | 检查所有权表（4.7）：最常见是保存了事件回调里的 `ncl_message*`，或用了 `ncl_ptrvec_at()` 拿到的借用指针却去 free |
| 程序退出时偶发崩溃 | 释放顺序：先 `ncl_server_free()`（停采样/FTP/线程池任务）→ 再 `ncl_mqtt_client_destroy()` → 最后 `ncl_env_shutdown()`；不要在两个线程同时销毁同一对象 |
| 想看线上报文 | `ncl_log_set_level(NCL_LOG_DEBUG)`；解析失败时 `ncl_message_parse()` 返回 NULL 并记 WARN |
| 想验证参数不合法时的应答 | 发 `check: true` 的 MethodCall，看 `reason` 里的消息列表 |

---

## 8. 许可

本库以 **MIT License** 授权，全文见包内 `LICENSE`：

```
Copyright (c) 2026 huienming
```

- 可以自由使用、修改、分发、商用与再授权，只需在副本或实质部分中保留上述
  版权声明与许可声明。
- 所有源文件都带 `SPDX-License-Identifier: MIT` 头；静态库与文档同样适用。
- 软件按"现状"提供，不附带任何明示或默示担保（详见 `LICENSE` 全文）。

新增源文件请照抄这两行头（脚本类文件放在 shebang 之后）。构建末尾的
`license` 测试套件会逐个检查，缺头直接判失败；也可以手工跑
`tools/check_license.ps1` 检查、`-Fix` 批量补齐。

---

## 附录 A · API 索引

按头文件分组，由 `tools/gen_api_index.py` 从 `include/nclink/*.h` 自动生成（重新生成：`python tools/gen_api_index.py`）。

### `nclink/ncl_client.h`

- `ncl_err (*publish)(ncl_message_channel *self, const char *topic, const ncl_message *message, int qos, const ncl_mqtt_properties *properties);`
- `ncl_err (*subscribe)(ncl_message_channel *self, const char *topic, int qos);`
- `ncl_err (*unsubscribe)(ncl_message_channel *self, const char *topic);`
- `static inline ncl_err ncl_channel_publish(ncl_message_channel *channel, const char *topic, const ncl_message *message, int qos, const ncl_mqtt_properties *properties)`
- `static inline ncl_err ncl_channel_subscribe(ncl_message_channel *channel, const char *topic, int qos)`
- `static inline ncl_err ncl_channel_unsubscribe(ncl_message_channel *channel, const char *topic)`
- `ncl_client *ncl_client_create(const char *sn, ncl_message_channel *channel);` — Create a client for @p sn bound to @p channel (not owned).
- `void ncl_client_free(ncl_client *client);`
- `const char *ncl_client_sn(const ncl_client *client);`
- `ncl_err ncl_client_subscribe(ncl_client *client);` — Subscribe to every response topic for this serial number (QoS 2).
- `ncl_err ncl_client_unsubscribe(ncl_client *client);`
- `void ncl_client_on_message(ncl_client *client, const char *topic, ncl_message *message);` — Deliver an inbound message.
- `ncl_node *ncl_client_root_node(const ncl_client *client);` — Device model accessors (the model is borrowed, not owned).
- `void ncl_client_set_root_node(ncl_client *client, ncl_node *root_node);`
- `char *ncl_client_get_id(ncl_client *client, const char *path);` — Path -> id and id -> path lookups through the device model.
- `char *ncl_client_get_path(ncl_client *client, const char *id);`
- `ncl_err ncl_client_subscribe_events(ncl_client *client, int qos);` — Subscribe to the device's event topic ("Event/<sn>").
- `ncl_err ncl_client_unsubscribe_events(ncl_client *client);`
- `void ncl_client_set_event_handler(ncl_client *client, ncl_client_event_fn fn, void *user);` — Install (or clear, with @p fn == NULL) the event callback.
- `size_t ncl_client_event_count(const ncl_client *client);` — Number of event messages delivered to the callback so far.
- `ncl_err ncl_client_subscribe_samples(ncl_client *client, int qos);` — Subscribe to every sample channel of this device ("Sample/<sn>/#").
- `ncl_err ncl_client_unsubscribe_samples(ncl_client *client);`
- `void ncl_client_set_sample_handler(ncl_client *client, ncl_client_sample_fn fn, void *user);` — Install (or clear, with @p fn == NULL) the sample callback.
- `size_t ncl_client_sample_count(const ncl_client *client);` — Number of sample messages delivered to the callback so far.
- `const char *ncl_client_sample_topic(const ncl_client *client);` — Topic filter used by ncl_client_subscribe_samples(), e.g.
- `ncl_err ncl_client_ping(ncl_client *client, unsigned timeout_ms, ncl_message **out);`
- `ncl_err ncl_client_query(ncl_client *client, ncl_message *request, unsigned timeout_ms, ncl_message **out);`
- `ncl_err ncl_client_set(ncl_client *client, ncl_message *request, unsigned timeout_ms, ncl_message **out);`
- `ncl_err ncl_client_probe(ncl_client *client, unsigned timeout_ms, ncl_message **out);` — Probe query: the response is matched by its response topic.
- `ncl_err ncl_client_probe_set(ncl_client *client, ncl_message *request, unsigned timeout_ms, ncl_message **out);`
- `ncl_err ncl_client_method_call(ncl_client *client, ncl_message *request, unsigned timeout_ms, ncl_message **out);`
- `ncl_err ncl_client_get_value(ncl_client *client, const char *path, unsigned timeout_ms, ncl_json **out);` — Read a single value: *out receives a clone of values[0].
- `ncl_err ncl_client_get_value_range(ncl_client *client, const char *path, int start, int end, unsigned timeout_ms, ncl_json **out);` — Read the values in the index range [start, end].
- `ncl_err ncl_client_get_length(ncl_client *client, const char *path, unsigned timeout_ms, long long *out_length);` — getLength(path, timeout).
- `ncl_err ncl_client_set_value(ncl_client *client, const char *path, ncl_json *value, unsigned timeout_ms);` — setValue(path, value, timeout).
- `ncl_err ncl_client_set_value_index(ncl_client *client, const char *path, ncl_json *value, int index, unsigned timeout_ms);` — setValue(path, value, index, timeout).
- `ncl_err ncl_client_add_sample(ncl_client *client, const ncl_node *config, unsigned timeout_ms);` — addSample(config) / removeSample(id) method calls.
- `ncl_err ncl_client_remove_sample(ncl_client *client, const char *id, unsigned timeout_ms);`
- `bool ncl_client_is_ready(ncl_client *client);` — True when the last operation completed within @p timeout_ms.
- `ncl_err ncl_client_holder_init(const char *server_uri, const char *username, const char *password);` — Initialise the process wide MQTT client.
- `ncl_client *ncl_client_holder_get(const char *sn);` — Fetch (creating on demand) the client for @p sn.
- `bool ncl_client_holder_is_initialised(void);` — True once ncl_client_holder_init() succeeded.
- `ncl_mqtt_client *ncl_client_holder_mqtt(void);` — The underlying MQTT client, for diagnostics.
- `void ncl_client_holder_shutdown(void);` — Disconnect MQTT, drop every client and stop the FTP-less file hook.
- `size_t ncl_client_holder_client_count(void);` — Number of live per-device clients (idle expiry runs on every access).

### `nclink/ncl_codec.h`

- `void ncl_buffer_free(ncl_buffer *buf);`
- `ncl_err ncl_codec_encode_hex(const unsigned char *src, size_t src_len, ncl_buffer *out);` — Hex encode: two upper case characters per input byte.
- `ncl_err ncl_codec_decode_hex(const unsigned char *src, size_t src_len, ncl_buffer *out);` — Hex decode.
- `ncl_err ncl_codec_encode_compress(const unsigned char *src, size_t src_len, ncl_buffer *out);` — Deflate (zlib container) @p src into @p out.
- `ncl_err ncl_codec_decode_compress(const unsigned char *src, size_t src_len, ncl_buffer *out);` — Inflate (zlib container) @p src into @p out.
- `bool ncl_codec_compress_available(void);` — True when the compress codec was built in (-DNCLINK_WITH_ZLIB=ON).
- `ncl_err ncl_codec_encode(ncl_codec_kind kind, const unsigned char *src, size_t src_len, ncl_buffer *out);` — Encode with the codec named by @p kind.
- `ncl_err ncl_codec_decode(ncl_codec_kind kind, const unsigned char *src, size_t src_len, ncl_buffer *out);` — Decode with the codec named by @p kind.

### `nclink/ncl_common.h`

- `const char *ncl_err_name(ncl_err err);` — Stable, human readable name of an error code.
- `char *ncl_strdup(const char *s);` — Heap copy of @p s (NULL safe).
- `char *ncl_strndup(const char *s, size_t len);` — Heap copy of the first @p len bytes of @p s (NUL terminated).
- `ncl_err ncl_asprintf(char **out, const char *fmt, ...);` — printf into a freshly allocated buffer.
- `ncl_err ncl_vasprintf(char **out, const char *fmt, va_list ap);`
- `bool ncl_streq_ignore_case(const char *a, const char *b);` — Case-insensitive ASCII equality.
- `bool ncl_str_starts_with(const char *s, const char *prefix);`
- `bool ncl_str_ends_with(const char *s, const char *suffix);`
- `bool ncl_str_is_empty(const char *s);` — True when @p s is NULL or empty.
- `bool ncl_str_is_blank(const char *s);` — True when @p s is NULL, empty, or contains only whitespace.
- `char *ncl_str_trim_dup(const char *s);` — Duplicate @p s with leading and trailing ASCII whitespace removed.
- `void ncl_free_safe(void *ptr);` — Free a heap pointer and set the variable to NULL.
- `ncl_err ncl_uuid4(char *out, size_t out_len);` — Format a random version 4 UUID into @p out in lower case (8-4-4-4-12).
- `void ncl_strbuf_init(ncl_strbuf *sb);`
- `void ncl_strbuf_free(ncl_strbuf *sb);`
- `void ncl_strbuf_reset(ncl_strbuf *sb);`
- `ncl_err ncl_strbuf_reserve(ncl_strbuf *sb, size_t additional);`
- `ncl_err ncl_strbuf_append(ncl_strbuf *sb, const char *data, size_t len);`
- `ncl_err ncl_strbuf_puts(ncl_strbuf *sb, const char *s);`
- `ncl_err ncl_strbuf_putc(ncl_strbuf *sb, char c);`
- `ncl_err ncl_strbuf_printf(ncl_strbuf *sb, const char *fmt, ...);`
- `char *ncl_strbuf_detach(ncl_strbuf *sb);` — Detach the buffer contents; caller frees.
- `const char *ncl_strbuf_cstr(ncl_strbuf *sb);` — NUL terminated view of the buffer (never NULL for an initialised buffer).
- `void ncl_ptrvec_init(ncl_ptrvec *v, ncl_free_fn free_fn);`
- `void ncl_ptrvec_free(ncl_ptrvec *v);`
- `void ncl_ptrvec_clear(ncl_ptrvec *v);`
- `ncl_err ncl_ptrvec_push(ncl_ptrvec *v, void *item);`
- `ncl_err ncl_ptrvec_push_owned(ncl_ptrvec *v, void *item);`
- `void *ncl_ptrvec_at(const ncl_ptrvec *v, size_t index);`
- `size_t ncl_ptrvec_len(const ncl_ptrvec *v);`
- `void *ncl_ptrvec_take(ncl_ptrvec *v, size_t index);` — Detach ownership of element @p index; slot is removed.
- `void ncl_strvec_init(ncl_strvec *v);`
- `void ncl_strvec_free(ncl_strvec *v);`
- `void ncl_strvec_clear(ncl_strvec *v);`
- `ncl_err ncl_strvec_push(ncl_strvec *v, const char *s);` — Copies @p s; returns NCL_OK / NCL_ERR_NOMEM.
- `const char *ncl_strvec_at(const ncl_strvec *v, size_t index);`
- `size_t ncl_strvec_len(const ncl_strvec *v);`
- `bool ncl_strvec_contains(const ncl_strvec *v, const char *s);`

### `nclink/ncl_config.h`

- `ncl_err ncl_config_init(const char *sn, char **out_sn);` — Create bin/, conf/ and log/ under the install root and write bin/sn.txt,
- `char *ncl_config_get_sn(void);` — The serial number, or NULL when bin/sn.txt is missing.
- `ncl_json *ncl_config_get_model(void);` — The parsed model document.
- `ncl_err ncl_config_set_model(const char *json);` — Write @p json to conf/model/nclink.json.
- `ncl_json *ncl_config_get_driver(void);` — The parsed driver configuration.
- `ncl_err ncl_config_set_driver(const char *json);` — Write @p json to conf/driver/driver.json.
- `ncl_json *ncl_config_get_server_list(void);` — The known servers.
- `ncl_err ncl_config_set_server_list(const char *json);` — Store the server list (JSON array or object).
- `ncl_json *ncl_config_get_mqtt(void);` — {"url":..,"username":..,"password":..} read from conf/mqtt.cfg, or NULL when
- `ncl_err ncl_config_set_mqtt(const ncl_json *config);` — Write the three fields back to conf/mqtt.cfg, one per line.
- `ncl_json *ncl_config_get_ip_conf(void);` — The parsed conf/ipConf.json (same rules as ncl_config_get_model()).
- `ncl_err ncl_config_set_ip_conf(const char *json);` — Write @p json to conf/ipConf.json.

### `nclink/ncl_env.h`

- `void ncl_env_set_root(const char *path);` — Override the installation root.
- `const char *ncl_env_root(void);` — Installation root; defaults to the current working directory.
- `const char *ncl_env_conf_path(void);`
- `const char *ncl_env_run_path(void);`
- `const char *ncl_env_driver_path(void);`
- `const char *ncl_env_log_path(void);`
- `const char *ncl_env_log_file(void);`
- `const char *ncl_env_mqtt_cfg_file(void);`
- `const char *ncl_env_model_file(void);`
- `const char *ncl_env_driver_cfg_file(void);`
- `const char *ncl_env_sn_file(void);`
- `void ncl_env_set_server_list(const char *const *servers, size_t count);` — Known NC-Link servers.
- `size_t ncl_env_server_count(void);`
- `const char *ncl_env_server_at(size_t index);`
- `void ncl_env_shutdown(void);` — Release memory held by the module (call once at shutdown).
- `ncl_err ncl_mqtt_config_read(ncl_mqtt_config *out);` — Read <conf>/mqtt.cfg.
- `void ncl_mqtt_config_free(ncl_mqtt_config *cfg);`
- `char *ncl_sn_read(void);` — Read <root>/bin/sn.txt, generating and persisting a serial number with
- `char *ncl_sn_generate(void);` — Generate a serial number: "V2" followed by nine upper-case hexadecimal
- `bool ncl_path_exists(const char *path);` — True when a file or directory exists.
- `ncl_err ncl_mkdir_p(const char *path);` — Create @p path and any missing parents.
- `ncl_err ncl_file_read_all(const char *path, char **out, size_t *out_len);` — Read a whole file into memory (NUL terminated).
- `ncl_err ncl_file_write_all(const char *path, const void *data, size_t len);` — Write @p data to @p path, creating parents as needed.
- `ncl_err ncl_file_append(const char *path, const void *data, size_t len);` — Append @p data to @p path, creating it (and its parents) when missing.
- `ncl_err ncl_file_copy(const char *src, const char *dst);` — Copy @p src to @p dst, creating the parent directory of @p dst.
- `long long ncl_file_size(const char *path);` — Size of @p path in bytes, or -1 when it cannot be read.
- `int64_t ncl_file_mtime_ms(const char *path);` — Last modification time of @p path in epoch milliseconds, 0 when unknown.
- `bool ncl_path_is_dir(const char *path);` — True when @p path exists and names a directory.
- `ncl_err ncl_path_remove(const char *path);` — Remove @p path; directories are removed recursively.

### `nclink/ncl_file.h`

- `ncl_file_attribute *ncl_file_attribute_new(void);`
- `void ncl_file_attribute_free(ncl_file_attribute *attribute);`
- `void ncl_file_attribute_release(void *attribute);` — ncl_free_fn compatible destructor for ncl_ptrvec.
- `bool ncl_file_attribute_is_dir(const ncl_file_attribute *attribute);` — True when the attribute describes a directory.
- `ncl_json *ncl_file_attribute_to_json(const ncl_file_attribute *attribute);` — Property order: fileName, fileType, fileSize, totalChunks, compressed,
- `ncl_file_attribute *ncl_file_attribute_from_json(const ncl_json *json);`
- `ncl_json *ncl_file_attributes_to_json(const ncl_ptrvec *attributes);` — Serialise a list of attributes into a JSON array.
- `ncl_err ncl_sha256_hex(const void *data, size_t len, char **out_hex);` — SHA-256 of @p len bytes at @p data, lower case hex into a heap string.
- `bool ncl_file_need_compression(const char *file_name);` — True when the extension of @p file_name is compressed on transfer.
- `int ncl_file_total_chunks(long long size);` — ceil(size / NCL_FILE_CHUNK_SIZE), 0 for a size of 0.
- `ncl_err ncl_file_checksum(const char *path, char **out_hex);` — SHA-256 over the contents of @p path.
- `ncl_err ncl_file_attribute_of(const char *path, const char *parent, ncl_file_attribute **out);` — Attribute of the local file or directory @p path: a directory yields type 1,
- `ncl_err ncl_file_attribute_from_entry(const ncl_ftp_entry *entry, const char *parent, ncl_file_attribute **out);` — Attribute of the remote @p entry reported by the peer's FTP server.
- `ncl_err ncl_ftp_info_read(ncl_ftp_response *out);` — Populate from bin/ftp.txt, creating it with the built in defaults (admin /
- `ncl_err ncl_ftp_info_write(const ncl_ftp_response *info);` — Persist to bin/ftp.txt.
- `void ncl_ftp_info_free(ncl_ftp_response *info);`
- `ncl_server_file_tool *ncl_server_file_tool_create(const char *ip, unsigned port, const char *user, const char *password, const char *sn);`
- `void ncl_server_file_tool_free(ncl_server_file_tool *tool);`
- `bool ncl_server_file_tool_detect(ncl_server_file_tool *tool);` — Validate the link with NOOP first, reconnecting when it is gone.
- `void ncl_server_file_tool_disconnect(ncl_server_file_tool *tool);` — Log out and drop the control connection.
- `bool ncl_server_file_tool_write(ncl_server_file_tool *tool, const char *local_path, const char *remote_dir);` — Upload @p local_path into "/<sn><remoteDir>/".
- `char *ncl_server_file_tool_read(ncl_server_file_tool *tool, const char *remote_file_path);` — Download "/<sn>/<remoteFilePath>" to <root>/uploadFile/<remoteFilePath> and
- `ncl_err ncl_server_file_tool_ll(ncl_server_file_tool *tool, const char *remote_dir, ncl_ptrvec *out);` — List @p remote_dir into @p out (initialize with
- `bool ncl_server_file_tool_mkdir(ncl_server_file_tool *tool, const char *remote_dir);` — Create @p remote_dir on the peer.
- `bool ncl_server_file_tool_delete(ncl_server_file_tool *tool, const char *remote_file_path);` — Delete @p remote_file_path, recursive for directories.
- `ncl_file_client_tool *ncl_file_client_tool_create(ncl_client *client);`
- `void ncl_file_client_tool_free(ncl_file_client_tool *tool);`
- `bool ncl_file_client_tool_detect(ncl_file_client_tool *tool);` — Create <cwd>/<sn>.
- `bool ncl_file_client_tool_write(ncl_file_client_tool *tool, const char *local_file_path);` — Upload @p local_file_path with "set" on /CONTROLLER/FILE.
- `char *ncl_file_client_tool_read(ncl_file_client_tool *tool, const char *remote_file_path);` — Download @p remote_file_path with "get_value"; returns the heap local path
- `ncl_err ncl_file_client_tool_ll(ncl_file_client_tool *tool, const char *remote_dir, ncl_ptrvec *out);` — List @p remote_dir with "get_attributes".
- `bool ncl_file_client_tool_mkdir(ncl_file_client_tool *tool, const char *remote_dir);` — Create @p remote_dir with "add".
- `bool ncl_file_client_tool_delete(ncl_file_client_tool *tool, const char *remote_file_path);` — Delete @p remote_file_path with "delete".
- `ncl_err ncl_client_method_call_file(ncl_client *client, ncl_message *request, const char *const *file_keys, const char *const *file_paths, size_t file_count, unsigned timeout_ms, ncl_message **out);` — Method call with file parameters: every entry of @p file_paths is copied to
- `ncl_err ncl_client_write(ncl_client *client, const char *local_file_path);` — Upload @p local_file_path through the file channel installed by
- `char *ncl_client_read(ncl_client *client, const char *remote_file_path);` — Download @p remote_file_path; returns a heap local path or NULL.
- `ncl_err ncl_client_ll(ncl_client *client, const char *remote_dir, ncl_ptrvec *out);` — List @p remote_dir on the peer.
- `void ncl_client_set_file_tool(ncl_client *client, ncl_file_client_tool *tool);` — Install the file channel of @p client, or remove it when @p tool is NULL.
- `ncl_file_client_tool *ncl_client_file_tool(ncl_client *client);`
- `ncl_err ncl_client_holder_start_ftp(void);` — Start the process wide FTP server on 127.0.0.1:2323 rooted at the current
- `void ncl_client_holder_stop_ftp(void);` — Stop the process wide FTP server.
- `ncl_err ncl_client_holder_restart(void);` — Rebuild the process wide client from conf/mqtt.cfg: the running connection is
- `ncl_err ncl_server_register_file_tool(ncl_server *server);` — Register the built in "file" tool on @p server.
- `ncl_err ncl_server_start_ftp(ncl_server *server);` — Start the server side FTP endpoint: read bin/ftp.txt for the port and
- `void ncl_server_stop_ftp(ncl_server *server);` — Stop the server side FTP endpoint.

### `nclink/ncl_ftp.h`

- `void ncl_ftp_entry_free(void *entry);` — Release an entry allocated by ncl_ftp_client_list().
- `bool ncl_ftp_entry_is_dir(const ncl_ftp_entry *entry);` — True when @p entry is a directory.
- `ncl_ftp_client *ncl_ftp_client_create(const char *host, unsigned port, const char *user, const char *password);` — Create a client with the defaults (active mode, 5 s / 30 s timeouts).
- `ncl_ftp_client *ncl_ftp_client_create_ex(const char *host, unsigned port, const char *user, const char *password, const ncl_ftp_client_options *options);`
- `void ncl_ftp_client_free(ncl_ftp_client *client);`
- `const char *ncl_ftp_client_host(const ncl_ftp_client *client);` — Host the client was created for.
- `unsigned ncl_ftp_client_port(const ncl_ftp_client *client);`
- `bool ncl_ftp_client_detect(ncl_ftp_client *client);` — Validate the current connection with NOOP, or (re)connect, log in, switch to
- `void ncl_ftp_client_disconnect(ncl_ftp_client *client);` — Log out and drop the control connection.
- `bool ncl_ftp_client_is_connected(const ncl_ftp_client *client);` — True when a control connection is open (no NOOP is sent).
- `bool ncl_ftp_client_noop(ncl_ftp_client *client);` — Send NOOP over the control connection.
- `int ncl_ftp_client_reply_code(const ncl_ftp_client *client);` — Last reply code seen on the control connection (0 before the first reply).
- `const char *ncl_ftp_client_reply_text(const ncl_ftp_client *client);` — Last reply text, or "" when there is none.
- `void ncl_ftp_client_set_passive(ncl_ftp_client *client, bool passive);` — Switch between active (PORT/EPRT) and passive (PASV/EPSV) transfers.
- `bool ncl_ftp_client_is_passive(const ncl_ftp_client *client);`
- `ncl_err ncl_ftp_client_pwd(ncl_ftp_client *client, char *buf, size_t buf_len);`
- `ncl_err ncl_ftp_client_chdir(ncl_ftp_client *client, const char *path);`
- `ncl_err ncl_ftp_client_mkdir(ncl_ftp_client *client, const char *path);`
- `ncl_err ncl_ftp_client_rmdir(ncl_ftp_client *client, const char *path);`
- `ncl_err ncl_ftp_client_delete(ncl_ftp_client *client, const char *path);`
- `ncl_err ncl_ftp_client_rename(ncl_ftp_client *client, const char *from, const char *to);`
- `ncl_err ncl_ftp_client_size(ncl_ftp_client *client, const char *path, long long *out_size);` — SIZE.
- `ncl_err ncl_ftp_client_mdtm(ncl_ftp_client *client, const char *path, int64_t *out_time);` — MDTM.
- `bool ncl_ftp_client_is_dir(ncl_ftp_client *client, const char *path);` — True when @p path names a directory on the server (CWD probe).
- `ncl_err ncl_ftp_client_store(ncl_ftp_client *client, const char *remote, const void *data, size_t len);` — STOR: upload @p len bytes to @p remote.
- `ncl_err ncl_ftp_client_store_file(ncl_ftp_client *client, const char *remote, const char *local_path);` — Upload @p local_path with STOR as @p remote_name.
- `ncl_err ncl_ftp_client_retrieve(ncl_ftp_client *client, const char *remote, ncl_strbuf *out);` — RETR into @p out.
- `ncl_err ncl_ftp_client_retrieve_file(ncl_ftp_client *client, const char *remote, const char *local_path);` — Download @p remote_path with RETR into @p local_path.
- `ncl_err ncl_ftp_client_list(ncl_ftp_client *client, const char *path, ncl_ptrvec *out);` — LIST @p path into @p out, an ncl_ptrvec of ncl_ftp_entry*.
- `ncl_err ncl_ftp_client_nlst(ncl_ftp_client *client, const char *path, ncl_strvec *out);` — NLST @p path: the entry names only, appended to @p out.
- `ncl_ftp_server *ncl_ftp_server_create(void);` — Create a server with the defaults.
- `ncl_ftp_server *ncl_ftp_server_create_ex(const ncl_ftp_server_options *options);`
- `void ncl_ftp_server_free(ncl_ftp_server *server);` — Stop the server if it is running and release it.
- `ncl_err ncl_ftp_server_start(ncl_ftp_server *server);` — Bind and start accepting sessions.
- `void ncl_ftp_server_stop(ncl_ftp_server *server);` — Stop accepting, close every session and join all threads.
- `bool ncl_ftp_server_is_running(ncl_ftp_server *server);`
- `unsigned ncl_ftp_server_port(const ncl_ftp_server *server);`
- `size_t ncl_ftp_server_session_count(ncl_ftp_server *server);`
- `long long ncl_ftp_server_command_count(ncl_ftp_server *server);`
- `const char *ncl_ftp_server_root(const ncl_ftp_server *server);` — Login root, as an absolute path.

### `nclink/ncl_general.h`

- `const char *ncl_code_to_string(ncl_code code);` — Keyword of @p code: "OK", "NG" or "PENDING".
- `bool ncl_code_parse(const char *text, ncl_code *out);` — Parse "OK" / "NG" / "PENDING" (case sensitive).
- `const char *ncl_operation_to_string(ncl_operation op);` — Keyword of @p op, e.g.
- `bool ncl_operation_parse(const char *text, ncl_operation *out);` — Parse an operation name; false when unknown.
- `bool ncl_check_is_code_valid(const char *code);` — True when @p code is "OK" or "NG" (case insensitive); NULL is invalid.
- `bool ncl_check_is_code_ok(const char *code);` — True when @p code is "OK".
- `bool ncl_check_is_code_ng(const char *code);` — True when @p code is "NG".
- `bool ncl_check_is_pending(const char *code);` — True when @p code is "PENDING".
- `bool ncl_check_is_data_type_valid(const char *data_type);` — True when @p data_type is NULL (no suffix), "LIST" or "HASH".

### `nclink/ncl_http.h`

- `ncl_http_server *ncl_http_server_create(unsigned port);` — Create a server bound to @p port once started (0 = ephemeral).
- `void ncl_http_server_free(ncl_http_server *server);`
- `ncl_err ncl_http_server_route(ncl_http_server *server, const char *method, const char *path, ncl_http_handler handler, void *user);` — Register a handler for an exact path.
- `ncl_err ncl_http_server_start(ncl_http_server *server);` — Bind and start serving.
- `void ncl_http_server_stop(ncl_http_server *server);` — Shut the listener down and join the accept thread.
- `unsigned ncl_http_server_port(const ncl_http_server *server);` — Port actually bound (useful when creating with port 0).
- `size_t ncl_http_server_request_count(const ncl_http_server *server);` — Number of requests handled so far.
- `void ncl_http_server_set_cors(ncl_http_server *server, bool enabled);` — Enable or disable the Access-Control-Allow-Origin: * header (default on).
- `const char *ncl_http_method(const ncl_http_request *request);`
- `const char *ncl_http_path(const ncl_http_request *request);` — Path without the query string.
- `const char *ncl_http_query_string(const ncl_http_request *request);` — Raw query string (without '?'), or "".
- `const char *ncl_http_query(const ncl_http_request *request, const char *name);` — Value of a query parameter, or NULL.
- `const char *ncl_http_header(const ncl_http_request *request, const char *name);` — Header lookup, case insensitive.
- `const char *ncl_http_body(const ncl_http_request *request);`
- `size_t ncl_http_body_len(const ncl_http_request *request);`
- `ncl_json *ncl_http_json_body(const ncl_http_request *request);` — Parse the body as JSON.
- `const char *ncl_http_form_field(const ncl_http_request *request, const char *name);` — Value of a form field in an application/x-www-form-urlencoded body.
- `void ncl_http_set_status(ncl_http_response *response, int status);`
- `void ncl_http_set_header(ncl_http_response *response, const char *name, const char *value);`
- `void ncl_http_reply(ncl_http_response *response, int status, const char *content_type, const char *body, size_t body_len);` — Set the body.
- `void ncl_http_reply_text(ncl_http_response *response, int status, const char *text);`
- `void ncl_http_reply_json(ncl_http_response *response, int status, const ncl_json *json);`
- `const char *ncl_http_status_text(int status);` — Reason phrase for a status code.
- `char *ncl_http_url_decode(const char *value);` — URL-decode @p value into a heap string ('+' becomes a space).
- `char *ncl_http_url_encode(const char *value);` — Percent-encode @p value for use in a query string.

### `nclink/ncl_json.h`

- `ncl_json *ncl_json_new_null(void);`
- `ncl_json *ncl_json_new_bool(bool value);`
- `ncl_json *ncl_json_new_int(long long value);`
- `ncl_json *ncl_json_new_double(double value);`
- `ncl_json *ncl_json_new_string(const char *value);`
- `ncl_json *ncl_json_new_string_len(const char *value, size_t len);`
- `ncl_json *ncl_json_new_array(void);`
- `ncl_json *ncl_json_new_object(void);`
- `ncl_json *ncl_json_clone(const ncl_json *j);`
- `void ncl_json_free(ncl_json *j);`
- `ncl_json_type ncl_json_type_of(const ncl_json *j);`
- `bool ncl_json_is_null(const ncl_json *j);`
- `bool ncl_json_is_number(const ncl_json *j);`
- `ncl_err ncl_json_obj_set(ncl_json *obj, const char *key, ncl_json *value);` — Insert or replace @p key.
- `ncl_err ncl_json_obj_set_string(ncl_json *obj, const char *key, const char *value);`
- `ncl_err ncl_json_obj_set_int(ncl_json *obj, const char *key, long long value);`
- `ncl_err ncl_json_obj_set_double(ncl_json *obj, const char *key, double value);`
- `ncl_err ncl_json_obj_set_bool(ncl_json *obj, const char *key, bool value);`
- `ncl_err ncl_json_obj_set_null(ncl_json *obj, const char *key);`
- `ncl_json *ncl_json_obj_get(const ncl_json *obj, const char *key);`
- `bool ncl_json_obj_has(const ncl_json *obj, const char *key);`
- `size_t ncl_json_obj_len(const ncl_json *obj);`
- `const char *ncl_json_obj_key_at(const ncl_json *obj, size_t index);`
- `ncl_json *ncl_json_obj_val_at(const ncl_json *obj, size_t index);`
- `ncl_err ncl_json_obj_remove(ncl_json *obj, const char *key);`
- `const char *ncl_json_obj_get_string(const ncl_json *obj, const char *key);` — Convenience getters.
- `long long ncl_json_obj_get_int(const ncl_json *obj, const char *key, long long def);`
- `double ncl_json_obj_get_double(const ncl_json *obj, const char *key, double def);`
- `bool ncl_json_obj_get_bool(const ncl_json *obj, const char *key, bool def);`
- `ncl_err ncl_json_arr_push(ncl_json *arr, ncl_json *value);` — Append @p value; ownership transfers to @p arr in every case.
- `ncl_json *ncl_json_arr_get(const ncl_json *arr, size_t index);`
- `size_t ncl_json_arr_len(const ncl_json *arr);`
- `ncl_json *ncl_json_arr_take(ncl_json *arr, size_t index);`
- `const char *ncl_json_as_string(const ncl_json *j);` — String payload, or NULL when @p j is not a JSON string.
- `const char *ncl_json_number_raw(const ncl_json *j);` — Raw number literal as written in the source document.
- `bool ncl_json_as_int(const ncl_json *j, long long *out);` — True when the value is a number, or a string holding a decimal integer.
- `bool ncl_json_as_double(const ncl_json *j, double *out);` — True when the value is a number, or a string holding a finite number.
- `bool ncl_json_as_bool(const ncl_json *j, bool *out);`
- `char *ncl_json_as_text(const ncl_json *j);` — Textual representation of a scalar value, used by the request parsers
- `ncl_json *ncl_json_parse(const char *text, size_t len, ncl_strbuf *err);` — Parse a JSON document.
- `ncl_json *ncl_json_parse_cstr(const char *text, ncl_strbuf *err);`
- `ncl_err ncl_json_write(const ncl_json *j, ncl_strbuf *out);` — Serialise compactly into @p out.
- `char *ncl_json_write_string(const ncl_json *j);` — Serialise into a freshly allocated string.
- `bool ncl_json_equals(const ncl_json *a, const ncl_json *b);` — Deep structural equality (object key order is not significant).
- `ncl_json *ncl_strvec_to_json(const ncl_strvec *v);` — Serialise a string vector (ncl_common.h) as a JSON array of strings.

### `nclink/ncl_logger.h`

- `bool ncl_log_init(const char *log_dir);` — Start writing to <log_dir>/out.txt.
- `void ncl_log_shutdown(void);` — Stop writing to the file and flush.
- `void ncl_log_set_console(bool enabled);` — Enable or disable the stderr mirror (default: enabled).
- `void ncl_log_set_level(ncl_log_level level);` — Minimum level written to the file and console (default: NCL_LOG_INFO).
- `void ncl_log_write(ncl_log_level level, const char *fmt, ...);`
- `void ncl_log_info(const char *fmt, ...);` — Informational line.
- `void ncl_log_error(const char *fmt, ...);` — Error line.
- `void ncl_log_warn(const char *fmt, ...);` — Warning and debug lines; useful for MQTT tracing.
- `void ncl_log_debug(const char *fmt, ...);`

### `nclink/ncl_message.h`

- `const char *ncl_msg_type_name(ncl_msg_type type);`
- `ncl_msg_type ncl_msg_type_from_topic(const char *topic);` — The message kind implied by a topic prefix (NCL_MSG_UNKNOWN when none).
- `ncl_query_request_item *ncl_query_request_item_new(const char *id);`
- `void ncl_query_request_item_free(ncl_query_request_item *item);`
- `bool ncl_query_request_item_is_valid(const ncl_query_request_item *item);`
- `const char *ncl_query_request_item_operation(const ncl_query_request_item *item);`
- `ncl_err ncl_query_request_item_indexes(const ncl_query_request_item *item, long long **out, size_t *count);` — Expands an "indexes" string ("3" or "1-4") into a flat id list.
- `ncl_query_response_item *ncl_query_response_item_new(const char *id);`
- `void ncl_query_response_item_free(ncl_query_response_item *item);`
- `bool ncl_query_response_item_is_valid(const ncl_query_response_item *item);`
- `const char *ncl_query_response_item_operation(const ncl_query_response_item *item);`
- `bool ncl_query_response_item_has_data(const ncl_query_response_item *item);`
- `ncl_json *ncl_query_response_item_data(const ncl_query_response_item *item);`
- `ncl_err ncl_query_response_item_add_value(ncl_query_response_item *item, ncl_json *value);`
- `bool ncl_query_response_item_matches(const ncl_query_response_item *item, const ncl_query_request_item *request);` — True when the response item answers this request item.
- `ncl_set_request_item *ncl_set_request_item_new(const char *id);`
- `void ncl_set_request_item_free(ncl_set_request_item *item);`
- `bool ncl_set_request_item_is_valid(const ncl_set_request_item *item);`
- `const char *ncl_set_request_item_operation(const ncl_set_request_item *item);`
- `ncl_set_response_item *ncl_set_response_item_new(const char *id);`
- `void ncl_set_response_item_free(ncl_set_response_item *item);`
- `bool ncl_set_response_item_is_valid(const ncl_set_response_item *item);`
- `bool ncl_set_response_item_matches(const ncl_set_response_item *item, const ncl_set_request_item *request);`
- `bool ncl_set_response_item_has_error(const ncl_set_response_item *item);` — True when the item reports a failure, i.e.
- `ncl_sample_item *ncl_sample_item_new(void);`
- `ncl_sample_item *ncl_sample_item_clone(const ncl_sample_item *item);`
- `void ncl_sample_item_free(ncl_sample_item *item);`
- `bool ncl_sample_item_is_valid(const ncl_sample_item *item);`
- `ncl_err ncl_sample_item_add_value(ncl_sample_item *item, ncl_json *value);`
- `bool ncl_sample_item_is_same(const ncl_sample_item *a, const ncl_sample_item *b);`
- `const char *ncl_params_operation(const ncl_json *params, const char *fallback);` — Helpers for the derived members shared by the request and response items,
- `bool ncl_params_has(const ncl_json *params, const char *key);`
- `const char *ncl_params_string(const ncl_json *params, const char *key);`
- `bool ncl_params_int(const ncl_json *params, const char *key, long long *out);`
- `ncl_json *ncl_params_get(const ncl_json *params, const char *key);`
- `ncl_err ncl_params_set_string(ncl_json **params, const char *key, const char *value);` — Sets params[key] = string, creating the params object when needed.
- `ncl_err ncl_params_set_int(ncl_json **params, const char *key, long long value);`
- `ncl_err ncl_params_set(ncl_json **params, const char *key, ncl_json *value);` — Sets params[key] = value (ownership transfers).
- `ncl_err ncl_params_append_string(ncl_json **params, const char *key, const char *value);` — Appends a string to the array stored at params[key].
- `ncl_err ncl_params_indexes(const ncl_json *params, long long **out, size_t *count);` — Expands ["3","1-4"] into [3,1,4].
- `ncl_message *ncl_message_new(ncl_msg_type type);`
- `void ncl_message_free(ncl_message *msg);`
- `ncl_err ncl_message_finalise(ncl_message *msg);` — Returns NCL_ERR_INVALID_MESSAGE when the object is not valid, otherwise
- `ncl_err ncl_message_set_message_id(ncl_message *msg, const char *id);` — Sets "@id".
- `ncl_err ncl_message_set_code(ncl_message *msg, const char *code);`
- `ncl_err ncl_message_set_reason(ncl_message *msg, const char *reason);`
- `ncl_err ncl_message_set_open_api_schema(ncl_message *msg, const char *schema);`
- `ncl_err ncl_message_set_version(ncl_message *msg, const char *version);`
- `ncl_err ncl_message_set_device_id(ncl_message *msg, const char *device_id);`
- `ncl_err ncl_message_set_model(ncl_message *msg, ncl_node *model);`
- `ncl_node *ncl_message_take_model(ncl_message *msg);` — Detach the device model carried by a probe message.
- `ncl_err ncl_message_set_method(ncl_message *msg, const char *method);`
- `ncl_err ncl_message_set_params(ncl_message *msg, ncl_json *params);`
- `ncl_err ncl_message_set_token(ncl_message *msg, const char *token);`
- `ncl_err ncl_message_set_check(ncl_message *msg, bool check);`
- `ncl_err ncl_message_set_data(ncl_message *msg, ncl_json *data);`
- `ncl_err ncl_message_set_event(ncl_message *msg, ncl_json *event);`
- `ncl_err ncl_message_set_sample_id(ncl_message *msg, const char *id);`
- `ncl_err ncl_message_set_event_time_ms(ncl_message *msg, int64_t millis);` — Event.time in milliseconds since the epoch.
- `ncl_err ncl_message_set_begin_time(ncl_message *msg, const char *begin_time);`
- `ncl_err ncl_message_set_sample_interval(ncl_message *msg, long long interval);`
- `ncl_err ncl_message_set_upload_interval(ncl_message *msg, long long interval);`
- `ncl_err ncl_message_add_sample_path(ncl_message *msg, const char *path);`
- `ncl_err ncl_message_add_sample_item(ncl_message *msg, ncl_sample_item *item);`
- `ncl_err ncl_message_add_query_request_item(ncl_message *msg, ncl_query_request_item *item);`
- `ncl_err ncl_message_add_query_response_item(ncl_message *msg, ncl_query_response_item *item);`
- `ncl_err ncl_message_add_set_request_item(ncl_message *msg, ncl_set_request_item *item);`
- `ncl_err ncl_message_add_set_response_item(ncl_message *msg, ncl_set_response_item *item);`
- `size_t ncl_message_item_count(const ncl_message *msg);`
- `void *ncl_message_item_at(const ncl_message *msg, size_t index);` — Item accessors; the concrete type depends on the message kind.
- `char *ncl_message_sample_header(const ncl_message *msg, const char *separator);` — "表头" of a Sample message - the list of data items this report collected.
- `bool ncl_message_sample_is_complete(const ncl_message *msg);` — True when a Sample message is ready to hand to a consumer: it carries a
- `bool ncl_sample_item_is_nested(const ncl_sample_item *item);` — 该列是否含数组元素（即是否是亚毫秒批量采样）。
- `size_t ncl_sample_item_value_count(const ncl_sample_item *item);` — 该列的总点数：标量（含 null）算 1，数组算其长度。
- `size_t ncl_message_sample_point_count(const ncl_message *msg);` — 整个报文的总点数（各列应相同，否则报文不完整）。
- `bool ncl_message_is_valid(const ncl_message *msg);` — True when the message satisfies the rules of its kind.
- `bool ncl_message_matches(const ncl_message *response, const ncl_message *request);` — True when @p response answers every item of @p request.
- `bool ncl_message_has_data(const ncl_message *msg);` — Whether a query response carries a "data" member, and that member.
- `ncl_json *ncl_message_get_data(const ncl_message *msg);`
- `ncl_json *ncl_message_to_json(const ncl_message *msg);` — Serialise using the property order of the specification (null entries are
- `char *ncl_message_write_string(const ncl_message *msg);`
- `ncl_message *ncl_message_from_json(ncl_msg_type type, const ncl_json *json);` — Build a message of @p type from its JSON form.
- `ncl_message *ncl_message_parse(const char *topic, const char *payload, size_t payload_len);` — Convenience: infer the type from @p topic then parse @p payload.

### `nclink/ncl_model.h`

- `const char *ncl_node_type_name(ncl_node_type type);`
- `const char *ncl_upload_type_name(ncl_upload_type type);`
- `bool ncl_upload_type_parse(const char *text, ncl_upload_type *out);`
- `ncl_sample_params *ncl_sample_params_new(void);`
- `ncl_sample_params *ncl_sample_params_clone(const ncl_sample_params *p);`
- `void ncl_sample_params_free(ncl_sample_params *p);`
- `bool ncl_sample_params_is_valid(const ncl_sample_params *p);`
- `ncl_json *ncl_sample_params_to_json(const ncl_sample_params *p);`
- `ncl_sample_params *ncl_sample_params_from_json(const ncl_json *j);`
- `ncl_sample_ref *ncl_sample_ref_new(const char *id);`
- `void ncl_sample_ref_free(ncl_sample_ref *ref);`
- `bool ncl_sample_ref_is_valid(const ncl_sample_ref *ref);`
- `char *ncl_sample_ref_path(ncl_sample_ref *ref);` — Path of the referenced data item, including the LIST/HASH suffixes.
- `ncl_node *ncl_node_new(ncl_node_type type);` — Create an empty node of the given kind (fields NULL / vectors empty).
- `void ncl_node_free(ncl_node *node);`
- `ncl_node *ncl_node_clone(const ncl_node *node, bool shallow_children);` — Deep copy.
- `ncl_err ncl_node_set_name(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_id(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_type_name(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_description(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_number(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_data_type(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_mapping(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_value_type(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_source(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_version(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_guid(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_unique_id(ncl_node *node, const char *value);`
- `ncl_err ncl_node_set_settable(ncl_node *node, bool value);`
- `ncl_err ncl_node_set_value(ncl_node *node, ncl_json *value);` — Takes ownership of @p value.
- `ncl_err ncl_node_add_config(ncl_node *parent, ncl_node *config);`
- `ncl_err ncl_node_add_data_item(ncl_node *parent, ncl_node *item);`
- `ncl_err ncl_node_add_component(ncl_node *parent, ncl_node *component);`
- `ncl_err ncl_node_add_device(ncl_node *root, ncl_node *device);`
- `ncl_err ncl_node_add_sample_item(ncl_node *config, ncl_sample_ref *ref);`
- `ncl_err ncl_node_add_child(ncl_node *parent, ncl_node *child);` — Generic child insertion: the parent kind decides which slot @p child goes
- `size_t ncl_node_child_count(const ncl_node *node);`
- `ncl_node *ncl_node_config_at(const ncl_node *node, size_t index);`
- `ncl_node *ncl_node_data_item_at(const ncl_node *node, size_t index);`
- `ncl_node *ncl_node_component_at(const ncl_node *node, size_t index);`
- `ncl_node *ncl_node_device_at(const ncl_node *node, size_t index);`
- `size_t ncl_node_sample_count(const ncl_node *node);`
- `ncl_sample_ref *ncl_node_sample_at(const ncl_node *node, size_t index);`
- `ncl_node *ncl_node_find_by_id(const ncl_node *node, const char *id);` — Depth first lookup by id below @p node.
- `bool ncl_node_is_sample_node(const ncl_node *node);` — True when the node type string equals NCL_NODE_TYPE_SAMPLE_CHANNEL.
- `ncl_err ncl_node_set_path(ncl_node *node, const char *parent_path);` — Recompute the path of @p node and of its subtree, applying the "parent of a
- `const char *ncl_node_path(const ncl_node *node);` — Effective path: the root derives it from its type, others return the stored
- `void ncl_node_build_relations(ncl_node *node);` — Wire the parent pointers through the subtree rooted at @p node.
- `bool ncl_node_is_valid(const ncl_node *node);` — True when the node and its subtree satisfy the model rules.
- `ncl_node *ncl_root_node_parse(const char *text);` — Parse @p text into a root node and run ncl_root_node_post_construct().
- `ncl_node *ncl_root_node_from_json(const ncl_json *json);`
- `ncl_node *ncl_node_from_json(const ncl_json *json, ncl_node_type type);` — Build a node of the requested kind from its JSON representation.
- `ncl_node *ncl_root_node_post_construct(ncl_node *root);` — Fill in parents, paths, sample channel defaults and path/id maps.
- `ncl_json *ncl_node_to_json(const ncl_node *node);` — Serialise a node (and its subtree) using the property order of the
- `char *ncl_node_write_string(const ncl_node *node);`
- `void ncl_node_map_init(ncl_node_map *map);`
- `void ncl_node_map_free(ncl_node_map *map);`
- `ncl_err ncl_node_map_put(ncl_node_map *map, const char *key, ncl_node *node);`
- `ncl_node *ncl_node_map_get(const ncl_node_map *map, const char *key);`
- `size_t ncl_node_map_len(const ncl_node_map *map);`
- `const char *ncl_node_map_key_at(const ncl_node_map *map, size_t index);`
- `ncl_node *ncl_node_map_val_at(const ncl_node_map *map, size_t index);`
- `ncl_err ncl_root_node_path_map(const ncl_node *root, ncl_node_map *out);` — Map every path in the subtree to its node.
- `ncl_err ncl_root_node_id_map(const ncl_node *root, ncl_node_map *out);` — Map every id in the subtree to its node.

### `nclink/ncl_mqtt.h`

- `void ncl_mqtt_properties_init(ncl_mqtt_properties *props);`
- `void ncl_mqtt_properties_free(ncl_mqtt_properties *props);`
- `ncl_err ncl_mqtt_properties_add_user(ncl_mqtt_properties *props, const char *key, const char *value);` — Append a user property, preserving order.
- `const char *ncl_mqtt_properties_get_user(const ncl_mqtt_properties *props, const char *key);` — First value of @p key, or NULL.
- `bool ncl_mqtt_properties_has_user(const ncl_mqtt_properties *props, const char *key);`
- `ncl_err ncl_mqtt_properties_write(const ncl_mqtt_properties *props, ncl_strbuf *out);` — Encode the property set as it appears inside a packet: a variable byte
- `bool ncl_mqtt_properties_block_is_empty(const ncl_strbuf *block);` — True when the encoded block produced by ncl_mqtt_properties_write holds no
- `ncl_err ncl_mqtt_properties_read(const unsigned char *data, size_t len, ncl_mqtt_properties *props, size_t *consumed);` — Decode @p len bytes of properties.
- `ncl_err ncl_mqtt_encode_connect(const ncl_mqtt_connect_options *options, ncl_buffer *out);` — Encode a complete CONNECT packet.
- `void ncl_mqtt_connack_free(ncl_mqtt_connack *connack);`
- `ncl_err ncl_mqtt_decode_connack(const unsigned char *body, size_t len, ncl_mqtt_connack *out);` — Decode a CONNACK body (the bytes after the fixed header).
- `ncl_err ncl_mqtt_encode_publish(const char *topic, const unsigned char *payload, size_t payload_len, int qos, bool retain, bool duplicate, uint16_t packet_id, const ncl_mqtt_properties *properties, ncl_buffer *out);` — Encode a PUBLISH packet.
- `void ncl_mqtt_publish_free(ncl_mqtt_publish *publish);`
- `ncl_err ncl_mqtt_decode_publish(uint8_t header_flags, const unsigned char *body, size_t len, ncl_mqtt_publish *out);`
- `ncl_err ncl_mqtt_encode_ack(ncl_mqtt_packet_type type, uint16_t packet_id, uint8_t reason_code, ncl_buffer *out);` — Encode an acknowledgement packet (PUBACK/PUBREC/PUBREL/PUBCOMP).
- `ncl_err ncl_mqtt_decode_ack(const unsigned char *body, size_t len, uint16_t *packet_id, uint8_t *reason_code);` — Decode an acknowledgement body: packet id plus optional reason/properties.
- `ncl_err ncl_mqtt_encode_subscribe(uint16_t packet_id, const char *topic_filter, int qos, const ncl_mqtt_properties *properties, ncl_buffer *out);` — Encode a SUBSCRIBE packet for a single topic filter.
- `ncl_err ncl_mqtt_encode_unsubscribe(uint16_t packet_id, const char *topic_filter, const ncl_mqtt_properties *properties, ncl_buffer *out);` — Encode an UNSUBSCRIBE packet for a single topic filter.
- `void ncl_mqtt_suback_free(ncl_mqtt_suback *suback);`
- `ncl_err ncl_mqtt_decode_suback(const unsigned char *body, size_t len, ncl_mqtt_suback *out);`
- `ncl_err ncl_mqtt_encode_ping(bool response, ncl_buffer *out);` — Encode PINGREQ (and, with @p response, PINGRESP).
- `ncl_err ncl_mqtt_encode_disconnect(uint8_t reason_code, const ncl_mqtt_properties *properties, ncl_buffer *out);` — Encode DISCONNECT with an optional reason code and properties.
- `void ncl_mqtt_disconnect_free(ncl_mqtt_disconnect *disconnect);`
- `ncl_err ncl_mqtt_decode_disconnect(const unsigned char *body, size_t len, ncl_mqtt_disconnect *out);`
- `size_t ncl_mqtt_varint_encode(uint32_t value, unsigned char out[4]);` — Encode a variable byte integer.
- `size_t ncl_mqtt_varint_decode(const unsigned char *data, size_t len, uint32_t *value);` — Decode a variable byte integer.
- `bool ncl_mqtt_peek_header(const unsigned char *data, size_t len, ncl_mqtt_packet_type *type, uint8_t *flags, uint32_t *remaining_length, size_t *header_len);` — Peek at the fixed header of a buffer.
- `const char *ncl_mqtt_packet_type_name(ncl_mqtt_packet_type type);` — Human readable name of a packet type, used by the logger.
- `void ncl_mqtt_client_options_default(ncl_mqtt_client_options *options);` — Fill @p options with the defaults (clean start, 60 s keep alive, 10 s
- `ncl_mqtt_client *ncl_mqtt_client_create(const ncl_mqtt_client_options *options);`
- `void ncl_mqtt_client_destroy(ncl_mqtt_client *client);` — Stop the reader thread and release every resource.
- `ncl_err ncl_mqtt_client_connect(ncl_mqtt_client *client);` — Open the TCP connection, send CONNECT and wait for CONNACK.
- `ncl_err ncl_mqtt_client_disconnect(ncl_mqtt_client *client);` — Send DISCONNECT and close the socket (no reconnect afterwards).
- `bool ncl_mqtt_client_is_connected(ncl_mqtt_client *client);`
- `ncl_err ncl_mqtt_client_publish(ncl_mqtt_client *client, const char *topic, const void *payload, size_t payload_len, int qos, const ncl_mqtt_properties *properties, unsigned timeout_ms);` — Publish a message.
- `ncl_err ncl_mqtt_client_subscribe(ncl_mqtt_client *client, const char *topic_filter, int qos, unsigned timeout_ms, int *granted_qos);` — Subscribe and wait for the SUBACK.
- `ncl_err ncl_mqtt_client_unsubscribe(ncl_mqtt_client *client, const char *topic_filter, unsigned timeout_ms);` — Unsubscribe and wait for the UNSUBACK.
- `const char *ncl_mqtt_client_last_error(ncl_mqtt_client *client);` — Last transport level error message (never NULL).
- `size_t ncl_mqtt_client_subscription_count(ncl_mqtt_client *client);` — Number of topics the client is subscribed to (including while offline).
- `bool ncl_mqtt_client_wait_connected(ncl_mqtt_client *client, unsigned timeout_ms);` — Wait until the client is connected, up to @p timeout_ms (0 = forever).

### `nclink/ncl_platform.h`

- `int64_t ncl_time_millis(void);` — Milliseconds since the Unix epoch (wall clock).
- `int64_t ncl_time_monotonic_millis(void);` — Monotonic milliseconds, suitable for measuring intervals.
- `void ncl_sleep_millis(unsigned ms);` — Sleep for the given number of milliseconds.
- `bool ncl_random_bytes(void *buf, size_t len);` — Fill @p buf with @p len cryptographically-seeded random bytes.
- `ncl_mutex *ncl_mutex_create(void);`
- `void ncl_mutex_destroy(ncl_mutex *m);`
- `void ncl_mutex_lock(ncl_mutex *m);`
- `void ncl_mutex_unlock(ncl_mutex *m);`
- `ncl_cond *ncl_cond_create(void);`
- `void ncl_cond_destroy(ncl_cond *c);`
- `void ncl_cond_wait(ncl_cond *c, ncl_mutex *m);` — Wait until signalled; @p m must be held and is re-acquired on return.
- `bool ncl_cond_wait_timeout(ncl_cond *c, ncl_mutex *m, unsigned timeout_ms);` — Wait at most @p timeout_ms (0 means "no limit").
- `void ncl_cond_signal(ncl_cond *c);`
- `void ncl_cond_broadcast(ncl_cond *c);`
- `ncl_thread *ncl_thread_start(ncl_thread_fn fn, void *arg);` — Start a thread running @p fn.
- `ncl_err ncl_thread_join(ncl_thread *t);` — Block until the thread finishes, then release the handle.
- `void ncl_thread_detach(ncl_thread *t);` — Release the handle without waiting (the thread keeps running).

### `nclink/ncl_rest.h`

- `ncl_json *ncl_result_success(ncl_json *data);` — Success answer: takes ownership of @p data (may be NULL).
- `ncl_json *ncl_result_failed(const char *message);` — Failure answer carrying @p message as "data".
- `ncl_json *ncl_result_success_bool(bool value);` — Success answer carrying a boolean.
- `ncl_json *ncl_result_success_string(const char *value);` — Success answer carrying a string.
- `void ncl_rest_reply(ncl_http_response *response, int status, ncl_json *data);` — Reply with a Result envelope in one call.
- `void ncl_rest_reply_error(ncl_http_response *response, int status, const char *message);`
- `ncl_err ncl_rest_attach(ncl_http_server *http, ncl_server *server);` — Register the endpoints that are derived from the NC-Link server state:
- `ncl_err ncl_rest_attach_config(ncl_http_server *http);` — Register the device configuration endpoints:

### `nclink/ncl_schema.h`

- `ncl_schema *ncl_schema_compile(const ncl_json *schema, char **error);` — Parse and prepare @p schema.
- `ncl_schema *ncl_schema_compile_text(const char *text, size_t len, char **error);`
- `void ncl_schema_free(ncl_schema *schema);`
- `ncl_err ncl_schema_validate(const ncl_schema *schema, const ncl_json *value, ncl_strvec *errors);` — Validate @p value against @p schema, appending one message per violation to
- `ncl_err ncl_schema_validate_text(const ncl_schema *schema, const char *text, size_t len, ncl_strvec *errors);` — Parse @p text then validate it.
- `ncl_err ncl_json_schema_validate(const char *json_text, const char *schema_text, ncl_strvec *errors);` — Validate a JSON document given as text against a schema given as text.
- `char *ncl_schema_join_errors(const ncl_strvec *errors);` — Render a message list as "[msg1, msg2]".
- `ncl_regex *ncl_regex_compile(const char *pattern, char **error);` — Compile an ECMA-style pattern; NULL with *error set when unsupported.
- `void ncl_regex_free(ncl_regex *regex);`
- `bool ncl_regex_search(const ncl_regex *regex, const char *text, size_t len);` — True when @p regex matches anywhere inside @p text (unanchored search).

### `nclink/ncl_server.h`

- `ncl_server *ncl_server_create(const ncl_server_options *options);`
- `void ncl_server_free(ncl_server *server);`
- `ncl_err ncl_server_set_user_data(ncl_server *server, void *data, ncl_server_cleanup_fn cleanup);` — Attach caller-owned data to the server, released (through @p cleanup when it
- `void *ncl_server_user_data(const ncl_server *server);`
- `const char *ncl_server_sn(const ncl_server *server);` — Serial number this server answers for.
- `ncl_err ncl_server_load_model(ncl_server *server, const char *model_json);` — Parse a model document, run post-construction and take ownership.
- `ncl_err ncl_server_set_model(ncl_server *server, ncl_node *root);` — Install a model constructed by the caller (ownership transfers).
- `ncl_node *ncl_server_model(ncl_server *server);`
- `ncl_err ncl_server_save_model(ncl_server *server);` — Persist the model to the file ncl_env_model_file() names.
- `ncl_err ncl_server_register_tool(ncl_server *server, const char *tool_name, void *instance, const ncl_tool_method *methods, size_t method_count, const ncl_tool_binding *bindings, size_t binding_count);`
- `size_t ncl_server_binding_count(const ncl_server *server);` — Number of "<operation>#<path>" bindings currently registered.
- `size_t ncl_server_operation_count(const ncl_server *server);` — Number of distinct (tool, method) pairs, i.e.
- `const char *ncl_server_operation_tool(const ncl_server *server, size_t index);` — Name of the tool owning operation @p index.
- `const char *ncl_server_operation_method(const ncl_server *server, size_t index);` — Method name of operation @p index.
- `ncl_json *ncl_server_openapi_schema(ncl_server *server, const char *base_url);` — Build the OpenAPI 3.0 document describing the server's operations: one POST
- `char *ncl_server_openapi_schema_json(ncl_server *server, const char *base_url);` — ncl_server_openapi_schema() serialised to a heap JSON string.
- `ncl_message *ncl_server_invoke_query(ncl_server *server, const ncl_message *request);` — Takes ownership of nothing; returns a new message the caller frees.
- `ncl_message *ncl_server_invoke_set(ncl_server *server, const ncl_message *request);`
- `ncl_message *ncl_server_invoke_method_call(ncl_server *server, const ncl_message *request);`
- `ncl_message *ncl_server_check_method_call(ncl_server *server, const ncl_message *request);` — A *dry run* of a method call: the parameters are validated against the
- `ncl_message *ncl_server_dispatch(ncl_server *server, const char *topic, const ncl_message *request);` — Dispatch a parsed request to the matching invoke_* function.
- `ncl_err ncl_server_subscribe(ncl_server *server);` — Subscribe to the six request topics of this serial number.
- `void ncl_server_on_message(ncl_server *server, const char *topic, ncl_message *request);` — Handle one inbound message: process it and publish the response.
- `ncl_err ncl_server_publish(ncl_server *server, const char *topic, const ncl_message *response);` — Publish a response message on @p topic.
- `void ncl_server_set_publish_sink(ncl_server *server, ncl_server_publish_fn fn, void *user);` — Install (or clear) the outbound transport hook after creation.
- `ncl_err ncl_server_add_sample(ncl_server *server, const ncl_node *config);` — Register a sample channel and start its sampling/upload task.
- `ncl_err ncl_server_remove_sample(ncl_server *server, const char *id);` — Stop and remove a sample channel by id.
- `void ncl_server_stop_all_samples(ncl_server *server);` — Stop every sample channel.
- `ncl_err ncl_server_start_sample(ncl_server *server, const ncl_node *config);` — Start a sampling task without checking the configuration first.
- `ncl_err ncl_server_init_samples(ncl_server *server);` — Start a task for every SAMPLE_CHANNEL config of the first device in the
- `size_t ncl_server_sample_count(ncl_server *server);`
- `size_t ncl_server_sample_upload_count(ncl_server *server);` — Number of uploads published so far on the sample topics (diagnostics).
- `ncl_err ncl_server_register_builtin_tool(ncl_server *server);` — Register the built in "nclinkServer" tool (addSample / removeSample).
- `ncl_err ncl_server_push_event(ncl_server *server, const char *event_id, const ncl_json *event);` — Publish an Event message on "Event/<sn>".
- `ncl_err ncl_server_push_event_ex(ncl_server *server, const char *event_id, const ncl_json *event, int64_t time_ms, const char *message_id);` — Push an event, overriding the message time and/or "@id".
- `size_t ncl_server_event_count(ncl_server *server);` — Number of events published so far (diagnostics).

### `nclink/ncl_socket.h`

- `ncl_err ncl_socket_system_init(void);` — Initialise the platform networking stack (idempotent).
- `void ncl_socket_system_shutdown(void);`
- `void ncl_socket_system_release(void);` — Ask for the networking stack to be released once every socket is closed.
- `ncl_socket *ncl_socket_connect(const char *host, unsigned port, unsigned timeout_ms, char *err, size_t err_len);` — Connect to @p host:@p port.
- `ncl_socket *ncl_socket_listen(unsigned port, char *err, size_t err_len);` — Create a listening socket bound to @p port (0 picks an ephemeral port).
- `ncl_socket *ncl_socket_accept(ncl_socket *listener, unsigned timeout_ms);` — Accept one connection; returns NULL on timeout or error.
- `unsigned ncl_socket_local_port(const ncl_socket *s);` — Local port of a bound socket, or 0 when unknown.
- `ncl_err ncl_socket_local_ip(const ncl_socket *s, char *buf, size_t buf_len);` — Local address of @p s as text ("192.168.1.7" or "fe80::1%12"), which is the
- `ncl_err ncl_socket_peer_ip(const ncl_socket *s, char *buf, size_t buf_len);` — Peer address of @p s as text.
- `const char *ncl_net_local_ipv4(void);` — First non-loopback IPv4 address of an up interface: the address a peer on the
- `char *ncl_net_ip_map_json(void);` — Build `{"<interface>":"<ipv4>", ...}` in interface enumeration order.
- `ncl_err ncl_socket_send(ncl_socket *s, const void *data, size_t len);` — Send exactly @p len bytes.
- `int ncl_socket_recv(ncl_socket *s, void *buf, size_t len, unsigned timeout_ms);` — Receive up to @p len bytes.
- `ncl_err ncl_socket_recv_exact(ncl_socket *s, void *buf, size_t len, unsigned timeout_ms);` — Receive exactly @p len bytes, looping over partial reads.
- `void ncl_socket_set_nodelay(ncl_socket *s, bool enable);` — Disable Nagle's algorithm (used by the MQTT client).
- `void ncl_socket_set_keepalive(ncl_socket *s, bool enable);` — Enable TCP keep-alive probes.
- `void ncl_socket_close(ncl_socket *s);`
- `void ncl_socket_shutdown(ncl_socket *s);` — Close the underlying handle without freeing the ncl_socket.
- `ncl_err ncl_socket_parse_url(const char *url, char **host, unsigned *port, bool *tls);` — Parse "tcp://host:port", "mqtt://host:port" or a bare "host:port".

### `nclink/ncl_thread.h`

- `void ncl_thread_pool_options_default(ncl_thread_pool_options *options);` — Fill @p options with the defaults listed above.
- `ncl_thread_pool *ncl_thread_pool_create(const ncl_thread_pool_options *options);`
- `ncl_err ncl_thread_pool_submit(ncl_thread_pool *pool, ncl_thread_fn fn, void *arg);` — Queue @p fn for execution.
- `void ncl_thread_pool_shutdown(ncl_thread_pool *pool, bool wait);` — Stop accepting work; when @p wait is true, join every worker.
- `size_t ncl_thread_pool_pending(const ncl_thread_pool *pool);`
- `int ncl_thread_pool_worker_count(const ncl_thread_pool *pool);`
- `ncl_thread_pool *ncl_thread_service(void);` — Process wide pool, created on first use.
- `void ncl_thread_service_shutdown(void);` — Shut the shared pool down (call once during process teardown).
- `ncl_cache *ncl_cache_create(unsigned ttl_ms, bool expire_after_access, ncl_cache_free_fn free_fn);` — Create a cache.
- `void ncl_cache_free(ncl_cache *cache);`
- `void ncl_cache_clear(ncl_cache *cache);`
- `ncl_err ncl_cache_put(ncl_cache *cache, const char *key, void *value);` — Insert or replace @p key.
- `void *ncl_cache_get(ncl_cache *cache, const char *key);` — Look up @p key; returns NULL when absent or expired.
- `bool ncl_cache_remove(ncl_cache *cache, const char *key);` — Remove @p key; returns true when an entry was removed.
- `void *ncl_cache_take(ncl_cache *cache, const char *key);` — Remove @p key and hand the value to the caller without invoking the cache's
- `size_t ncl_cache_size(ncl_cache *cache);`
- `void ncl_cache_purge_expired(ncl_cache *cache);` — Drop expired entries (also done lazily by get/put).

### `nclink/ncl_topic.h`

- `char *ncl_topic_build(const char *prefix, const char *device_id, const char *client_id);` — Build "<prefix><deviceId>[/<clientId>]".
- `char *ncl_topic_ping(const char *sn);`
- `char *ncl_topic_pong(const char *sn);`
- `char *ncl_topic_probe_query_request(const char *device_id, const char *client_id);`
- `char *ncl_topic_probe_query_response(const char *device_id, const char *client_id);`
- `char *ncl_topic_probe_set_request(const char *device_id, const char *client_id);`
- `char *ncl_topic_probe_set_response(const char *device_id, const char *client_id);`
- `char *ncl_topic_query_request(const char *device_id, const char *client_id);`
- `char *ncl_topic_query_response(const char *device_id, const char *client_id);`
- `char *ncl_topic_set_request(const char *device_id, const char *client_id);`
- `char *ncl_topic_set_response(const char *device_id, const char *client_id);`
- `char *ncl_topic_sample(const char *device_id, const char *client_id);`
- `char *ncl_topic_probe_version(const char *device_id, const char *client_id);`
- `char *ncl_topic_method_call_request(const char *device_id, const char *client_id);`
- `char *ncl_topic_method_call_response(const char *device_id, const char *client_id);`
- `char *ncl_topic_event(const char *device_id, const char *client_id);`
- `char *ncl_topic_edge_get_request(const char *edge_id);`
- `char *ncl_topic_edge_get_response(const char *edge_id);`
- `char *ncl_topic_edge_register(const char *edge_id);`
- `char *ncl_topic_edge_message(const char *edge_id);`
- `char *ncl_topic_register_request(void);`
- `char *ncl_topic_extract_sn(const char *topic);` — Extract the device serial number from an inbound topic: split on '/', and for

## 附录 B · 错误码全表

来自 `nclink/ncl_common.h`。第三列是 `ncl_err_name()` 返回的稳定名称，
便于跨语言比日志。

| 名称 | 值 | `ncl_err_name()` |
|------|----|------------------|
| `NCL_ERR` | (-1) | （通用失败） |
| `NCL_ERR_NOMEM` | (-2) | 内存不足 |
| `NCL_ERR_PARSE` | (-3) | JSON/报文解析失败 |
| `NCL_ERR_TIMEOUT` | (-4) | 请求超时 |
| `NCL_ERR_IO` | (-5) | IoException / 文件失败 |
| `NCL_ERR_NOT_FOUND` | (-6) | 查找失败 |
| `NCL_ERR_EXISTS` | (-7) | 重复条目 |
| `NCL_ERR_NOT_SUPPORTED` | (-8) | 功能未编译 / 未实现 |
| `NCL_ERR_INVALID_ARG` | (-9) | IllegalArgumentException / 调用方传参错误 |
| `NCL_ERR_STATE` | (-10) | 对象不可用 |
| `NCL_ERR_RANGE` | (-11) | 越界 |
| `NCL_ERR_CONNECT` | (-12) | MqttException / 连接失败 |
| `NCL_ERR_CLOSED` | (-13) | 对象已关闭 |
| `NCL_ERR_INVALID_CODE` | (-100) | InvalidCodeException |
| `NCL_ERR_INVALID_DATA_NAME` | (-101) | InvalidDataNameException |
| `NCL_ERR_INVALID_DATA_TYPE` | (-102) | InvalidDataTypException |
| `NCL_ERR_INVALID_DEVICE_ID` | (-103) | InvalidDeviceIdException |
| `NCL_ERR_INVALID_ENCODING` | (-104) | InvalidEncodingException |
| `NCL_ERR_INVALID_ID` | (-105) | InvalidIdException |
| `NCL_ERR_INVALID_INDEX_RANGE` | (-106) | InvalidIndexRangeException |
| `NCL_ERR_INVALID_ITEM` | (-107) | InvalidItemException |
| `NCL_ERR_INVALID_KEY` | (-108) | InvalidKeyException |
| `NCL_ERR_INVALID_MESSAGE` | (-109) | InvalidMessageException |
| `NCL_ERR_INVALID_MESSAGE_ID` | (-110) | InvalidMessageIdException |
| `NCL_ERR_INVALID_MODEL` | (-111) | InvalidModelException |
| `NCL_ERR_INVALID_NODE` | (-112) | InvalidNodeException |
| `NCL_ERR_INVALID_NUMBER` | (-113) | InvalidNumberException |
| `NCL_ERR_INVALID_REQUEST` | (-114) | InvalidRequestException |
| `NCL_ERR_INVALID_TYPE` | (-115) | InvalidTypeException |
| `NCL_ERR_INVALID_VALUE` | (-116) | InvalidValueException |
| `NCL_ERR_INVALID_VERSION` | (-117) | InvalidVersionException |

## 附录 C · 主题前缀一览

来自 `nclink/ncl_topic.h`；除 `Register` 外都带设备 SN。

| 常量 | 值 |
|------|----|
| `NCL_TOPIC_PING_PREFIX` | "Ping/" |
| `NCL_TOPIC_PONG_PREFIX` | "Pong/" |
| `NCL_TOPIC_PROBE_QUERY_REQUEST_PREFIX` | "Probe/Query/Request/" |
| `NCL_TOPIC_PROBE_QUERY_RESPONSE_PREFIX` | "Probe/Query/Response/" |
| `NCL_TOPIC_PROBE_SET_REQUEST_PREFIX` | "Probe/Set/Request/" |
| `NCL_TOPIC_PROBE_SET_RESPONSE_PREFIX` | "Probe/Set/Response/" |
| `NCL_TOPIC_QUERY_REQUEST_PREFIX` | "Query/Request/" |
| `NCL_TOPIC_QUERY_RESPONSE_PREFIX` | "Query/Response/" |
| `NCL_TOPIC_SET_REQUEST_PREFIX` | "Set/Request/" |
| `NCL_TOPIC_SET_RESPONSE_PREFIX` | "Set/Response/" |
| `NCL_TOPIC_SAMPLE_PREFIX` | "Sample/" |
| `NCL_TOPIC_REGISTER_REQUEST` | "Register/Request" |
| `NCL_TOPIC_PROBE_VERSION_PREFIX` | "Probe/Version/" |
| `NCL_TOPIC_METHOD_CALL_REQUEST_PREFIX` | "Method/Call/Request/" |
| `NCL_TOPIC_METHOD_CALL_RESPONSE_PREFIX` | "Method/Call/Response/" |
| `NCL_TOPIC_EVENT_PREFIX` | "Event/" |
| `NCL_TOPIC_EDGE_GET_REQUEST_PREFIX` | "Edge/Get/Request/" |
| `NCL_TOPIC_EDGE_GET_RESPONSE_PREFIX` | "Edge/Get/Response/" |
| `NCL_TOPIC_EDGE_REGISTER_PREFIX` | "Edge/Register/" |
| `NCL_TOPIC_EDGE_MESSAGE_PREFIX` | "Edge/Message/" |

## 附录 D · 安装根目录布局

```
<root>/
  bin/sn.txt              设备序列号
  bin/ftp.txt             FTP 端口与账号（设备端 FTP 端点用）
  conf/mqtt.cfg           url/username/password
  conf/model/nclink.json  数据模型
  conf/driver/*.json      驱动配置
  conf/ipConf.json        网络配置
  conf/server.json        服务器列表
  log/out.txt             日志（10 MB 轮转）
  uploadFile/             设备侧文件镜像（相对路径的基准）
  temp/                   文件通道的临时交换目录
  <sn>/                   客户端侧文件镜像（相对路径的基准）
```
