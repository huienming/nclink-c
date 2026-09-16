# NC-Link Core (C 实现)

NC-Link 协议核心的 C 语言实现，覆盖国家标准
**GB/T 41970-2022《智能工厂数控机床互联接口规范》** 的协议 3.0.0。

- 语言：C11，不依赖第三方库（zlib 可选）
- 构建：CMake ≥ 3.16，MSVC 2019+ / GCC / Clang
- 平台：Windows、Linux、macOS

自下而上分为协议核心层（JSON、消息、模型、主题、编解码）、传输层（MQTT 5.0、
FTP、TCP）、以及客户端与服务端模块，全部已完成并有测试覆盖。

## 目录结构

```
nclink-c/
├── CMakeLists.txt
├── build.ps1              # Windows 一键配置+编译+测试
├── include/nclink/        # 公共头文件（对外 API）
│   ├── ncl_common.h       # 错误码、字符串、缓冲区、容器
│   ├── ncl_platform.h     # 平台抽象（时间、线程、互斥量、条件变量）
│   ├── ncl_json.h         # JSON DOM（解析/序列化/访问器）
│   ├── ncl_general.h      # 常量、Code/Operation 枚举、校验工具
│   ├── ncl_topic.h        # MQTT 主题构造
│   ├── ncl_model.h        # 设备数据模型（节点树、路径、采样绑定）
│   ├── ncl_message.h      # 全部 NC-Link 消息类型
│   ├── ncl_codec.h        # 十六进制 / zlib 编解码
│   ├── ncl_mqtt.h         # MQTT 5.0 报文编解码
│   ├── ncl_socket.h       # 跨平台 TCP 套接字
│   ├── ncl_client.h       # NC-Link 客户端 API（单设备客户端 / 进程级管理器）
│   ├── ncl_server.h       # NC-Link 服务端（工具注册、请求分发、采样管理）
│   ├── ncl_http.h         # HTTP/1.1 服务端（路由、请求解析、应答）
│   ├── ncl_rest.h         # REST 层：应答封装、/api/schema、Swagger 页面
│   ├── ncl_config.h       # 设备配置：SN、模型、驱动、服务器列表、mqtt.cfg
│   ├── ncl_thread.h       # 线程池与 TTL 缓存
│   ├── ncl_logger.h       # 日志
│   ├── ncl_env.h          # 运行环境、conf/mqtt.cfg、sn.txt
│   ├── ncl_ftp.h          # FTP 服务端与客户端（自研，无第三方依赖）
│   ├── ncl_file.h         # 文件传输：文件属性、校验和、FTP 文件工具、file 工具
│   └── ncl_schema.h       # JSON Schema 校验（draft-07 子集）与正则引擎
├── src/core/              # JSON、字符串、日志、环境、线程、平台
├── src/general/           # 常量、主题
├── src/message/           # 消息与消息项
├── src/model/             # 数据模型
├── src/codec/             # 编解码
├── src/mqtt/              # MQTT 5.0 报文层
├── src/client/            # 客户端与进程级客户端管理器
├── src/server/            # 服务端
├── src/http/              # HTTP/1.1 服务端基础层
├── src/rest/              # REST 应答封装与 schema/UI 端点
├── src/config/            # 设备配置文件读写
├── src/ftp/               # FTP 协议两端（RFC 959/2389 子集）
├── src/file/              # 文件属性/SHA-256/FTP 文件工具/临时目录交换
├── src/schema/            # JSON Schema 校验器 + 正则引擎
└── tests/                 # 单元测试 + 协议黄金样本
```

## 构建与测试

> 集成与使用请看 [MANUAL.md](MANUAL.md)（使用手册：构建参数、模块 API、
> 典型任务、排错表、API 索引）。版本变更见 [CHANGELOG.md](CHANGELOG.md)。
> Word 版手册是同目录的 `MANUAL.docx`，由 `tools/md_to_docx.py` 从
> `MANUAL.md` 生成（改完 markdown 重新生成即可，`tools/check_docx.py`
> 与 `tools/compare_docx_md.py` 用来核对没有掉内容）。

