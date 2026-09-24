# NC-Link C 实现 · 发布包说明

版本 **3.6.0**（实现 GB/T 41970-2022 协议 3.0.0）
本包为 **零第三方依赖** 的 C11 静态库，交付内容为**头文件 + 两个平台的预编译库 +
使用手册 + 示例程序**，头文件与库都含**厂商协议客户端**那一层；不含实现源码（需要源码
请见第 5 节）。

## 1. 包内清单

```
include/nclink/*.h                           公共头文件（全部对外 API）
include/nclink/clients/*.h                   厂商协议客户端（FOCAS / 新代 / Modbus / MC / FINS /
                                       S7 / KND / MELDAS / LSV2 / MTConnect）的公共头文件，
                                       与核心头文件同一个根：`#include <nclink/clients/focas.h>`
lib/windows-x64-msvc/nclink_core.lib   Windows x64 静态库（MSVC，Release）
lib/windows-x86-msvc/nclink_core.lib   Windows x86（32 位）静态库（MSVC，Release）
lib/windows-x64-msvc-tls/…             x64 + TLS（ssl://，OpenSSL 静态链接，无 DLL 依赖）
lib/windows-amd64-mingw/*.a            Windows x64 静态库（mingw-w64 GCC 13.2.0 编，供 Go/cgo 链接；含非 TLS 与 `libnclink_core_tls.a`）
lib/linux-x86_64-gcc/libnclink_core.a  Linux x86_64 静态库（gcc，-O2）
lib/linux-x86_64-gcc-tls/…             同上，但启用了 TLS（ssl://，链接 -lssl -lcrypto）
lib/windows-x64-msvc-staticmem/* 静态内存版（无堆）x64 静态库，池默认 20 MiB
lib/windows-x86-msvc-staticmem/* 同上，32 位
lib/linux-x86_64-gcc-staticmem/* 静态内存版 Linux 静态库
lib/windows-x64-msvc-staticmem-tls/* 静态内存版 + TLS（x64）
lib/linux-x86_64-gcc-staticmem-tls/* 静态内存版 + TLS（Linux）
lib/<平台>/nclink_clients.lib|.a       协议客户端静态库：与同平台的核心库配套（Windows 六种
                                       变体 + Linux 各一份），里面对应上面那些头文件的 API
examples/client/, examples/device/      两个示例程序的源码（设备端 / 客户端，目录与仓库一致）+ CMakeLists.txt
examples/device/c/device_model.c, device_model.h  设备模型（编译进设备端示例与各语言绑定的垫片：
                                      五个语言的设备端示例共用同一份，不依赖外部文件）
examples/bin/windows-x64-msvc/*.exe    **编好的示例可执行文件**（x64、MSVC Release）
examples/bin/windows-x86-msvc/*.exe    同上，32 位
examples/bin/linux-x86_64-gcc/*        Linux 版示例可执行文件（gcc 13 + glibc）
examples/bin/*-staticmem/               链接静态内存版构建的示例可执行文件（x64 / x86 / Linux）
bindings/go/                            Go 绑定源码（cgo，链接上面的静态库；客户端 + 设备端，含 nclink_thunks.c）
bindings/csharp/                        C# 绑定源码（客户端 + 设备端 + HTTP/REST + 文件通道 + TLS 选项；netstandard2.0 / .NET 8 / .NET Framework 4.7.2 三目标，含设备端示例与自检）
bindings/java/                          Java 绑定源码（JNI，Java 8 字节码，无第三方依赖；客户端 + 设备端 + HTTP/REST + 文件通道 + TLS 选项）
bindings/python/                        Python 绑定源码（ctypes，只用标准库；客户端 + 设备端 + HTTP/REST + 文件通道 + TLS 选项）
bindings/native/                        三种托管绑定共用的原生垫片（C#/Java/Python）
MANUAL.md / MANUAL.docx                使用手册（Word 版由 md 生成，内容一致）
TRANSFER_PERF.md                       文件通道传输效率报告（同机与跨容器的吞吐实测）
README.md                              工程概览与测试清单
CHANGELOG.md                           版本变更记录
RELEASE.md                             本文件
LICENSE                                MIT 许可全文
SHA256SUMS.txt                         包内每个文件的 SHA-256
```

实现源码（`stack/src/`）与 43 个测试套件（`stack/test/<模块>/`）不在本包内，见第 5 节；
手册第 3 章另有一份最小可用示例代码，可直接抄进你的工程。

协议客户端的 C 源码、适配器模块（`plugins/`）与设备程序 `ncl_server` 也不在本包内：适配器
单独出包——一个目录 `nclink-adapter-<版本>-win-x64`，里面放 host（`bin/ncl_server.exe`）、
`plugins/` 下**各厂商的驱动模块**（`ncl_driver_focas.dll`、`ncl_driver_syntec.dll` …）、
每个驱动一份配置样例与运行脚本，**装载哪个驱动由配置里的 `plugins` 说**。见
`tools/make_adapter_release.ps1`。本包给的是两侧各自链接的库与头文件。


**同一个包里有两种构建**，按平台各放一份，目录名区分、文件名相同：

```
lib/windows-x64-msvc/nclink_core.lib            默认：库内分配走 C 运行库堆（与 3.2.0 相同）
lib/windows-x64-msvc-staticmem/nclink_core.lib  **静态内存版**：库内分配走 .bss 里的固定池，不调用 malloc
lib/windows-x86-msvc-staticmem/                 同上（32 位）
lib/linux-x86_64-gcc/libnclink_core.a           默认：堆
lib/linux-x86_64-gcc-staticmem/libnclink_core.a **静态内存版**
lib/windows-x64-msvc-staticmem-tls/nclink_core.lib   静态内存版 + TLS（ssl://）
lib/linux-x86_64-gcc-staticmem-tls/libnclink_core.a  同上（Linux，链接 -lssl -lcrypto）
```

- **静态内存版是"库的分配全静态"**：库内 471 处分配/释放都走同一个接缝，池是一个静态数组
  （`NCL_MEM_POOL_BYTES`，本包按 **默认 20 MiB** 编译）。它**不调用 `malloc`**，池耗尽时
  返回 `NCL_ERR_NOMEM` 而不是回退到堆；C 运行库自身、线程栈、`getaddrinfo()` 与 OpenSSL
  仍由系统分配（要"整个进程零堆"需要把这些也换掉，见手册 4.9）。
- **池大小是编译期常量**：本包固定 20 MiB（进程里多 20 MiB 的 .bss，不占文件体积、不占栈）。
  换尺寸只要重编，不用改代码：
  `.\build.ps1 -StaticMem -MemPoolBytes 65536`（Linux：`NCL_STATIC_MEM=1 NCL_MEM_POOL_BYTES=65536 ./build-linux.sh`）。
  现场调参用 `-MemReport`（退出时打印峰值/每类用量/拒绝快照）。
- **所有权约定**：静态内存版交出来的指针只能用 `ncl_free_safe()` / `ncl_*_free()` 释放，
  **不要用 libc 的 `free()`**（两个堆）。默认（堆）版没有这个约束。
- **静态内存 + TLS**：`lib/*-staticmem-tls/` 是两者的组合。池覆盖的是**库自身**的分配，`ssl://` 用到的 OpenSSL 仍走系统堆（Linux 用系统 `libssl.so.3`/`libcrypto.so.3`，Windows 版是 OpenSSL 静态链接）；要「整个进程零堆」得把 OpenSSL 的分配也接过来，本包不做。
- **示例可执行文件也给了两份**：`examples/bin/<平台>-staticmem/` 就是链接静态内存版构建出来的，
  直接跑就能看到池版本的行为。
- 本包的五份语言绑定按**默认（堆）版**验证；静态内存版与绑定混用未做实测，绑定的宿主进程若
  需要静态内存，建议自行重编并跑一遍绑定的自检套件。

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
| Windows 测试 | **43/43**：x64（堆 / 静态内存 / 堆+TLS / 静态内存+TLS 各一套）、x86（堆 / 静态内存） |
| mingw-w64（Windows 目标的 GCC） | 库 / 示例 / 测试全量 **43/43** —— 在容器里用 mingw-w64 gcc 13.2.0-posix（Debian/Ubuntu 包，msvcrt）编，再把 PE 产物拿回 Windows 上跑（`CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar CXX=x86_64-w64-mingw32-g++ NCL_RUN_TESTS=0 sh build-linux.sh builds/build-mingw`），Go 绑定的 cgo 走的就是这份库 |
| 内存检查 | Linux：ASan + LeakSanitizer（`tools/asan-linux.sh`，含并发用例）**0 发现**、ThreadSanitizer **0 数据竞争**；Windows：MSVC `/fsanitize=address` 构建同样可跑 |
| 断开握手 | 客户端断开前先收干净在途字节再 FIN（避免 RST 吞掉 DISCONNECT），`mqtt_client` 套件由 40 次里 10 次失败 → 40/40 通过 |
| 测试并发提示 | 套件之间用固定端口（FTP 2323/3131 等）与相对路径，**同一构建目录里别并发跑两份 ctest**，否则互相抢端口/文件 |
| Linux 编译 | 零警告（gcc 13.4.0，`-Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes`） |
| Linux 测试 | **43/43**（堆 / 静态内存 / TLS / 静态内存+TLS，gcc 13 + OpenSSL 3.0.20） |
| 示例实跑 | 包内三种产物（Windows x64 / Windows x86 / Linux x86_64）都与 **EMQX 5.8.9** 对跑通过：模型交换、读写、参数校验、文件传输、事件推送、两个采样通道（1 s 状态；1 ms 采样 / 100 ms 上报的功率振动，共 12 列，主轴两路传感器）。窗口节奏实测：Linux ≈118 ms 一条；Windows ≈222 ms 一条（短等待走高精度计时器，1 ms 等待实测 1.56 ms，见手册 4.5）；Linux 下给设备端发 SIGTERM 也能优雅退出（退出码 0） |
| broker 互操作 | `stack/test/test_broker` 对 EMQX 5.8.9 实测 **44 项检查、0 失败**：QoS 0/1/2、通配订阅、40 KB 报文、退订、空闲保活、会话顶替、重连后订阅恢复（`tools/interop.sh` 可在 Docker 里同时跑 EMQX 与 Mosquitto） |
| x86（32 位） | 库 / 示例 / 测试全部通过；产物 PE 头 Machine = 0x014c（i386），与 x64 同一套源码、同一套编译选项 |
| TLS | Windows（MSVC + OpenSSL 3.0.18 静态链接）与 Linux（gcc + OpenSSL 3.0.20）四套 TLS 构建都编过并 **43/43** 通过；**x86 暂未出 TLS 版** |
| 托管绑定自检 | C# 106 项（`examples/sdk/csharp/tests/Nclink.SelfTest`，net472 与 net8.0 各跑一遍）、Java 107 项、Python 46 项，全部 0 失败；覆盖客户端、设备端（工具注册 / 采样通道 / 事件 / 离线 dispatch / 自研传输）、HTTP/REST 端点（OpenAPI、swagger-ui、工具端点、配置端点、自定义路由）与文件小工具（压缩判断、分片数、SHA-256、属性），都不需要 broker |
| Go 绑定自检 | `cd examples/sdk/go && go test ./...`（cgo）：**Windows（mingw-w64 gcc 13.2.0-posix）与 Linux（gcc 12/13）两侧都跑通**，客户端 + 设备端（工具注册与路径绑定、离线 dispatch、采样通道、事件、内建工具、文件工具、HTTP 端点与自定义路由、关闭语义、注册上限），离线跑，不需要 broker；`-tags nclink_tls` 那份在 **Windows 与 Linux 两侧也都跑通**（链 `libnclink_core_tls.a` + OpenSSL，`nclink.TLSAvailable()` 为 true） |
| 绑定对真 broker | `NCLINK_TEST_BROKER=tcp://host:port` 打开的可选用例（设备端 + 客户端同进程，报文真的过 MQTT）：C# 132 项、Java 12 项、Python 46 项，对 **Mosquitto 2** 与 **EMQX 5.8.9** 各跑一遍都 0 失败（probe、路径绑定读写、methodCall、采样、事件、文件通道上传下载与带文件参数的方法调用） |
| 文件通道 | 3.4.0 起要**显式握手**（`file/openFileChannel`）或由设备钉静态对端（`ncl_server_set_file_peer`）；托管绑定：上传 / 列目录 / 下载 / 建目录 / 删文件 / 带文件参数的方法调用（含"工具返回文件"的反向）、自定义 FTP 端口与显式指定对端，都实测通过；跨语言也过一遍：C 客户端示例对 **C# 设备端示例** 的 `上传 /demo.txt` → `文件回读路径` → `远端文件 demo.txt (14 字节)` 全通 |
| 绑定 + TLS | 三份托管绑定都过一遍（`build-shim.ps1 -Tls` / Java 的 `build-native.ps1 -Tls`）+ Mosquitto 的 8883 TLS 监听：`ssl://` 设备端与客户端都用 CA 连通（C# 134 项、Java 13 项、Python 46 项，对 Mosquitto 2 与 EMQX 5.8.9 都 0 失败），不给 CA 时握手被拒（证书校验），`verify_peer=false` 放行；库没编 TLS 时 `ssl://` 返回明确的 `NCL_ERR_NOT_SUPPORTED` |
| 跨语言互读 | C# 设备端 ← C 客户端 / Python 客户端（Mosquitto 与 EMQX）、C# 客户端 ← Java 设备端（`GET`、采样、事件；`SET /STATUS` 按对端模型拒绝）、C# 与 Java 设备端的 REST 端点实测（`/api/schema`、`/swagger-ui`、工具端点、自定义路由） |
| HTTP/REST 端点实跑 | C# 设备端示例（离线 + REST）与 Java / Python 设备端示例都挂上了端点：`GET /api/schema`、`GET /swagger-ui`、`POST /api/<工具>/<方法>`、`GET /api/cfg/*` 与自定义路由实测通过 |
| Go 绑定用的 mingw 库 | 随包提供 `lib/windows-amd64-mingw/` 两份（mingw-w64 gcc 13.2.0-posix 编的 x64 库：`libnclink_core.a` 与 `libnclink_core_tls.a`），Windows 上 `go test ./...` 与 `go test -tags nclink_tls ./...` 都实测通过 |

测试套件：json、common、topic、model、message、codec、thread、mqtt、mqtt_client、
client、server、http、rest、config、ftp、file、schema、event、license、broker、
tls、cpp（broker 需要真实 broker，`tools/interop.sh` 一键起，默认跳过）。

> 注：本节与 3.1.x 里写的"25/25"是 3.4.0 / 3.3.0 **发布当时**的口径——那时仓库里只有
> 核心库的这 31 个套件。`plugins/`（厂商适配器 + 可装载的
> 适配器模块，未发布）进来后仓库全量是 **42 个套件**（26 核心 + 16 适配器，核心多的是
> `ncl_library_*` 的装载接口），两个平台的 **默认堆版**都已复测 **42/42**
> （Windows/MSVC 与 MinGW/gcc 16.2）；静态池那几组仍是改造之前的 39 套口径
> （32 KiB → 37/39、64 KiB → 38/39、1.5 MiB → 39/39），本轮未重跑，详见手册 4.9。

### 3.1 3.6.0 的验证（本次发布前实测）

| 项 | 结果 |
|----|------|
| Windows x64 MSVC（`build.ps1`） | 编译零警告、`ctest` **43/43** |
| 采样周期（本次修的 bug） | 离线跑示例：`EdgeSersors` 8 项 / 1 ms 槽位 / 100 ms 上报 → **9.8 包/秒、每拍都有数据、无 warn**（修之前 20 项时只有 2.4 包/秒，周期被拖到 420 ms） |
| FANUC 写参数 | NCGuide 0i-MF 实机：6711 / 6712 / 1 / 1320 轴 1 写进去又读回，逐次核对一致；写不存在的号如实报错（01 册 §11.25） |
| 设备模型两份一致 | 编译进 `device_model.c` 的那份与 `conf/model/nclink.json` 逐字节相同（模型 1.2.0，EdgeSersors 8 项） |

### 3.2 3.4.0 的验证（本次发布前实测）

| 项目 | 结果 |
|------|------|
| Windows（MSVC 14.44.35207，Release） | x64 全量 **43/43**（`file` 套件 **308 项断言**：握手、`conf/ftp.txt`、64 MiB 大文件、两种续传起点、数据连接中途掐断后的续传重试）；x86 / 静态内存 / TLS / 静态内存+TLS / x86 静态内存 各 **43/43** |
| Linux（gcc 13.4，容器内） | 默认堆 / TLS / 静态内存 / 静态内存+TLS 四套各 **43/43**（`docker run --rm -v <repo>:/work -w /work gcc:13 sh build-linux.sh <目录>`） |
| mingw-w64（gcc 13.2.0-posix，msvcrt；容器内交叉编，PE 产物回 Windows 跑） | 库 / 示例 / 测试 **43/43**（`CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar CXX=x86_64-w64-mingw32-g++ NCL_RUN_TESTS=0 sh build-linux.sh builds/build-mingw`，`NCL_RUN_TESTS=0` 只编译不执行）；TLS 变体（`NCL_WITH_TLS=1 NCL_OPENSSL_ROOT=/opt/mingw-openssl NCL_EXTRA_LIBS=-lcrypt32`，链 OpenSSL 3 的导入库 —— 运行时需要 `libssl-3-x64.dll`/`libcrypto-3-x64.dll`）同样 **43/43**，`test_tls` 通过 |
| 内存检查 | Linux ASan + LeakSanitizer（`tools/asan-linux.sh --docker`，6 个套件）**0 发现**；ThreadSanitizer（分配器并发用例）**0 数据竞争** |
| 托管绑定自检（离线） | C# 106 项、Java 107 项、Python 46 项，**0 失败** |
| 异步方法调用 | `Method/Status`、`Method/Result` 两对已实现并接入设备端线程池：`async: true` 立刻回 `code=OK`+`handler`，状态/结果按句柄查询（未完成 `PENDING`、完成 `finished|error` 并释放句柄）；`test_server` 端到端用例通过 |
| 托管绑定对真 broker（EMQX 5.x，`NCLINK_TEST_BROKER`） | C# **135 项**、Java **15 项**、Python 46 项，**0 失败**——含文件通道握手全流程（上传 / 列目录 / 下载 / 建目录 / 带文件参数的方法调用 / close 撤销） |
| Go 绑定 | Linux（golang:1.22 容器）与 Windows（cgo + mingw gcc 13.2.0-posix）`go test ./...` 均通过；`-tags nclink_tls`（链 `libnclink_core_tls.a`）两侧同样通过 |
| 文件通道握手 | 新增用例覆盖：无通道时文件方法被拒（`NoFileChannelException`）、握手后可用、同租约重复握手幂等、换租约需 `force`、`closeFileChannel` 撤销临时账号后登录失败、重复 close 幂等；`conf/ftp.txt` 往返 / 坏文件容忍 / 端点跟随文件里的端口与账号 / 函数参数优先于文件 |
| 文件传输（流式 / 续传 / 效率） | 传输改成流式（256 KiB 读、64 KiB 写，内存不随文件大小增长）+ 可续传（上传按对端 SIZE 用 APPE、下载按本地大小用 REST；单次调用内 3 次重试都从断点继续）。用例：64 MiB 大文件、对端已有前半、本地已有前半、数据连接第 3 MiB 被掐断。吞吐实测见 **TRANSFER_PERF.md**（同机上传 128~512 MiB/s、下载 106~140 MiB/s；跨容器上传 786~901 MiB/s、下载 136~155 MiB/s；续传行线上字节恰好一半且逐字节一致） |
| 跨主机（容器 ↔ 容器） | 客户端与设备端各占一个容器（同桥接网络 + EMQX 控制面），设备按握手里的 `ncl-client:2323` **被动模式**回拨，16 / 64 / 256 / 512 MiB 上传下载与续传全部逐字节一致；Windows 客户端 ←→ 容器设备、以及两台容器之间都已跑到 |
| 地址推导 | broker 在本机（回环）时改取本机非回环 IPv4；无 broker 语境回退 `127.0.0.1` |

### 3.2.1 3.3.0 的验证

| 项目 | 结果 |
|------|------|
| Windows（MSVC，x64 / x86） | 全量 **25 个套件通过**（默认堆、静态内存、x64+TLS、32 位四种构建） |
| Linux（gcc 13，容器内） | 全量 **25 个套件通过**（默认堆、静态内存、TLS 三种构建） |
| 静态池尺寸边界 | 64 KiB 池 **25/25**；32 KiB 池 23/25（ftp/file 需要大块连续空间） |
| 蒙特卡洛（单线程） | 1 小时、7 个池尺寸并行：**260389 轮 / 约 52.1 亿次操作 / 0 失败** |
| 蒙特卡洛（多线程） | 8 线程 × 3 个池尺寸、10 分钟：**约 5.63 亿次操作 / 8030 万次跨线程交接 / 0 失败** |
| AddressSanitizer + LeakSanitizer | 库与分配器套件（含并发用例）**0 发现** |
| ThreadSanitizer | 并发分配器用例 **0 数据竞争** |
| 包内库自检 | 用**包里的库**编程序实测：默认版 `ncl_mem_mode()=="heap"`；静态内存版 `"static-pool"`、池 20971520 字节、分配/释放与统计正常 |
| 真 broker 互操作 | Mosquitto 2.1.2 与 EMQX 5.8.9 各 **44 项检查全过**（含静态内存版二进制）；**包内 staticmem-tls 库**编出的用例走 `ssl://` 对 Mosquitto 的 TLS 监听同样 **44 项全过** |

### 3.3 从 3.3.0 升级：文件通道要显式握手

3.3.0 的设备端文件工具会按 `conf/mqtt.cfg` 里 broker 的主机名 + 端口 2323 +
admin/123456 去拨 FTP。3.4.0 删掉了这条隐式路径，升级时二选一：

```c
/* A. 推荐：上位机开通道（库按需起本机 FTP 端点并给一个临时账号） */
ncl_client_open_file_channel(client, NULL);   /* 传完 ncl_client_close_file_channel() */

