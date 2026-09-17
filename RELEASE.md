# NC-Link C 实现 · 发布包说明

版本 **3.1.0**（实现 GB/T 41970-2022 协议 3.0.0）
本包为 **零第三方依赖** 的 C11 静态库，交付内容为**头文件 + 两个平台的预编译库 +
使用手册 + 示例程序**；不含实现源码（需要源码请见第 5 节）。

## 1. 包内清单

```
include/nclink/*.h                     公共头文件（全部对外 API）
lib/windows-x64-msvc/nclink_core.lib   Windows x64 静态库（MSVC，Release）
lib/windows-x86-msvc/nclink_core.lib   Windows x86（32 位）静态库（MSVC，Release）
lib/windows-x64-msvc-tls/…             x64 + TLS（ssl://，OpenSSL 静态链接，无 DLL 依赖）
lib/windows-amd64-mingw/libnclink_core.a  Windows x64 静态库（mingw-w64，供 Go/cgo 链接）
lib/linux-x86_64-gcc/libnclink_core.a  Linux x86_64 静态库（gcc，-O2）
lib/linux-x86_64-gcc-tls/…             同上，但启用了 TLS（ssl://，链接 -lssl -lcrypto）
examples/*.c, *.cpp, CMakeLists.txt    两个示例程序的源码（设备端 / 客户端）
examples/bin/windows-x64-msvc/*.exe    **编好的示例可执行文件**（x64、MSVC Release）
examples/bin/windows-x86-msvc/*.exe    同上，32 位
examples/bin/linux-x86_64-gcc/*        Linux 版示例可执行文件（gcc 13 + glibc）
bindings/go/                           Go 绑定源码（cgo，链接上面的静态库）
bindings/csharp/                       C# 绑定源码（客户端 + 设备端 + HTTP/REST；netstandard2.0 / .NET 8 / .NET Framework 4.7.2 三目标，含设备端示例与自检）
bindings/java/                         Java 绑定源码（JNI，Java 8 字节码，无第三方依赖；客户端 + 设备端 + HTTP/REST）
bindings/python/                       Python 绑定源码（ctypes，只用标准库；客户端 + 设备端 + HTTP/REST）
bindings/native/                       三种托管绑定共用的原生垫片（C#/Java/Python）
MANUAL.md / MANUAL.docx                使用手册（Word 版由 md 生成，内容一致）
README.md                              工程概览与测试清单
CHANGELOG.md                           版本变更记录
RELEASE.md                             本文件
LICENSE                                MIT 许可全文
SHA256SUMS.txt                         包内每个文件的 SHA-256
```

实现源码（`src/`）与 22 个测试套件（`tests/`）不在本包内，见第 5 节；
手册第 3 章另有一份最小可用示例代码，可直接抄进你的工程。

## 2. 平台与 ABI

| 项目 | Windows x64 | Windows x86（32 位） | Linux |
|------|--------------|------------------------|-------|
| 库文件 | `nclink_core.lib`（静态） | `nclink_core.lib`（静态） | `libnclink_core.a`（静态） |
| 编译器 | MSVC 14.44.35207（`vcvars64`） | 同一套 MSVC（`vcvars32`） | gcc 13.4.0 (Debian bookworm) |
| 目标 | x64 | Win32 / x86 | x86_64 |
| 编译选项 | `/W4 /utf-8 /O2`，Release，**/MD（动态 CRT）** | 同左 | `-std=c11 -O2 -Wall -Wextra` |
| 依赖 | 系统库 `ws2_32`、`iphlpapi`、`winmm`（源码内已带 `#pragma comment`） | 同左 | `-lpthread`（glibc） |
| 第三方 | 无（zlib 可选） | 无（zlib 可选） | 无（zlib 可选） |

**x86（32 位）说明**：库、示例、测试与 x64 是同一套源码，行为一致；目前**只出非 TLS
版**——32 位 OpenSSL 静态库本机没有，要 TLS 就用 `-OpenSslRoot` 指到自编的 32 位
OpenSSL，再跑 `-Arch x86 -Tls`。