### Windows（MSVC + Ninja/CMake）

```powershell
.\build.ps1                 # 配置 + 编译 + 运行全部测试
.\build.ps1 -Clean          # 先清空 build 目录
.\build.ps1 -Arch x86 -BuildDir build-x86   # 32 位（Win32）：库 + 示例 + 测试
```

`build.ps1` 会自动定位 Visual Studio Build Tools 自带的 CMake 与 Ninja，
并调用 `vcvars64.bat` 准备编译环境。

### 手工构建（任意平台）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Linux（已验证：gcc 13.4，22/22 测试通过）

没有 CMake 也能编（只需要 gcc/binutils 与 sh）：

```bash
./build-linux.sh                 # 产出 build-linux/libnclink_core.a + 示例 + 跑全部测试
CC=clang ./build-linux.sh out    # 换编译器/输出目录
```

用 CMake 时同样可以：

```bash
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j && ctest --test-dir build-linux --output-on-failure
```

自行手工编译（不用上面两个脚本）时注意两点：`-Iinclude -Isrc`，以及
`-D_POSIX_C_SOURCE=200809L`（`-std=c11` 会隐藏 `strdup`/`getaddrinfo`/
`localtime_r`/`pthread_*` 等 POSIX 接口）。

### 与真实 broker 的互操作验证

套件之间用固定端口（FTP 2323/3131 等）与相对路径：**别在同一个构建目录里并发跑两份
ctest**，否则会互相抢端口/文件，表现为偶发失败（单跑稳定通过）。

`tests/test_broker.c` 需要真实 broker，默认跳过；用 Docker 一键跑
EMQX 与 Mosquitto（各自监听 18830 / 18831，不动你本机 1883 上的 broker）：

```bash
./tools/interop.sh               # 两个 broker 都跑
./tools/interop.sh emqx          # 只跑一个
```

也可以手工指定任意 broker：

```bash
NCL_TEST_MQTT_BROKER=tcp://host:1883 ./build-linux/bin/test_broker
NCL_TEST_MQTT_BROKER=tcp://host:1883 .\build\tests\ncl_test_broker.exe   # Windows
```

### TLS（可选的 MQTT over ssl://）

默认构建**零依赖、不含 TLS**；需要 `ssl://` 时用 OpenSSL 打开（可选，不影响默认交付）：

```bash
cmake -S . -B build-tls -DNCLINK_WITH_TLS=ON     # CMake 路线
NCL_WITH_TLS=1 ./build-linux.sh build-linux-tls   # 免 cmake 路线
.\build.ps1 -Tls -BuildDir build-tls              # Windows：自动找 OpenSSL
```

Linux 链接时加 `-lssl -lcrypto`（包内 `lib/linux-x86_64-gcc-tls/` 就是这份）；
Windows 需要 OpenSSL 3 的**静态库**（`OPENSSL_ROOT_DIR`、vcpkg 或自编
`no-shared`，`-OpenSslRoot <dir>` 可显式指定），配 `-DOPENSSL_USE_STATIC_LIBS=ON`
即静态链入、**运行时不带任何 OpenSSL DLL**（包内 `lib/windows-x64-msvc-tls/` 是这份）。
客户端选项：`tls_ca_file`（PEM 信任库，NULL 用系统信任库）、`tls_verify_peer`
（默认 true，校验链与主机名）、`tls_server_name`（SNI/校验名，默认取 URL 主机）、
`tls_client_cert` / `tls_client_key`（双向认证，可选）。

### 发布包

```powershell
.\build.ps1                                    # 1. Windows 静态库 + 示例 exe + 测试
.\build.ps1 -Tls -BuildDir build-tls           #    （可选）Windows TLS 版
.\build.ps1 -Arch x86 -BuildDir build-x86  #    （可选）Windows 32 位
# 2. Linux 静态库与示例（任选其一；TLS 版加 NCL_WITH_TLS=1 与 libssl-dev）
docker run --rm -v ${PWD}:/work -w /work gcc:13 bash -lc "sh build-linux.sh build-linux"
./build-linux.sh            # 或直接在 Linux 机器上
# 3. 组装（会带上 build/ 与 build-linux/bin 里编好的示例可执行文件）
.\tools\make_release.ps1 -Version 3.1.0
```