/* B. 设备自己钉静态对端：对端要有自己的 FTP 服务端，布局 "/<sn>/..." */
ncl_server_set_file_peer(server, "10.0.0.7", 2323, "admin", "123456");
```

跨网段 / 端口映射用 `ncl_file_channel_options.host/port`，或者写进可选的
`conf/ftp.txt`（`host` / `port` / `advertisePort` / `root` / `userName` / `password` /
`path` / `force`，键全可选；优先级：函数参数 > 文件 > 推导默认）。三个托管绑定的
`DeviceClient` 便利方法会自动握一次手，绑定用户通常不用改代码。

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

**静态内存版**：把路径换成 `lib\windows-x64-msvc-staticmem\nclink_core.lib`（或`lib/linux-x86_64-gcc-staticmem/libnclink_core.a`）即可，编译选项与用法完全一样；额外要遵守两条：库交出来的指针只能用 `ncl_free_safe()` / `ncl_*_free()` 释放，池大小是编译期常量（本包 20 MiB，换尺寸见第 1 节）。

### 4.2 Linux（gcc/clang）

```sh
gcc -std=c11 -O2 -Wall -Iinclude your_app.c lib/linux-x86_64-gcc/libnclink_core.a -lpthread

# 静态内存版：换成 lib/linux-x86_64-gcc-staticmem/libnclink_core.a
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
gcc -std=c11 -O2 -Iinclude examples/device/c/ncl_device_demo.c examples/device/c/device_model.c \
    lib/linux-x86_64-gcc/libnclink_core.a -lpthread -o ncl_device_demo