**ABI 提示**：Windows 库是 `/MD` 构建（x64 与 x86 都是），包内示例可执行文件同样是
`/MD`，运行需要 **VC++ 2015-2022 运行库**（多数机器已有；没有就装 `vc_redist.x64.exe`
/ `vc_redist.x86.exe`）。你的工程若使用 `/MT` 或与 14.4x 不兼容的 MSVC 版本，请用包里
源码重新编译；Linux 库与示例请用 glibc 2.31+ 且 ABI 兼容的 gcc/clang 链接（如需 musl，
也请自行重编）。
MSVC 版本，请用包内源码重新编译；Linux 库请用 glibc 2.31+ 且 ABI 兼容的
gcc/clang 链接（如需 musl，也请自行重编）。

## 3. 验证状态

| 项 | 结果 |
|----|------|
| Windows 编译 | x64 与 x86 均零警告（`/W4 /utf-8 /O2`，MSVC 14.44.35207） |
| Windows 测试 | 22/22 通过：x64 Release、x86 Release、x64 TLS 三套各自 22/22 |
| 内存检查 | ASan（`/fsanitize=address`）连跑 10 轮 22/22 |
| 断开握手 | 客户端断开前先收干净在途字节再 FIN（避免 RST 吞掉 DISCONNECT），`mqtt_client` 套件由 40 次里 10 次失败 → 40/40 通过 |
| 测试并发提示 | 套件之间用固定端口（FTP 2323/3131 等）与相对路径，**同一构建目录里别并发跑两份 ctest**，否则互相抢端口/文件 |
| Linux 编译 | 零警告（gcc 13.4.0，`-Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes`） |
| Linux 测试 | 22/22 通过（含 TLS 套件，OpenSSL 3.0.20） |
| 示例实跑 | 包内三种产物（Windows x64 / Windows x86 / Linux x86_64）都与 **EMQX 5.8.9** 对跑通过：模型交换、读写、参数校验、文件传输、事件推送、两个采样通道（1 s 状态；1 ms 采样 / 100 ms 上报的功率振动，共 12 列，主轴两路传感器）。窗口节奏实测：Linux ≈118 ms 一条；Windows ≈222 ms 一条（短等待走高精度计时器，1 ms 等待实测 1.56 ms，见手册 4.5）；Linux 下给设备端发 SIGTERM 也能优雅退出（退出码 0） |
| broker 互操作 | `tests/test_broker` 对 EMQX 5.8.9 实测 **44 项检查、0 失败**：QoS 0/1/2、通配订阅、40 KB 报文、退订、空闲保活、会话顶替、重连后订阅恢复（`tools/interop.sh` 可在 Docker 里同时跑 EMQX 与 Mosquitto） |
| x86（32 位） | 库 / 示例 / 测试全部通过；产物 PE 头 Machine = 0x014c（i386），与 x64 同一套源码、同一套编译选项 |
| TLS | Windows（MSVC + OpenSSL 3.0.18 静态链接）与 Linux（gcc + OpenSSL 3.0.20）都编过并 22/22 通过；**x86 暂未出 TLS 版** |
| 托管绑定自检 | C# 98 项（`bindings/csharp/tests/Nclink.SelfTest`，net472 与 net8.0 各跑一遍）、Java 99 项、Python 36 项，全部 0 失败；覆盖客户端、设备端（工具注册 / 采样通道 / 事件 / 离线 dispatch / 自研传输）与 HTTP/REST 端点（OpenAPI、swagger-ui、工具端点、配置端点、自定义路由），都不需要 broker |
| 绑定对真 broker | `NCLINK_TEST_BROKER=tcp://host:port` 打开的可选用例（设备端 + 客户端同进程，报文真的过 MQTT）：C# 130 项、Java 11 项、Python 42 项，对 **Mosquitto 2** 与 **EMQX 5.8.9** 各跑一遍都 0 失败（probe、路径绑定读写、methodCall、采样、事件、文件通道上传下载与带文件参数的方法调用） |
| 文件通道 | 托管绑定：上传 / 列目录 / 下载 / 建目录 / 删文件 / 带文件参数的方法调用（含"工具返回文件"的反向）都实测通过；跨语言也过一遍：C 客户端示例对 **C# 设备端示例** 的 `上传 /demo.txt` → `文件回读路径` → `远端文件 demo.txt (14 字节)` 全通 |
| 跨语言互读 | C# 设备端 ← C 客户端 / Python 客户端（Mosquitto 与 EMQX）、C# 客户端 ← Java 设备端（`GET`、采样、事件；`SET /STATUS` 按对端模型拒绝）、C# 与 Java 设备端的 REST 端点实测（`/api/schema`、`/swagger-ui`、工具端点、自定义路由） |
| HTTP/REST 端点实跑 | C# 设备端示例（离线 + REST）与 Java / Python 设备端示例都挂上了端点：`GET /api/schema`、`GET /swagger-ui`、`POST /api/<工具>/<方法>`、`GET /api/cfg/*` 与自定义路由实测通过 |
| Go 绑定用的 mingw 库 | 本次发布**未带**（本机没有 mingw 工具链）；需要时按 `tools/stage-go-libs.sh` 里的命令行自编 |