产物：`dist/nclink-core-c-<版本>/`（头文件 + Windows x64/x86 与 Linux 静态库 + 文档 +
示例源码与**编好的示例可执行文件** + 语言绑定 + `SHA256SUMS.txt`）与同名 `.zip`；
包内说明见 [RELEASE.md](RELEASE.md)。

### 构建选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `NCLINK_BUILD_TESTS` | `ON` | 构建单元测试 |
| `NCLINK_WITH_ZLIB` | `OFF` | 启用 zlib 压缩编解码（CompressEncoder/Decoder） |
| `NCLINK_WITH_MQTT` | `ON` | 构建 MQTT 传输层（关掉后只剩纯协议层，便于嵌入式裁剪） |

## 语言绑定

C++ 封装在 `include/nclink/ncl.hpp`（header-only，RAII + 异常）；四个语言绑定都在
`bindings/`，其中 C# / Java / Python 三种托管绑定**共用同一份原生垫片**
`bindings/native/nclink_shim.c`——它把 C API 摊平成"不透明句柄 + 标量 + UTF-8 文本"，
托管侧不依赖 C 结构体的内存布局：

| 绑定 | 目录 | 构建与自检 |
|------|------|------------|
| Go（cgo） | `bindings/go/` | `sh tools/stage-go-libs.sh` 后 `cd bindings/go && go test ./...` |
| C#（P/Invoke，net472 + net8.0） | `bindings/csharp/` | `.\bindings\native\build-shim.ps1` 后 `dotnet build bindings/csharp/samples/Nclink.Demo.Cli -c Release` |
| Java（JNI，Java 8 字节码） | `bindings/java/` | `.\bindings\java\build.ps1`（native + javac + 自检） |
| Python（ctypes，只用标准库） | `bindings/python/` | `python -m unittest discover -s bindings/python/tests`（19 项） |

每个目录的 `README.md` 里都有用法、内存/线程规则与示例输出，Java / Python 还给了
对着设备端示例跑的端到端命令。

## 快速上手

```c
#include "nclink/ncl_message.h"
#include "nclink/ncl_topic.h"
#include "nclink/ncl_model.h"

/* 1. 组装查询请求：GET /STATUS */
ncl_message *req = ncl_message_new(NCL_MSG_QUERY_REQUEST);
ncl_message_set_message_id(req, "msg-1");

ncl_query_request_item *item = ncl_query_request_item_new("/STATUS");
ncl_params_set_string(&item->params, "operation", "get_value");
ncl_message_add_query_request_item(req, item);

char *wire = ncl_message_write_string(req);
/* {"@id":"msg-1","ids":[{"id":"/STATUS","params":{"operation":"get_value"}}]} */
free(wire);
ncl_message_free(req);

/* 2. 解析对端报文（按主题推断类型） */
ncl_message *res = ncl_message_parse(
        "Query/Response/V203243111F",
        "{\"@id\":\"msg-1\",\"values\":[{\"id\":\"/STATUS\",\"code\":\"OK\","
        "\"values\":[42]}]}", 0 /* 0 = 以 NUL 结尾 */);
if (ncl_message_has_data(res)) {
    long long value = 0;
    ncl_json_as_int(ncl_message_get_data(res), &value);   /* 42 */
}
ncl_message_free(res);

/* 3. 加载设备模型并查询路径 */
char *text = NULL;
ncl_file_read_all("conf/model/nclink.json", &text, NULL);
ncl_node *root = ncl_root_node_parse(text);
const char *path = ncl_node_path(ncl_node_device_at(root, 0));
/* "/NC_LINK_ROOT/PLC" */
ncl_node_free(root);
free(text);
```

### 连服务器：完整客户端

