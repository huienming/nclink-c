# NC-Link C 实现 · 发布包说明

版本 **3.0.0**（GB/T 41970-2022 协议 3.0.0）
本包为 **零第三方依赖** 的 C11 静态库，交付内容为**头文件 + 两个平台的预编译库 +
使用手册 + 示例程序**；不含实现源码（需要源码请见第 5 节）。

## 1. 包内清单

```
include/nclink/*.h                     21 个公共头文件（全部对外 API）
lib/windows-x64-msvc/nclink_core.lib   Windows x64 静态库（MSVC，Release）
lib/linux-x86_64-gcc/libnclink_core.a  Linux x86_64 静态库（gcc，-O2）
MANUAL.md / MANUAL.docx                使用手册（Word 版由 md 生成，内容一致）
README.md                              工程概览与测试清单
CHANGELOG.md                           版本变更记录
RELEASE.md                             本文件
LICENSE                                 MIT 许可全文
examples/                              两个示例程序（设备端 / 客户端）及其 CMakeLists
SHA256SUMS.txt                         包内每个文件的 SHA-256
```

实现源码（`src/`）与 19 个测试套件（`tests/`）不在本包内，见第 5 节；
手册第 3 章另有一份最小可用示例代码，可直接抄进你的工程。

## 2. 平台与 ABI

| 项目 | Windows | Linux |
|------|---------|-------|
| 库文件 | `nclink_core.lib`（静态） | `libnclink_core.a`（静态） |
| 编译器 | MSVC 14.44.35207（VS 2022 Build Tools） | gcc 13.4.0 (Debian bookworm) |
| 目标 | x64 | x86_64 |
| 编译选项 | `/W4 /utf-8 /O2`，Release，**/MD（动态 CRT）** | `-std=c11 -O2 -Wall -Wextra` |
| 依赖 | 系统库 `ws2_32`、`iphlpapi`（源码内已带 `#pragma comment`） | `-lpthread`（glibc） |
| 第三方 | 无（zlib 可选） | 无（zlib 可选） |

**ABI 提示**：Windows 库是 `/MD` 构建。你的工程若使用 `/MT` 或与 14.4x 不兼容的
MSVC 版本，请用包内源码重新编译；Linux 库请用 glibc 2.31+ 且 ABI 兼容的
gcc/clang 链接（如需 musl，也请自行重编）。

## 3. 验证状态

| 项 | 结果 |
|----|------|
| Windows 编译 | 零警告（`/W4 /utf-8`） |
| Windows 测试 | 19/19 通过；ASan（`/fsanitize=address`）19/19 |
| Linux 编译 | 零警告（`-Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes`） |
| Linux 测试 | 19/19 通过（同一批测试源码） |
| 稳定性 | Windows Release 连跑 8 轮、ASan 12 轮无失败 |
| 示例 | 设备端与客户端两个示例已实测对跑：模型交换、读写、参数校验、文件传输、事件推送 |

测试套件：json、common、topic、model、message、codec、thread、mqtt、mqtt_client、
client、server、http、rest、config、ftp、file、schema、event、license。

## 4. 在你的工程里使用

### 4.1 Windows（MSVC）

```bat
cl /nologo /W4 /utf-8 /MD /Iinclude ^
   your_app.c lib\windows-x64-msvc\nclink_core.lib ws2_32.lib iphlpapi.lib
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

包内 `examples/` 有两个可直接编译的程序，用预编译库链接即可：

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
| TLS | MQTT 的 `ssl://`/`tls://` 未实现，返回 `NCL_ERR_NOT_SUPPORTED`；需要时接 OpenSSL/mbedTLS 或平台 TLS |
| 压缩编解码 | 默认关闭；开启需 zlib（`NCLINK_WITH_ZLIB=ON`） |
| 驱动层 | Modbus RTU、串口、Q0/Q1 继电器接口未实现（按需求排除） |
| 边缘接口 | `Edge/*` 主题未实现（暂不使用） |
| JSON Schema | 校验器为 draft-07 子集，不支持 `patternProperties`/`dependencies`/外部 `$ref` 等 |
| FTP | 实现 RFC 959/2389 子集（覆盖 NC-Link 文件通道用到的命令与两种数据连接模式） |
| POSIX 分支 | 已在 gcc 13.4 + glibc 验证；musl、FreeBSD 等未验证 |

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