测试套件：json、common、topic、model、message、codec、thread、mqtt、mqtt_client、
client、server、http、rest、config、ftp、file、schema、event、license、broker、
tls、cpp（broker 需要真实 broker，`tools/interop.sh` 一键起，默认跳过）。

## 4. 在你的工程里使用

### 4.1 Windows（MSVC）

```bat
:: x64
cl /nologo /W4 /utf-8 /MD /Iinclude ^
   your_app.c lib\windows-x64-msvc\nclink_core.lib ws2_32.lib iphlpapi.lib

:: 32 位（Win32）：换成 x86 的目录即可
cl /nologo /W4 /utf-8 /MD /Iinclude ^
   your_app.c lib\windows-x86-msvc\nclink_core.lib ws2_32.lib iphlpapi.lib
```

`/utf-8` 不可省（库的日志与设备描述是 UTF-8，而且你自己的源码里往往也有中文）。

### 4.2 Linux（gcc/clang）

```sh
gcc -std=c11 -O2 -Wall -Iinclude your_app.c lib/linux-x86_64-gcc/libnclink_core.a -lpthread
```

### 4.3 CMake 工程

```cmake
add_library(nclink_core STATIC IMPORTED)
set_target_properties(nclink_core PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/lib/linux-x86_64-gcc/libnclink_core.a")
target_include_directories(your_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(your_app PRIVATE nclink_core)
if(WIN32)
    target_link_libraries(your_app PRIVATE ws2_32 iphlpapi)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(your_app PRIVATE Threads::Threads)
endif()
```

### 4.4 先跑示例（推荐）

包内已经带好**编好的示例可执行文件**，拿到就能跑（Windows 版要 VC++ 2015-2022
运行库；Linux 版要 glibc 2.31+）：

```powershell
examples\bin\windows-x64-msvc\ncl_device_demo.exe D:\sim           # 一直运行，Ctrl+C 退出
examples\bin\windows-x64-msvc\ncl_client_demo.exe tcp://127.0.0.1:1883 <设备SN> 8
examples\bin\windows-x86-msvc\ncl_device_demo.exe D:\sim           # 32 位版
```

```sh
./examples/bin/linux-x86_64-gcc/ncl_device_demo /tmp/sim
./examples/bin/linux-x86_64-gcc/ncl_client_demo tcp://127.0.0.1:1883 <设备SN> 8
```

要自己编的话，按下面来（源码与 CMakeLists 也在包里）：

```sh
# Linux
gcc -std=c11 -O2 -Iinclude examples/ncl_device_demo.c \
    lib/linux-x86_64-gcc/libnclink_core.a -lpthread -o ncl_device_demo
gcc -std=c11 -O2 -Iinclude examples/ncl_client_demo.c \
    lib/linux-x86_64-gcc/libnclink_core.a -lpthread -o ncl_client_demo

./ncl_device_demo <安装根目录> 20        # 设备端：起服务、采样、发事件
./ncl_client_demo tcp://<broker>:1883 <设备SN> 8   # 客户端：读值/写值/校验/文件/采样/事件
```