```c
#include "nclink/ncl_client.h"

/* 初始化（内部创建 MQTT 连接，clientId 为随机 UUID，cleanStart，保活 60s） */
if (ncl_client_holder_init("tcp://iot.hz2025.com:1883", "admin", "123456") != NCL_OK) {
    /* 连接失败，ncl_client_holder_mqtt() 上可查具体原因 */
}

/* 取得某台设备的客户端（首次调用会订阅该设备的全部响应主题） */
ncl_client *dev = ncl_client_holder_get("V203243111F");

/* 读一个值 */
ncl_json *value = NULL;
if (ncl_client_get_value(dev, "/STATUS", 5000, &value) == NCL_OK) {
    long long v = 0;
    ncl_json_as_int(value, &v);
    ncl_json_free(value);
}

/* 写一个值 */
ncl_client_set_value(dev, "/STATUS", ncl_json_new_int(7), 5000);

/* 探测设备模型并装入客户端，随后可按路径/ID 互查 */
ncl_message *probe = NULL;
if (ncl_client_probe(dev, 5000, &probe) == NCL_OK) {
    ncl_client_set_root_node(dev, ncl_message_take_model(probe)); /* 所有权转移 */
    ncl_message_free(probe);
    char *id = ncl_client_get_id(dev, "/STATUS");   /* "030001" */
    free(id);
}

/* 方法调用：添加/删除采样通道 */
ncl_client_add_sample(dev, config_node, 5000);
ncl_client_remove_sample(dev, "ch1", 5000);

ncl_client_holder_shutdown();
```

### 提供服务：服务端

#### HTTP 接口

挂上 REST 层后即可获得两个端点：

```c
#include "nclink/ncl_http.h"
#include "nclink/ncl_rest.h"

ncl_http_server *http = ncl_http_server_create(9008);
ncl_rest_attach(http, srv);        /* GET /api/schema、GET /swagger-ui */
ncl_http_server_start(http);
```

- `GET /api/schema` 返回 OpenAPI 3.0 文档（`openapi`/`info`/`servers`/`paths`），
  每个已注册操作对应一条 `POST /<工具>/<方法>`；
- `GET /swagger-ui` 返回内置浏览页（读 `/api/schema` 列出全部操作），
  也可把 `/api/schema` 直接填进任意 OpenAPI 客户端；
- 业务应答统一用 `Result` 封装：`ncl_result_success(data)` →
  `{"status":true,"data":...}`，`ncl_result_failed(msg)` → `{"status":false,"data":"..."}`，
  无数据时按 NON_NULL 省略 `data`。

另外 `Ping` 的应答 `Pong` 携带该 OpenAPI 文档。

#### 采样与上报

模型里的 `SAMPLE_CHANNEL` 会被自动启动：

```c
ncl_server_init_samples(srv);        /* 扫描首个设备的 SAMPLE_CHANNEL 并启动 */
ncl_server_add_sample(srv, config);  /* 运行时新增：启动采样线程 */
ncl_server_remove_sample(srv, "ch1");/* 停止并回收 */
ncl_server_stop_all_samples(srv);
```

每个通道一个任务：每 `sampleInterval` 对每个采样项查询一次，累计
`uploadInterval / sampleInterval` 次后向 `Sample/<sn>/<通道id>`
发布一条 `Sample` 报文。

服务端把「模型路径」映射到 C 函数，通过显式注册建立
（绑定键形如 `<operation>#<path>`）：