gcc -std=c11 -O2 -Iinclude examples/client/c/ncl_client_demo.c \
    lib/linux-x86_64-gcc/libnclink_core.a -lpthread -o ncl_client_demo

./ncl_device_demo <安装根目录> 20        # 设备端：起服务、采样、发事件
./ncl_client_demo tcp://<broker>:1883 <设备SN> 8   # 客户端：读值/写值/校验/文件/采样/事件
```

```bat
:: Windows
cl /nologo /W4 /utf-8 /MD /Iinclude examples\device\c\ncl_device_demo.c examples\device\c\device_model.c ^
   lib\windows-x64-msvc\nclink_core.lib ws2_32.lib iphlpapi.lib
```

两个示例覆盖了「连接 → probe 取模型 → 读值 → 写值 → 参数校验 → 采样（含亚毫秒批量）
→ 事件 → 文件传输」全流程；手册第 3 章另有一份最小示例，可直接抄进你的工程。

## 5. 源码与自行重新编译

本发布包**不含实现源码**（`stack/src/`）；示例程序在包内，测试套件与构建脚本需要从
工程仓库获取（`stack/src/`、`stack/test/`、`tools/` 与 `CMakeLists.txt`、`build.ps1`、
`build-linux.sh`），然后：

```powershell
.\build.ps1                # Windows：CMake + Ninja + ctest
```

```sh
./build-linux.sh           # Linux：gcc + ar，无需 cmake；末尾自动跑全部测试
```

协议客户端与适配器的 C 源码同样只在带 `-WithSource` 的包里；适配器包另有一支脚本。

打包脚本本身支持两种发布形态：

```powershell
.\tools\make_release.ps1                # 默认：头文件 + 库 + 文档 + 示例（本包）
.\tools\make_release.ps1 -WithSource    # 额外带上 stack/src/tests/tools 与构建脚本
.\tools\make_adapter_release.ps1        # 适配器包：一个目录（host + 各驱动模块 + 配置 + 脚本）
```

从源码手工编译时的三个要点（详见手册 2.3）：`-Istack/include -Istack/src`、
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
| 边缘接口 | `Edge/*` 主题与 4 个 `ncl_topic_edge_*()` 构造函数在 3.4.0 **移除**（不使用）；需要时按 GB/T 41970-2022 自行拼主题即可 |
| JSON Schema | 校验器为 draft-07 子集，不支持 `patternProperties`/`dependencies`/外部 `$ref` 等 |
| FTP | 实现 RFC 959/2389 子集（覆盖 NC-Link 文件通道用到的命令与两种数据连接模式） |
| POSIX 分支 | 已在 gcc 13.4 + glibc 验证；musl、FreeBSD 等未验证 |
| x86 的 TLS | 32 位只出非 TLS 版：要用 `ssl://` 得自编 32 位 OpenSSL 静态库，再 `-Arch x86 -Tls -OpenSslRoot <dir>` |
| Go 绑定的 TLS 变体（Windows） | 包内给了 `libnclink_core_tls.a`，但它和 Linux 的 TLS 版一样是**动态依赖 OpenSSL**：链接时需要 OpenSSL 3 的导入库（`-lssl -lcrypto`，例如 Strawberry Perl 的 `c/lib`），运行时需要 `libssl-3-x64*.dll` / `libcrypto-3-x64*.dll`；MSVC 那份 TLS 库用的静态 OpenSSL 在 Windows 的 Go 工具链下用不了 |
| 静态内存版的池大小 | 编译期常量：包内两份静态内存库都按默认 **20 MiB** 编译（换尺寸要重编，见第 1 节）；池不支持运行时扩容 |
| 静态内存版与绑定 | 五份绑定按默认（堆）版验证；静态内存版与绑定混用未实测 |
| 静态内存 + TLS | 池只覆盖库自身的分配：OpenSSL（`ssl://`）仍用系统堆，所以「整个进程零堆」在这份变体里不成立 |
| 静态内存 + TLS 的验证 | 包内 `lib/linux-x86_64-gcc-staticmem-tls` 编出的 `test_broker` 对 Mosquitto 的 TLS 监听 44 项全过；Windows/Linux 两侧该组合的 25 个套件（含 `test_tls`）也全过 |

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