```bat
:: Windows
cl /nologo /W4 /utf-8 /MD /Iinclude examples\ncl_device_demo.c ^
   lib\windows-x64-msvc\nclink_core.lib ws2_32.lib iphlpapi.lib
```

两个示例覆盖了「连接 → probe 取模型 → 读值 → 写值 → 参数校验 → 采样（含亚毫秒批量）
→ 事件 → 文件传输」全流程；手册第 3 章另有一份最小示例，可直接抄进你的工程。

## 5. 源码与自行重新编译

本发布包**不含实现源码**（`src/`）；示例程序在包内，测试套件与构建脚本需要从
工程仓库获取（`src/`、`tests/`、`tools/` 与 `CMakeLists.txt`、`build.ps1`、
`build-linux.sh`），然后：

```powershell
.\build.ps1                # Windows：CMake + Ninja + ctest
```

```sh
./build-linux.sh           # Linux：gcc + ar，无需 cmake；末尾自动跑全部测试
```

打包脚本本身支持两种发布形态：

```powershell
.\tools\make_release.ps1                # 默认：头文件 + 库 + 文档 + 示例（本包）
.\tools\make_release.ps1 -WithSource    # 额外带上 src/tests/tools 与构建脚本
```

从源码手工编译时的三个要点（详见手册 2.3）：`-Iinclude -Isrc`、
`-D_POSIX_C_SOURCE=200809L`（Linux）、`/utf-8`（MSVC）。

可选开关：`-DNCLINK_WITH_ZLIB=ON` 启用 zlib 压缩编解码；
`-DNCLINK_BUILD_TESTS=OFF` / `-DNCLINK_BUILD_EXAMPLES=OFF` 精简构建。

## 6. 已知限制

| 项 | 说明 |
|----|------|
| TLS | 可选：`-DNCLINK_WITH_TLS=ON`（或 `NCL_WITH_TLS=1 ./build-linux.sh`）链接 OpenSSL 后即支持 `ssl://`；包内 `lib/*-tls/` 就是这两份，默认的两个库仍零依赖、对 `ssl://` 返回 `NCL_ERR_NOT_SUPPORTED` |
| TLS 运行时依赖 | Linux 用系统 `libssl.so.3` / `libcrypto.so.3`；**Windows 版是 OpenSSL 静态链接**（`no-shared no-asm`），除 MSVC 运行库外无额外 DLL |
| 压缩编解码 | 默认关闭；开启需 zlib（`NCLINK_WITH_ZLIB=ON`） |
| 驱动层 | Modbus RTU、串口、Q0/Q1 继电器接口未实现（按需求排除） |
| 边缘接口 | `Edge/*` 主题未实现（暂不使用） |
| JSON Schema | 校验器为 draft-07 子集，不支持 `patternProperties`/`dependencies`/外部 `$ref` 等 |
| FTP | 实现 RFC 959/2389 子集（覆盖 NC-Link 文件通道用到的命令与两种数据连接模式） |
| POSIX 分支 | 已在 gcc 13.4 + glibc 验证；musl、FreeBSD 等未验证 |
| x86 的 TLS | 32 位只出非 TLS 版：要用 `ssl://` 得自编 32 位 OpenSSL 静态库，再 `-Arch x86 -Tls -OpenSslRoot <dir>` |
| Go 绑定的 mingw 库 | 包内未含 `lib/windows-amd64-mingw/`（本次机器上没有 mingw）；Windows 上跑 Go 绑定前按 `tools/stage-go-libs.sh` 自编 |

## 7. 校验

```sh
sha256sum -c SHA256SUMS.txt      # Linux
certutil -hashfile <文件> SHA256  # Windows，或 PowerShell Get-FileHash
```

## 8. 许可

本包（头文件、静态库、文档与示例）以 **MIT License** 授权，全文见 `LICENSE`：

```
Copyright (c) 2026 huienming
```

再分发或嵌入到自己的产品时，只需保留版权声明与许可声明；软件按"现状"提供，
不附带任何担保。

包内每个头文件与示例的首行都是 `SPDX-License-Identifier: MIT`，单文件复制
出去也不会丢许可信息；源码仓库侧由测试套件 `license` 强制检查。