```c
#include "nclink/ncl_server.h"

/* 1. 一个“工具”：一组方法 */
static ncl_err get_status(void *inst, const ncl_json *params,
                          ncl_json **result, char **reason) {
    (void)inst; (void)params; (void)reason;
    *result = ncl_json_new_int(42);   /* NULL 表示无值 → 应答 code=NG */
    return NCL_OK;
}
static ncl_err set_status(void *inst, const ncl_json *params,
                          ncl_json **result, char **reason) {
    long long v = 0;
    (void)inst; (void)reason;
    ncl_json_as_int(ncl_json_obj_get(params, "value"), &v);
    /* ...写入设备... */
    *result = ncl_json_new_bool(true);
    return NCL_OK;
}

static const ncl_tool_method methods[] = {
    {"getValue", get_status},
    {"setValue", set_status},
};
static const ncl_tool_binding bindings[] = {
    /* 路径取自模型：数据项直接挂在设备下时路径为 "/<type>" */
    {"/STATUS", NCL_OP_GET_VALUE, "getValue", NULL},
    {"/STATUS", NCL_OP_SET_VALUE, "setValue", NULL},
};

/* 2. 装载模型、注册工具、订阅请求主题 */
ncl_server_options opts = { 0 };
opts.sn = "V203243111F";
opts.mqtt = mqtt_client;          /* 已连接的 ncl_mqtt_client */
opts.model_json = model_text;     /* conf/model/nclink.json 内容，可为 NULL */
ncl_server *srv = ncl_server_create(&opts);
ncl_server_register_tool(srv, "plcTool", &tool, methods, 2, bindings, 2);
ncl_server_register_builtin_tool(srv);   /* /nclinkServer/addSample 等 */
ncl_server_subscribe(srv);

/* 3. 在 MQTT 回调里交给服务端：内部转交线程池，
 *    避免在收包线程上阻塞发布应答 */
void on_message(void *user, const ncl_mqtt_publish *p) {
    ncl_server *server = user;
    ncl_message *req = ncl_message_parse(p->topic,
                                         (const char *)p->payload,
                                         p->payload_len);
    if (req != NULL) {
        ncl_server_on_message(server, p->topic, req);
    }
}
```

#### 文件传输（`File/*`）

方法参数与返回值里的文件用标记对象 `{"@file": "<本地路径>"}` 表示。服务端只要
注册 `file` 工具，数据面（FTP）与控制面（MQTT）就自动配合。

```c
#include "nclink/ncl_file.h"

/* 服务端（设备） */
ncl_server_register_file_tool(srv);      /* write/read/ll/mkdir/delete */
ncl_server_start_ftp(srv);               /* 读 bin/ftp.txt，默认端口 2121 */

/* 工具返回文件：用标记对象，服务端会自动复制到 <root>/temp/<名字> 并
 * 把结果替换成 "/temp/<名字>"，键名进入 "fileKeys" */
ncl_json *marker = ncl_json_new_object();
ncl_json_obj_set_string(marker, NCL_FILE_MARKER, "/path/to/local.bin");
*result = ncl_json_new_object();
ncl_json_obj_set(*result, "copy", marker);

/* 客户端 */
ncl_client_holder_init("tcp://broker:1883", "admin", "123456");
ncl_client *c = ncl_client_holder_get("V203243111F");

/* 上传：把本地文件送到设备（走 /CONTROLLER/FILE + FTP） */
ncl_client_write(c, "/data/report.txt");          /* 落在设备的 uploadFile/ 下 */

/* 下载：取回设备侧的文件 */
char *path = ncl_client_read(c, "/data/report.txt");

/* 方法调用里的文件参数：keys 与 paths 一一对应 */
const char *keys[]  = { "key" };
const char *paths[] = { "/tmp/local.bin" };
ncl_client_method_call_file(c, request, keys, paths, 1, 5000, &response);
```

`ncl_ftp_server` / `ncl_ftp_client` 也可以单独使用（同样零依赖）：

```c
ncl_ftp_server_options opt = { 0 };
opt.port = 2323;
opt.root = "/srv/share";      /* 登录根；".." 无法越出 */
opt.user = "admin";
opt.password = "123456";
opt.allow_write = true;
ncl_ftp_server *ftp = ncl_ftp_server_create_ex(&opt);
ncl_ftp_server_start(ftp);
```

#### 事件推送（`Event/*`）

```c
#include "nclink/ncl_server.h"

/* 设备侧：发布一条事件到 Event/<sn> */
ncl_json *event = ncl_json_new_object();
ncl_json_obj_set_string(event, "key", "PART_COUNT");
ncl_json_obj_set_int(event, "value", 12);
ncl_json_obj_set_int(event, "oldValue", 11);
ncl_server_push_event(srv, "030002", event);   /* time/@id 自动补齐 */
ncl_json_free(event);

/* 对端：订阅并处理 */
static void on_event(ncl_client *c, const char *topic,
                     const ncl_message *msg, void *user) {
    /* msg 在回调返回后即被释放，不要保存指针 */
    ncl_log_info("event %s", ncl_json_obj_get_string(msg->as.event.event, "key"));
}
ncl_client_subscribe_events(client, 2);
ncl_client_set_event_handler(client, on_event, NULL);
```

#### 方法调用的参数校验（`check`）

工具方法可以声明参数 JSON Schema（draft-07 子集）。当请求带 `check: true` 时，
服务端**只校验不执行**，不调用工具方法：

```c
static const ncl_tool_method methods[] = {
    {"setSpeed", set_speed,
     "{\"type\":\"object\",\"properties\":{"
     "\"speed\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100}},"
     "\"required\":[\"speed\"]}"},
};
/* 请求 check=true、params={"speed":500} → NG
 *   reason = [#/speed: expected maximum: 100, found 500] */
```

也可以直接调用 `ncl_server_check_method_call(server, request)`，或者用
`ncl_json_schema_validate(json, schema, &errors)` 独立校验任意 JSON 文档。

## 设计要点

1. **零依赖**。JSON、线程池、TTL 缓存、主题路由、编解码、MQTT、HTTP、
   FTP 两端、SHA-256 与 JSON Schema 校验全部自带实现，
   仅在使用压缩编解码时才需要 zlib。工业现场部署无需包管理器。
2. **内存所有权显式化**。所有 `*_new`/`*_free` 成对出现；集合元素通过
   析构回调自动释放（`ncl_ptrvec` / `ncl_strvec`）。
3. **错误码**。19 条协议域校验规则各有一个 `NCL_ERR_INVALID_*`，
   `ncl_err_name()` 返回稳定的文本名称。所有会失败的接口统一返回 `ncl_err`。
4. **字节级兼容**。JSON 字段顺序、`@id` 命名、空值省略规则、路径计算规则
   都按规范固定；`tests/data/model_nclink.json` 是协议黄金样本，测试要求
   **往返序列化字节完全一致**。
5. **单一节点结构**。设备/组件/数据项/配置项/采样通道共用一个带 `type`
   判别字段的 `ncl_node`，避免层层继承与强制转换。
6. **显式工具注册**。工具方法、参数类型与参数 JSON Schema 都由调用方显式
   声明（schema 在注册时编译一次），对外表现为 `<operation>#<path>` 绑定键、
   methodCall 的 `check` 语义，以及由注册表生成的 OpenAPI 3.0 文档。

## 测试

| 测试 | 覆盖内容 |
|------|----------|
| `json` | JSON 解析/序列化、转义、数字格式、深克隆与相等 |
| `common` | 错误码命名、字符串工具、缓冲区、指针/字符串容器 |
| `topic` | 全部主题构造函数、从主题提取序列号的规则 |
| `model` | 模型文件往返字节一致、路径计算（含组件/数据项的 `number`）、采样通道绑定、映射表 |
| `message` | 全部 18 种消息的线格式、校验规则、匹配规则、按主题解析 |
| `codec` | 十六进制编解码（含非法输入）、zlib 往返（可选） |
| `thread` | 线程池（5/10/100 + CallerRuns）、单例服务、TTL 缓存过期策略 |
| `mqtt` | MQTT 5.0 报文：变长整数、CONNECT/CONNACK、PUBLISH、ACK 系列、SUBSCRIBE/SUBACK、PING、DISCONNECT、属性块（按 OASIS 规范逐字节校验） |
| `mqtt_client` | MQTT 客户端端到端（内置假 broker，可接受多次连接）：连接/保活、QoS 0/1/2 状态机（含 PUBREL 段）、入站消息投递与应答、退订、断开、连接失败，**断线自动重连 + 订阅恢复（并断言恢复不阻塞接收线程）**、**服务器 DISCONNECT 0x8E 停止重连** |
| `client` | 客户端全链路：管理器初始化、按 SN 分配客户端、getValue/getLength/setValue（含索引与区间）、probe 装载模型、路径/ID 互查、addSample/removeSample、**采样订阅 `Sample/<sn>/#` 与回调（含通道/周期/多值解析、未注册处理器丢弃）**、请求超时（内置假 NC-Link 服务器） |
| `server` | 服务端全链路：模型装载与后构造、工具/路径绑定、Query/Set/MethodCall 分发、probe 返回模型、Ping→Pong、addSample/removeSample（经 MQTT 往返验证） |
| `server`（采样） | 采样通道启停、按 `sampleInterval` 采集、按 `uploadInterval` 聚合上报，校验 `Sample/<sn>/<通道id>` 报文结构与取值 |
| `http` | HTTP 服务：真实 socket 往返验证状态行/头部/正文、查询参数与 URL 解码、JSON 与表单正文、404/405/500、通配路由、CORS 预检、URL 编解码与状态文本 |
| `rest` | 应答封装（`{status,data}`、空值省略）、OpenAPI 3.0 文档生成（info/servers/paths、每个操作一个 POST）、Pong 携带 schema、`/api/schema` 与 `/swagger-ui` 端点 |
| `config` | 配置读写与对应 REST 接口：初始化/SN、模型/驱动/IP 配置的读写往返、`mqtt.cfg` 三字段往返、服务器列表、缺失文件与不支持方法（404/405）、隔离的临时安装根目录 |
| `ftp` | FTP 两端互相验证：登录/鉴权失败、主动与被动两种数据连接、STOR/RETR 二进制往返、LIST/NLST、MKD/RMD/DELE/RNFR-RNTO、SIZE/MDTM、NOOP 保活、路径无法越出登录根、连接计数与停止清理 |
| `file` | 文件传输全链路：SHA-256 标准向量、文件属性的字段顺序与往返、`needCompression`/`totalChunks`、`bin/ftp.txt` 往返、FTP 文件工具对真实 FTP 服务的写/列/建目录/下载/删除、`/CONTROLLER/FILE` 工具经协议往返（客户端上传→设备落地→回传）、methodCall 的 `@file` 标记与 `fileKeys` 替换、设备端 FTP 端点 |
| `schema` | 自带正则引擎（字面量/字符类/分组/选择/锚点/量词/转义）、JSON Schema draft-07 子集（类型、required、properties、additionalProperties、items 元组与逐项、长度、数值上下界与 multipleOf、enum/const、allOf/anyOf/oneOf/not/if-then-else、`$ref`、format）、错误消息与列表形式的聚合 |
| `event` | Event 消息线格式与往返校验、事件主题构造、`ncl_server_push_event` 的发布与参数校验、客户端事件订阅/回调/未注册处理器丢弃、methodCall 的 `check` 语义（校验消息、`参数数量不匹配`、`没有找到方法`、不执行工具）、文件工具的参数 schema |
| `license` | `LICENSE` 存在且完整、每个源文件都带 `SPDX-License-Identifier: MIT` 头（缺一个就失败），`tools/check_license.ps1 -Fix` 可批量补齐 |
| `broker` | **可选套件**：对真实 broker（EMQX / Mosquitto，`tools/interop.sh` 一键起）验证 CONNECT/SUBSCRIBE/PUBLISH 的 QoS 0/1/2、通配订阅、40 KB 报文、退订、空闲保活、会话被顶替（0x8E）与显式重连后订阅恢复；不设 `NCL_TEST_MQTT_BROKER` 时自动跳过 |
| `tls` | **可选套件**：内置 TLS 服务端（OpenSSL）+ MQTT over `ssl://`——握手、CONNECT/SUBSCRIBE、8 KB 报文跨 TLS 记录、服务端推送；证书校验（正确 CA 通过、陌生 CA 拒绝、主机名不符拒绝）与 `verify_peer=false` 模式。未启用 TLS 的构建自动跳过 |

## 许可

**MIT License**，全文见 [LICENSE](LICENSE)。

```
Copyright (c) 2026 huienming
```

所有源文件（`include/`、`src/`、`tests/`、`examples/`、`tools/`）都带
`SPDX-License-Identifier: MIT` 头，复制单个文件出去时许可信息不会丢。
静态库（`nclink_core.lib` / `libnclink_core.a`）与文档同样以 MIT 授权；
引用时请保留版权声明。
