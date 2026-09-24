# 变更记录

本文件记录 NC-Link C 实现（`nclink-core-c`）的版本变更。版本号跟随
NC-Link 规范版本：**3.0.0** 对应 GB/T 41970-2022 协议 3.0.0。

## 3.6.0

### 新增：伪机床适配器（`plugins/pseudo.c`）—— 不接硬件的机床

  * **为什么**：整条链路（声明 → 模型 → 采样通道 → 绑定 → 请求应答 → 上位机）的验证
    平时要等一台真机床。伪机床把"机床"换成模块内置的模拟器：**点位模型照
    `plugins/syntec.c` 摆**（同一套路径与类型、同一个 1000/1000 周期、同样四张配置表与
    操作位、同名字面量 `running`/`holding`/`free`），客户端不需要知道对面是真是假。
  * **点位面**：72 个点位 + 1 个方法 —— 采样四项（`/STATUS`、`/PART_COUNT`、
    `/CONTROLLER/PROGRAM`、`/CONTROLLER/WARNING`）、覆盖量五项、轴 **九字母 × 六格 = 54**
    （`SCREW`/`SERVO_DRIVER`/`MOTOR`/`VARIABLE@ABSOLUTE|RELATIVE|DISTANCE`）、四张配置表
    （`PARAMETER` / `TOOL` / `REGISTER@R|I|O|C|S|A` / `VARIABLE`）、`/SESSION`。没配进
    `axes` 的轴照新代的规矩回 `NotFound`。
  * **值从哪来**：一条相位时钟（`cycleMs` 一循环）—— 轴位置走三角波（所以采样画出来是
    连续曲线）、状态按相位给、计件每循环 +1、报警按 `alarmEvery`/`alarmFor` 出现；
    **写进去的值（倍率、参数、刀补、寄存器、变量）落在模拟器内存里，读回来就是新值**。
    `frozen: true` 把时钟钉住，用例可以断言精确值。
  * **故障注入**（平时要拔网线才能验的那几条路，改一行配置就能验）：`latencyMs` 拖慢、
    `fail` 前 N 次取值失败、`unavailable` 让指定路径回"还读不了"。
  * **`pseudo/SESSION` 是遥控器**：`{"status":"holding"}`、`{"alarm":{...}}`、
    `{"fail":{...}}`、`{"reset":true}`、`{"frozen":true}` …… **通过 NC-Link 协议本身**
    把这台假机床拨到任意状态，自动化用例从客户端侧就能跑完全流程。
  * **有意与新代不同的两处**（文档里写明）：`SERVO_DRIVER/POSITION` 新代那边还待抓包，
    伪机床给值；`/FEED_OVERRIDE`、`/SPINDLE_OVERRIDE` 新代只读，伪机床可写（权限仍在
    适配器外面控）。
  * **不做什么**：不实现 `last_raw`（没有线上字节，审计里如实"无帧"，不编一串 JSON 冒充
    报文）；不与静态内存版混（模块设计的已知边界）。
  * **配置与文档**：`conf/pseudo.json` 样例（`/conf/*` 仍然整体忽略，只放行这一份），
    `plugins/PSEUDO-ADAPTER.md` 现场手册，`plugins/README.md` 新增"④ 模拟器"一种写法。
  * **验证**：`ncl_server --once` 自检 **72/72、0 待抓包、0 失败**（九轴全开）；
    `plugins/tests/test_host_tool.c` 里同一个套件加载这个真模块，**245 项检查 0 失败**
    （形状、frozen 下的精确值、写后读回、四张表、遥控、注入、`axes` 收窄后的 NotFound）。

### 文档：Linux 工具链口径 gcc 13.4 → 13.5.0

  * **原因**：`gcc:13` 是滚动 tag，现在拉到的镜像已经是 **13.5.0**（判定方式：包内 Linux
    产物的 `.comment` 与容器 `gcc -dumpversion` 两边都是 13.5.0）；文档里写的 "gcc 13.4"
    与实际编出这批二进制的编译器不符。
  * **改正的 6 处**：README「Linux（已验证）」、MANUAL 2.3、RELEASE 的平台表「编译器」行、
    「Linux 编译」零警告行、3.2 验证表的 Linux 行、边界表的 POSIX 行。RELEASE 的验证行
    顺带补了一句说明 —— `gcc:13` 是滚动 tag，表里的数字是实测值 —— 免得镜像下次再动时
    又对不上。
  * **验证结论不变**：43/43、零警告这些结论本来就成立于 13.5.0（当初就是在现在这个镜像
    里跑的），这次只修版本号。CHANGELOG 里 3.4.0 那条 "gcc 13.4" 是历史记录，按原样保留。
  * **dist**：包内的 README / MANUAL / RELEASE 随之重打（`tools/make_release.ps1`），
    SHA256SUMS 与 zip 重算入库。

### 目录重排：stack / examples / clients / plugins / tools / dist，测试与模块一一对应

  原来的顶层是 `include/ + src/ + tests/ + examples/ + bindings/`，测试全堆在 `tests/` 里。
  现在：

  * **`stack/` 是协议栈**：`stack/include`（公共头，对外仍是 `<nclink/*.h>`）、
    `stack/src/<模块>`、`stack/test/<模块>` —— 单元测试与模块一一对应（core、general、model、
    message、codec、mqtt、client、server、http、rest、config、ftp、file、schema、tool、cpp、
    license），共用的夹具与测试头在同一层（`stack/test/`：`ncl_test.h`、`fake_nclink_server.*`、
    `data/`、`fuzz/`）。
  * **`plugins/tests/`**：适配器那一侧的测试与夹具（声明式夹具、两个"装载器必须拒收"的夹具、
    宿主端到端），原来散在 `tests/test_host_tool.c` 与 `tests/module_*.c`。`clients/tests/` 不动。
  * **`examples/` 合并了原来的 `examples/` 与 `bindings/`，按"哪一侧"分**：
    `examples/client/<语言>`、`examples/device/<语言>`（c/cpp/java/python/csharp/go），语言绑定
    本体（含各自自检）在 `examples/sdk/<语言>`（native 垫片、csharp、java、python、go）。
    Go 的两个示例各自是自己的模块，用 `replace` 指回 `examples/sdk/go`，`go run .` 照旧能跑。
  * **跟着改的**：根 `CMakeLists.txt`（`add_subdirectory(stack/test)`、`plugins/CMakeLists.txt`
    里 `add_subdirectory(tests)`）、`build-linux.sh`、`tools/*`（asan/fuzz/soak/interop/
    stage-go-libs/gen_api_index/gen_shim_header/make_release）、五个绑定的构建脚本与 csproj、
    `.gitignore`、文档（README 与 MANUAL 的目录结构、RELEASE 的验证口径、各语言的 README）。
  * **发布包内部布局不变**（`include/ lib/ examples/ bindings/` 那套，面向交付）：打包脚本从新
    目录取值、按老布局拼装，包里"`bindings/…`"那几句说明仍然成立；包内的 Go 示例仍在同一模块
    里（不需要 `replace`）。
  * **验证**：Windows 六个变体（x64/x86 堆、x64 TLS、x64/x86 静态内存、x64 静态内存+TLS）各
    **43/43**；Linux 堆 / TLS / 静态内存 / 静态内存+TLS（gcc:13 容器内）各 **43/43**；mingw
    两份库 **43/43**（PE 产物拿回 Windows 跑）；dist 重建入库。

### 构建：mingw 库换工具链重建（GCC 13.2.0-posix，容器内交叉编）

  * **原因**：`lib/windows-amd64-mingw/` 两份 `.a` 是另一台机器上用 mingw-w64 16.2.0
    （UCRT）编的，跟包里其余产物不是同一次构建 —— 上一轮打包时它们就没跟上
    "采样表头去设备段"那个改动（符号里查不到 `ncl_path_without_device`）。
  * **现在**：在本机的 `ncl-mingw:gcc13` 镜像里（ubuntu:24.04 +
    `gcc-mingw-w64-x86-64-posix` 13.2.0，msvcrt）交叉编，PE 产物拷回 Windows 跑：
    库 / 示例 / 测试 **43/43**；TLS 那份同样 **43/43**（链 OpenSSL 3 的导入库，运行时
    需要 `libssl-3-x64.dll` / `libcrypto-3-x64.dll`，与原有口径一致）。
  * **`build-linux.sh` 新增三个环境变量**（都服务交叉构建，不设就是原来的行为）：
    `NCL_RUN_TESTS=0` 只编译不执行测试（PE 在 Linux 里跑不起来，产物拿到目标平台跑）、
    `NCL_OPENSSL_ROOT=<前缀>` 指 OpenSSL 的头文件与库（跟 `build.ps1 -OpenSslRoot`
    一个意思）、`NCL_EXTRA_LIBS` 补链接参数（静态 OpenSSL 要的 `-lcrypt32` 之类）。
  * **文档**：RELEASE.md 与 MANUAL.md 的 mingw 口径按实际工具链改写（版本、C 运行库、
    容器内怎么编、`NCL_RUN_TESTS=0` 之后怎么在 Windows 上跑），并写上现成镜像的一条
    命令 —— 本机不再重复搭环境。

### 采样表头：路径去掉设备段（`/MACHINE/STATUS` → `/STATUS`）

  * **现象**：一条采样报文里每一列的 `paths` 都带同一段设备前缀（`/MACHINE/...`）。
    一条通道里的所有采样项都在同一台设备上，是哪台设备看主题里的 `<sn>` 就够了 ——
    前缀在表头里只是逐列重复，消费端还得先把它剥掉才能对上自己的列名。
  * **现在**：表头是**设备内路径**（`/STATUS`、`/AXIS@X/POSITION@REAL`）。**取值那条路
    一点没动**：绑定键、模型路径、REST 地址、`ncl_host_point_path()`、`sampleItemPaths`
    仍是绝对路径。两种采样通道形态都按"服务端那台设备"剥前缀（模型里定义的通道按它自己
    那台设备）；对不上的路径（别家的点位）原样保留。
  * **新增**：`ncl_node_device_path()`（节点往上找设备段）、`ncl_node_path_in_device()`、
    `ncl_path_without_device()`（字符串版，只认整段相同的前缀：`/PLX` 不会被 `/PLC` 吃掉）。
    设备端（`ncl_sample_task_create`）与客户端的模型补齐（`ncl_message_sample_normalise`）
    走同一条规则，所以"设备带了表头"与"客户端按模型补表头"两种情形形态一致。
  * **影响面**：`src/server/server.c`（表头与取值分成两个向量：`paths` 查绑定、`header`
    上线）、`src/message/message.c`、`src/model/model.c`、`include/nclink/ncl_model.h` /
    `ncl_server.h` / `ncl_message.h`；测试 `test_server` / `test_message` / `test_client`
    的期望值跟着改，`test_model` 新增"设备内路径"用例。验证：`build.ps1` **43/43**。
  * **消费端**：拿表头去查模型 / 打 REST / 发查询时补上设备段即可（`device_path` 就是模型里
    device 节点的路径："`/MACHINE`" + "`/STATUS`"）。MANUAL 4.5 / 5.5 / 6.6、C# / Java /
    Python 绑定的注释与 C# 实测输出同步。

### 发布：dist 入库、clients 出库 + 头文件、适配器一个包多驱动

  * **`dist/` 入库**（原来只在本地）：`nclink-core-c-3.6.0`（库包）与
    `nclink-adapter-3.6.0-win-x64`（适配器包），各带 zip 与 `.sha256`。`.gitattributes`
    给 `dist/**` 按原字节保存（包内 `SHA256SUMS.txt` 得跟同包文件的实际字节对得上，不能被
    换行归一化改掉）；`.gitignore` 放行包内二进制（`*.dll` / `*.exe` / `*.so`）但挡住包内
    样例的 `obj/`。
  * **库包**：新增 `include/nclink/clients/*.h`（10 个协议客户端的公共头，与核心头同一个
    根）与 `lib/<平台>/nclink_clients.{lib,a}`（与同平台核心库配对：Windows 六种变体 +
    Linux）。协议客户端的 **C 源码不进包**（只在 `-WithSource` 里）——库里给的是编译产物与
    头文件；设备程序与适配器模块同样不在库包里。
  * **适配器包**：一个目录 `nclink-adapter-<版本>-win-x64`，`plugins/` 下**各厂商驱动模块
    并存**（`ncl_driver_focas.dll` + `ncl_driver_syntec.dll`），**装载哪个驱动由配置说**
    （`"plugins": ["focas"]`，不写 = 目录里全部装载），配置样例每个驱动一份；驱动自己的现场
    手册进 `docs/`，包内 `README.md` 换成 `plugins/ADAPTER-PACKAGE.md`（讲清单、怎么选驱动、
    怎么跑）。打包脚本 `tools/make_fanuc_release.ps1` → `tools/make_adapter_release.ps1`：
    打包版本号默认从 `include/nclink/ncl_common.h` 的 `NCL_VERSION` 读，不再手写。
  * **Linux 库重编**：`build-linux` 在 gcc 13 容器里重建（**43 套件全通过**），包内
    `lib/linux-x86_64-gcc/` 两份（核心库 + 客户端库）都是这次产物。
  * **内容体检**：MTConnect 的 `severity` 是协议自身拼写（`severity="FAULT"`），扫描前先把
    这个词从待检行剪掉，其余关键词照旧拦。

### 示例：振动只留主轴（X/Y/Z/C 四轴的 ACCELERATION 去掉）

  * **模型**（`examples/device_model.c` 与 `conf/model/nclink.json` —— 两份本来就是
    一模一样的同一份文本，按同一套渲染一起改）：去掉 X/Y/Z/C 四轴的 12 条
    `ACCELERATION` 数据项；EdgeSersors 采样通道 **20 项 → 8 项**（5 个功率 + 3 个主轴
    加速度）；模型版本 1.1.0 → 1.2.0。现场只有主轴装了振动传感器，别的轴不该凭空摆。
  * **工具**：`examples/ncl_device_demo.c` 删掉那 12 个 getter/方法/绑定（留主轴
    SX/SY/SZ）；四个绑定的设备端示例（Java/Go/C#/Python）同步只注册主轴那 3 条。
  * **顺带修好"拍不满"的另一半原因**：
    1. 排拍改用**微秒钟** `ncl_time_monotonic_us()`（Windows 走 QueryPerformanceCounter）
       —— 原来的 `ncl_time_monotonic_millis()` 在 Windows 上是 `GetTickCount64()`，
       粒度 ~15.6 ms，拿它判"1 ms 这一拍到了没"会把大量拍误判成"错过"（8 项也会只
       采到 4 成）；
    2. 离这一拍不到 2 ms 就**不睡**（Windows `Sleep(1)` 实际 1.5~2 ms，睡下去反而把
       自己睡过一拍）。
  * **实测**（离线跑示例，Windows，日志时间戳算的）：EdgeSersors 8 项 / 1 ms / 100 ms →
    **9.7 包/秒、满槽、不再打 warn**（去振动之前 20 项是拍不满的）。

### 采样：上报周期不再被"采样跟不上"偷偷拉长（示例 EdgeSersors 每秒只有 2 包）

  * **现象**：示例模型里 EdgeSersors = `sampleInterval 1 ms` / `uploadInterval 100 ms`，
    实际每秒只收到 ~2.4 包（中位间隔 403 ms）。
  * **原因两层**（都量过）：
    1. 一轮（把该通道 20 个采样项挨个取一遍）走的是"请求报文 → 应答报文 → 克隆"那一套，
       实测一轮 4.2 ms（每项 ~200 µs）—— 1 ms 一拍根本塞不进；
    2. 采样线程**只在"还有富余"时才睡**，塞不进就背靠背连着跑，于是
       **一包的时间 = rounds × 一轮时间**（100 × 4.2 ms ≈ 420 ms），上报周期被悄悄拉长，
       而且没有任何提示。
  * **改法**：
    * 采样这一路**直接叫绑定**（不再造请求/应答报文、不再克隆），一轮 4.2 ms → ~1 ms；
      `ncl_sample_item_add_value` 接管所有权，取不到就记 null（与走查询那条路一致）。
    * 采样循环按**墙钟排拍**：一包 = `rounds` 拍、每拍每列正好一格；没到点就睡到那一刻，
      **已经落后一整拍就留空批 `[]`**（"这一拍没数据"，客户端 normalise 会摊成 null），
      **不补采、也不再把整包拖长**；上报周期始终 = `uploadInterval`。
    * 第一次"拍不满"打一条 warn（sampleInterval / 拍数 / 采到几拍），不刷屏。
  * **实测**（离线跑示例，Windows，2026-09-23；日志时间戳算的相邻包间隔）：

    | 模型（采样/上报） | 修前 | 修后 |
    |---|---|---|
    | 1 ms / 100 ms | 2.4 包/秒（403 ms） | **9.1 包/秒**（110 ms；约 4 成拍有数据，其余 `[]`） |
    | 2 ms / 100 ms | 3.6 包/秒（268 ms） | 9.1 包/秒（109 ms） |
    | 5 ms / 100 ms | 6.3 包/秒（159 ms） | 9.8 包/秒（103 ms，**满槽**） |
    | 10 ms / 100 ms | 8.8 包/秒（112 ms） | 10.1 包/秒（101 ms，**满槽**） |
    | 1 ms / 50 ms | 5.0 包/秒（207 ms） | 20.3 包/秒（48 ms） |

  * **结论**：`1 ms × 20 项` = 每秒两万次取值，超出这台设备的实际能力（一轮 ~1 ms 量级），
    所以那个通道**注定拍不满**（现在如实留空批 + warn，不再拖长周期）；
    要有满槽数据就把 `sampleInterval` 调到 **≥5 ms**（示例模型注释里写清了实测口径），
    或者把取值做成"一次多给几点"（振动那条已经一次 4 点）。
  * 顺带补上两个新探针（`tools/site-probe/probe8e.py`、`sdkwr2.py`）缺的 SPDX 头
    —— `license` 自检会拦。

### 01 册：写参数**真的通了**（写进去又读回核过）

  * **记录形状核死了**：`0x8e` 的 264 字节载荷 = 机床自己那条读应答（`0x8d`）**原样回填、
    只换值那一格** —— `@0..4 号（BE32）`、`@4..6 **轴号**（BE16）`、`@6..8 属性 prm_type`、
    `@8..12 值（BE32）`；块头 `d/e/arg2/arg3` 全 0、`tag1 = 264`。载荷给 12/8 字节 →
    `EW_LENGTH=2`。解析出机床返回码：`2=EW_LENGTH`、`3=EW_NUMBER`、`4=EW_ATTRIB`。
  * **官方 SDK 在这台机器上自己写不动**（照它抓的帧对出来的）：它把 `IODBPSD.type` 那个
    short 原样塞进"轴号"那一格 → `EW_ATTRIB`；换老口径（`type=0`、`length=5`）它又把号
    按主机序写 → `EW_NUMBER`。所以这一条是按机床自己的读应答形状做的，不是抄 SDK。
  * **读那侧补上了带轴参数**：`arg2` = 轴号（无轴 0；1320/1420/1825… 必须 1..n，
    给 0 机床回"块返回码非 0"）。新增 `ncl_focas_parameter_axis()`。
  * **写后要等几百毫秒**：机床对参数块有缓存（写后立刻读回是旧值，300 ms 后才是新值），
    复核做成轮询（5 × 150 ms），对不上如实回 `NCL_ERR_UNAVAILABLE`。
  * 适配器 `/CONTROLLER/PARAMETER` 开 `set_value`（`{"keys":6711,"value":1234}`，带轴加
    `"axis"`）；权限在外面控。**实机实测**：6711/6712/1/1320 轴 1 都写进去又读回核对一致
    （写 1320 轴 1 时轴 2 没被动），写不存在的号如实回错；见 01 册 §11.25。

### 01 册：`pmc_rdcntldata` / `pmc_rdcntlexrelay` / `pmc_rdalmmsg` 都接上了

  * 码：`0x8004`（数据表 D 控制数据）、`0x8057`（扩展继电器控制数据）、`0x8010`（PMC
    报警文本）；组数那两条是 `0x8006` / `0x8059`（码也抓到了，client 没包 —— 控制数据
    那边"从 1 号组读起、机床不收就停"等价且更稳）。
  * client：`ncl_focas_pmc_control_table(f, exrelay, &json)` 与
    `ncl_focas_pmc_alarm(f, start, count, &json)`；实测
    `{"1":{"tableParam":0,"size":10000,"address":0}}`、报警 `[]`（这台没有 PMC 报警）。
  * 适配器：先落成两个**方法**（现场调试用，模型不动）——`/PMC/CONTROL`（`{"exrelay":…}`）
    与 `/PMC/ALARM`（`{"start":…,"count":…}`）。册 4 表 7 没有"PMC 参数区"这一格，
    口径与新代对齐之后再进模型。
  * ⚠️ PMC 报警**文本的切法**没在真机上核过（这台没有 PMC 报警）：按"号 4 字节 + 后面
    全是文本"出门，有真机报警时再对一次。

### 01 册：`pmc_rdalmmsg` / `pmc_rdcntl*` 的码与入参都定了

  * **`pmc_rdcntldata` = 0x8004**（读 PMC 数据表 D 的控制数据）：入参 `s_number/e_number`
    是**组号、从 1 起**（`s=0` → EW_NUMBER），第三个参数是 **`length` = IODBPMCCNTL
    结构体字节数**（一开始按"要几条"猜 → EW_LENGTH，绕了一圈）；应答 8 字节
    （库解出 `data_size=10000`、`data_dsp=0`）。
  * **`pmc_rdcntlexrelay`**：同签名同形状，`(1,1,16)` 也回 `rc=0`（码待抓）。
  * **`pmc_rdalmmsg` = 0x8010**（读 PMC 报警文本）：`type` 枚举出 **1/2 才收**
    （`-1/0` → EW_NUMBER），起始报警号给 0 回 EW_DATA；这台没有 PMC 报警，应答 8 字节
    `00000000 ffffffff`。
  * `pmc_rdcntlgrp` / `pmc_rdcntl_exrelay_grp` 早前已通（各回 `num=1`）。
  * client / 点位这一轮**没接**（时间用尽）—— 帧与参数已记进 §11.24.1/§11.24.2，
    接的时候照 §11.21/§11.22 那套（item 表 + `first=2` + 载荷复核）走一遍即可。

### 01 册：`pmc_rdcntldata` 通了（枚举出了入参的含义）

  * 病根：第三个参数不是"要几条"，而是 **`length` = `IODBPMCCNTL` 结构体字节数**
    （spec 原文），而且**组号从 1 起**（`s=0` 回 EW_NUMBER）。
  * `pmc_rdcntldata(1, 1, 16)` → **`rc=0`**；帧：`code = 0x8004`、载荷 `[s=1][e=1]`，
    应答体 8 字节（`00002710 00000000`，逐格切法下一轮补）。
  * `pmc_rdalmmsg`：枚举出 **`type = 1/2` 才被收**（`-1/0` → EW_NUMBER），
    `type=1/2` 回 EW_DATA 是因为起始报警号还是 0 —— 下一轮传一个真报警号就能拿文本。
  * `pmc_rdcntlgrp` / `pmc_rdcntl_exrelay_grp` 早已 `rc=0`（各回 `num=1`）。

### 01 册：先把 `pmc_rdalmmsg` 与 `pmc_rdcntl*` 看了一遍（还没接）

  * 签名与用途记进 §11.24（含 `IODBPMCCNTL` / `ODBPMCALM` 结构）。
  * 实测（模拟器）：`pmc_rdcntlgrp`、`pmc_rdcntl_exrelay_grp` **通了**（各回 `num=1`
    —— 这台机器有 1 组数据表控制数据、1 组扩展继电器控制数据）；
    `pmc_rdcntldata` / `pmc_rdcntlexrelay` / `pmc_rdalmmsg` 还是"入参没给对"
    （`rc=2` EW_LENGTH / `rc=3` EW_NUMBER），下一步把 `(type, group, num)` 枚举着打。
  * 口径：`pmc_rdalmmsg` 读的是 **PMC 自己的报警**，与 `cnc_alarm`（CNC 侧）不是一回事。

### 01 册：PMC 的定时器/计数器（`T`/`C`）接上

  * 这两族在 FOCAS 里**没有单独调用**（`pmc_rdpmctm`/`pmc_rdpmccnt` 这份 SDK 没有），
    就走同一条 `pmc_rdpmcrng`（族号 `6 = T`、`8 = C`）—— 客户端一行没加。
  * 口径：一个定时器/计数器在 FOCAS 里是**两个字**（`[值][类型/设定值]`），点位这一格
    给的是**值**；要看类型/设定读下一个号。
  * 实测（模拟器）：`T`/`C` 读得到、**也写得进**（写 `T0..T1`/`C0..C1 = [64,65]` →
    读回一致、隔 1 秒还在），所以 `@T`/`@C` 与别的可写族一样读 + 写都开；容量先问机床
    （`pmc_rdpmcinfo`），问不到才用文档值（T 500、C 200）。

### 01 册：PMC **写也通了** —— 错在载荷多塞了 4 个字节（2026-09-23）

  * 上一轮"帧形状一样、官方库能落、本 client 不落"的病根找到了：官方库**写**那帧的
    块尾是 `[长度 4 字节][数据]`，而**长度那一格正好压在块头末尾那两格上**
    （`[24..28)` 读成 BE32 = "要写几个字节"）。我们早先把长度又塞进载荷里 → 多送 4 个
    字节，机床把它们当成了数据（于是像"没写进去"）。
  * 改法：**载荷里只放数据**，长度交给块头末尾（驱动的 `data` 机制本来就填 `tag1`）。
  * 实测（模拟器，客户端自己写 + 读回复核，隔 1 秒再看）：`X`/`G`/`K`/`D`/`R` 都写进去了
    （`K 0..1 = [64,65]`、`D 300..301 = [256,257]`…）；`Y` 机床直接拒；
    4 个点一次写本机也拒（与读那侧"一次只认一个点"同一个脾气 → 复核不过就**逐个号重写**）。
  * 适配器把 `set_value` 开在 `@X @G @R @K @D`（实测能写），`@Y @F` 保持只读
    （`Y` 这台拒写、`F` 是 CNC 驱动的信号）。
  * 写后复核 + 逐个号重写都在：对不上一律 `NCL_ERR_UNAVAILABLE`，**不回假成功**。

### 01 册：PMC 写接上（`pmc_wrpmcrng` = 0x8002）

  * 帧：同一个四格载荷（号段/族/宽度）+ **`[数据长度 BE32][数据…]`**；块头第 2 格
    也是 2。官方库抓帧 + 实测：写 `R0..R1 = 0x33 0x44` → 读回 `[0x32, 0x44]`
    （**R0 那一位被跑着的梯形图当场改回去了** —— 正是"有的寄存器不能写/不保证每一位
    都听你的"）。
  * 写这一侧 `e` 给"起始 + 1"（官方库就这么发的），写几个由载荷里的长度说了算。
  * client 新增 `ncl_focas_pmc_write()` 与 `ncl_focas_pmc_bit_write()`（位写 = 读回
    所在字节、改那一位、写回）；机床不收就如实回错，绝不假装成功。
  * 适配器：**这一轮先不声明 `set_value`**（点位只读）—— 写还没落地（见下），
    对外不留"回了 rc=0 却没写进去"的口子；`FOCAS_REGISTER_RW_OPS` 宏留着，写通了
    把声明换回来即可。
  * **本 client 的写还没落地**（§11.22.1）：官方库写 R0..R1 = 0x33 0x44 能落（本
    client 读回 [0x32,0x44] ✓），但本 client 自己发同一个帧形状，K/R/D 扫下来值都没
    到位 —— 还差一轮"把官方库能落的帧与本 client 的帧逐字节 diff"。客户端写后**读回
    复核**（读 3 遍、每遍隔 20 ms），对不上如实回 `NCL_ERR_UNAVAILABLE`，**不回假成功**。

### 01 册：寄存器（PMC）**读通了**（2026-09-23 抓帧）

用户口径："FANUC 的 PLC 寄存器就是 PMC"。`pmc_rdpmcrng` = **0x8001**：一个块，载荷四格
= `[起始号][结束号][族][宽度]`；族 `0=G 1=F 2=Y 3=X 4=A 5=R 6=T 7=K 8=C 9=D`、
宽度 `0=字节 1=字 2=长字`、**结束号 = 起始号 + 要几个**。应答体就是数据
（1/2/4 字节一个点）。应答块的**返回码与数据长度各占 4 字节**（16 字节块头之后就是数据）。

  * **块头第 2 格官方库写 2**（别的命令都是 1）—— 这一格不对时机床回的块头是乱的，
    客户端会把它当"块返回码非 0"报错。驱动新增可选 `"first"` 覆盖，PMC 那两条用它。
  * client 新增 `ncl_focas_pmc_read()`（号段读，一段一个数组）与
    `ncl_focas_pmc_bit()`（位读：读所在字节再取位）；族认不出/越界/一段要太多都在
    本地挡下来。
  * **这台模拟器一次只回一个点**（要 8 个字节回 1 个）：段读先试一次，回得不够就
    **退化成一个号一个号读**补齐 —— 真机上次段读只会更快。
  * 适配器新增 7 条点位（与新代那几条寄存器点位同口径：LIST、`get_length` /
    `get_value`（`keys` 给号）/ `get_attributes`，**这一轮只读**）：
    `/CONTROLLER/REGISTER@{X,Y,G,F,R,K}`（位，号 = 字节×8+位）与 `@D`（字）。
    `get_length` 用 spec 上 0i-D 那版的范围（X/Y 1024 位、G/F 6144、R 64000、K 800、
    D 8000 字）；机器实际范围可以问 `pmc_rdpmcinfo`（0x8003，帧已抓、逐条布局待核）。
  * 实测（模拟器）：F = `[64,128,0,8,4,0,0,0]`、Y = `[4,0,0,1,0,0,2,0]`、
    X = `[0,0,0,0,0,0,73,1]`、D0 = 260 —— 都是真的信号位，不是桩。
  * 没做的：**写**（`pmc_wrpmcrng` 帧还没核，所以点位不声明 `set_value`）、
    定时器/计数器（T/C 是结构体那两族）。

### 01 册：宏变量表做完、主轴倍率接上、`SUBPROGRAM` 删掉（2026-09-23）

  * **`/CONTROLLER/VARIABLE`（宏变量表）做完**：`0x15` 本来就能**按号段读** ——
    `d` = 起始号、`e` = 结束号（`cnc_rdmacror`），一段回"N 条 8 字节记录"（第 j 条 =
    起始号 + j），记录形状与单条读一样。client 按 **16 个号一段**读 1..999（本机一次
    要太多会只回 1 条 / 264 字节封顶），**空号（vacant：值 0 + dec −1）不进表**，
    也不硬凑 0。`ncl_focas_macro_variables()` 与 `ncl_focas_variable_table()` 都实现。
  * **主轴倍率接上**：`IODBSGNL.spdl_ovrd` 就在**进给倍率后面那一格**（`0x5d` 的
    载荷 @0xa 是进给、**@0xc 是主轴**），码值 → 百分比与进给倍率同一张表
    （0..20 = 0%..200%）。`ncl_focas_spindle_override()` 不再回"待抓包"。
    如实记的两点：spec 的 `slct_data` 位表写着"bit6 只有 15i"，而 0i-D 头文件的
    `IODBSGNL` 有这一格；**这台模拟器这块是空的**（`0x5d` 回没初始化的内容 →
    落进码表就像 0%、落外面就报错），真机要复核。
  * **`/CONTROLLER/SUBPROGRAM` 删掉**（用户口径不要了）：官方库对 `cnc_rdexecprog3`
    自己就回 EW_PROT、一帧不发 —— 不摆一个永远读不到的点位。client 的
    `ncl_focas_subprogram_number()` 一并删除。
  * 顺带修一个真判定：`record_read()` 会把小数位夹到 0..9，"vacant"（dec = −1）会被
    夹成 0 就判不出来 —— 新增 `record_is_vacant()` 直接看原始那两个字节。
  * 单测：mock 加宏变量号段应答（1..3 号按摆的给、别的号回 vacant）与主轴倍率的
    0x5d 载荷；新用例覆盖"vacant 不进表""码值 ×10""码表外如实报错"。43/43 绿。

### 01 册：坐标系 / 当前刀号 / 轴电流三条**通了**，轴温删掉（2026-09-23 抓帧）

  * **工件坐标系**（`/CONTROLLER/COORDINATE`）：原来问的是 `cnc_rdwkcdshft`（`0x63`，
    那是"工件坐标平移"，这台机器一律 rc=1）。**读要问 `cnc_rdzofs` = `0x0b`**：
    `d = e` = 偏移号（0 = 外部、1..6 = G54..G59、7.. = G54.1Pn）、`arg2` = 轴号
    （-1 = 全轴），应答载荷 = **32 条 8 字节记录**（第 i 根轴的值在 `@8×(i-1)`，
    记录形状与位置/负载那一族相同）。**写 = `cnc_wrzofs` `0x0c`**，载荷与写刀补同形
    （`[值 BE32][00 00][ff ff]`）、一次一根轴。client 新增
    `ncl_focas_work_offset()` / `ncl_focas_work_offsets()` / `ncl_focas_work_offset_write()`；
    实测：读 G54 → `{"x":…,"y":…,"z":…}`，写 G54 的 X = 12.345 → rc=0 且读回一致。
  * **当前刀号**（`/TOOL_NUMBER`）：`cnc_rdgcode`（`0x96`）只报 G 组，刀号在**指令值**里 ——
    `cnc_rdcommand`（**`0x97`**，`d = -1` 全读模态非 G 码）回 12 字节一条的
    `[adrs][num][flag(2)][cmd_val(4)][dec_val(4)]`，取 `adrs = 'T'` 那条的 `cmd_val`。
    `ncl_focas_tool_number()` 实现；机床没给 T 那条时如实回 `NOT_FOUND`（不报 0）。
  * **轴电流**（`/AXIS@X/CURRENT`）：还是 `cnc_rdsvmeter`，只是 **`d = 3` = 安培**
    （`d = 1` 是负载表 %）。官方 `cnc_rdaxisdata(cls=2)` 发的就是这一格，本轮把它的
    `cls/type → item` 对应表也记进了 01 册 §11.19.3。`ncl_focas_axis_current()` 实现。
  * **轴温删掉**：FOCAS 里没有读轴温的调用（官方头 + spec 全文搜过），
    `/AXIS@X/TEMPERATURE` 这个点位与 `ncl_focas_axis_temperature()` 一并删除，
    不再留"待抓包"。
  * **写后复核要重试**：这台机床的坐标偏移**写完不是立刻读得到**（紧跟着读回的是旧值），
    所以写后复核读 6 遍、每遍隔 50 ms —— 别把成功报成"没写进去"。
  * 另两条这轮没抓到的（如实记在 §11.19.4）：**子程序号**（官方库对 `cnc_rdexecprog3`
    自己就回 EW_PROT、一帧不发）、**主轴倍率**（`cnc_rdspdata` 这份 SDK 没这个导出；
    `IODBSGNL` 的 bit6 = 主轴倍率信号 spec 写明只有 15i 用）。
  * 单测：mock 新增轴名表（`0x89`）与坐标偏移表（`0x0b` 读 / `0x0c` 写，写进去就更新，
    好让"写后复核"真走一遍），三条新用例覆盖坐标系（含 `G54.1P3` 这种名字）、
    刀号（含"没给 T 那条"）、轴电流；43/43 绿。

### 01 册：**程序下发（写内容）通了** —— 病根是两条 TCP 有分工（§11.18）

  * 症状（上一轮留下的开口项）：本 client 发的 `0x11` 与官方 SDK **逐字节相同**，
    机床对 SDK 回 256 字节、对本 client 直接断连接（§11.14.4 排了一整张"排除表"）。
  * 病根：**不是帧，是道** —— 机床把两条连接分了工，**hello 计数器 1** 那条只收传输帧
    （`0x11/0x12/0x13`、`0x15/0x18/0x19`），**计数器 2** 那条只收命令帧（`func 0x21`），
    发错一族机床一声不响就断连接；而且分的是**计数器**不是先后（把两边的计数器对调，
    角色跟着对调）。本 client 当时把三条传输帧跟业务调用一样发在计数器 2 那条上。
  * 修法：`focas_handshake()` 把两条都 hello 完之后**按计数器**定角色 —— `ctx->socket`
    = 命令通道、新增 `ctx->transfer` = 传输通道；`focas_transfer_exchange()` /
    `focas_send_only()` 改走传输通道。默认 `helloCounter = 1`，与官方库一致。
  * 实测（NCGuide 0i-MF Plus）：`cnc_dwnstart4 → cnc_download4 → cnc_dwnend4` **`rc=0`**，
    程序真进机床（目录里多出这个号、按文件名读回来一字不差）；下已存在的号回
    `EW_DATA` + 细码 4（号已存在）、目录名写错在 start 那一步就回 `EW_DATA` + 细码 1。
  * 顺带校准 **EW 返回码表**：`5` 是 **`EW_DATA`**（不是 `EW_ATTRIB`），`4` 才是
    `EW_ATTRIB`（照官方头 `Fwlib64.h` 逐条重抄）；状态回执体
    `[返回码 4][细码 2][err_dtno 2]` 里的**细码**现在一起进 `last_error`
    （`… 机床回码 5（EW_DATA（数据错）），细码 4（这个程序号已经登记过（下行 end））`）。
  * **程序目录改成翻页**：`cnc_rdprogdir3` 一次只回 8 条，原来只发一条请求 → 列表少一半
    （现场"程序列表不全"就是这么来的）。现在顺着 `d`（起始程序号）问到空为止，并滤掉
    "不认 `d` 的机床"重铺的旧号（宁可少列、不会重复）。
  * 流式**上行**在正确的通道上重测：`0x15` 收 256 字节应答、`0x18` **六种形状**
    （`dir 1`/`dir 4`、体 4/8/1024/1400）一律不答、`0x19` 回 `EW_REJECT=13` ——
    机床就是不给上行。取程序继续走 `cnc_rdpdf_line`（0xf0，§11.16）。
  * 新增 `tools/site-probe/focas_channel_probe.py`：一帧不改、只换连接，把"是帧错了还是
    道错了"一次分清；`focas_tap.py` 的日志加上**连接标签**并按连接加锁（不串行错位，
    才看得出哪条帧走哪条连接）。
  * 单测：mock 现在**照机床的分工来**（按 hello 计数器认角色、发错道就断连接），并在下发
    用例里断言"传输三件套走计数器 1、命令帧走计数器 2"——把 `focas_transfer_socket()`
    改回旧写法，测试立刻变红。

### 01 册：程序文件那一族（建/读/删）在以太网上都通了

  * **建**：`cnc_pdf_add` = Cb **`0xb5`**（`d` = 0 文件 / 1 文件夹 + 256 字节路径载荷），
    实测 `rc=0`，机床自己写程序号那一行（内容是 `O2200%` —— 末尾 `%`、**没有换行**）。
  * **删**：`cnc_pdf_del` = Cb **`0xb6`**（同一个形状）—— 上一轮判它"码不对"是拿**不存在的
    路径**试的（机床回 `EW_ATTRIB`），对存在的路径 `rc=0`。client 的删程序改走它。
  * **读**：`cnc_rdpdf_line` = Cb `0xf0`（§11.16）。修了一个真 bug：建出来的内容末尾是 `%`
    不是换行，原来用"数到的换行数"当"有没有读到东西"会误报"没有这个程序"；改成按**文字
    长度**判，行数只用来推进。
  * client 新增 `ncl_focas_program_create(name, folder)`；`ncl_focas_program_delete()`
    改走 0xb6（路径形式，裸文件名/`O1234` 自动补默认文件夹）。
  * 排查记录：并行跑两个官方 SDK 下载**都成功** → 机床不是"同时只服务一个客户端"；
    下载前先做文件族读/建文件/写刀补都不影响 → 不是"要先点着传输"。写内容那条仍卡在
    §11.14.4 的"会话属性"上（帧逐字节相同也没用）。

### 01 册：**取程序通了** —— 按文件名按行读（`cnc_rdpdf_line`，Cb `0xf0`）

  * 流式上行（`cnc_upstart4`/`cnc_upload4`）在这台模拟器上卡死在 `0x18` 不答
    （机床连理由都不给：`cnc_getdtailerr` 细码 0）。换**文件那一家族**试通了：
    `cnc_rdpdf_line` = Cb **`0xf0`**、`d` = 起始行号、`e` = 一次读几行、
    **载荷 = 256 字节的程序路径**（`tag1` = 256、块长 = `0x1c + 256`），
    **应答体就是程序正文**。
  * client：`ncl_focas_program_upload(type = 0, name, &program, &len)` 现在**真读**：
    一次 64 行分页读、末行没有 `'\n'` 或机床报错就收尾、上限 256 KiB；`name` 带 `/`
    原样用，裸文件名自动补 `//CNC_MEM/USER/PATH1/`；读不到如实回 `NOT_FOUND`。
    实测读 O3001 → `rc=0`、384 字节正文（`O3001(SUBPOCKET) … M99 %`）。
  * ⚠️ 手册把 `cnc_rdpdf_line` 标成 **HSSB 专用**（支持表 `O-O-`），这台模拟器的
    以太网照答 —— 真机上要复核（换机器先打一次，见 §11.16）。
  * 新增 `tools/site-probe/focas_inject.py`：**借官方 SDK 的会话、代理在中间插帧**，
    用来扫那些"自己的客户端连 start 都被关"的传输帧（就是靠它扫出 `0x19` 会被答、
    回 `dir 3` + `EW_REJECT`/`EW_PATH`）。01 册新增 §11.16。

### 01 册：程序下行的"配置"就是数据格式 —— 开头 LF + 结尾 `%`（用户提示后查官方 spec 查出来的）

  * 官方 spec（`Document/SpecE/Program/cnc_download4.xml`）写死：`LF Block1 LF … LF %`，
    "**'LF' must be placed at the top of the whole program, and `%` at the end**"、
    "**address 'O' and program number must be placed in the program**"；例子就是
    `"\nO1234\nG1F0.3W10.\nM30\n%"`。
  * **实测**：同一台 NCGuide 0i-MF，正文少开头 LF、少结尾 `%` → end 回 `EW_ATTRIB=5`、
    程序进不去；按 spec 补上 → start/download4/end **全 0**，程序目录立刻出现
    `{"number":1}`。所以先前那个"机床收不下"不是开关没配，是**正文格式不对**。
  * client：`ncl_focas_program_download()` 现在自己补齐并校验（`program_frame()`：
    开头 LF、结尾 `%`、`O<号>` 行），调用方给一份普通程序文件即可；start 帧要的仍是
    **目录**（`program_dir_part()` 取目录那一段）。
  * spec 里另外几条现场注意事项也记进 §11.15.1：上传的 `file_name` 三种写法与
    `*length ≥ 256 且为 256 的倍数`、读回来最后一个字符是 `%` 再读是 `EW_RESET`、
    `EW_DATA` 细码（文件夹名错/程序数满/同号已注册/同号正被选中）、`EW_PROT`
    （O8000-/O9000- 保护）、`EW_REJECT`（加工/复位/换模式中不能传）、`EW_PARAM`
    （参数写入使能）、`cnc_saveprog_start/end`（频繁注册删除时用），以及"会话只有两条
    TCP"（第三条连接 hello 回 `dir 3` + 码 4）。

### 01 册：程序上下行的两条现场口径 + 传输状态回执在方向 3（同一天，接着写这一侧）

  * **现场口径（用户给的，FANUC 的规矩）**：程序正文**第一行必须是程序号**
    （`O0001` / `O00001`），机床从这一行认程序号。client 对 `type = 0`（NC 程序）先挡
    一道：正文没有这一行就回参数错（不发注定被拒的帧）；type 1..5（刀补/参数/宏变量…）
    不查这一条。
  * **start 帧给的是目录**（`//CNC_MEM/USER/PATH1/`），不是文件名 —— 给文件路径机床在
    start 那一步就回 `EW_ATTRIB=5`（官方 SDK 也一样）。`ncl_focas_program_download()`
    现在自己取"目录那一段"，调用方给文件名也行。
  * **传输的状态回执在方向 3**：`0x13`（end）的应答是 `dir 3`、体前 4 字节是机床返回码
    （实测 `00000005` = EW_ATTRIB）。以前这一族被当成"没有这个数"，等于把"机床为什么不
    收"整条信息丢了；现在 `focas_transfer_exchange()` 单独认，翻成
    `NCL_FOCAS_ERR_TRANSFER(码)`，`last_error` 出人话（`机床回码 5（EW_ATTRIB…）`）。
  * **这台模拟器上两个方向都到不了**（**官方 SDK 同样**）：下行 start/download4 都 rc=0、
    end 回 `EW_ATTRIB=5`（目录里不会多出程序）；上行的 `0x18 dir 4` 数据请求机床一声不响
    （SDK 回 `EW_DATA=10`）。上行仍是"读不到"，但缺的东西更清楚了：请求帧已核，差的是
    **应答里程序文本的切法**，要一台肯答 `0x18` 的真机。
  * **一条没解决的如实记进 §11.14.4**：本 client 的 `0x11` 与 SDK 逐字节相同，机床却只回
    SDK 那份、对本 client 直接关连接（用裸 socket 重放 SDK 的字节同样被关）；机理未明，
    client 侧只保证"拿不到应答就报传输错、不假装成功"。
  * 探针补 `--data TEXT`（下行正文），另加 `focas_replay.py`（裸 socket 重放，用来分辨
    "帧不对"还是"客户端不对"）。

### 01 册：写这一侧打通（刀补写进去又读回来）+ 单条读/写的 `d=e=号` + 机床"有什么/没什么"清单

  * **写刀补通了**：`cnc_wrtofs` = **`0x09`**，`d = e = 刀补号`、`arg2 = 1000 + 类型`、
    载荷 8 字节（BE32 值 + `0000` + `ffff`）。**关键那一格**：载荷长度写在块的
    **`tag1`**（`[26..28)`），块长 = `0x1c + 载荷`；上一轮把长度写进 `tag0` 才会被机床回
    `EW_LENGTH=2`。对 NCGuide 0i-MF Plus 实测：读 1 号刀补 `0.008` → 写 `2.2345` → 再读
    `2.234`（一致）→ 还原 `0.008`。`ncl_focas_tool_offset_write[_typed]()` /
    `ncl_focas_tool_param[_write]()` 与适配器的 `/CONTROLLER/TOOL` 的 `set_value` 都开。
  * **单条读/写 `d` 与 `e` 都是号**（以前写死 `e = 1`，于是只有 1 号读得出来）：
    改过来之后刀补 2 号、参数 6711、宏变量 500 全通。
  * **两处以前读错，已纠正**：加工件数改读 **6711 号参数**（原来是 `cnc_rdcount`，那是
    刀具寿命计数器、机床要开选件，这台回 `EW_NOOPT`）；**参数的值在载荷 `@8`**，不是 `@0`
    （`@0` 是参数号自己 —— 只有 1 号看着对）。
  * **新补的读**：`TOOL`/`TOOLPARAM` 刀具表（`cnc_rdtofsinfo` = `0x0a` 给号上限，本机 400；
    元素 `{id,kind,radius,length,radius_wear,length_wear}`）、轴扭矩（`cnc_loadtorq` =
    `0xfd`）、参数表（逐号 `0x8d`）、删程序（`cnc_delete` = `0x05`，帧照官方 SDK 抄，
    这台机器不收）。
  * **机床自己的返回码 `1`/`6` 翻成 `NCL_ERR_UNAVAILABLE`**（"这台没有"）：`PART_COUNT`、
    `TOOL_GROUP_COUNT`、`cnc_rdtooldata`（`EW_FUNC`）、寿命管理 / 用户宏变量的读
    （`EW_NOOPT`）现在都是明确的"机床不提供"，不再是"模块错"。
  * **写参数 / 写宏变量：帧收下了但值没对**，所以两条都做**写后复核**：读回来不对就回
    `NCL_ERR_UNAVAILABLE`（写参数这台收下但不生效；宏变量这台差 10 倍的刻度），
    **绝不回成功**。程序上传仍未通（SDK 在这台机器上不发帧）。
  * 工具链：探针补 `--in HEX`（铺初值，写的那几条全靠它）与 `--shape`；新增
    `focas_capture.py`（一条命令抓一串）与 `focas_log_summary.py`（两个客户端逐帧对齐，
    就是靠它把 `tag0/tag1` 那一个字节对出来的）。01 册新增 §11.13。

### 01 册：接上 VM 里的 FANUC 模拟器，把"读"逐条核了一遍（写还差抓包）

  * **环境**：VMware 里那台 `Fanuc CNC Guide & NC Trainer plus`（CNC Guide，机床显示
    `Series 0i-MF Plus` / `0M D4G3` / `28.0`）。用户给的 `169.254.178.13` 是客户机自己的
    APIPA（VM 网卡桥接、链路没 DHCP），主机没路由够不着；把 vmx 的 `ethernet0.connectionType`
    设成 `nat`（`vmrun stop/start`，不占 GUI、不要管理员）之后，客户机 DHCP 到 192.168.79.128，
    主机走现成的 `8193 = 192.168.79.128:8193` 转发就通了。
  * **读**：新增 `tools/site-probe/focas_live.c`（拿本仓库 client 把语义接口逐条打一遍）对着
    真模拟器跑：状态/模式/型号/版本/程序名/行号/程序号/进给/倍率/主轴转速、**X/Y/Z 的
    position 一族（含 machine/relative/cmd/distance/srv_delay/load/feedrate）**、轴类型、
    执行程序段、参数 #1、刀补 #1、程序目录、模态、报警 全部 rc=0；A/C 轴这台没有（-6）、
    负载/电流/温度仍是桩（-15）。01 册新增 §11.12 记全过程。
  * **写**：按"读的码 + 1"猜的三条（`0x8d→0x8e` 参数、`0x15→0x16` 宏变量、`0x08→0x09` 刀补，
    值作命令块后面的载荷）**被机床拒**（`NCL_FOCAS_ERR_RB_CODE`）→ 三条**退回 `not_yet()`**，
    只留下可复用的机制：`call("payload")` 现在支持带 `data`（请求载荷 + 块 `tag0` 长度）。
  * **抓包路子打通**：官方 SDK（`D:\downloads\focas-test2x64\Fwlib64.dll`）必须**在它自己的
    目录里**跑（否则依赖 DLL 找不到，`cnc_allclibhndl3` 回 `EW_SOCKET=-15`）；
    `focas_sdk_probe64.exe` + `tools/site-probe/focas_tap.py 8194 127.0.0.1 8193` 已经能对着
    模拟器把 SDK 的帧原样抄下来（这一轮的 tap 日志 310 行）。探针的 `kCalls` 补了写入那几条，
    但入参还是全零缓冲区 → SDK 本地判 `EW_NUMBER` 不发帧；下一步给探针加 `--in HEX` 铺初值即可。

### 01 册：拿 10 册（新代）的代码核对 FOCAS 的缺项 + 修掉 pull 不落盘

  * **新增 §11.11 缺项核对**（逐条按代码对，不看注释）：把两边的 client 头文件与适配器
    点位声明摆在一起，client 落到 `not_yet()` 的算"桩"、适配器没声明的算"没摆出来"。
  * **结论要点**：① **PLC/寄存器/位整格缺**（新代 `/CONTROLLER/REGISTER@{R,I,O,C,S,A}`
    读写都在，FOCAS 连 `pmc_*` 都没接）；② 参数/变量/刀补**写**是桩（新代三个都能写）；
    ③ 三个位置格（机械/相对/剩余）与执行程序段、程序目录、模态**只差声明/接线**；
    ④ 轴名发现（`cnc_rdaxisname`）client 没有；⑤ G 代码文件只接了 push/pull/remove
    （新代还有 exist/dir_exist/new/create/delete/copy/move/list），且 `pull` 不落盘。
  * **顺手修掉真 bug**：`plugins/focas.c` 的 `focas_file_pull()` 原来 `(void)path;` ——
    取回的程序字节被直接丢掉，本地什么都不写；按新代的写法改成 `ncl_file_write_all(path, …)`。
  * 纠正三处**注释与代码漂移**：`feed_speed` / `feed_override` / `axis_load` 头注释写着
    "还没实现"，实现其实都在（已实现 48 条 / 桩 22 条）。
### 文档：01 册（FANUC FOCAS）按 10 册（新代）的蓝本重排

  * **结构对齐**：01 册从原来的"速查 / 连接（含一大堆解剖档案）/ 函数表 / 类型 / 错误码 /
    坑 / 参考实现"改成 10 册那套 —— 速查、连接建立、**帧格式**、常用函数表（按域）、
    **地址·数据类型·数据字典**、**典型流程**、**同包附带能力**、实现坑、参考实现与待办、
    **线协议解剖**（这些结论怎么钉出来的）、**本仓库实现**（逐能力的码/帧 + 验证程度）。
  * **原来的 2.x 解剖档案整段搬进 §10**（NCGuide、官方 SDK 反查、控制轴闸门、真机两条 TCP
    与 8 字节记录、报警、程序目录、模态、这台机器做不到的那些），内容一字未改，只是归位；
    帧格式 / 应答体 / item 码表归 §3，函数表顺延到 §4。
  * **新增 §5.2 数据字典映射**（册 4 的数据项 / 册 7 的集合对象 → FOCAS 调用 → 验证状态）、
    **§6 典型流程**（采集循环 / 报警 / 程序上下行 / 写）、**§7 同包附带能力**（881 个函数的
    分布：cnc 758 / pmc 95 / flnt 14 / ds 8 / pbm 3 …）、**§11 本仓库实现**（会话与验证环境、
    点位一览、位置一族、负载一族、参数与宏变量、刀补与刀具、报警、程序与文件、写这一侧、
    两条口径）。
  * **交叉引用跟着改**：21/29/31/32 册与 README 里引 01 册的那几处（`§2.3`→`§3.2`、
    `§2.4`→`§3.3`、`§2.5.x`→`§10.1.x`、`§2.6`→`§10.2`、`§2.8.x`→`§10.4.x`），册内引用同步。
### SYNTEC：G 代码上下行落地（文件服务接进 client 与适配器）

  * **client**：新增文件服务的一整套 —— `ncl_syntec_file_exist/dir_exist/file_new/dir_create/
    file_delete/file_copy/file_move/file_list`，以及 `ncl_syntec_file_push()`（下发，一帧一块
    ≤ 4 KiB 的 `FileSending`）与 `ncl_syntec_file_pull()`（取回，按 `min(块, 剩余)` 分块）。
    会话多了 `config.file_port`（默认 5572），**`file_port == port` 时共用会话那条连接**。
  * **适配器**：接上仓库的文件工具（像 FOCAS 那样 `ncl_file_tool_set_backend()`），设备侧的
    `/CONTROLLER/FILE` 于是有了 `push`（下发 G 代码）/ `pull`（取回）/ `remove`（删程序）；
    新增参数 `filePort`。
  * **测试**：mock 补文件服务（同一条监听按 uFuncID 分流，且记住 Start 之后的路径与已收字节），
    用例覆盖路径帧是 UTF-16 且**长度按字符数**、上传→存在性→取回→逐字节比对→删除，
    以及跨块的大文件（4 KiB + 100 字节）。
  * **21A 实测（本仓库 C 客户端，全部 rc=0）**：`DirExist(C:/CNC)` → true、push 50 字节 →
    `FileExist` true → pull 50 字节**逐字节一致** → delete → false。
  * 还留着：`GetAllFileList` 在这台上回 0 个（列表语义待真机再核）；程序下发按 10 册 §6.2
    是危险操作，权限照旧在适配器外面。
### 新代：G 代码上下行的协议打通了（文件服务在 5572，还在等接进 client）

  * **先把服务找出来**：§4.7/§6.2 那 12 个程序/文件 API 不走 KrnlAPI，而是一套**独立的文件服务**。
    四个监听端口分别是：**5566** = 设备/KrnlAPI（刀具、参数、PLC、变量都在这里）、5568 = 要客户端
    先握手、5570 = 对任何命令回空帧、**5572 = 文件服务**。
  * **帧与语义**（10 册新增 §11.11）：12 字节包头 + `{ uFuncID, ... }`，dispatch 在 uFuncID 上
    （= `FileTransferCmd`：1 FileSendStart / 2 FileSending / 3 FileRecvStart / 4 FileRecving /
    8 GetAllFileList / 11 FileExist / 12 DirExist / 13 FileNew / 14 FileDelete / 15 FileCopy /
    16 FileMove / 17 DirCreate / 48 Install）。路径是 **UTF-16LE**、长度字段是**字符数**；
    `FileRecving` 的应答是**裸数据**（没有 4 字节头）。**这套是带状态的**：路径记在连接上，
    所以一次传输必须共用一条连接（每条命令另开一条会 `hr = -1`）。
  * **21A 实测**（探针进仓库 `tools/site-probe/syntec_file_xfer_probe.py`）：49 字节的 G 代码
    从 `FileSendStart` → `FileSending` 上传（hr=0）→ `FileExist` 变 true → `FileRecvStart` 报
    `nFileLength = 49` → 按块 `FileRecving` 取回**逐字节一致** → `FileDelete` 删掉（回到 false）。
    `DirExist` 确认这台上有 `C:/CNC`、`C:/Job`、`C:/MPF`（`C:/CNC/` 带尾斜杠反而不存在）。
  * **还没落地**（这一轮只到"协议通了 + 探针"）：client 侧还没有到 5572 的文件服务连接与命令；
    适配器也还没接仓库的文件工具（`/CONTROLLER/FILE` 的 push/pull/remove，FOCAS 已经接了）——
    接上之后 G 代码上下行就是现成的三条。`GetAllFileList` 在这台上回 0 个（列表语义待真机再看）。
### SYNTEC：寄存器 / 位 / 变量能**写**了（2026-09-22，21A 实测）

  * **写这一侧的码与帧**（都是 `{ nNo, 新值 }`，帧 = 16 字节桩头 + In）：
    `0x041B` R 寄存器（In 8）、`0x0413`/`0x0416`/`0x0418` I/C/S 位（In 8，u8 值）、
    `0x0422` 变量（In **20** = `{ nNo i32, TOcVariant 16 }`，值在 `[12..]`，
    按控制器侧 `OCK_TOcVariantToPtr` 摆）。`A = dwSizeOut = 4`（Out 只有 `hr`）。
  * **client**：`ncl_syntec_plc_register_put()` / `_plc_bit_put()` / `_variable_put()`，
    外加通用写帧 `syntec_krnl_frame()`（任意长 In）；顺带把参数写 `0x0403` 的 `dwSizeIn`
    从 4 纠正成 **8**（`In_OCK_ParamPutValueParams` 就是 8 字节）。
  * **适配器**：`/CONTROLLER/REGISTER@R|@I|@C|@S` 与 `/CONTROLLER/VARIABLE` 声明 `set_value`
    （`keys` 给号、`value` 给新值；变量按字面量分整数/浮点变体）。**`@O`、`@A` 不声明写**：
    O 位只能 Force（`0x0494`）、A 位没有写接口，写了明确回 `NCL_ERR_NOT_SUPPORTED`。
  * **现场口径（写后一定读回确认）**：`hr = 0` 只代表控制器收下了 —— 这台 21A 上 `#500`、
    `#2000/#5000/#9999` 写了读回不变（号由控制器自己管）；位归梯形图（C0 连写三次读回
    0/0xFF/0/0xFF/0，S 位稳定）。所以 `set_value` 答的是"写下去的值"，确认要再查一次 `get_value`。
  * **21A 实测（本仓库 C 客户端，全部 rc=0）**：`R4000=123456 → 读回 123456 → 还原 0`；
    `S/C 位写 1 → 读回 0xFF → 还原 0`；`#700=31337 → 读回 type=1 int=31337 → 写回"空"`；
    `O0=1` 回 `NCL_ERR_NOT_SUPPORTED`（-8）。
  * **测试**：mock 补三条写分支（寄存器/位/变量）+ 写帧逐字段断言（In 8 / 8 / 20、
    `dwSizeIn`、值的位置）、写回读、`hr != 0` 回 `NCL_ERR_IO`、O/A 位拒；适配器级 Set 三条
    （寄存器置数并读回、变量整数、变量浮点）。**ctest 43/43**。
### 修正：HASH 才有 get_keys、LIST 才有 get_length；PLC 改成一族一条 REGISTER（2026-09-22）

  * **操作按取值形状分**（册 4）：`get_keys` 是 HASH（dict）的操作，`get_length` 是 LIST 的，
    两者不是同一件事、也不该同时声明。于是：
    `/CONTROLLER/PARAMETER`（dict/HASH）= `get_keys` + `get_value` + `get_attributes` + `set_value`
    （去掉 get_length）；`/CONTROLLER/TOOL`、`/CONTROLLER/VARIABLE`（list/LIST）= `get_length`
    + `get_value`（+ 刀具的 `set_value`）+ `get_attributes`（去掉 get_keys）。
  * **这条规则进了核心**：`src/tool/tool.c` 装载时按 dataType 核对声明的操作，声明反了会
    警告（"只有 HASH 答 get_keys" / "只有 LIST 答 get_length"）。
  * **PLC 改成"一类、一族一条"**：新增数据对象类型 `REGISTER`（册 4 表 7 没有这一格，
    是扩展；`k_data_types` 里按 LIST 摆：按号排的表），六个点位
    `/CONTROLLER/REGISTER@{R,I,O,C,S,A}` —— R 寄存器（0..65535）与五种位（各 0..511），
    族写在 `number` 上（现场口径："PLC 都是 REGISTER，number 是 R / I / …"）。
    每一族答 `get_length`（这一族有多少个）+ `get_value`（keys 给号）+ `get_attributes`。
  * **测试**：新增"HASH 答 get_keys 且不答 get_length"/"LIST 答 get_length 且不答 get_keys"
    两组黑盒用例，以及 REGISTER@R 的 get_length（65536）/ get_value（771 → 1000）/ 越界拒、
    REGISTER@I 的取位；模型 72 个点位。**ctest 43/43**。
### SYNTEC：PLC（位 / R 寄存器 / 定时器 / 计数器）与变量能读了（2026-09-22，21A 实测）

  * **码表补齐**：控制器侧 `Syntec.OpenCNC.OCK_CODE` 的静态构造里 433 个 code 全取出来了
    （`CODE(type,id) = (type<<10)|id`），PLC 与变量这一族终于有名有姓：`PlcGetIBit` 0x0412、
    `PlcGetOBit/CBit/SBit/ABit` 0x0414/15/17/19、`PlcGetRRegister` 0x041A、`PlcGetTimer/Counter`
    0x041C/1D、`PlcGetCapacity` 0x041E、`NcGlobalGetValue` 0x0421、`NcGlobalGetCapacity` 0x0423、
    `NcStateGetCapacity` 0x0408。帧形状与 §11.7 的刀补同一套路（In 跟在 16 字节桩头后面，
    `A` = `dwSizeOut`）。10 册新增 §11.9。
  * **顺手解开一个老疑问**：九项里的 PART_COUNT / SPDL_SPEED 用的就是 `0x041A` —— 它们本来就是
    **PLC R 寄存器读**（R1000 / R771）。现在按号读 R771 读出来还是 1000，两条路对得上。
  * > **下面的路径下一版就改了**：PLC 后来改成 `REGISTER` 一类、一族一条
    > （`/CONTROLLER/REGISTER@R` …），操作也按 HASH/LIST 分开 —— 见上面那条修正。
  * **新增客户面**（都只答读）：`/CONTROLLER/PLC/REGISTER`（R 寄存器表）、
    `/CONTROLLER/PLC/{I,O,C,S,A}BIT`（五种位表）、`/CONTROLLER/VARIABLE`（变量表，册 4 表 7 的
    `VARIABLE`，与 FANUC 的宏变量表同一个位置）。前两个名字是**扩展**：册 4 没有 PLC 这一格，
    现场网关那侧也没有对应路由，文档里写明。
  * **client**：`ncl_syntec_plc_capacity()` / `_plc_register()` / `_plc_bit()` /
    `_variable_capacity()` / `_variable()` + 各自的帧与解码（`TOcVariant` 16 字节：i16 类型 + 值）。
  * **21A 实测**：容量 = I/O/C/S/A 各 512、R 65536、T/C 各 256；全局变量 14096 个；R771 = 1000；
    #500 = 整数 1；位读答 hr=0（这台的梯形图没跑，位全是 0）。**R 寄存器写**现场试过：
    写 R3000 = 123456 → 读回 123456 → 还原 0。**变量写不算验过**（`0x0422` 的 In 结构体在
    控制器侧没找到），所以没接线进来。
  * **测试**：mock 补 PLC 容量/位/变量三条分支 + 一张寄存器由条目表提供的"同源"分支；
    新增设备级用例（容量、R771、位帧码字、变量 int/double 两条、帧字段断言），模型点位数 65 → 72。
    **ctest 43/43**。
### SYNTEC：刀补**能写了**，线协议那 16 字节桩头也补对（2026-09-22，21A 实测）

  * **先把线协议读对**：`OCAPIServer.exe` 是 .NET，反汇编定下真实结构 ——
    包头之后**没有**那个 8 字节 function header，`m_WorkBuffer` 直接就是
    `MMI_Request_KrnlAPI { uFuncID i4, dwCode i4, dwSizeIn i4, dwSizeOut i4, pBufferIn }`，
    所以 `[12..15] uFuncID`、`[16..19] dwCode`、`[20..23] dwSizeIn`、`[24..27] dwSizeOut`，
    **In 本体从 [28..] 起**（§3.1 里原来的 “type = 4 / 参数 A / 参数 B / flag” 就是这四格）；
    应答 = 12 字节包头 + 传输层 hr + Out，而**每个 `Out_OCK_*` 的第一个字段都是 `hr`**，
    即 `[16..19]`（`[20..]` 才是 Out 的第二个字段起）。10 册新增 §11.8 记这一段。
  * **写刀补上线**（`0x0440` `NcPutToolCompensation`）：`/CONTROLLER/TOOL` 的配置对象现在
    同时声明 `get_value` 与 **`set_value`**。一帧 256 字节 = 12 + **16 字节桩头** + 228 字节 In
    （`{ nToolNo i32, TToolOffset 224 }`，控制器侧 `SizeOfOCK_ToolOffsetArray()` = 4 + 224）；
    写着给一个对象、**只写要改的字段**（先读回整条打底再覆盖），未知字段当场拒（写错名字不静默）；
    `hr != 0` 回 `NCL_ERR_IO` 并写进 `last_error`。**权限/白名单照旧在适配器外面**。
  * **刀号从 1 起，读写同一个号**（和 `/CONTROLLER/TOOL` 的 key 一致）：写第 5 把、读第 4/5/6 把，
    只有第 5 把变 —— 原先 “B = 刀号索引（从 0）” 是零数据下看不出来的偏移，已更正。
  * **两处更正**：参数写入 `0x0403` 的 `A` 由 12 改成 **4**（`Out_OCK_ParamPutValueParams` 只有
    `{ hr }`，`A` 就是 `dwSizeOut`）；`hr` 从 `[20..23]` 改读 **`[16..19]`**（新增
    `ncl_syntec_reply_hr()`）—— 之前那格读的是 Out 的第二个字段，真机上"拒绝"会被当成成功。
  * **21A 闭环**：本仓库 C 客户端造出 256 字节写帧 → 模拟器 `hr = 0` → 读回正是写下去的值
    （`nose=3 radius=0.750 rwear=0.250 len0=1.500 angle=60.0`），邻刀未动，**并且落盘**
    （`SysData/CNC/ToolTable.Dat` 的 sha256 变了，验完已按备份还原）。探针进了仓库：
    `tools/site-probe/syntec_tool_write_probe.py`（写—读回—还原一条龙）。
  * **测试**：新增写帧 256 字节的逐字段断言（Length/dwCode/dwSizeIn/dwSizeOut/刀号/记录偏移）、
    写—读回、`hr != 0` 回 `NCL_ERR_IO`、0 号刀拒；适配器级补 `set_value` 两条（改两个字段、
    未知字段拒）。mock 的应答也按真形状改成 `{ 传输层 hr, Out }`。**ctest 43/43**。

### 新代 SYNTEC 适配器：九项按 10 册 §3.1/§3.2 的现场闭环实现

  * **client 侧**（`clients/include/nclink/clients/syntec.h` + `clients/syntec/`）：§3.1
    的九项做成一张表（flags / code / 请求号 / 参数 A / 参数 B / 标志），
    `ncl_syntec_item_frame()` 逐字段拼出那 36 字节（`uSerial` 是唯一会动的字节，与
    抓到的帧逐字节一致）；应答按 §3.2 读（数字项取 `[20..21]` 的 u16、PROGRAM 整段
    文本、WARNING 空正文 = `[]`）。会话是 `ncl_syntec_open()` / `ncl_syntec_close()`，
    九个语义函数 `ncl_syntec_status()` … `ncl_syntec_warning()`；FEED_SPEED 老实地问
    三帧（寄存器 700 → 状态 12 → 状态 76）再合并。
  * **适配器**（`plugins/syntec.c`）：9 个点位 —— `/STATUS`、`/PART_COUNT`、
    `/CONTROLLER/PROGRAM`、`/WARNING`（这四样进默认采样通道，现场口径）+
    `/LINE_NUMBER`、`/FEED_OVERRIDE`、`/SPINDLE_OVERRIDE`、`/FEED_SPEED`、
    `/MOTOR@S1/SPEED`；一个 `/SESSION` 调试方法（会话状态 + 上一次失败的原话）与审计
    原始帧。只读：client 里没有写调用，适配器就不声明写。
  * **两处缺口如实标注**，都不编数：FEED_SPEED 的单位换算表只实测过档位 (0,0)；其余
    档位、以及 WARNING 的非空条目布局，都回 `NCL_ERR_UNAVAILABLE` 并写明"待抓包"。
  * **测试**（`clients/tests/test_syntec_driver.c`）：STATUS 请求逐字节对照文档里那张
    完整帧；九项各读一次；单位档 / 空报警 / 非空报警三种边界；最后把 `plugins/syntec.c`
    **当模块装载**、由宿主读九个点位 —— 适配器 + client + 抓到的帧一起跑，就是 10 册
    说的"整机仿真"（适配器的集成测试挂在 client 的套件里，由 `plugins/CMakeLists.txt`
    把模块目录与测试目标接上）。
  * `conf/syntec.json`（交付配置）与 10 册 §11（实现落点 + 缺口）同批落地。

  * **数据项路径按 iNC-BOX 的模型定义对齐**（2026-09-22）：`/LINE_NUMBER` →
    **`/CONTROLLER/LINE_NUMBER`**、`/WARNING` → **`/CONTROLLER/WARNING`**、
    `/MOTOR@S1/SPEED` → **`/SPINDLE_SPEED`**；其余六条（`/STATUS`、`/PART_COUNT`、
    `/FEED_SPEED`、`/FEED_OVERRIDE`、`/SPINDLE_OVERRIDE`、`/CONTROLLER/PROGRAM`）
    本来就同名。现场 9 项与新代适配器现在**逐条同名**，对齐后重跑 21A 模拟器自检仍
    **9/9 可读**。仓库里 KND 早就是这套命名；**FANUC 是唯一的历史差异**（`/LINE_NUMBER`、
    `/WARNING`、`/MOTOR@S1/SPEED` + 轴按字母的 `POSITION@REAL` 型），未动 —— 它对已接
    FANUC 模型的现场是破坏性改名，要改单独一轮（10 册 §11.2）。另外记明一处与册 32 字典
    的偏离：`SPINDLE_SPEED` 不在表 1-9 的类型表里（表 4 只有 `SPEED`，主轴按表 2 归
    `MOTOR`），这次跟 iNC-BOX 走。

  * **位置四组落地**（2026-09-22）：从模拟器自带的 `Syntec.RemoteCNC.Win32.dll`
    （同版本 10.116.54）反解出**状态区号**——机械坐标 101、相对 141、绝对 181、剩余距离
    221、轴小数位 261——并把读法在 21A 上试定：`request 0x0407`，`A = 4 + 2×轴数`，
    `B = 区号`，应答是**轴数个 int16**，真实值 = 原始值 ÷ 10^小数位（`B=261` 答 3，
    与画面 `0.000` 一致）。适配器新增 8 个点位（4 组 × X/Z）：
    `/AXIS@X|Z/MOTOR/POSITION`（**机械坐标 = 实际位置**）与
    `/AXIS@X|Z/MOTOR/VARIABLE@ABSOLUTE|RELATIVE|DISTANCE`（iNC-BOX 的格子）。
    **跟随误差不做**（用户口径），指令位置随之也不需要。21A 上 `--once`：**17 个点位
    17 个可读、0 失败**（八个位置点位 0.0 = 画面 X/Z `0.000`）；mock 用例另把 X 摆
    1234、Z 摆 -500、小数位 3 → 读到 1.234 / -0.5，证明 int16 + 10^-dec 这条解码。

  * **21A 模拟器联调（2026-09-22，真靶机）**：`SYNTEC 21A 模拟器 10.116.54N`
    （`CncMon32.exe` + `OCAPIServer.exe`，端口 **5566/5570/5572**）上跑
    `ncl_server -c conf/syntec-sim.json --offline --once` → **9 个点位 9 个可读、0 失败**，
    读到的值与模拟器自己的画面逐项对上（STATUS=free／PART_COUNT=0／LINE_NUMBER=1／
    两个倍率=100／FEED_SPEED=0.0／主轴转速=1000／WARNING=[]）。
    联调改了两处实现：**序列号**（模拟器与官方客户端都发 0、且**不回显**——原先要求回显
    把九项全挡了；现在发 0，且只有"另一个非 0 序列号"才算过期）与 **WARNING 的应答**
    （模拟器按请求要的字节数回一整块零，接收缓冲从 4 KiB 提到 16 KiB，正文全零 = 无报警
    = `[]`）。10 册 §11.1 记了完整实测：两种请求方言它都收、应答包头 `CmdID=200`、
    保留字回显请求的码、官方客户端 6 帧（含 `0x777e` 标志与 `0x05xx` 码族）等。
  
  * **轴名读到了：在参数区（0x0404）**（2026-09-22）：轴名不在状态区，在**系统参数区**——
    同一个帧形状、另一个请求号（控制器侧 `OCK_CODE::CODE(type, id) = (type << 10) | id`）：
    `0x0401` 容量、`0x0402` 参数表（`TParamSpec`，21A 3784 条）、`0x0404` 读一个参数（**i32**，
    与状态区的 int16 不同宽），`0x0407` 就是我们一直在用的状态区。轴名在 `321 + 槽`
    （`*Nth axis axis name`），`21 + 槽` 是端口号（0 = 这一槽没接轴），`221 + 槽` 是轴型；
    解码抄客户端 `get_AllAxisName()`：`代号 / 100` 是 `"XYZABCUVW"` 的位置，`代号 % 100` 是
    后缀（0 与 >= 10000 = 这一槽没名字）；“哪些轴在用”抄 `get_EnableAxisMappingID()`
    （端口 > 0 且 0 < 代号 < 10000）。client 侧新增 `ncl_syntec_param()` /
    `ncl_syntec_axis_name()` / `ncl_syntec_axis_name_decode()` / `ncl_syntec_axes()`；
    适配器侧新增方法 `/AXES`（REST：`POST /api/syntec/AXES`）。**21A 实测**：`/AXES` 答
    4 条轴 `X(槽1,口1)/Y(槽2,口1)/Z(槽3,口3)/C(槽6,口6)`，`stateCount = 6` 与状态区 261
    的 6 项对得上；mock 用例证了解码与“一条轴都没有就回 NCL_ERR_UNAVAILABLE”。
  * **路径写死、轴号现查**（2026-09-22）：点位路径照旧是 iNC-BOX 的 `/AXIS@<名>/...`
    （声明里写死），但轴号在**取值的时候**才问控制器：点位的 `arg` 就是路径里那个字母
    （`'X'` / `'Z'`），`ncl_syntec_axis_index()` 把名字换成参数槽号，再按槽号从状态区
    取值（客户端的 `get_MachineCoordinate()` 就是 `r[i] = data[EnableAxisMappingID[i]]`，
    `data` 按槽排）。轴表缓存 5 秒（一次刷新问 32 个参数），刷新失败沿用上一张好表；
    名字对不上回 `NCL_ERR_NOT_FOUND` 并报出控制器说在用的轴名，不拿声明顺序猜轴号。
    mock 把状态区摆成 `{1234, 777, -500}`（下标 1 放陷阱值），Z 必须落在下标 2 才读到
    -500 ✓ —— 证明轴号是现查的，不是声明顺序。
  * **轴按九个字母配全 + 指令位置占位**（2026-09-22）：位置点位从五轴扩到
    **X/Y/Z/A/B/C/U/V/W 九轴 × 6 格 = 54 个**（加 9 项 = 63 个点位）：
    `SCREW/POSITION`（**实际位置**，区 101）、`SERVO_DRIVER/POSITION`（**指令位置**）、
    `MOTOR/POSITION`（机械坐标，与 SCREW 同源）与 `MOTOR/VARIABLE@ABSOLUTE|RELATIVE|DISTANCE`。
    实际/指令这一对的摆放照 `examples/device_model.c` 的设备模型（实际在丝杠侧、指令在驱动侧）。
    **取不到的照实报错**：机器没配的轴（控制器轴表里没这个名字）回 `NCL_ERR_NOT_FOUND`；
    指令位置控制器里还没有这一项（官方客户端只有四个坐标 getter），回 `NCL_ERR_UNAVAILABLE`
    并写明"待抓包"，不拿"实际 + 剩余距离"凑一个数（10 册 §11.3.4 记了这条与两条候选路）。
    实测（21A）：63 个点位里 **29 个读得到**（9 项 + X/Y/Z/C × 5 格）、**25 个报错**
    （A/B/U/V/W × 5 格，`NotFoundException`）、**9 个待抓包**（九轴的指令位置）；
    mock 侧另断言 SCREW 与 MOTOR 同值、SERVO_DRIVER 回 UNAVAILABLE、没配的字母每一格都报错。
  * **参数按设备模型做成配置对象 `/CONTROLLER/PARAMETER`**（2026-09-22）：照
  * **刀具表上线：一条 `/CONTROLLER/TOOL`**（2026-09-22）：按用户口径**只声明一个
    `TOOL`**（刀具列表，list），**刀补就是 tool 的元素**（不再单开 `TOOLPARAM`）。
    条数 `0x04C2`（`CODE(1,194)`，21A = **96**）、一把刀 `0x043F`（`CODE(1,63)`，`A = 4 + 224`、
    `B` = 刀号索引）；一条记录 **224 字节**（从 `JMarshal::get_SizeOfToolOffset()` 的 IL 读出来：
    `8 + 27×8`）：刀尖号 i32 + 留白 4 + 半径几何/磨损 + 长度几何[12] + 长度磨损[12]
    + **刀尖角（排在最后，也是 double）**。元素对齐册 4 的 `TOOLPARAM`：
    `id / kind（←刀尖号）/ radius（←RadiusGeometry）/ length（←LengthGeometry[0]）`
    + `tool_angle / radius_wear / length_geometry[12] / length_wear[12]`；`time_usage` 不给（没来源）。
    只读：写刀补 `0x0440` 的帧装不下 224 字节，待真机抓包。
    实测：21A `--model` 出 `{"id":"p65","name":"刀具","type":"TOOL","dataType":"LIST"}`、`0x04C2` 答 96、
    `0x043F` 取 A=228 答 224 字节；mock 用例验了 224 字节的字段取值与适配器级 Query。
    `examples/device_model.c` 的摆法（参数在 CONTROLLER 的 `configs` 里、`type` = `PARAMETER`；
    册 4 说这类归 configs、`dataType` = `HASH`，因为参数本身是字典），改成**配置点**：
    `NCL_CONFIG_OPS("/CONTROLLER/PARAMETER", ...)` 答标准的 Query 四个操作——
    `get_length`（条数，21A 3784）、`get_keys`（参数号清单）、`get_value`（`keys` 给号 →
    `{"321":100,...}`）、`get_attributes`（`keys` 给号 → 标题/默认值那条记录）。
    写（set_value/add/delete）不声明：控制器侧没验过怎么写参数，让宿主回
    "Unsupported Operation"，比给个假写入口诚实。没给 `keys` 时答空的 `{}` / `[]`
    （自检会对每个点位盲读一次，四千个参数没有"盲读"这一说）。原来那两个方法
    `/PARAMETER`、`/PARAMETER_TABLE` 撤掉——模型里只留一条 config，不再改来改去。
    实测：21A `--model` 出 `{"id":"p64","name":"参数","type":"PARAMETER","dataType":"HASH"}`；
    mock 走 `ncl_server_invoke_query`：`get_value {"keys":"321"}` → `{"321":100}`、`get_length`
    → 条数、号不在表里 → NG。值仍走 0x0404、表仍走 0x0401/0x0402（整表 1 MB 缓存 + 本地翻页）。
    （REST 那 12 条路由没有 Query，配置对象按标准走 MQTT 的 Query/Set。）
  * **参数写入开放（权限在外面控制）**（2026-09-22）：`CncParamPutValue` =
    `CODE(1,3) = 0x0403`，帧还是 36 字节那个形状，只是最后一位换了意思——
    **A = 12、B = 参数号、flag = 新值**，应答 4 字节 `hr` 全零就算成功（读法里 flag 固定是 1）。
    21A 实测：给参数 336（`*16th axis axis name`，两轴机床用不到的槽）写 908 → `hr=0`，
    再用 `0x0404` 读回 = 908 ✓；写回 907 → 读回 907 ✓；对照的几种候选形状（号放 A、
    值放 B、值跟在帧后面）都不对。**写会落盘**：动作后 `OpenCNC/Data/param.dat` 与备份
    逐字节比只差 10 个字节（文件头的时间戳/计数 + 该参数的两个字节），说明参数写不是内存态；
    验证完已把 `param.dat` 按 sha256 还原，模拟器数据回到原样。
    适配器**没有**声明 `set_value`：白名单（写哪些号）、审计先读旧值、危险操作二次确认
    适配器已在 `/CONTROLLER/PARAMETER` 上声明 `set_value`：写法两种——
    `{"keys":"321","value":111}` 或直接给字典 `{"321":111}`；一次最多 64 个号。
    **权限、白名单、二次确认均由外面控制**（用户口径），适配器里不做判断；
    写之前先读旧值是框架自己做的（要求声明 `set_value` 必须同时声明 `get_value`，我们满足）。
    client 侧新增 `ncl_syntec_param_put()`（`hr != 0` 回 `NCL_ERR_IO`，hr 写进 `last_error`）。
    mock 用例：写 321 = 111 → 读回 111、`hr = 0x1234` → `NCL_ERR_IO` 且报出 hr；
    适配器级走 `ncl_server_invoke_set`（10 册 §11.6）。
  * **`Pong` 轻量化**。`Pong` 原来背着整份 OpenAPI 文档（几 KB），心跳成了最重的
    一次发布。现在它**只有 `code`**：`{"@id":"..","code":"OK"}`，报文体固定两个字
    段；`ncl_message_set_open_api_schema()` 随之删除（`ncl_message_set_code()` 认得
    `Pong`，读用新增的 `ncl_message_code()`）。`Ping` 的应答不再生成文档，省掉
    每次心跳一次 JSON 构建。

  * **方法清单进模型**（`ncl_server.h` 的 `NCL_METHODS_NODE_ID`）。模型根部多一个
    保留配置项：路径 `/METHODS`、id `methods`、type `METHODS`、`dataType` `LIST`，
    值是每个可调用方法一条：

    ```json
    {"tool":"plc","method":"setValue","address":"/plc/setValue",
     "params":{...},"result":{...},
     "bindings":[{"operation":"set_value","path":"/MACHINE/STATUS"}]}
    ```

    于是 `probe` 一次就把整个能力面带回来（客户端不用再问、也不用猜），
    `params` / `result` / `bindings` 没有就不出现。注册只把它标记成待重建（工具层
    按点位注册，几十次注册不该做几十次重建）：读模型 / 应答 probe /
    `ncl_server_methods_json()` 时顺手重建，`ncl_server_set_model()` 装模型时立刻建
    一次。单独取这份数组用 `ncl_server_methods_json()`。它是运行期能力面，**不写进**
    `conf/model/nclink.json`（`ncl_server_save_model()` 落盘时摘掉，启动时再生成）。

  * **schema 补齐（REST 侧仍然返回）**。`ncl_tool_method` 多一个可省的
    `result_schema`（返回值的 JSON Schema），语言绑定的 `register_tool` 方法描述
    里对应 `"result"` 键；OpenAPI 文档不再只有自由对象：**requestBody 用声明过的
    入参 schema，200 用应答信封**（`code` / `return`（带返回 schema）/ `result`）。
    内置 `nclinkServer` 的 `addSample` / `removeSample` 也补了 schema。HTTP 客户端
    照着 `/api/schema` 就能拼出调用。

  * 顺带修掉一个隐患：`src/tool/tool.c` 里逐个字段填 `ncl_tool_method` 的数组没有
    清零，宿主没写的字段是栈上的垃圾（新增字段后立刻炸出段错误）。现在填之前先
    `memset`，这类字段以后再加也不会漏。

  * 绑定侧：垫片 `nclshim_server_register_tool()` 的方法描述多认一个 `"result"`
    键（`methods_json: [{"name":..,"schema":..,"result":..}]`，C#/Java/Python 都
    走它，Go 直接填结构体）；Python / Java 的离线 `dispatch("Ping/<sn>", ..)` 现在
    按 `Pong/<sn>` 解析应答（原来是拿请求主题硬套，Pong 会被当成 Ping 解，`code`
    就丢了），Python 侧加了用例。

  * **要重编的东西**：`ncl_tool_method` 加了一个字段、`Pong` 的报文体变了，所以
    语言绑定的垫片（`bindings/native`）与任何自己填 `ncl_tool_method` 的宿主代码
    要跟着重编；对端实现也要按新报文改（`Pong` 里不再有 `OpenApiSchema`）。

### 客户端取能力面的四个入口 + 各语言绑定同名方法

  * `ncl_client_methods_node()` / `ncl_client_methods()` —— 设备模型的 `METHODS` 项与
    它的数组；`ncl_client_find_method(client, "/plc/setValue")` —— 按地址取一个方法的
    元数据（前导斜杠可省），拿到就能拼调用（`address` + `params` schema）。
  * `ncl_node_find_by_type(node, "METHODS")` —— 按 `type` 深度优先找节点（含自身）：
    协议用类型点名的保留项靠它找，客户端不必知道路径。
  * 绑定：C++ `Client::methods()` / `find_method()`；Python `DeviceClient.methods()` /
    `find_method()` 与 `Server.methods()`；Java `DeviceClient.methods()` /
    `findMethod()`、`Server.methods()`；C# `Methods()` / `FindMethod()`（客户端与设备端）；
    Go `MethodsJSON()` / `FindMethodJSON()`（客户端与设备端，`ToolMethod` 多一个 `Result`
    字段）。垫片多三个导出：`nclshim_client_methods_json` /
    `nclshim_client_find_method_json` / `nclshim_server_methods_json`。

  * **根节点路径就是 `/`**（它本身就是分隔符，不是一段路径）。Python / Java 的自检里
    那条 `/NC_LINK_ROOT` 期望是错的，已按这个更正。

  * 顺手修掉两处自检夹具的老账：C# / Java 的离线 Query 用 `"/STATUS"` 查一条绑在
    `"/MACHINE/STATUS"` 的路径（现在按绑定路径查），C# / Java 的采样夹具 `paths` 写的是
    `"/STATUS"` 而期望是 `"/MACHINE/STATUS"`。**C# 自检 107 项全过、Java 自检 108 项全过**
    （Java 那 2 项"异常文本进 reason / lastCallbackError"的失败就是这条老路径：
    查不到绑定 → 答 NG 的 reason 是库的"未找到"，处理函数根本没被调用）。

  * 排障记录：期间见到过 Java 在注册工具时原生崩溃（ucrtbase 访问违例）。那不是 JNI 的
    bug —— 是**重编不一致**：`ncl_tool_method` 加了字段之后，旧 `nclink_jni.dll`
    （按旧布局填结构体）和新核心库混用，核心按新布局读到了错位的字段（日志里那句
    "plc 的 getValue 参数 schema 无效" 就是签名）。整棵重编后注册、回调、异常应答
    （`reason` = `java.lang.IllegalStateException: …`）都正常。

    这条已经**当场复现**（`sizeof(ncl_tool_method)` = 24 的数组喂给新核心：先打
    "…的 result schema 无效: schema is not valid JSON"，然后 0xC0000005 —— 和当时的
    崩溃日志一模一样），所以顺手加了道闸：

  * **ABI 形状闸门**。`NCL_SERVER_ABI_SHAPE`（`ncl_server_options` / `ncl_tool_method` /
    `ncl_tool_binding` 三个结构体的大小指纹）由头文件算出、核心库用
    `ncl_server_abi_shape()` 报出自己的那一份；垫片用 `nclshim_abi_shape()` 报自己
    编译时的值，并在 `nclshim_server_create()` 里先比一次：不一致直接
    `"垫片与核心库的结构体形状不一致（shim %u / core %u）：请把绑定垫片与核心库一起重编"`
    返回 NULL。这样"忘了重编"就是一条清楚的错误，而不是静默的内存破坏。

## 3.5.0

### 又扫出来三条：模态、执行中的程序段、合成进给速度（33 条读得到）

  * **模态**（`cnc_rdgcode`，`0x96`）：`d` = **第几组**（0..23，24 以上 rc=3），应答 12
    字节 —— 代码在 `@6`、**小数标志在 `@10`**（非 0 就是"值 ×10"）。24 组全扫、与官方
    SDK 的渲染逐条对上：`G17`/`G40`/`G54`/`G80`/`G98` 是整数，`G40.1`/`G13.1`/`G54.2`/
    `G80.5` 带一位小数 —— 全是真 G 码，这条规则钉死了。
    上一轮那句"机床只在显示时才填文本"是**误读**：那段 ASCII 是 SDK 自己渲染的，线上
    只有数字代码。
  * **执行中的程序段**（`cnc_rdexecprog`，`0x20`，`d` = 要多少字节）：体 = 4 字节 +
    ASCII 文本。这台机器回的是**从执行位置起的整段程序**（真机 515 字节），不是"当前
    那一段"——所以 `executed_block()` 老实交出去，而**刀具号先不接**（T 码在整段程序里
    出现多次，从里面挑一个等于编数）。
  * **合成进给速度**：`cnc_rddynamic2` 死活 rc=4（长度 4..192 都试过），改走每轴的
    `cnc_actf`（`0x24`）取最大 —— 官方 SDK 的 `cnc_rdaxisdata(cls=5)` 最后也是发 `0x24`。
    顺带发现这台机器 `0x24` 只回一根轴，读不到的那几根跳过（不算错）。

真机跑一遍：**33 条读得到**（52 个点位），3 条失败（RDCOUNT/RDNGROUP 机床不认、
宏变量 100 这台没有），16 条待抓包。`focas` 套件 294 项检查全过。

**顺带把"这台机器做不到的"列成表**（01 册 §2.8.6）：件数/刀具组数/刀具寿命 rc=6、
工件坐标 `cnc_rdwkcdshft` **type 0..20 全试**一律 rc=1、`cnc_rddynamic2` 长度 4..192
全 rc=4、轴扭矩 `cnc_loadtorq` rc=4、程序上行 `cnc_upload4` rc=10、`cnc_rdaxisdata`
的 `cls=2`（伺服负载/电流/温度）回的是桩数据 —— **官方 SDK 用同样参数也被拒**，
所以现场排障时先查这张表，别去查 client。

### 轴名/轴类型接上（34 条读得到），整表那几条钉成"机床不给"

  * **轴名**（`cnc_rdaxisname`，一个 `0x89`）：应答**每轴 4 字节** = 名字 2 字节 + 2 字节
    代码（真机 X/Y/Z = `58 00 94 06` / `59 00 …` / `5a 00 …`）。那个代码三根轴一样、
    分不出类型，所以 **`axis_type` 按 FANUC 命名约定推**（X/Y/Z/U/V/W 直线、A/B/C 回转）
    —— 注释与文档里都写明这是约定、不是读到的一格，现场命名不按套路时覆盖档自己改。
  * **整表那几条这台机器不给**（为它们给探针加了 `n_n`/`s4_n` 两种原型才试出来）：
    `cnc_rdparanum` 数量为 0、`cnc_rdparar` **把 SDK 直接带崩**、`cnc_rdmacror` rc=2、
    `cnc_rdtooldata` rc=1、`cnc_rdtoolrng` rc=3 —— 参数表/宏变量表/刀具表三条在这台机器
    上做不了，不是我们的帧问题。

真机 **34 条读得到**（52 个点位），15 条待抓包，3 条失败（机床不认 + 这台没有那个号）。
`focas` 套件 298 项检查全过，43 个套件全绿。

### 收尾扫一轮：`cnc_rddynamic2` 原来能给，只是 axis 不能给 0

  * **`cnc_rddynamic2` 是通的那条**（之前一直 rc=4）：**`axis` 必须 ≥ 1**，长度 ≥ 48
    （48..256 都 rc=0）。抄包看清它**不是一条命令、是九条捆在一起**：`0x1a`（报警）
    `0x1c`（程序号）`0x1d`（顺序号）`0x24`（actf）`0x25`（acts）`0x26 d=4/1/6/7` —— 全是
    **我们已经逐条读到的量**，所以这一份 client 不用为它单开接口；顺带确认 **ODBDY2
    里没有倍率字段**（那九条里没有 `0x5d`）。
  * **操作面板信号（`0x5d`）在这台机器上是桩**：32 字节里除 `@2 = 0xffff` 全是 0 ——
    所以本机 `feed_override = 0` 是**机床没填**，不是偏移读错。现场遇到"倍率一直 0"，
    先查这条（文档里写了）。
  * `cnc_rdaxisdata` 的类补全：`cls=4`（主轴）rc=0、`cls=0/6/7/8` rc=3；
    `cnc_rdexecprog3` 与 `cnc_rdspdata` **这套 SDK 根本没导出**；
    `cnc_rdtofsinfo`/`cnc_rdmacroinfo` 通（`{type=2,400}` / `{33,1}`，整表要是哪天有
    机器支持，范围就从这两条来）。

结论都写进 01 册 §2.8.7。这一轮的产出是"把最后这点不确定性关掉"，没有新接口 ——
剩下的 15 条要么是机床不给（§2.8.6 那张表），要么是 SDK 里压根没有那个调用。

### 程序目录（`cnc_rdprogdir3`）接上：真机一次读四个程序

请求形状是**一个 `0x06`，`d = 0`、`e = 8`、`arg2 = 1`**（官方 SDK 对这台机器发的是
这个；原来表里写 `d = 0x13` 是照假机床定的）。应答 72 字节一条：程序号在 @2（BE16）、
注释在 @8（NUL 结尾）。真机出门：

```json
[{"number":2001,"comment":"(DEMOMAINGEAR)"},{"number":3000,"comment":"(SUBGEAR)"},
 {"number":3001,"comment":"(SUBPOCKET)"},{"number":3002,"comment":"(SUBCENTER)"}]
```

真机跑一遍：**30 条读得到**（52 个点位），3 条失败（RDCOUNT/RDNGROUP 机床不认、
宏变量 100 这台没有），19 条待抓包。`focas` 套件 283 项检查全过。

顺带两笔：

  * **模态（`cnc_rdgcode`，`0x96`）先放着**：应答 12 字节、文本在 @4（SDK 解出过
    "G00"），但这台机器只在"显示模态"那一刻才填 —— 同一组参数 `d=0..7` 试了 8 遍
    都是全 0。等真显示时再对，或者走 `cnc_rddynamic2`（要把 OBDDY2 的长度给对）。
  * 假机床的载荷缓冲从 80 字节提到 512（真机有 264 字节的参数、72×N 的程序目录）——
    原来按 80 写"144 字节的目录"是越界读，症状是"第二条记录的内容不对"。

### 再啃三条：刀补 / 宏变量 / 参数（顺带把"机床说没有这个号"钉出来）

这三条是"一个块、`d` = 号"那一族，应答都是那条 8 字节记录（值@0 + 小数位@6）：

  * **刀补** `cnc_rdtofs`（`0x08`，`d` = 号、`e` = 1、`arg2` = 1000）→ 真机刀补 1 =
    0.000 mm，client 出门 `{"number":1,"value":0.0}`；
  * **宏变量** `cnc_rdmacro`（`0x15`，`d` = 号、`e` = 1）→ 变量 1 = 0；
  * **参数** `cnc_rdparam`：**码是 `0x8d`，不是 `0x0e`**（官方 SDK 对这台机器发的就是
    `0x8d`；`0x0e` 那条被机床拒 rc=1，原来表里写 `0x0e` 是照假机床定的）。应答 264
    字节、头 4 字节就是值 → 参数 1 = 1，client 出门 `{"number":1,"value":1.0}`。

**"没有这个号"是方向 3 的帧**（本轮最有价值的一条协议知识）：机床对不存在的号
（宏变量 100 / 刀补 2 / 参数 2）回 `dir = 3`、块数 0、带一个 −17 —— 既不是协议错、
也不是"读到 0"。client 原来判成 `NCL_FOCAS_ERR_HEADER`，现在：

  * 驱动新增 `NCL_FOCAS_ERR_NO_DATA`（方向 3 = 机床说"没有"）；
  * 语义层翻成 **`NCL_ERR_NOT_FOUND`**，于是上层能分清"这台没配"和"读不到"。

驱动那边还给 `call("payload")` 加了 `d`/`e`/`arg2`/`arg3` 覆盖 —— "同一个 item、
每次问不同的号"这一族（刀补/宏变量/参数/位置）都靠它，不用为每个号建一条表项。

真机跑一遍：**29 条读得到**（52 个点位里），3 条失败（`RDCOUNT`/`RDNGROUP` 机床不认，
宏变量 100 这台没有 → NotFound），20 条待抓包。`focas` 套件 276 项检查全过，43 个套件全绿。

### 真机再进一步：位置/负载/主轴/报警都通了（顺带把"每轴 12 字节"改成真机的 8 字节）

上一轮把会话与 `RDPOSITION` 的帧修好之后，这一轮拿同一台 0i-MD 把**值的形状**钉死，
并把伺服负载、主轴负载、报警消息接上：

  * **每轴（每主轴）一条 8 字节记录**：`data`(BE32)@0、预留@4（这台恒 `00 0a`）、
    `dec`(BE16)@6，值 = `data / 10^dec`。判据是三条对齐的真机证据（载荷 256 = 32×8、
    官方 `cnc_rdaxisdata` 报 X=116583/dec=3 而载荷那一格在 +6、主轴转速 2200 rpm），
    全部写进 01 册 §2.8.1。**原来按 12 字节 POSELM 切**（照假机床定的）会把 dec 读成
    10，等于把每个值都除成 0 —— 位置、进给速度、主轴转速三族一起中招，现在一起修好。
  * **新增四条语义**：伺服负载（`0x56` d=1）、主轴负载/转速（`0x40` d=4/d=5）、
    报警消息（`0x23`）。真机逐条试过：**一个块就够**，官方库捎的那些上下文块不是必须。
  * **报警**（现场报一次警之后一次问清的）：`cnc_rdalmmsg2` 的 Cb 有两个**暗格** ——
    `arg2 = 2` 才填消息文本、`arg3 = 64` 是要多少字节文本（一条记录长度 = 16 + arg3）。
    同一条报警三种请求的对照写在 01 册 §2.8.2：不给这两格，载荷只有 16 字节抬头；
    给 `arg2=0, arg3=64` 有记录但文本区是 0；照官方 SDK 给 `2/64` 才拿到文本。
    记录形状：报警号(BE32)@0、类型(BE32)@4、轴号@8、**文本长度@12**、文本@16（GB2312）。
    没报警时载荷 0 字节 → 回**空数组**（"没有报警"），有报警时这一份 client 报
    `{"number":75,"type":3,"text":"保护"}`、`alarm_status = 0x8`（SV）、三态 holding ✓。

### 新增字符集模块：GB2312 → UTF-8（中文报警真正落地）

机床的文本量是**机床自己的字符集**（FANUC 中文报警就是 GB2312），而 NC-Link 的 JSON
是 UTF-8 —— 不转就是乱码。这一版加了 `nclink/ncl_charset.h` + `src/core/charset.c`：

  * `ncl_gb2312_to_utf8(in, len, &out, &out_len)`：纯查表（**7445 个码位**，GB 双字节
    按升序二分查，13 次比较封顶），**无第三方依赖、无 locale 依赖**，Windows/Linux
    行为一致；ASCII（< 0x80）原样过去，不合法字节变 U+FFFD（半截汉字不会把整条报警
    带没）；两趟走（先算长度再写），不按"最坏 3 倍"浪费内存，也吃 `ncl_mem_alloc`
    那一层，**静态内存版（无堆）照样能用**。
  * 码表是**生成**的（Python 的 `gb2312` codec 逐个解 0xA1A1..0xF7FE，脚本写在
    `charset.c` 头上），不是手抄的 —— 7445 行手抄必错。
  * FOCAS 的报警文本已接上：真机上 `{"number":75,"type":3,"text":"保护"}`
    （机床给 4 个 GB2312 字节，出门 6 字节 UTF-8）。
  * 新套件 `tests/test_charset.c`（24 项检查：码表抽查、ASCII/混排/非法字节/空串/
    非 NUL 结尾片段/参数错）。ctest 从 42 个套件变成 **43 个**，全绿。
  * 驱动加了一个 `call("payload")`：把某一块载荷**原样**取回来（不管多长）——
    "机床可以回 0 字节"的调用就得靠它，普通读取要报长度、分不清"没有"和"读错"。

真机跑一遍：**27 条读得到**（位置六个点位全部有值：X 116.583、机械/相对同值、剩余 0；
主轴 2201 rpm；程序号 3001 / 主程序 2001；报警为空数组）。假机床跟着改成真机的
记录形状，`focas` 套件 260 项检查全过，42 个套件全绿。

顺带修了现场工具的毛病：`focas_live.c` 里原来把"调用"和"取值"写在一个实参表里
（`fmt(调用(...), 值, ...)`），**求值顺序未定义** —— MSVC 先算后面的实参，于是非零值
一律被打印成调用前的 0（位置明明读到 116.583，探针却显示 0，白查了一轮）。现在
先调用、再格式化。

### 接上一台真的 FOCAS2 服务端（以太网 0i-MD）：会话是**两条 TCP**，`code 24` 才是 ODBSYS

有新环境了：`192.168.110.192:8193` 上有一台能连的 FOCAS2 服务端（`cnc_sysinfo` 报
**0i-MD / series `D4G3` / 版本 `28.0` / 3 轴**）。官方 SDK（`Fwlib64.dll` +
`fwlibe64.dll`）`cnc_allclibhndl3` **rc=0**，于是拿它当口径、拿 `focas_tap.py`
抄包，把"这一份 client 接真机"这条路一次走通。

**先量出来的病**：这一份 client 对着这台机器**会话都建不起来**（第一帧就被拒），
而官方 SDK 好好的。抄包对出来两个错，都在握手：

  * **一条会话是两条 TCP**。控制通道先 hello（计数器 **1**，只发 hello），数据通道
    再 hello（计数器 **2**），命令**只走数据通道**；往控制通道上发 `func 0x21`
    机床直接 **RST**。原来把第二条连接读成了"没应答时 SDK 的重试"（那段抓包是在
    假机床上抓的，机床一条都不回），于是单连接、还把"记录表"当块表发。
  * **握手应答的体长判据写严了**。真机回 **360 字节**、`[8..10)` 写 **8**，
    `16 + 8n`（§2.2 判据 5）对不上 —— 可官方 SDK 自己收下并 rc=0，所以那条是从
    反汇编里误读出来的。现在只要求"至少 16 字节的块头"。
  * 顺带把"step 2 / step 3 探针"（按记录数发一串 `code 24`、再跟 `code 14`）换成
    真机上 SDK 的实际形状：**一个** `code 24` 的块（应答载荷就是 ODBSYS）。

**ODBSYS 从哪来**（`cnc_sysinfo` 那一格）：`code 24` 回 rc=0、载荷 18 字节
（`addinfo / max_axis / cnc_type / mt_type / series / version / axes`）；老写法那条
`code 0x0e` + `d=e=0x26f0` 被这台机床**拒了（rc=1）**。所以 `ODBSYS` 升成主路径，
`VERSION` 留作退路（只有机床明确拒了 24 才用）。`ncl_focas_system()`/`model()`/
`version()` 三条从此都读得到（真机上 `"0M"`/`"D4G3"`/`"28.0"`）。

**同轮对照过的其余几条**（这一份 client 读到的值与官方 SDK 逐条一致）：`RDPRG`
（@2/@6 = 2001）、`RDSEQ`、`EXEPRGNAME2`（`"//CNC_MEM/USER/PATH1/O2001"`）、
`STATINFO`（三块载荷 14/4/2 字节）、`RDNGROUP`（**rc=6，这台不认**）。

**新增的现场工具**：`tools/site-probe/focas_live.ps1`（拿**这一份 client** 去接一台
真服务端，逐条读语义接口、`-Raw` 连报文一起打）。假机床过了只说明自洽，真服务端过
得了才说明那套是照真机抄的。证据与判据（含"哪条通道能发什么""哪些试法会 RST"）
写在 01 册 §2.8。

假机床（`clients/tests/test_focas.c`）跟着改成"一条连接一个线程"，`focas` 套件
255 项检查全过，42 个套件全绿。

**改完对着真机跑一遍**（`tools/site-probe/focas_live.ps1`，用同一份 client）：
**18 条读到了** —— `status=free`、`mode=manual`、`emergency=false`、`alarm_status=0`、
`program_name="//CNC_MEM/USER/PATH1/O2001"`、`program_number=main=0`、`line_number="N0"`、
三个时钟、`model="0M D4G3"`、`version="28.0"`、`feed_override=0`、`spindle_speed=0`、
`axis_feedrate.X=0`。这是这一份 client 第一次接上一台真的 FOCAS 服务端。
**9 条失败**：其中 8 条是**机床拒了块**（块返回码非 0）——`RDPOSITION`（9 块那条，
6 个点位都挂在它上面）、`RDCOUNT`（`0x8b`）、`RDNGROUP`（`0x4a`）。后两条 SDK 也是
rc=6 —— **机床自己不认**，不是我们的帧错；`RDPOSITION` 是**我们多发了一个块**：
逐块试下来，去掉那条 `0x0e` + `d=e=0x26f0`（"能力块"，照假机床定的）之后，
**8 个块全部 rc=0**，四种位置就在应答下标 1..4。改完再跑：**24 条读得到**，
**六个位置点位（绝对/机械/相对/剩余/指令/跟踪误差）全部读通**（机床静止 → 全 0）。
剩下 3 条：`RDCOUNT`/`RDNGROUP`（机床不提供）+ `RDMACROR` 的参数检查（一次最多 5 个）；
另外 25 条是点位表里本来就写着"帧待抓包"的。

> 顺带看清一件事：**官方库是按机型选帧的**。对这台 0i-MD 它发 7 块帧
> （`0xa4`/`0x89`/`0x88`×2/`0xa3`/`0x26`/`0xa4`），对假机床（hello 是填充字节）
> 它发 9 块帧 —— 所以"照 SDK 抄帧"必须先看清是**对哪台机器**发的（01 册 §2.8）。

> 测这台机器时它在 Wi-Fi 上走 DHCP，地址从 `.192` 漂到 `.195`（IP 一变、会话就断），
> 探针按当时地址打。宿主机的 VMware NAT 上那条 `8193 = 192.168.79.128:8193`
> 端口转发当时不通（`127.0.0.1:8193` 无人应答）。

### NCGuide 那条路：把"正确环境"的配方抄全了，也把"它为什么起不来"钉死了

用户问得对：手上就有 FANUC 的模拟器，本该"客户端发一帧、服务器回一帧"。这轮就照这个做，
把两件事都办了 —— 一件是**配方**，一件是**本机为什么起不来**（后者卡在模拟器自己的画屏
上，最后一步得在 GUI 侧动手）。

**配方**（出自 NCGuide 自带的 `NCGuide FOCAS2 Function.pdf`，官方 SDK 包
`Document/NCG/`，21 页）：

  * §3.1 只有**一个**选项要求：**`Extended driver and library function`** —— 就是
    `Message.xml` 里的 **`OPTPRM_401`**（`cncgoptif.dll` 里也带着这张 ID 表）。用
    `OptionSetting.exe`，**要在 NCGuide 起来之后**开、勾上、再**重启 NCGuide**。
  * §3.1 **以太网不用再开别的选项**（"equal to embedded Ethernet function"）；屏幕上
    那个 "Ethernet function" 勾是机床侧的，**别开**（FS0i-F 就是勾了它之后起不来的）。
  * §4.2 **HSSB 不用装驱动**；§4.3 以太网要跑 NCGuide 那台的网卡/IP。
  * §4.4 取句柄：HSSB = `cnc_setdefnode(9)` + `cnc_allclibhndl()`（节点 **9**）；
    以太网 = `cnc_allclibhndl3(IP,…)`，IP 用跑 NCGuide 那台的（手册 NOTE：屏幕上那个
    以太网设置对 FOCAS2/Ethernet **无效**）。§5 还有一张每函数的 HSSB/Ether 可用性矩阵。
  * 这条也把上一轮那道闸门解释圆了：`cnc_rdaxisdata` 一族回 `EW_FUNC`，正是"没开
    `OPTPRM_401`"；基本函数（位置/状态/程序号…）不开也能读（HSSB 早就实测过）。

**本机为什么起不来**：所有机型（FS0i-F、FS0i-F Plus、FS31i-B…）都是起来十几秒后自己死，
SIM.LOG 只有 `CNCSIMULATOR STARTED`、没有 `FINISHED`。本机没装 cdb/windbg，也没管理员
权限开 WER 的 LocalDumps —— 所以新写了 `tools/site-probe/win_minidbg.py`：自己当调试器
（`CreateProcess` + `DEBUG_ONLY_THIS_PROCESS`，只报**二次异常** = WER 记的那一枪，另外把
栈上落在已知模块里的返回地址列成近似调用链）。抓到的现场：

  * FS31i-B / FS0i-F Plus：致命异常 `ACCESS_VIOLATION`、**写**到**页对齐**地址
    （`0x21050000`、`0xef670004`），指令是 `MSVCR80!memset` 的 `movdqa [edi], xmm0`；
    调用链 `USER32 → System.Windows.Forms.ni.dll → mscorwks(.NET 2.0) → msvcr80`
    —— **画 CNC 屏幕那一步 memset 写过了区域边界**。
  * FS0i-F 还有第二种：`ns.dll+0x74e2c1`（紧跟"往 `0x125e2402/0x125e2404` 写
    `0x50/0x1e`"之后按 0..3 分支 —— 像显示尺寸处理）。
  * **时好时坏**：连开 3 次大约活 1 次；活下来的一路涨到 160→409 MB 之后死（正好在
    "开始画屏"那一步）。删掉 `SimBaseSetting.xml` 死得更早（那文件必需）；给
    `Simbase.exe` 挂用户级 DPI 兼容标记 `HIGHDPIAWARE` 也没救（已撤回）。

结论：**卡的是模拟器自己的显示这一路，不是机床数据、也不是 FOCAS2/协议**。所以"把环境
立起来"的最后一步在 GUI 侧：① 让 NCGuide 起稳（换显示/缩放，或看它弹的框）；②
`OptionSetting.exe` 勾 `OPTPRM_401`；③ 要以太网就用 NCGuide 的以太网设置把服务开到
8193（之前 FS0i-F 确实听过 8193）。做完这三步，我们的 client 直接 `--offline` 连
`127.0.0.1:8193` 就是第一台"真"机床。

顺手把资料归了位：`Document/NCG/*.pdf` 抽成文本、`Fwlib/30i/Fwlib64.h` 与
`Document/SpecE/**`（1691 份 XML，每份带 `<prottype>`）作为核 item 的依据。
本机清理：FS0i-F 的机床数据已还原成我动手之前的样子（12:36 那份仍在
`%TEMP%\ncg-bak-123643`，我挪走的那份在 `%TEMP%\ncg-cur-f0if`）；没留后台进程。

### 型号/版本接上（`ODBSYS`），并把"多块那几条"卡在哪钉出来

**型号与版本**（标准表 6 的 `MODEL` / `VERSION`）原来挂在"待抓包"，来源写的是
`cnc_rdmodel`/`cnc_sysinfo`。这轮查清楚了：FOCAS **没有**单独的"型号"调用（`cnc_rdmodel`
在官方文档包里根本没有），型号信息就在 `ODBSYS` 里 —— 也就是**连接期那条能力块**
（Cb `0x0e`，`d = e = 0x26f0`），跟 `cnc_sysinfo` 是同一个结构，官方文档
`SpecE/Misc/cnc_sysinfo.xml` 把每一格都写明了：

```
[0..2)  addinfo   (BE16：bit0 上料器 / bit1 i 系列 / bit8..15 MODEL A..F)
[2..4)  max_axis  (BE16，最大控制轴数)
[4..6)  cnc_type  (ASCII，如 " 0" = Series 0i)
[6..8)  mt_type   (ASCII，如 " M" = 加工中心 / " T" = 车床)
[8..12) series    (ASCII)       [12..16) version (ASCII)      [16..18) axes (ASCII)
```

client 跟着实现两处（口径写在 `focas_values.c` 里）：`MODEL` = `cnc_type` +
`mt_type` 去空格再拼 `series`（例 `"0M D4G2"`）、`VERSION` = `version`（例 `"49.0"`）；
这两格都是**空格补齐**的 ASCII，两头都要去空格。想换拼法的站点直接读 item `VERSION`
的 `@4`/`@6`/`@8`/`@12`（名字叫 `VERSION` 是因为它本来就是那条 `0x0e` 能力块）。

**"一条请求带多个块"那几条**（`cnc_rdsvmeter` / `cnc_rdspmeter` / `cnc_rdaxisdata`）这轮
也往前推了一格，并且**把卡点钉死了**：

1. 假机床 `focas_sdk_mock.py` 加了"按块看请求"的能力 —— `--axis-table N` 让 Cb `0x89`
   那一块回**像样的轴表**、`0x0e/0x26f0` 那一块回 **ODBSYS**。于是 `cnc_rdsvmeter`
   （`0x56` + `0x89`）从 `rc = -17 EW_PROTOCOL` 变成 **`rc = 0`** —— 之前"伺服负载
   核不出来"就是 `0x89` 那块回了填充字节。
2. 再往下撞到官方库自带的闸门：**`cnc_rdaxisdata` 对假机床一律 `rc = 1 (EW_FUNC)`，
   一个字节都不发**（连接正常、能力块也答了）。反汇编 `fwlib30i64.dll` 看出
   `cnc_rdsvmeter`/`cnc_rdspmeter` 都是薄壳，内部 **`call cnc_rdaxisdata`**
   （RVA `0x18d60`）—— 这一族（伺服/主轴负载、电流、速度）在官方库实现里是同一条。
   所以"拿 SDK 当裁判"这条路要先把扩展功能的闸门喂对（和单轴那条 `EW_ATTRIB` 同一族）。
3. 写 client 需要的东西官方文档已经给全了：
   `cnc_rdaxisdata(h, cls, short *type, short num, short *len, ODBAXDT*)`，
   `cls` = 1 位置 / 2 伺服 / 3 主轴 / 4 选中的主轴 / 5 速度，
   `ODBAXDT = {char name[4]; long data; short dec; short unit; short flag; short reserve;}`
   （16 字节）。下一轮照这个写，SDK 那边等闸门。

新增工具能力：`focas_sdk_layout.py --mock ...`（把剩下的参数原样交给假机床，多块调用
要它）。

验证：`ncl_test_focas` **245 checks / 0 failures**（新增"型号与版本 = ODBSYS 的 ASCII 格"
一条，铺的就是 NCGuide 实测那串字节）；全量 `ctest` **42/42**；假机床 +
`ncl_server --offline --once`：`/MACHINE/MODEL = "0M D4G2"`、`/MACHINE/VERSION = "49.0"`、
`MANUFACTURER = "FANUC"`、`PART_COUNT = 952`、`FEED_OVERRIDE = 130.0`，
自检 **29 可读 / 14 待抓包、0 个读取失败**。

顺带把"控制轴数那条闸门"的影响面钉清楚了（写进 01 册 §2.7）：官方库连接期把"几根控制
轴"记进上下文，凡**按轴数决定长短**的调用都吃这份缓存，而它对我们假机床记的是 **0**——
于是：指定轴号 → 本地 `EW_ATTRIB` 不发帧；`ALL_AXES` → 帧发得出去但 `data[]` 一条都没有
（`cnc_rdwkcdshft` 的出参只剩 `type = 0xffff`）；连请求里的"长度"都被算成 0；整族入口
`cnc_rdaxisdata` 干脆本地 `EW_FUNC`。**所以多块那几条核不出来是这道闸门，不是"应答怎么
切"**；反过来把那一格喂对，`TORQUE`/`CURRENT`/`TEMPERATURE`/主轴负载/`FEED_SPEED`
就一起开（写 client 要的 `cnc_rdaxisdata` + `ODBAXDT` 官方文档已给全）。另外两处口径
更正：`cnc_rdexecprog3`（子程序号那条）**在官方文档包里根本没有这个函数**，
`cnc_rdmodel` 也没有；`COORDINATE` 那条读的是**当前选中**的那个坐标系
（`cnc_rdwkcdshft`，Cb 0x63），不是整张 G54… 表。

### 进给倍率接上 `cnc_rdopnlsgnl`（顺手更正两条 not_yet 的注记）

标准表 7 的 `FEED_OVERRIDE` / `SPINDLE_OVERRIDE` 一直挂在"待抓包"，client 里的注记
写的是 `cnc_rddynamic2` 的 `ODBDY2.feed_override` / `.spindle_override` —— 这两个
字段在官方头里**根本不存在**（`ODBDY2` 只有 dummy/axis/alarm/prgnum/prgmnum/seqnum/
actf/acts 加位置联合体）。倍率在**操作面板信号** `IODBSGNL` 里，走 `cnc_rdopnlsgnl`。

用上一节那套反查工具把它核了出来（`cnc_rdopnlsgnl` 加进了 `focas_sdk_probe.c` 的
调用表）：**Cb 码 0x5d**，`d` 是"读哪几路"的位掩码（doc: bit 5 = 进给倍率、bit 3 =
快移倍率、bit 6 = 主轴倍率但**只有 15i**），应答载荷就是 **@0 起的一串 BE16**：

```
@0x00 mode      @0x02 hndl_ax   @0x04 hndl_mv   @0x06 rpd_ovrd
@0x08 jog_ovrd  @0x0a feed_ovrd @0x0c spdl_ovrd @0x0e blck_del …
```

（反查结果：出参 `IODBSGNL` 的 `mode` 起逐格对上载荷 @0/@2/@4…，每格一个 BE16。）
值是**信号码**，官方文档把换算写死了：`feed_ovrd` 的 0..20 就是 0%..200%，每级 10%
（`jog_ovrd` 那张表另有 24 级，快移倍率是 100/50/25/F0）。所以 client 里
`ncl_focas_feed_override()` = 读 `RDSGNL@10` 的 BE16 × 10，出门就是标准要的百分比；
码超出 0..20 回 `NCL_ERR_RANGE`。

**主轴倍率**这一条改成"换路子"：`IODBSGNL.spdl_ovrd` 在 16/18/21、16i/18i/21i、0i、
30i、PMi-A 上是 **(Not used)**（文档明说只有 Series 15i 有），所以它不是"还没抓包"，
而是这一格读不到 —— 要拿主轴倍率得走 `cnc_rdspdata` 或相关参数，两者都还没核。

顺带把 `ncl_focas_feed_speed` 的注记改对：`cnc_rddynamic2` 的字段叫 `actf`（不叫
`feedrate`），而且它的 `length` 必须给 `sizeof(ODBDY2)`（随轴数变）；单轴进给速度
已经能走 `cnc_actf`。

新增 item `RDSGNL`（`d = 0xffff`，全都要 —— 位掩码只决定机床回哪几路，回来的仍是整个
结构体，偏移才站得住）；假机床 `focas_machine.py` 加 `--feed-override`（百分比，按
文档每级 10% 折算成码）；golden 用例新增"进给倍率 @0xa 码 × 10 = %"（并把 @0xe 的
`blck_del` 填成干扰值，确认没被当倍率读走）。

验证：`ncl_test_focas` **241 checks / 0 failures**；全量 `ctest` **42/42**；假机床 +
`ncl_server --offline --once` 端到端：`/MACHINE/FEED_OVERRIDE = 130.0`（假机床给
`--feed-override 130`）、`/MACHINE/PART_COUNT = 952`，自检从"26 可读 / 17 待抓包"
变成 **27 可读 / 16 待抓包、0 个读取失败**。

### 反查工具：把"应答载荷第几字节是哪一格"变成机器算出来的（顺手抓到一件数读错位置）

核 FOCAS item 一直是"铺斜坡载荷 + 人眼看结构体"，对 `ODBST` 那种十来个 short 的结构还
行，对"值藏在 @12 还是 @20"就很容易看岔 —— 本轮就抓到一处：**`cnc_rdcount`（标准
`PART_COUNT`）的值不在载荷 0 处，在 @20**，client 一直按 @0 读（假机床也一直按 @0 铺，
所以两边"自洽"地错着）。两处独立证据把它钉死：

1. **官方 SDK 反查**：新工具 `tools/site-probe/focas_sdk_layout.py` 给假机床铺一份"每个
   字都不一样"的载荷（第 i 个字 = `0x1000 + i*0x101`），跑一次官方 SDK 的调用，把 SDK
   填进出参的字节抠出来，再对出参每一格**反查**它来自载荷哪一格（大端/小端 × 16/32 位
   各试一遍，唯一命中才报）。`cnc_rdcount` → `datano`@2、件数 **@20**；同族的
   `cnc_rdlife` → 寿命 **@12**（两条不是一个偏移）。
2. **现场包的 Linux 实现**：`libfwlib32.so.1`（x86 版在官方 SDK 包的 `Fwlib/Linux/x86/`
   下）里 `cnc_rdcount` 取块 +0x10 的 4 字节 `bswap` 后低 16 位当 `datano`（= 载荷 @2 的
   BE16）、取块 +0x24 的 4 字节 `bswap` 当 `data`（块 +0x10 是载荷起点 → 载荷 @20）。
   两边完全一致。

client 改一处：`ncl_focas_part_count()` 读 `"RDCOUNT@20"`（原来 `"RDCOUNT"`）；
假机床 `focas_machine.py` 的 `0x8b` 那块改成按 `ODBTLIFE3` 的真实位置铺（`--count` 落
@20、新增 `--life` 落 @12）；golden 用例新增"件数在 @20"，并故意把 @0 填成 `0xDEADBEEF`
当干扰值 —— 这条以后不会被猜回 @0。

**同一套反查顺手核出/更正一批码与布局**（都并进 01 册 §2.4，新方法写在 §2.6）：

| 事实 | 说明 |
|---|---|
| `cnc_rdtofsinfo` = **Cb 0x0a**（不是 0x0e），`use_no`@2、`ofs_type`@4 | 新核出来 |
| `cnc_rdmacroinfo` = **0x17**；`cnc_rdexecprog` = **0x20**（文本从载荷 @4 起，原样字节） | 新核出来 |
| `cnc_rdgcode` = **0x96**、`cnc_rdwkcdshft` = **0x63**、`cnc_loadtorq` = **0xfd** | 新核出来（后两条还差"长度给多少"） |
| `cnc_rdparam` = `datano`@2、`type`@4、`ldata`@8；`cnc_rdtofs` = `data`@0 | 字段位置核出来 |
| `cnc_rdblkcount` 就是**载荷 @0 的 BE32**（上一版写"不是 @0"，反了） | 更正 |
| `cnc_rdprgnum`@2/@6、`cnc_rdseqnum`@0、`cnc_alarm2`@0、`cnc_rdngrp`@0、`cnc_rdtimer`@0+@4 | 复核：client 原来的读法都对 |

新工具三支（都在 `tools/site-probe/`）：`focas_sdk_layout.py`（反查）、
`fwlib_struct.py`（从 `Fwlib64.h` 抠结构体）、`fwlib_proto.py`（从官方文档的
`<prottype>` 抠原型 —— 原型不对的话，探针出参落在哪一格全是噪声）。`elf_dis.py` 补了
x86 / x64（之前只认 ARM），用来开那份 Linux 库；`focas_sdk_probe.c` 修了
`s1_n_s1_n`（`cnc_rdaxisdata` 第 2 个 short 是**指针**，原来拿整数当指针传）、给
`s2_n` 的 `*num` 一个非零初值（给 0 会被本地回 `EW_LENGTH`），并补
`cnc_rddynamic2` / `cnc_loadtorq` / `cnc_rdgcode` 三个表项。

还没啃下来的（都不用等真机，接着用这套工具磨）：`cnc_rdalmmsg2` 的**中间三个字段**、
`cnc_rdprogdir3` 与 `cnc_rdmacro` 的 `--len`
（给 12 仍回 `EW_LENGTH`）、`cnc_rdsvmeter`/`cnc_rdspmeter`/`cnc_rdposition` 那几条
"一条请求带多个块"的逐块形状；以及官方 SDK 那几条**单轴**调用的本地闸门（`EW_ATTRIB`）
——那只是"拿 SDK 当裁判"这条路，不影响 client。

验证：`ncl_test_focas` **237 checks / 0 failures**（新增件数在 @20 一条）；全量 `ctest`
**42/42**；编译零 warning；假机床 + `ncl_server --offline --once` 端到端：
`/MACHINE/PART_COUNT = 952`（值由假机床铺在 @20）、`POSITION@REAL` 12.345 / 67.89 / −3.5、
`POSITION@CMD` 11.111 / 66.656 / −4.734、`STATUS` running、`WORK_MODE` auto，
自检 **43 个点位（26 可读 / 17 待抓包）、0 个读取失败**。

同一轮的收尾：`cnc_rdalmmsg2` 往前推了一格 —— 假机床 `focas_sdk_mock.py` 加了
`--almmsg2`（铺一串像样的报警记录），核出**每条记录 80 字节**（不是
`sizeof(ODBALMMSG2)` 的 76）、`alm_no` 在记录 +0（BE32）、文本 `alm_msg[64]` 在 +0x10：
Linux `libfwlib32.so` 里那一段是"条数 = 载荷长度 / 80"，并把记录 +0 与 +0x10 拷进出参，
官方 SDK 的出参 `[0..4)`/`[12..)` 也正好是这两格。**中间三个字段（type/axis/msg_len）
还没钉死**（Linux 库读 +4/+8/+0xc，官方 SDK 的出参没跟这三格对齐），所以 client 的
`WARNING` 这一档先不开 —— 差的就是这三格的位置。下一轮拿 `ALMMSG2_MARK=1` 再核。

### 伺服延迟量的形状定死：反汇编以太网库 `fwlibe64.dll`（把上一节"还差一格"关掉）

上一节留了一句"延迟量的小数位是照 `POSELM` 一族猜的、真机移动轴再核"。这轮不猜了，
直接读**我们 client 真正对的那条协议**的官方实现：x64 以太网库 `fwlibe64.dll` 里
`cnc_srvdelay` 是薄壳（`kind = 9`，与 32 位 HSSB 库 `fwlibNCG.dll` 那张表一致），
共享函数 `sub_180059180` 里四行说明一切：

```
180059321  shr ax, 3                     ; 轴数 = 载荷长度 / 8 → 每轴 8 字节
180059343  lea rcx, [rax*4 + 4]          ; 长度规则 = 4 + 4×轴数（不够回 EW_LENGTH）
18005938c  mov ecx, [载荷 + i*8 + 0x10]  ; 取记录第 0 个 dword
180059391  call bswap32                  ; 线上是大端
180059396  mov [out + i*4 + 4], eax      ; → ODBAXIS.data[i]，type 在 +2 = 轴号
```

于是：**每轴 8 字节、值在记录第 0 个 dword（大端）、长度规则 `4+4×轴数`** —— 与 §2.5
的 NCGuide 实测一致；**小数位不在这条载荷里**（官方库一个字节都不多看），位数走
`cnc_getfigure`，也就是该轴的显示小数位 = 同一条 `POSELM` 里的 `dec`。

client 跟着改一处：`srv_delay_raw()` 不再把记录 `[4..6)` 当 dec（那是猜的），改成
借同一条 POSELM 的 `dec` 缩放（`cnc_getfigure` 口径），`[4..8)` 当保留位不解释。
`ncl_focas_axis_srv_delay()` 自己读一趟位置拿 dec；`ncl_focas_axis_position_cmd()`
复用同一趟的 dec，所以还是两次请求。golden 用例把 `[4..6)` 填成 `0x9999`（垃圾值），
断言结果不受影响 —— 这条以后不会再被猜回去。

新增工具：`tools/site-probe/elf_dis.py`（按符号反汇编 ELF，ARM/Thumb 自动）——
用来开交付包里那份 ARM 的 `libfwlib32.so.1`；PE 那边继续用 `focas_dis_range.py`。
文档：01 册新增 §2.5.2、§2.4 的 `cnc_srvdelay` 行、`tools/site-probe/README.md`。

验证：`ncl_test_focas` **235 checks / 0 failures**；全量 `ctest` **42/42**；编译零 warning；
桥 + client 端到端复跑：`POSITION@REAL` 12.345 / 67.89 / −3.5，`POSITION@CMD`
11.111 / 66.656 / −4.734（各减注入的 1.234）。

### 修正：`STATINFO` 的 ODBST 偏移（`mode` 一直是错的）+ 真 FOCAS2 假机床

用"**斜坡载荷 + 官方 SDK 填它自己的 `ODBST`**"把 `cnc_statinfo` 的切法钉死了：
结构体偏移 0 收**块 1**（码 225 → `dummy`）、偏移 2 收**块 2**（码 152 → **`aut`**）、
偏移 4 起收**块 0 的载荷**（码 25 → `manual, run, edit, motion, mstb, emergency,
alarm, spindle, oper`）。所以块 0 载荷的下标：`0 = manual`、`1 = run`、
`5 = emergency` —— **`aut` 不在块 0**，它在块 2。

client 原来按"整个 ODBST 都在块 0"读，于是有三处错：

- `ncl_focas_mode()` 读下标 2/3（那实际是 `edit`/`motion`）→ **mode 永远是错的**；
  现在改成 `aut` 读**块 2**、`manual` 读块 0 下标 0。
- `ncl_focas_status()` 把下标 5 当 "holding"，而下标 5 就是 **`emergency`** ——
  急停会被报成 holding；现在按标准口径（表 6/表 8：RUN + EMERGENCY 推三态）：
  急停优先 → `holding`，否则 run → `running`，否则 `free`。
- `ncl_focas_emergency()` 跟着改成读下标 5（原来是按"字节偏移 10"算的）。
- 三处读的长度也收到够用为止（6 / 1 个 int16），不再要求机床把块 0 铺满 20 字节。

`ncl_focas_mode()` 的 manual 那条还踩了一个坑：读 **1 个** int16 时驱动给的是**标量**
（不是数组），原来按 `arr_get(json, 0)` 取，永远取到 0 → `manual` 报成 `other`。
这个是新加的那条 manual golden 用例抓出来的（`ncl_json_as_int(json, &manual)` 才对，
和 `per_unit_float` 一个约定）。

golden 用例同步改成实测布局（含 `holding` 那条）→ `ncl_test_focas` **231 checks /
0 failures**（加 manual 那条后 235 checks），全量 `ctest` **42/42**，编译零 warning。

顺带入库 `tools/site-probe/focas_machine.py`：**真 FOCAS2 假机床**（命令行给
XYZ / 进给 / 主轴 / 件数 / 程序号 / 报警 / 跟踪误差，字节按"证据表"铺，没证据的 item
回错块、不编字节）。官方 SDK 实测 `cnc_statinfo` / `cnc_actf` / `cnc_acts` /
`cnc_rdcount` / `cnc_rdprgnum` / `cnc_rdseqnum` / `cnc_rdngrp` / `cnc_alarm2` /
`cnc_exeprgname2` 全部 **rc=0 且数值对得上**。还差一步：`ODBAXIS` 那一族
（`cnc_srvdelay` / `cnc_absolute`）的 `data[]` 填不出来 —— 驱动眼里的"轴数"来自握手，
得先把 `0x18`（记录详情）的载荷试出来（`--srv-shape rec8/bare4/hdr4` 已经预置在桥里）。

这座桥现在还能**从仿真器拉点值**（`--protoforge <REST 基址>`）：把 ProtoForge
`fanuc` 设备的 `x_abs/y_abs/z_abs`、`feed_rate`、`spindle_speed`、`run_status`、
`tool_number` 映射成这台假机床的状态，件数/程序号/跟踪误差这些没有对应点的仍走命令行。
实测（同形状桩）：`POSITION@REAL` = 77.25 / −11.5 / 3.0、`STATUS="holding"`、
`POSITION@CMD` = 77.0 / −11.75 / 2.75（各减 0.25）。

### 跟踪误差与指令位置：`POSITION@CMD` = 实际位置 − 伺服延迟量

现场口径是 **跟踪误差 = 实际位置 − 指令位置**，所以指令位置拿得到：实际位置走
`cnc_rdposition`，跟踪误差走 `cnc_srvdelay`，两条相减。这一轮把后者接上：

- **item `SV_DELAY`**（`clients/focas/focas_codec.c`）：一条 Cb —— `0x26`、d = 9、
  e = `ALL_AXES`，这是官方 SDK 自己发的请求帧（假机床实测，01 册 §2.4）。`0x26` 的
  d = 0..3 是四种位置，d = 9 才是延迟量，所以没复用 `RDPOSITION`。
- **`ncl_focas_axis_srv_delay()`**（新）+ **`ncl_focas_axis_position_cmd()`**（原来是
  `NCL_ERR_UNAVAILABLE`）：应答每轴一条 **8 字节记录**，值取记录第 0 个 int32（大端）、
  `[4..6)` 当小数位（值 = `data / 10^dec`）。依据是官方库 `fwlibNCG.dll` 里那一层：
  `axis` 越界回 `EW_ATTRIB`、`length < 4 + 4×轴数` 回 `EW_LENGTH`，取值按 **8 字节
  步长**、每轴取记录第 0 个 dword 写进 `ODBAXIS.data[i]`（01 册 §2.5.1，工具
  `tools/site-probe/focas_dis_range.py`）。
- **测试**（`clients/tests/test_focas.c`）：假机床上铺"实际 12.345 / 延迟 1.234"和
  "实际 67.89 / 延迟 **−2.500**"两组，断言 `指令 = 实际 − 延迟`（含负延迟、含静止
  时 `指令 = 实际`）→ `ncl_test_focas` **225 checks / 0 failures**；全量 `ctest`
  **42/42**，编译零 warning。

> 还差一格：延迟量的小数位是照 `POSELM`/`LOADELM` 同一族的排布取的（记录 `[4..6)`）。
> 真机让轴**动起来**再看一眼就能钉死两件事——延迟量非 0 时的正负号、以及这 2 字节到底
> 是不是 dec；NCGuide 上的机床是静止的，这一条实测只能给出 0（§2.5）。

### 取证：用 NCGuide（FS0i-F 模拟器）抓真应答，五组 🟡 转 🟢

FANUC 自己的模拟器 **NCGuide 自带 FOCAS2 服务** —— 那些"码已核、应答待核"的调用不用
等真机了。2026-09 在本机装好的 `C:\Program Files (x86)\FANUC\NCGuide FS0i-F` 上把
路走通（配方写进 01 册 §2.5 与 `tools/site-probe/README.md`）：

- **走 HSSB 不走以太网**：节点号 9（手册 §4.4）+ NCGuide 自带的 32 位
  `Fwlib32.dll`/`fwlibNCG.dll`，`cnc_setdefnode(9)` → `cnc_allclibhndl()` = rc 0、
  handle 18433。以太网那条（Simbase 在听 8193）用官方 SDK 一律 **-17 EW_PROTOCOL**
  —— 它的以太网握手是 NCGuide 自己的一套，别在这条上耗。
- **探针两个真 bug**：32 位下 SDK 导出是 WINAPI（__stdcall），函数指针不标调用约定会
  把栈弹坏（症状 `0xC0000409`）；再加一个 `--hssb` / `--node` 入口。修在
  `tools/site-probe/focas_sdk_probe.c`（x64 那条老路不受影响）。
- **实测到的形状**（默认机床 `1path-3axis-M`）：`cnc_rdposition` = 每轴一个 **POSELM**
  （`int32 data` + `dec=3` + `unit=0` + `disp=1` + `name='X'`，位置 = `data/10^dec`）；
  `cnc_rdsvmeter` = 每轴一个 **LOADELM**（`int32 data` + `dec/unit` + 轴名）；
  `cnc_rdspmeter` = 每主轴一个 LOADELM（`name='S'` + `suff1='1'` → "S1"，负载/转速各一）；
  `cnc_rdalmmsg2` = **ODBALMMSG2** 数组（这台没报警，形状与手册一致）；
  `cnc_rdblkcount` = 就是个 **int32**（原来"不在载荷 0 处"的判断作废）；
  `cnc_statinfo` 与 §2.3 的切法一致。`cnc_absolute` 对长度很挑（12/16/36 都回
  `EW_LENGTH`），**位置直接走 `cnc_rdposition`** 更省；`cnc_rdtofs`/`rdmacro`/`rdparam`
  的 `--len` 要按各自结构体长度给（下一轮）；`cnc_upstart4` 探针没返回（要真程序）。

结论：**`POSITION`/`ANGLE`、伺服负载、主轴负载/转速、`WARNING`、`cnc_rdblkcount`
这五组从 🟡 变 🟢**，client 侧照 POSELM/LOADELM 解码即可（下一步就做这个）。
另外那批 FOCAS 调用的**请求码**（§2.4）也在这台模拟机上对着真应答复验过一遍。

### 文件工具：文件名（key）必须带前导斜杠

`key` 是 NC-Link 的**文件路径**（`/data/source.txt`），文件工具按它拼本地落地区
（`<root>/uploadFile/<key>`）。少一个斜杠会拼成 `uploadFiledata/source.txt` ——
既不是路径也不是名字，而且只有这一条调用会用到那个名字，别的操作按 `/data/…` 找
就找不到。现在入口统一挡下来（`write` / `read` / `ll` / `mkdir` / `delete` 都过
`file_key_check()`），理由里写清正确写法；同时对内的 `upload_path()` 补一次分隔符，
别的调用点再传裸名字也不会拼歪。

测试：文件套件新增一条（裸名字回 NG 且理由里带 `/`，并且不会走到"最后一段"、
本地也不会留下拼歪的文件）——308 checks / 0 failures。

### 文件处理的"最后一段"：FOCAS 接到文件流程上（client → adapter → 机床）

文件的链路定了：**client → adapter → 机床**。前两段是文件流程本身（`/CONTROLLER/FILE`
的对象操作 + `call` 里带通道参数的传输，字节走文件通道 / FTP），**最后一段
（adapter → 机床）由厂商适配器用协议接口实现** —— FANUC 这份就是 FOCAS 的程序上下行。

- **core 加接缝**（`include/nclink/ncl_file.h`）：新增 `ncl_file_backend`
  （`push` / `pull` / `remove`）与 `ncl_file_tool_set_backend()`。core 不链接协议
  客户端，所以实现是插件在打开自己的连接时注册进来的；没注册就是现在的"只到本地
  目录"。
- **以设备本地为准**：`write` 把文件落到本地之后调 `push` 送进机床；`read` 在本地还
  没有那份文件时先调 `pull` 从机床取回（再按流程发布给对端）；`delete` 先删机床上的
  （删不掉——比如正在执行——本地也留着），再删本地。
- **plugins/focas.c 实现这个后端**：`push` = `ncl_focas_program_download()`
  （`cnc_dwnstart4` 三件套，已按官方库验通）、`pull` = `program_upload()`、
  `remove` = `program_delete()`（后两条的应答切法还待核，会明确回"还读不了"），在
  `focas_open()` 里注册、`focas_close()` 里撤销。
- **撤掉上一轮给 FOCAS 工具自造的 8 个方法**（`/PROGRAM@*`、`/*@WRITE`）：文件的门面
  只有文件工具那一个（现场门面是 `/CONTROLLER/{CONSOLE,FILE,PROGRAM_DATA}`，没有一条
  挂在设备节点上）。点位表里留了注释说明这件事。
- 测试：`tests/test_file.c` 新增 `test_file_backend()`（接缝的契约：函数指针不全要拒、
  注册/撤销干净），**并在真文件流程里断言三个调用点**：`openFileChannel` 握手之后
  `write` 落本地 → `push` 被调到（名字与本地路径都对）、`read` 本地没有 → 先 `pull`
  再从本地发布、`delete` → 先 `remove`；撤销后端之后不再调它。
  文件套件 303 checks / 0 failures，全量 42/42、零 warning。

### FANUC：按"数据字典 × FOCAS 能力"补全点表与方法面

前两轮是"补函数"，这一轮做成**完整映射**：32 册新增 §5.2「点表全映射」（字典的表
1/2/3/4/6/7 逐项 ↔ FOCAS 侧调用与 item 码 ↔ 状态 ↔ 归置/采样），声明与 client 照着它
补齐。状态三档：**✅ 已通** / **🟡 码已核·应答待核**（声明照常、函数回
`NCL_ERR_UNAVAILABLE` 并写明要抓哪个调用）/ **⛔ 供不了**（写清为什么：FANUC 没有这一
项，或只有 PMC 地址由现场自定）。

- **点位表**（plugins/focas.c，20 → 40 个点位）：新增 `WORK_MODE`、`LINE_NUMBER`、
  `PROGRAM_NUMBER`、`SUBPROGRAM`、`TOOL_NUMBER`、`FEED_SPEED`、`FEED_OVERRIDE`、
  `SPINDLE_OVERRIDE`、`PATH_LEFT_LENGTH`、`TORQUE`、`CURRENT`、`TEMPERATURE`、
  `ANGLE@REAL`（旋转轴按表 4 的 ANGLE 报）、`TYPE`（轴类型，configs）、
  `TOOLPARAM`/`VARIABLE`/`PARAMETER`/`COORDINATE`（表 7 的表类数据对象）、
  `MODEL`/`VERSION`/`MANUFACTURER`（表 6 元信息，configs）、`/MOTOR@S1/SPEED`
  （主轴转速；表 2 没有 SPINDLE，主轴按 MOTOR 归置 —— 口径写在 32 册 §5.2）。
- **方法面**（动作）：程序 `@DOWNLOAD`（✅ 已验通）/`@UPLOAD`/`@DIRECTORY`/
  `@SELECT_MAIN`/`@DELETE`；写动作 `PARAMETER@WRITE`/`TOOL@WRITE`（**改刀补会撞刀**，
  文档点名）/`VARIABLE@WRITE`。
- **client** 新增：`axis_torque/current/temperature/type`、`feed_speed/feed_override/
  spindle_override`、`subprogram_number`、`tool_number`、`tool_param_table`、
  `parameter_table`、`variable_table`、`work_offsets`、`model`、`version`、
  `manufacturer`（**唯一一条不用读机床的**：这一份 client 接的就是 FANUC）、
  `program_select_main/delete`、`parameter_write`、`tool_offset_write`、`macro_write`。
- 32 册 §5.3 划掉上一轮定下来的两条（`cnc_actf` = 实际进给速度、`cnc_acts` = 实际主轴
  转速；坐标类调用 = item 0x26），剩下"应答怎么切"的清单交给真机 / NCGuide。
- plugins/FANUC-ADAPTER.md §5 的点位表按同一张映射重写（每行带状态与归置）。

验证：`--model` 打出来的模型逐个核对（设备 9 个 dataItems + 4 个 configs、CONTROLLER
3 + 6、AXIS@X 7 + 1、AXIS@A|C 多一个 `ANGLE@REAL`、MOTOR@S1 1 个；采样通道仍是
`STATUS`/`PART_COUNT`/`CONTROLLER/PROGRAM`/`WARNING` 四条，按 `type: SAMPLE_CHANNEL`
认它 —— 设备级 config 点位排在它前面了）；MSVC 全量 **42/42**、零 warning。

### FANUC：按官方 SDK 补全 client 的 API 面 + 修正三处口径

拿到 FANUC 官方 FOCAS 开发包（`Fwlib64.dll` + `Fwlib64.h` + 每个函数一页的文档 +
函数手册）后，把"这台机床还能读什么"从**十二个调用**扩到**二十多个**，方法是让**官方库
自己对着一台假机床跑**：假机床打请求、铺可辨识载荷，探针印出 `Cb` 码与出参结构
（新工具 `tools/site-probe/focas_sdk_mock.py`、`focas_sdk_probe.c/.ps1`、
`focas_item_scan.py`，方法写在 01 册 §2.4）。

**核出来的请求码**（完整表见 01 册 §2.4）：`cnc_rdprgnum` = `0x1c`、
`cnc_rdseqnum` = `0x1d`、`cnc_alarm2` = `0x1a`、`cnc_rdngrp` = `0x4a`、
`cnc_rdtimer` = `0x120`（d 选哪种时钟）、`cnc_absolute`/`cnc_machine`/`cnc_relative`/
`cnc_distance`/`cnc_rdposition` = `0x26`（d 选位置类型、e 给轴号或 ALL_AXES）、
`cnc_rdalmmsg2` = `0x23`、`cnc_rdsvmeter` = `0x56`+`0x89`、`cnc_rdspmeter` = `0x40`+`0x8a`。

- **client 新增语义函数**（`clients/include/nclink/clients/focas.h`，按域分组）：
  状态/模式（`status` / `mode` / `emergency`）、报警（`alarm_status` / `alarm`）、
  轴与主轴（`axis_feedrate` / `spindle_speed` / `axis_position[_machine|_relative]` /
  `axis_distance` / `axis_position_cmd` / `axis_load` / `spindle_load`）、
  程序（`program_name` / `program_number` / `main_program_number` / `line_number` /
  `executed_block` / `program_directory`）、计数与计时（`part_count` /
  `tool_group_count` / `timer`）、刀具与参数（`tool_list` / `tool_offset` /
  `tool_life` / `macro_variable` / `parameter` / `work_offset` / `modal` / `system`）。
  **真读的**（码与载荷都核过）：件数、程序号/主程序号、行号、报警状态位、刀具组数、
  五种时钟、进给速度、主轴转速、模式与急停；**请求码已核、应答待真机核的**照常声明，
  回 `NCL_ERR_UNAVAILABLE`（理由里写明要抓哪个调用），抓包补上时只改函数体。
- **修正三处口径**（官方手册与线上实测对照出来的）：
  1. `cnc_actf` 是**轴的实际进给速度 F**、`cnc_acts` 是**主轴转速 S** —— 早先把它们
     当成"位置/速度"：`/MACHINE/AXIS@k/POSITION@REAL` 喂进去的其实是进给速度。
     现在 `ncl_focas_axis_feedrate()`（ACTF）绑 `/AXIS@k/SPEED`（表 4 的 SPEED，
     mm/min），`POSITION@REAL` 改绑 `cnc_absolute`（帧待抓包，先答"还读不了"）。
  2. `cnc_rdcount` 的 d/e 是 **0/0**、`cnc_rdlife` 才是 1/1 —— 原 item 表两条都写 1/1，
     件数读的其实是寿命那一支。
  3. `cnc_rdblkcount` 的 item 码是 **0x35**（原表写成 0x06，那是程序目录一族）。
- **plugin 点位表**：新增 `/MACHINE/WORK_MODE`（表 7 的 WORK_MODE，aut/manual 两位推
  `manual`/`auto`）、`/MACHINE/CONTROLLER/PROGRAM_NUMBER`（表 7 的 PROGRAM_NUMBER）、
  `/MACHINE/LINE_NUMBER`（表 7 的 LINE_NUMBER，文本 `N1234`）；`/AXIS@k/SPEED` 改绑
  进给速度。**主轴转速暂不进模型**：表 2 的组件类型里没有 `SPINDLE`，client 里的
  `ncl_focas_spindle_speed()` 先给 API 用，口径定了再加点位。
- 测试：`clients/tests/test_focas.c` 新增 `test_semantics()`（假机床喂值，真读的几条
  逐个核对；待抓包的几条核 `NCL_ERR_UNAVAILABLE` + 理由里的调用名），并锁死新核的
  item 码（含 RDCOUNT/RDLIFE 的 d/e 之别）；`clients` 套件 **197 checks / 0 failures**。
- **程序上下行（新增能力）**：FOCAS 里"传文件"是三件套，不是一条读 ——
  下行 `cnc_dwnstart4`（func `0x11`，定长 516 字节体：数据种类 + 目录名）→
  分块 `cnc_download4`（func `0x12`、**dir 4**，体就是程序文本，机床不应答）→
  `cnc_dwnend4`（func `0x13`，**下载的错误都在这条回**）；上行是
  `cnc_upstart4`(0x15) / `cnc_upload4`(0x18) / `cnc_upend4`。帧与体长按官方库实测
  （01 册 §2.4），并**用假机床把下行整条验通**（帧序 + 文本 + dir + end 收尾）。
  client 侧 `ncl_focas_program_download()`（已实现）/ `ncl_focas_program_upload()`
  （请求码已核、应答切法待核 → `NCL_ERR_UNAVAILABLE`），plugin 侧挂成**方法**
  `/MACHINE/PROGRAM@DOWNLOAD`、`/MACHINE/PROGRAM@UPLOAD`（程序是动作，不是数据对象）。
  测试 `test_program_transfer()`；`clients` 套件 **211 checks / 0 failures**。

### 重构：删掉"待抓包"的声明形状 —— 能不能读由 client 的函数返回值说

`NCL_DATAITEM_PENDING[_SAMPLED]` / `NCL_CONFIG_PENDING` 这种宏只表达一件事：**这条路径还
没有实现**。它让点位表替 client 说话，而且抓包补上时要改两处（点位表 + client）。现在只剩
一个机制：**点位照常声明、照常绑函数，函数回 `NCL_ERR_UNAVAILABLE`**。

- **新增错误码 `NCL_ERR_UNAVAILABLE`（-15，`ncl_err_name()` → `UnavailableException`）**：
  点位声明了、在模型里，但这一份构建还没有它要的协议调用（通常是帧还没抓到）。它不是"读
  失败"（根本没问过机器），也不是 `NCL_ERR_NOT_SUPPORTED`（那表示操作没声明）。
- 声明层：删掉三个 `*_PENDING` 宏，以及 `ncl_tool_point` 的 `available` / `summary` 两个
  字段（`summary` 原来兼作模型里数据项的 `description`，跟着删；描述性的东西不进这条链路）。
  每个点位都必须有函数（校验器照旧拒 `fn == NULL`）。**模块 ABI 2 → 3**：点位结构体变了，
  老模块会被装载器明确拒收，不会按错位的偏移读。
- 工具层（`src/tool/tool.c`）：点位函数回 `NCL_ERR_UNAVAILABLE` 时，这条点位答
  `<路径>：还读不了（UnavailableException）`（作者自己用 `ncl_tool_fail()` 带了理由就原样
  用作者的），**不写 §6 审计** —— 没问过机器，就没有"发生过的请求"。这个状态**学出来就记住**
  （"这一份构建没有那个调用"不会自己变），新增 `ncl_tool_point_unavailable()` 供宿主查询。
- 宿主（`src/tool/host.c`）：`ncl_host_point_available()` / `ncl_host_point_summary()` 换成
  `ncl_host_point_unavailable()`；轮询第一次读到"还读不了"就学下来，之后不再把周期浪费在它
  身上；`ncl_host_poll_one()` 对它直接回 `NCL_ERR_UNAVAILABLE`（不去找机器）；`--once` 自检
  把它列成 `<待抓包>`、**不计失败**（退出码照旧 0），真读失败才计失败。
- 新增取值形状 **`NCL_DATAITEM_JSON[_SAMPLED]` / `NCL_CONFIG_JSON`**（+ `ncl_tool_value_json()`）：
  值不是标量时（报警的 `{"number","text"}`、刀具表这类表），client 直接把 JSON 交出来。
- client（`clients/focas`）：新增 `ncl_focas_alarm()` / `ncl_focas_axis_position_cmd()` /
  `ncl_focas_tool_list()` —— 帧还没抓到的那三条调用现在**有名字、有位置、有一句话理由**
  （`ncl_focas_last_error()`；"要抓哪一帧"写在函数注释里）。`plugins/focas.c` 那 7 个点位
  改用普通绑定，一行都没多写。
- 测试：`tests/test_tool.c` 的 pending 用例改成"还没实现的帧"（模型、采样通道占位、
  `code=NG` + "还读不了"、作者自带理由原样出去、不进审计、状态是学出来的）；
  `tests/test_host_tool.c` / `tests/module_tool_basic.c` 同步；MSVC **42/42** 通过。
- 文档：`plugins/README.md`（2.5 节：`NCL_ERR_UNAVAILABLE`；形状表加 `_JSON`；ABI 3）、
  `plugins/FANUC-ADAPTER.md`（自检与 `--probe` 输出、点位表、"待抓包"一节、ABI 3）、
  `MANUAL.md`（错误码表 + 声明一节的"还没实现的点位"）。

### 重构：适配器层并回核心 —— tool 层（`src/tool/`）+ 唯一设备程序 `ncl_server`

写一台 NC-Link 设备原来要维护一棵与核心平行的树（`adapters/`：2500 行宿主 + 自己的
头文件目录 + 自己的驱动注册表 + 自己的测试）。这与本项目的初衷相反：**引用核心、
写一个声明式的 tool 文件、启动**就够了。现在适配器层的全部实现都在核心旁边：

- `src/tool/`（与 `src/server/` 同级）＝ tool 层：`tool.c`（声明 → 模型/绑定，原
  `src/core/tool.c`）、`driver.c`（驱动骨架：地址模型、错误分级、会话规则）、
  `module.c`（模块装载器）、`audit.c`（§6 审计）、`host.c`（宿主：声明 + 配置 → 设备）、
  `main.c`（唯一的程序入口）。
- 公共头：`include/nclink_adapter/{ncl_driver,ncl_audit,ncl_module}.h` 并入
  `include/nclink/`；`ncl_adapter.h` 变成 `include/nclink/ncl_host.h`（API 由
  `ncl_adapter_*` 更名为 `ncl_host_*`）。`include/nclink_adapter/` 与 `adapters/`
  两个目录删除。
- 厂商适配器：`adapters/plugins/focas.c` → `plugins/focas.c`，一个文件一个适配器，
  由 `plugins/CMakeLists.txt` 编成 `<build>/plugins/ncl_driver_<工具名>.dll|.so`。
  程序 `ncl_adapter` 更名为 `ncl_server`：装载 `<root>/plugins` → 注册模块声明的
  工具 → 跑 MQTT/REST/轮询。默认配置路径改为 `<root>/conf/device.json`。
- 测试：`adapters/tests/*` 并入 `tests/`（`test_adapter_tool.c` → `test_host_tool.c`，
  套件名 `adapter_tool` → `host_tool`，`test_driver.c` / `test_audit.c` 照旧）。

**按“没有消费者就不留”删掉的死代码**：

- 协议工厂注册表整块：`ncl_driver_register_protocol()`、`ncl_driver_register_builtin()`、
  `ncl_driver_create()`、`ncl_driver_protocol_count()`、`ncl_driver_protocol_known()`，
  以及 `driver.c` 里 12 条内置协议注册和随之而来的 `clients/**` 头文件依赖（只保留
  `ncl_driver_factory` 这个类型）。适配器现在直接 `ncl_focas_create()`、
  `ncl_modbus_tcp_create()`，协议名不再出现在配置里。
- 一代模块 ABI（模块交驱动工厂）在装载层本来就拒收，注释里"老式驱动模块"的提示保留。
- 由此核心库不再依赖协议客户端：`nclink_core`（含 tool 层）与 `nclink_clients` 彻底
  分开，适配器模块两个都连；CMake 选项 `NCLINK_BUILD_ADAPTERS` 换成
  `NCLINK_BUILD_CLIENTS`，`nclink_drivers` 静态库与 `nclink::drivers` 目标消失。
- 测试侧同步：`tests/test_driver.c` 删掉 registry 套件、mock 直接用
  `ncl_mock_driver_create()`；`tests/test_point_map.h` 去掉"按 type 查表"分支（驱动
  必须由工厂传进来）；`clients/tests/*` 各套件改为直接构造它测的那个客户端。

构建脚本同步：`build-linux.sh`（核心库排除 `src/tool/main.c`、产出
`libnclink_clients.a`、`bin/ncl_server`、`plugins/*` 适配器模块与测试夹具模块，
clients 套件改连客户端库）、`.vscode` 的 includePath 换成 `clients` / `clients/include`。

验证：Windows/MSVC `build.ps1` **41/41 通过**（含新的 `host_tool` 端到端套件；
`driver` / `audit` 曾被 CMake 顺序问题静默跳过，已修正并纳入）；`sh -n build-linux.sh`
语法通过（Linux 全量本轮未在本机执行，需在 Linux 容器里复跑一次）。

尚未完成（后续提交）：运行期目录方案（`conf/device/<id>.json`、模型 `conf/model/<id>.json`、
`data/` 下的本机文件目录、两个 `ftp.txt` 改名为 `conf/ftp-server.json` /
`conf/ftp-client.json`）、MQTT 监护线程、HTTP 配置面扩展、`plugins/README.md` 与
`plugins/FANUC-ADAPTER.md` 的改写、README/MANUAL 其余章节的措辞。

### 新增：声明式适配器的"绑定档" + client 的语义层（FOCAS 是样板）

写一台设备原来要写"一个 dispatch 函数 + 每个点位挂它"，点位表里还得把协议地址摆出来。
现在常见点位**一行绑定**就够：client 给出有名字的语义函数，适配器把函数绑到模型路径上。

- **绑定宏族**：`NCL_DATAITEM_I64 / F64 / BOOL / STR`（+ `_SAMPLED` / `_RW`），
  `NCL_CONFIG_*` 同形但没 `_SAMPLED`（配置不许采样）；方法用 `NCL_METHOD_CALL`。
  **实例就是 `open()` 返回的那个指针**，所以绑定宏里不写实例名 —— "必须有一个 client
  实例"是结构上的，不是纪律。语义函数签名按族固定（出参类型就是宏名里的类型）；
  给现场参数（轴号、子项）时同一个宏名加第三个参数即可，不用记第二个名字。
  失败 → 应答 `code=NG` + "路径 + 动作 + 错误名"的一句话理由。
- **FOCAS 语义层**：`clients/focas/focas_values.c` 把"哪个 item、哪一块、怎么由 ODBST
  位域推三态"固定进 client，公开头新增一节"现场接口（语义）"：
  `ncl_focas_open/close/connected/last_error` + `status / part_count / program_name /
  axis_position / axis_speed`，再加给覆盖档用的 `read_item / call / last_raw`。
- **`plugins/focas.c` 重写**：328 → 147 行，20 个点位里 18 条是绑定，2 个方法（会话/项表
  诊断）是覆盖 —— 绑定与覆盖混用一张表。协议细节（PDU 帧、item 码、回复块）从这个文件里
  彻底消失。
- **文件收敛**：删 `clients/focas/ncl_focas_driver.h`（内容就是过时的地址模型说明 + 一个
  构造函数声明），帧层与构造函数进新的内部头 `clients/focas/ncl_focas_pdu.h`；公开头
  `nclink/clients/focas.h` 从 344 行缩到 107 行，只剩现场接口。删掉 `focas_write_batch`
  这个只回 `NOT_SUPPORTED` 的桩（FOCAS 只读；骨架对空缺位本来就回同一个答案），
  `focas_raw`（逆向用的 raw 逃逸口）保留，但只在内部头里说明。
- **PART_COUNT 改成整型**：`ncl_focas_part_count(ncl_focas *, long long *)`，插件用
  `NCL_DATAITEM_I64_SAMPLED` 绑定。32 册表 7 与 FANUC 附录原来记成 string，是记错了
  （示例模型一直按数值用），两处文档一并改回 number。
- 测试：新增 `tests/test_bind.c`（四种取值、带现场参数、读写、只读点被拒、语义函数报错
  → NG 带错误名、配置型绑定落进 `configs`、方法与覆盖档拿到同一个实例）。
  Windows/MSVC 与 Linux/gcc 13.5 均 **42/42**。

### 变更：点位路径相对设备节点（设备类型只写一次）

点位路径原来要求每条都写设备段（`/MACHINE/STATUS`），而设备类型配置里已经有一份 —— 两处
必须一致，改机型要动整张表。现在：

```c
NCL_TOOL_BEGIN("focas", "FANUC 数控机床", "MACHINE", 1000, 1000, open, close)
    NCL_DATAITEM_STR_SAMPLED("/STATUS",             ncl_focas_status)
    NCL_DATAITEM_STR_SAMPLED("/CONTROLLER/PROGRAM", ncl_focas_program_name)
    NCL_DATAITEM_F64("/AXIS@X/POSITION@REAL", ncl_focas_axis_position, NCL_FOCAS_AXIS_X)
NCL_TOOL_END()
```

- **设备类型在 `NCL_TOOL_BEGIN` 的第三个参数里定义一次**（表 1 的设备对象类型）；
  配置里的 `device.type` 可以不写，写了就必须一致（不一致启动即报错，不再有"两处必须
  同步"的负担）。
- **路径相对设备节点，且可以任意深**：最后一段是数据/配置对象，前面每一段都是组件
  （组件可以嵌套）；每一段都能用 `type@number` 区分（`AXIS@X`、`SUB@1`、
  `POSITION@REAL`）。模型写出器据此逐段建组件、父子嵌套。
- **对外的路径不变**：模型里的路径、采样通道、REST 地址、`ncl_host_point_path()` 仍是
  绝对路径 `/MACHINE/...`（由设备段拼出来）；登记时把相对路径拼成绝对路径存进
  registration，绑定键与 §6 审计轨迹都用绝对路径。点位**名字**（方法名/schema）仍从相对
  路径推：`/AXIS@X/POSITION@REAL` → `AXIS_X.POSITION_REAL`，`focas/AXIS_X.POSITION_REAL`。
- 新增 `ncl_tool_model_path()`（相对 → 绝对，宿主拼轮询路径用它）；`tests/test_tool.c` 新增
  "路径任意深：中间每段是组件，子组件的 number 也照写"用例（顺带抓出模型写出器按"点数"
  分配组件槽位的越界写：深层路径下一个点位会建多个组件，槽位改成按路径斜杠数上界分配）。

### 文档：适配器作者指南重写、新增协议实现笔记、FANUC 现场手册改口径

- `plugins/README.md` 从 530 行重写为 203 行：三种写法（绑定 / 覆盖 / 写协议）、声明语法
  速查（路径规则、两族宏与形状表、操作位、PENDING、参数三条通道）、模块装载与 ABI、配置
  文件（只剩参数/设备/采样/broker）、命令行与自检审计、构建测试、排错表、例子索引。
- 新增 `clients/README.md`：协议一览（client 构造函数、端口、现场地址写法）+ 各协议笔记
  （原适配器文档里的协议知识，配置样例换成"现场怎么用"）+ 加一个新协议（最小面、错误分级、
  按批分配、测试要求）+ 语义层怎么补（FOCAS 是样板）。
- `plugins/FANUC-ADAPTER.md`：程序名 `ncl_adapter` → `ncl_server`，第 5 节改成"绑定 + 覆盖"
  的点位表（含 `PART_COUNT` 数值、刀具列表进 `configs`、7 个待抓包/待核对），第 6 节改成
  "换模块就是换机型"的三步。
- `MANUAL.md` / `README.md` / `RELEASE.md` / 32 册：目录结构、构建选项（`NCLINK_BUILD_CLIENTS`
  与 `NCLINK_BUILD_PLUGINS`）、套件口径（42 套 = 31 套核心与工具层 + 11 套协议客户端）。
### 变更：模型写 `dataType`（跟字典走）+ 刀具列表 `/MACHINE/CONTROLLER/TOOL`

声明式工具生成的模型原来一项 `dataType` 都没有，而 `FILE`（dict）、刀具列表（list）这类
集合类数据对象正是靠它告诉客户端"值是字典还是列表"。现在按字典（32 册）的"类型"列补上，
作者不用声明：

- `HASH`（dict / JSON 对象）：`WARNING`、`FILE`、`PARAMETER`、`PART`、`TOOLPARAM`；
- `LIST`：`COORDINATE`、`SHELF_UNIT`、`TOOL`、`VARIABLE`；
- 标量（string / number）不写这一项 —— 与手写的那份设备模型一致。

同时 FANUC 适配器加上刀具列表点位：`NCL_CONFIG_PENDING("/MACHINE/CONTROLLER/TOOL", …)`
—— 名字取自表 7 的 `TOOL`（刀具，list），FOCAS 侧是刀补表/刀具表那一族调用、帧还没核对
（32 册 §5 把它列在"待核"里），所以先按待抓包声明：模型里有它、问它有明确答复、轮询跳过。

改动面：`src/core/tool.c`（`k_data_types` 表 + `tool_data_type_of()` + 模型写入）、
`adapters/plugins/focas.c`、`tests/test_tool.c`（"FILE 是 HASH、刀具是 LIST"的模型断言）、
文档（`adapters/README.md`、32 册 §5、`MANUAL.md` 4.3）。

> 现场有的点表用 `TOOL_PARAM` 这个名字（28 册交付清单、29 册现场模型、Brother / KEDE），
> 那是现场自定名、本册没有；实现层只认册子上的名字 —— **刀具列表是 `TOOL`**（list）、
> **刀具参数是 `TOOLPARAM`**（JSON 对象）。现场核过的另两处：`PARAMETER` 是 `HASH`
> （参数本身是字典），`COORDINATE` 是 `LIST`（一张表，跟刀具表一样）。

验证：`build.ps1` 41/41；test_tool 242 checks / 0 failures。

### 变更：操作位扩到标准的全部 11 种 —— 集合类数据对象（list / dict）能声明了

声明层原来只有三个位（`readable` / `writable` / `callable`），也就是 `get_value` /
`set_value` / `call` 三种操作。而标准第 5 部分给集合类单独定义了 `get_length`、
`get_keys`、`get_attributes`（表 11）与 `add`、`delete`（表 13）—— file 工具那五条
绑定（`read`/`write`/`ll`/`mkdir`/`delete` 就是 `get_value`/`set_value`/
`get_attributes`/`add`/`delete`）因此一直写不进声明，只能绕过声明层直接注册。

现在 `ncl_tool_point` 只有一个 `ops` 位集（`NCL_OP_BIT(NCL_OP_*)`），`k_operations`
扩到 11 个（含方法调用之后的 `status` / `result` / `cancel`），宿主按位注册绑定：

- 宏名字不变（`_RW` = `get_value | set_value`），新增 `NCL_DATAITEM_OPS` /
  `NCL_CONFIG_OPS` 让作者按位写全 —— 字典里 list / dict 的数据项就用它；
- 方法名：`get_value`→`"<点位>.read"`、`set_value`→`"<点位>.write"`（不变），其余按操作名
  加后缀（`"<点位>.get_attributes"`、`"<点位>.add"`…），方法调用仍是裸名；校验里
  "名字不许含 `.read`/`.write`"扩成"不许含任何一个操作后缀"；
- 校验：`sampled` 要求 `get_value`；`NCL_OP_WRITE_MASK`（`set_value` / `add` /
  `delete`）要求 `get_value`（可写必然可读）；`ops == 0` 仍是"没有声明任何操作"；
- `ncl_tool_point_handles()` / `ncl_tool_point_is_method()`：宿主、适配器与作者问的是
  同一句（"这个点位答不答这个操作" / "它是不是方法"）；
- 顺带修：`src/core/tool.c` 少 include 了 `ncl_logger.h`（`ncl_log_warn` 隐式声明，
  MSVC C4013）。

改动面：`include/nclink/ncl_general.h`（`NCL_OP_BIT` / `NCL_OP_COUNT` 与 Query /
Write / Call / Value 四个掩码）、`include/nclink/ncl_tool.h`（字段、宏表与注释）、
`src/core/tool.c`（校验、模型、11 个 shim、按位注册）、`adapters/src/app/adapter.c`、
`adapters/src/main.c`、`tests/test_tool.c`、文档（`adapters/README.md`、
`FANUC-ADAPTER.md` §5、32 册 §5、`MANUAL.md`）。

验证：`build.ps1` 41/41；test_tool 237 checks / 0 failures（新增"集合类"用例：
5 个操作 = 5 个方法 + 5 条绑定，`get_attributes` 与 `add` 原样送到点位函数、
`add` 也记进审计、没声明的 `get_keys` 走不通）。

### 变更：禁"只写" —— `writable` 蕴含 `readable`，宏 11 → 9

数据对象只有两种访问能力（可读、可写），而且**可写必然可读**：

- 审计（§6）要记下写之前的旧值，宿主靠点位自己的 GET_VALUE 去取 —— 只写点位拿不到，
  日志只能空着；
- 读完写不回来就没法确认（客户端写完不知道生效没有），自检（`--once`）更没法碰它；
- 模型文档里"在 `dataItems`/`configs` 里"本身就是在说"能查询"（每个数据项只标一个
  `settable`），只写点位会让模型和实际能力对不上；而且它今天会把
  `ncl_adapter_poll_round()` 的整轮打断（读失败一律算 tier 1 → 剩余点位全记失败后
  break）。

所以校验直接拒掉 `writable && !readable`（`the point %s can be written but not read`）。
设备真只收命令的东西 —— 口令、复位脉冲、清零 —— 不是数据对象，用 `NCL_METHOD` 声明，
值走方法调用的参数。

- `include/nclink/ncl_tool.h`：删 `NCL_DATAITEM_WRITE` / `NCL_CONFIG_WRITE`（9 个宏），
  字段注释与宏表写明这条规则；顶部示例补上第三个参数（原来还是两参数写法）；
- `src/core/tool.c`：`ncl_tool_validate()` 加这条校验（排在"sampled 必须可读"之后）；
  `shim_read_old_value()` 不再判 `readable` —— 可写必然可读，旧值总在；
- `adapters/src/app/adapter.c`、`adapters/src/main.c`：点位表与方法计数只看 `readable`
  （可写必然可读，原来那句 `readable || writable` 说的是同一件事）；
- 文档：`adapters/README.md`、`FANUC-ADAPTER.md` §5 宏表、32 册 §5、`MANUAL.md`。

改动面：上面这些文件 + `tests/test_tool.c`（新增"可写必然可读"的校验用例）。
验证：`build.ps1` 41/41。

### 变更：声明宏分成两族 —— `NCL_DATAITEM_*` 与 `NCL_CONFIG_*`

数据对象有两种，声明里就得写清是哪一种，宏名替你说这句话（`NCL_POINT_*` 一族随之退场）：

- **`NCL_DATAITEM_*`**（dataItems）：物理量、从设备感知的实时量 —— 能进采样通道。
  形状齐全：`_SAMPLED`（进默认通道）、`_WRITE`、`_RW`、`_ARG`、`_NAMED`、
  `_PENDING[(_SAMPLED)]`。
- **`NCL_CONFIG_*`**（configs）：参数、坐标系、刀具表、元信息这类不常变的数据 ——
  **没有 `_SAMPLED` 形式**（册 3 表 1 注 b：配置中的数据对象不得作为采样数据源）。
  其余形状与 dataItem 一一对应：`_WRITE`、`_RW`、`_ARG`、`_NAMED`、`_PENDING`。
- `NCL_METHOD(_ARG|_NAMED)` 不变：方法不是数据对象。

实现：`ncl_tool_point` 多一个 `config` 标志，`ncl_tool_model()` 据它把点放进设备/组件的
`dataItems` 或 `configs`（不再按 type 猜）；`ncl_tool_validate()` 拒绝 `config && sampled`
（手写表时的兜底）。数据字典那张"配置型 type"清单保留下来**做核对**：把 `PARAMETER`
这类名字声明成 `NCL_DATAITEM_*` 时打一条告警，提醒复核。

改动面：`include/nclink/ncl_tool.h`（23 个宏重排成两族）、`src/core/tool.c`、
`adapters/plugins/focas.c`（19 个点全部改为 `NCL_DATAITEM_*`）、适配器与核心夹具
（`/MACHINE/NAME` 现在是 `NCL_CONFIG`，`/MACHINE/CONTROLLER/PARAMETER` 也是）、
文档（`adapters/README.md`、`FANUC-ADAPTER.md` §5 宏表、32 册 §5.1.1、`MANUAL.md` 4.3）。
验证：`build.ps1` 41/41；`--model` 输出里 dataItems / configs 各自的成员与预期一致。

### 变更：宏精简到 11 个 —— 名字从路径推，数据总是第三个参数

上一条那套宏有 30 个（kind × 读写 × `_ARG` × `_NAMED` × `_PENDING` 的组合爆炸）。
砍掉两根轴之后只剩 11 个：

- **`_NAMED` 整族退场**：点位名字**从路径自动推**（去掉设备段、`@`→`_`、`/`→`.`）——
  `/MACHINE/AXIS@X/POSITION@REAL` → `AXIS_X.POSITION_REAL`，正是原来手写的那一串；
  路径唯一名字就唯一，所以 `ncl_tool_point` 的 `name` 字段整个删掉，
  `ncl_tool_point_name()` 改成写进调用方的缓冲区。方法调用地址不变
  （`focas/AXIS_X.POSITION_REAL`、`focas/SESSION`）。
- **`_ARG` 整族退场**：数据就是第三个参数，点位没有自己的数据就写 `NULL`
  （`NCL_DATAITEM(path, fn, arg)` / `NCL_CONFIG(path, fn, arg)` / `NCL_METHOD(path, fn, arg)`）。
- 剩下只有 `_WRITE` / `_RW` / `_SAMPLED`（仅 dataItem）/ `_PENDING[_SAMPLED]` 这几个变体。

这一步之后剩 11 个宏：`NCL_DATAITEM[_SAMPLED|_WRITE|_RW]`、`NCL_DATAITEM_PENDING[_SAMPLED]`、
`NCL_CONFIG[_WRITE|_RW]`、`NCL_CONFIG_PENDING`、`NCL_METHOD`。

改动面：`include/nclink/ncl_tool.h`、`src/core/tool.c`（名字推导 + 校验）、
`adapters/plugins/focas.c`（表短了一大截）、适配器夹具与测试、`adapters/README.md`、
`FANUC-ADAPTER.md` §5。验证：`build.ps1` 41/41；FANUC `--once` = 19 个点位
（13 可读 + 6 待抓包）、0 失败。

### 变更：数据对象分两种 —— `dataItems`（感知量）与 `configs`（配置型数据）

第 3 部分把数据对象放在两个数组里，第 4 部分的数据项按"变不变"归置：

- **`dataItems`** = "可以采集的数据"：表 4 的物理量与从设备感知的实时量
  （`STATUS`、`WARNING`、`PART_COUNT`、`PROGRAM`、`WORK_MODE`、`POSITION`、`SPEED`…）。
  **采样通道只能引用它们。**
- **`configs`** = "配置信息"：参数、坐标系、刀具表这类不常变的数据 —— `PARAMETER`、
  `COORDINATE`、`TOOL`、`TOOLPARAM`、`VARIABLE`、`FILE`、`SHELF_UNIT`、`TYPE`，以及
  `MODEL`/`NUMBER`/`VERSION`/`MANUFACTURER`/`CREATOR`/`CREATE_TIME`/`NAME` 这类元信息。
  照样有路径、照样能按需读，但**表 1 注 b：配置中的数据对象不得作为采样数据源**。
  采样通道对象本身也是 `configs` 的一员（第 3 部分就是这么放的）。

实现：归置**由 `type` 决定**（名字来自数据字典，分类跟着名字走），作者不用写开关 ——

- `ncl_tool_model()`：设备与组件各自把配置型的点放进自己的 `configs`（`dataItems`
  照旧放感知量）；`source` 依然一个都不写。
- 校验：配置型 type 被声明成 `NCL_POINT_SAMPLED_*` 时**直接拒**（"…是配置型数据…不能进
  采样通道"），免得现场悄悄采到一个参数表。
- 厂商自定的 `type`（字典里查不到）默认按 `dataItems`。

测试：`tests/test_tool.c` 的夹具里 `/MACHINE/NAME`（表 6 元信息）现在落在 `configs`，
新增"配置型数据不能进采样通道"的拒绝用例；适配器夹具加了一条
`/MACHINE/CONTROLLER/PARAMETER`，断言它进 CONTROLLER 组件的 `configs`、能按路径读、
但不在通道里。验证：`build.ps1` 41/41。

文档：32 册新增 §5.1.1（两种归置与 type 清单）、`adapters/README.md`（模型一节）、
`FANUC-ADAPTER.md` §5（现有 19 个点全是 `dataItems`；参数/坐标系/刀具表等帧核对后再加，
宿主会自动放进 `configs`）、`MANUAL.md` 4.3（路径表与两种数据对象）。

### 变更：数据项与组件的 `name` 改成可读名（不再放路径）

第 3 部分说 `name` 是"用易于理解的词语或者词语的组合表示"，不是路径；路径由树
（`type` + `number`）拼出来。现在 `ncl_tool_model()` 这么写：

- 数据项的 `name` = **数据字典里那个 type 的中文含义**（表 4/表 6/表 7）：`STATUS` →
  运行状态、`PART_COUNT` → 加工件数、`PROGRAM` → 主程序名、`WARNING` → 报警信息、
  `POSITION` → 位置、`SPEED` → 速度……带 `number` 的缀在后面：`POSITION@REAL` →
  位置（实际）、`POSITION@CMD` → 位置（目标）、`POWER@1` → 功率（1）。
- 组件的 `name` = 表 2 的中文名，轴号放前面：`AXIS@X` → X 轴、`CONTROLLER` → 控制器。
- 字典里查不到的 `type`（厂商自定）照原名，至少不是空的。
- **名字与路径无关**：路径是 `type`（与 `number`）在树里拼出来的，改名不会动路径 ——
  上位机按路径问、按名字显示。

测试跟着改（`test_tool.c` 的模型断言、适配器夹具的 `/MACHINE/RUN` → 名字 `RUN`）；
文档同步（[32 册](protocal/docs/32-标准第4部分-数据项定义.md) §5.2、`adapters/README.md`、
`FANUC-ADAPTER.md` §5）。验证：`build.ps1` 41/41；FANUC `--model` 里数据项名字已是
运行状态 / 加工件数 / 报警信息 / 主程序名 / 位置（实际）/ 位置（目标）/ 速度。

### 变更：路径只有一种算法 —— `source` 是父路径的简写，两者必须一致

同一个节点有两条路可以算出路径：写了的 `source`（简写），或者向上遍历父节点拼接。
以前这两条路会给出**不同**的结果：设备下的数据项按声明是 `/MACHINE/STATUS`，而按
父节点拼出来是 `/STATUS`（组件更是被直接忽略成 `/AXIS@X`）。现在只有一条规则：

- **路径 = 父路径 + "/" + `type`（有 `number` 时写成 `type@number`）**。根节点只出
  一个 `/`（不是一段），所以设备对象（`device.type` = `MACHINE`）答在 `/MACHINE`，
  它下面的组件、数据项都带这一段。
- **`source` 只是"父路径的简写"**：写了就优先用它，但它必须和父节点拼接的结果一致 ——
  不一致时模型加载告警（列出两侧的路径），以 `source` 为准。客户端可以不认 `source`、
  自己走一遍树，两份结果必须一样。
- **适配器生成的模型里一个 `source` 都不写**："source 可以不给"，树的形状已经说明
  一切。`device.type` 与点位路径的设备段必须同名，不同名时 `ncl_tool_model()` 直接
  报错（否则模型路径和点位路径会分叉）。

影响面（路径变了：设备下的数据项与组件多出设备段）：

- 核心：`ncl_node_set_path()` 统一了四类子节点的规则（设备/组件/数据项/配置），
  去掉"父为设备则前缀清空"的老规则；根节点路径改为 `/`；`source` 一致性检查。
- 适配器：`ncl_tool_model()` 不再写 `source`，新增设备类型与设备段同名的校验。
- 测试：`tests/data/model_nclink.json`（通道 id 跟着改）、`test_model` / `test_message` /
  `test_client` / `test_server` / `test_tool`（新增"模型树路径 == 声明路径"用例）、
  适配器夹具 `module_tool_basic.c`（`/TEST/...` → `/MACHINE/...`）。
- 示例与绑定：`examples/device_model.c` + `ncl_device_demo.c` + `ncl_client_demo.c`
  （五个语言示例共用的模型与绑定），Python / C# / Java / Go 的示例、测试与 README，
  `MANUAL.md` 4.3 与示例表。
- 验证：`build.ps1` **41/41**；FANUC 假机床 `--once` = 19 个点位（13 可读 + 6 待抓包）、
  0 失败；`--model` 输出里 `source` 出现 **0** 次，数据项路径仍是 `/MACHINE/...`。

### 变更：模型里每个名字都来自数据字典 —— FANUC 私有位退出模型

模型里出现的每一个 `type` 都必须能在第 4 部分（[32 册](protocal/docs/32-标准第4部分-数据项定义.md)）
里查到。据此把 FANUC 适配器收了一遍口：

- 保留的名字全是字典名：`STATUS`（表 6 + 表 8）、`PART_COUNT`（表 7）、`WARNING`（表 6 +
  表 9）、`PROGRAM`（表 7）、`POSITION`（表 4）、`SPEED`（表 4）。
- **删掉 11 个自造名** `/MACHINE/FANUC_ODBST@MANUAL|RUN|EDIT|MOTION|MSTB|EMERGENCY|ALARM|
  SPINDLE|OPERATOR|DUMMY|AUTO`：字典里没有这些名字，第 3 部分的模型对象也只认字典里的
  `type`，所以 ODBST 位域**不进模型、不上报**。派生出来的量用字典名表达 —— `STATUS` 就是
  ODBST 的 RUN 与 EMERGENCY 两位推出来的 `running`/`free`/`holding`；要看原始位，走适配器
  自己的调试方法 `focas/ITEMS`（驱动项表，不是模型）。真机册 32 §5.3 记了这条口径。
- 出厂点位 **30 → 19 个取值**（4 个采样 + 15 个按需读，其中 6 个"待抓包"）+ 2 个方法；
  模块版本 1.2.0 → 1.3.0。采样口径不变（状态 / 计件 / 程序名 / 报警）。
- 需要"手动/自动"时，正解是表 7 的 `WORK_MODE`（取值 `manual`/`auto`，表 8），由 ODBST 的
  手动方式位（块 0 偏移 0）与自动方式位（块 2）推出；这次**没有声明**，现场要就加一条。
- 文档同步：`adapters/FANUC-ADAPTER.md` §5/§7、`adapters/README.md` 的 FANUC 一节、
  32 册 §5.1/§5.2/§5.3；`--model` 打出来的模型文件（`conf/fanuc-model.json`）已重新生成。

### 新增：`--model` 打印设备模型；JSON 有了给人看的写法

模型原先只在启动时生成一次、交给服务器就没人再见过它。现在适配器宿主留着这份文档，
`ncl_adapter --model` 把它打出来（`ncl_adapter_model()`），现场可以存成文件、用配置里的
`"model"` 指回去 —— 之后调采样周期、加减通道里的点位都只改这个文件，不用重编模块。

- 核心 JSON 新增 `ncl_json_write_pretty()` / `ncl_json_write_pretty_string()`：每层两格
  缩进、一个成员一行（同一份文档，只是排给人看），带测试（`tests/test_json.c`）。
- `ncl_adapter_model()`：适配器把发布出去的那份模型文档自己留一份（生成的那份，或配置
  里 `"model"` 指的文件），宿主可以打印、存盘、给状态页用。
- `--model` 只在本地读配置与模块，不连 broker、不碰机床。

### 变更：默认采样通道只放"状态 / 计件 / 程序名 / 报警"，点位可声明成"待抓包"
### 变更：默认采样通道只放"状态 / 计件 / 程序名 / 报警"，点位可声明成"待抓包"

现场口径：进默认采样通道的只有四样 —— **设备状态、加工计件、程序名称、报警**
（`/MACHINE/STATUS`、`/MACHINE/PART_COUNT`、`/MACHINE/CONTROLLER/PROGRAM`、
`/MACHINE/WARNING`）。位置、速度、厂商私有位一律按需读（`NCL_POINT_ARG`）；要上报就把那一行
换成 `NCL_POINT_SAMPLED_ARG`，不想上报就换回来 —— 改的是声明，不是配置。

- 声明层新增 **`available`** 字段与 `NCL_POINT_PENDING[_SAMPLED][_NAMED](路径[, 名字], 理由)`
  （`include/nclink/ncl_tool.h`），给"架构上已经定下来、协议调用还没抓到帧"的点位用：
  它在模型里看得见、`Query` 它会拿到理由明确的 `NCL_ERR_NOT_SUPPORTED`（不是"没有这个点位"）、
  轮询直接跳过；`*_PENDING_SAMPLED` 会占住采样通道的位置（抓包补上之前那一列是 `null`）。
  抓包补上以后把宏换成普通 `NCL_POINT_*` 即可，别处一行都不用改。理由（`summary`）是必填的。
- 宿主侧（`adapters/src/app/adapter.c`）：待抓包的点位**不进轮询、不计失败**；
  新增 `ncl_adapter_point_available()` / `ncl_adapter_point_summary()` 给自检与状态页用；
  `--once` 现在打 `自检：30 个点位（24 个可读，6 个待抓包），0 个读取失败` 并**退出码 0**
  （`--probe` 一个待抓包点位仍退出 1：问的是一个读不到的点位）。
- FANUC 适配器（`adapters/plugins/focas.c`，模块版本 1.2.0）：`/MACHINE/WARNING` 与五个
  `POSITION@CMD` 改成 `NCL_POINT_PENDING*` —— 报警按现场口径**留在采样通道里**（抓包前取值
  `null`），五个目标位置按需读；位置、速度、私有位全部改成按需读。删掉 `FOCAS_PENDING` 这种
  点位类型（"读不了"现在由声明表达，不由适配器里的分支表达）。
- 测试：`tests/test_tool.c` 新增待抓包用例（校验、模型与采样通道、`Query` 的答复与"没上线
  就不记审计"），`adapters/tests/module_tool_basic.c` 加了一个待抓包点位，`test_adapter_tool.c`
  断言宿主侧的跳过与不漏计。
- 文档：`adapters/FANUC-ADAPTER.md` §3/§5/§7、`adapters/README.md`（作者指南 + FANUC 一节）、
  [32 册](protocal/docs/32-标准第4部分-数据项定义.md) §5.2/§5.3、[31 册](protocal/docs/31-待真机抓包清单.md)
  §1 #8 同步。

### 变更：点位名对齐标准第 4 部分（数据项定义），模型树加组件层

读了标准第 4 部分原文（送审讨论稿）并记成 [32 册](protocal/docs/32-标准第4部分-数据项定义.md)，
适配器按它改了一轮：

- **模型路径首段 `MACHINE`**（设备对象 `type` 的取值，表 1；`CNC` 不是标准里的名字）。
- **数据项名用标准里的名字**：`/MACHINE/STATUS`（表 6 唯一的三态状态项，值
  `running`/`free`/`holding`，由 FOCAS 的 RUN/EMERGENCY 位推导）、
  `/MACHINE/PART_COUNT`（表 7，**string**）、`/MACHINE/CONTROLLER/PROGRAM`
  （表 7 的 `PROGRAM` 属于 `CONTROLLER` 组件）、10 条轴位置（`AXIS@X|Y|Z|A|C`，
  表 3 的轴名）与 5 条速度。
- **每轴两个位置**：`POSITION@REAL`（实际，读 `ACTF@4k`）与 `POSITION@CMD`（目标）。
- **模型树支持组件层**：`ncl_tool_model()` 现在把路径中段建成**组件节点**
  （`device → component → dataItem`，册 32 表 2/表 3），设备节点自己答在 `/MACHINE` 上；
  声明里的路径与模型树因此一致。
- **厂商私有项带前缀并存**：FANUC 的 ODBST 十一个位标准里没有名字，改为
  `/MACHINE/FANUC_ODBST@…`，不再占用 `STATUS`。（**这条后来被推翻**：这一版只解决了
  "不占用 `STATUS`"，没有解决"名字不是字典里的名字" —— 见上面那条变更，
  私有位现在整个退出模型。）
- **三个"待抓包"点位**：目标位置 `/MACHINE/AXIS@k/POSITION@CMD`、报警
  `/MACHINE/WARNING`（标准表 6 的 `WARNING`：JSON `number`/`text`）、以及 `ACTS`
  的量纲归属。对应的 FOCAS 调用（`cnc_rdposition`、`cnc_rdalmmsg2`、`cnc_rdaxisdata`）
  在 01 册 §2.3 里都没抓到帧 → 现在**读它们返回明确的"还没抓到帧"错误**，不给假值
  （当时是"按需读 + 读失败"；采样口径与"待抓包"的进位方式见上一条变更）；
  清单进了 [31 册](protocal/docs/31-待真机抓包清单.md) §1 #8。
  机床没有的轴同理：驱动在载荷不足时报错（`NCL_ERR_RANGE` / `NCL_FOCAS_ERR_LENGTH`）。
- 出厂点位：**30 个取值（24 个可读 + 6 个待抓包）+ 2 个方法**（采样口径见上一条变更）。

### 变更：适配器搬进 `adapters/plugins/`，`clients/` 只留协议

分层原先反了：适配器（点位声明、跟 NC-Link 对接的那一层）放在 `clients/focas/` 里，
而它用的 `ncl_driver_ops` 运行时属于适配器层（`include/nclink_adapter/`），等于客户端
反向依赖适配器。现在按"client = 原始协议，adapter = 跟 NC-Link 对接"分开：

- FANUC 适配器搬到 **`adapters/plugins/focas.c`**，编成 `plugins/ncl_driver_focas.dll`。
- **`adapters/plugins/` 自动发现**：`file(GLOB adapters/plugins/*.c)`，每个文件一个
  module 目标、输出 `plugins/ncl_driver_<文件名>.dll|.so`。新增品牌＝丢一个文件进去，
  不用改 CMake（客户编译链路 P4 会用同一条规则）。
- 从内置协议注册表里去掉 `focas`：适配器现在声明点位、自己调用 FOCAS 客户端，
  不再对外提供一个"协议工厂"，`NCL_DRIVER_FOCAS_IS_PLUGIN` 这个开关随之删除。

下一步（同一件事的另一半）：把 `clients/focas/` 变成**纯协议**——会话式惯用 API
（`ncl_focas_open/read/close/last_frame`，类型用字符串名、不认识 `ncl_address`/`ncl_dtype`），
适配器只调用它；随后 `ncl_driver_ops` 这一层在客户端里退场（含 `clients/*/ncl_*_driver.h`
的包装与对应测试）。

### 变更（破坏性）：配置点表与驱动管理器删除 —— 点位声明在适配器模块里

FANUC 适配器已经改成"一个文件 + 点位声明"（`adapters/plugins/focas.c`），配置点表
那条路随之删除：

- 删掉 `nclink_adapter/ncl_driver_manager.h` 与 `adapters/src/registry/driver_manager.c`：
  适配器宿主不再从配置读点位，也不再维护 path→驱动 的分派。
- `ncl_adapter` 只服务**声明式适配器**：模块声明点位（`nclink/ncl_tool.h`），宿主据此
  生成模型、采样通道与绑定；配置只写 `tools[].parameters`、`device`、`sample`、`mqtt`。
  配置里还留 `drivers[]` / `points[]` / `methods[]` 会明确报错并指出改法。
- 模块 ABI 只认**第 2 代**（点位声明）。老式驱动模块（交给宿主一个驱动工厂）被拒绝，
  错误信息里带"老式驱动模块请改写成声明式适配器"。
- 驱动的**接口**（`ncl_driver_ops`、`ncl_driver_read_one()` 等）与协议客户端原样保留：
  声明式适配器就在这一层实现，协议测试改用 `tests/test_point_map.h`（测试侧的小点表）。
- 受影响的 API：删 `ncl_adapter_drivers()`、`ncl_adapter_method_count()`、
  `ncl_modules_register()`、`ncl_module_registered()`；`ncl_adapter_create_with_modules()`
  成为入口（`ncl_adapter_create()` 仍在，相当于"没有模块"，会以"没有声明式适配器模块"
  失败）。

### 审计（§6）：记账回到宿主，作者不用写

上面的删除一度让声明式适配器的读写**不再落账**；现在由**宿主统一记账**，适配器作者
永远不碰审计：

- `ncl_tool_register()` 多一个 `const ncl_tool_audit *` 参数（`nclink/ncl_tool.h`）：
  宿主把"请求行 + 写行"的接口传进来，core 里的 shim 在每次点位调用前后记账
  （路径、操作、结果、耗时）；写操作先经点位自身读一次旧值，记「旧值 → 新值」。
- 原始报文由**声明里的可选回调**提供：`NCL_TOOL_END_WITH_RAW(fn)` 让模块把最近一次
  交换的字节交给宿主（`ncl_tool_frames`），宿主决定是否落盘（`--raw` 才记）。
- 会话生命周期（open/close）由适配器宿主直接记；`ncl_adapter` 的 `--stats` 现在对
  声明式设备有数（19 个点位自检后是 `"requests":19,"sessions":1`），`--raw` 会在
  日志里打出每次请求/应答的 hex（`AUDIT raw focas /MACHINE/... request 96 bytes a0a0...`）。
- 顺带修：`--raw` 原来把帧记在 DEBUG 行而默认级别是 INFO，等于看不见；现在
  `--raw` 会同时把日志级别抬到 DEBUG，这个开关名副其实了。

注：审计行里的"地址"列对声明式适配器是**点位名**（协议地址在模块内部），帧才是原始证据。

### 新增：动态库装载接口（`nclink/ncl_library.h`）

核心库原来没有"装载一个动态库"这件事（MQTT 的 TLS 走编译期），适配器要做插件就得
自己写 `LoadLibrary`/`dlopen`。现在这层收在核心里：

- `ncl_library_open(path, &error)` / `ncl_library_symbol()` / `ncl_library_close()` /
  `ncl_library_path()`；失败时给的是**平台自己的原因**（Windows 走
  `FormatMessageA`，POSIX 走 `dlerror`），不是一句笼统的"打不开"。
- `ncl_library_file_name(name)`：协议名 → 平台的文件名（`ncl_driver_focas.dll` /
  `libncl_driver_focas.so`），已经是文件名或带路径的原样返回；
  `ncl_library_name_is_file(name)` 判断这一点，三种后缀（`.dll`/`.so`/`.dylib`）
  在任何平台上都算"这是个文件"——配置里写 `focas.dll` 就是那个文件，不会被改写成
  `libfocas.dll.so`。
- 链接需要 `CMAKE_DL_LIBS`（CMake 工程已带上；`build-linux.sh` 本来就带 `-lpthread`，
  glibc 2.34 起 `dlopen` 也在 libc 里）。
- 测试 `tests/test_library.c` + 夹具模块 `tests/test_module.c`（真实动态库，覆盖
  装载、取符号、符号不存在、路径为空、库不存在、重复关闭）。

### 新增：适配器（`adapters/`）

厂商协议接入层：核心库只认 NC-Link 主题与消息，适配器负责另一侧——按机床/
PLC 自己的协议读写，并把点位映射成设备模型上的操作。构建目标是独立的静态库
`libnclink_drivers.a` 与守护进程 `ncl_adapter`（`-DNCLINK_BUILD_ADAPTERS=OFF`
可以整个不编）。

- **驱动接口**（`nclink_adapter/ncl_driver.h`）：统一地址模型
  `{area, offset, bit, length, dtype}`、统一七种数据类型、三类错误分级
  （传输 0x1xxx / 协议 0x2xxx / 业务 0x3xxx）、统一响应信封
  `{code, success, value, message, raw}`。
- **配置与分派**（`nclink_adapter/ncl_driver_manager.h`）：一条链路 = 一个驱动
  实例 + 一张点位表；点位按最长路径前缀分派，`"/"` 为兜底链路；点位默认只读
  （`writable` 显式开），可 `sample: false` 排除出采样。
- **守护进程**（`ncl_adapter`）：读配置 → 建驱动 → 建/载入模型 → 每个点位注册
  `get_value#<路径>`（可写点位再加 `set_value#<路径>`）→ 起采样与 REST；
  没有给模型文件时按点位表生成（数据项 `source` 取点位路径父级，模型路径与
  配置里的点位路径严格一致）。`--once` 可做一次读全部点位的自检。
- **协议**：`mock`（内存靶机：点位模型 + 错误注入 + 事件）、
  `modbus_tcp` / `modbus_rtu` / `modbus_rtu_tcp`（含跨平台串口层）、
  `mc_tcp`（三菱 MC/SLMP，二进制 3E/4E）、`fins_tcp`（欧姆龙 FINS/TCP，
  含节点地址分配握手）、`s7_tcp`（西门子 S7comm/ISO-TSAP，含 COTP 与 PDU
  尺寸协商）、`mtconnect`（HTTP/XML 只读接口，自带极简 HTTP 客户端与
  XML 扫描器）、`meldas`（三菱 CNC M70/M80 的 GIOP 私有操作集）。
  第二批继续：`meldas` 之后加了 `lsv2`（海德汉 iTNC530/TNC7，版本/远程状态/
  PLC 内存读，只读）。
  其余协议按 `protocal/docs/README.md` 的优先级推进。

- **`focas`（FANUC FOCAS / Fwlib32，TCP 8193，只读）**：按
  `protocal/docs/01-FANUC-CNC-FOCAS.md` §2.1–§2.3 实现。会话 = `func 1` 握手 +
  §2.3 的能力协商（`func 0x21` 两轮，`"negotiate": false` 可关掉）；每次读是
  一条 `func 0x21` 命令帧，**应答必须与请求一样多的块，且每块 `[8..10)` 返回码
  为 0**——这条是当初卡在 `-17` 的根因，现在有专门的错误码
  `NCL_FOCAS_ERR_RB_MISSING` / `NCL_FOCAS_ERR_RB_CODE` 报出来。
  点位地址：`area` 是数据项名（`STATINFO`/`ACTF`/`ACTS`/`RDCOUNT`/`RDLIFE`/
  `RDMACRO`/`RDPARAM`/`RDTOFS`/`RDPROGDIR3`/`EXEPRGNAME2`）或裸码
  （`"36"`/`"0x24"`/`"CB:0x24"`），`offset` 是**应答块号**，`bit` 是块内字节
  偏移，值一律大端（`float32` 就是 `cnc_actf` 的坐标）。`call("items")` 列
  已知项、`call("session")` 报握手结果，便于现场对点。写操作报
  `NCL_ERR_NOT_SUPPORTED`（§2.3 里只抓到读帧）。
  测试 `adapters/tests/test_focas.c`：黄金帧逐字节比对 §2.1/§2.3 的抓包
  （12 字节 hello、10 字节空帧、40 字节协商帧、96 字节 `STATINFO` 帧）＋
  假机床端到端（握手、逐块读、块数少一个要报 `RB_MISSING`）。

### 变更：批量读的临时表按批大小要内存（`modbus` / `mc` / `fins`）

这三个驱动读一批点位时，原来不管批里几个点都先 `calloc` 一张 `*_MAX_ITEMS`
（2048）项的临时表。元素 40 字节，也就是**一次三点的读要占 80 KiB 池**——
静态池版本里这是第一个被拒的大请求。现在先按"字符串地址算一项、其余每个元素
算一项"算出这批真要用多少项，再按这个数量分配；`*_MAX_ITEMS` 仍是上限，
超了照旧报错（modbus `NCL_ERR_RANGE`、fins `NCL_DRV_ERR_BUSINESS(0x41)`），
行为不变。静态池实测（Linux / gcc 13，全量 39 套）：64 KiB 池从
**35 通过 / 4 失败**变成 **38 通过 / 1 失败**，只剩 `file` 一套——它自己的
`ncl_file_read_all()` 会把 1 MiB 的文件整块读进池里比较，与驱动无关。

### 变更：池拒绝时报出"被拒的是多大的请求"

`-MemReport` / `NCL_MEM_REPORT=1` 原来只在退出时给一行汇总，"为什么被拒"要靠
汇总里的空闲快照侧面推。现在每次拒绝当场多打一行（最多 8 行）：

```
ncl_mem: refused 81920 bytes with 59248 free bytes (largest contiguous 44000)
```

上面那 80 KiB 就是这一行抓出来的：汇总里 `largest request 4280` 看着一切正常，
真正的拒绝却是一次比整个池还大的请求，而这条路径（请求大于池）本来不记进
直方图，只有当场打印才看得见。Linux 上另加 `-DNCL_MEM_TRACE=1` 还会打调用栈
（glibc `backtrace`），只用于定位，不进发行包。`build-linux.sh` 也跟着认
`NCL_MEM_REPORT=1`（原来只有 CMake 与 `build.ps1 -MemReport` 有）。

### 文档：测试套件计数与静态池边界更正

`adapters/` 进来以后全量是 **39 个测试套件**（25 个核心 + 14 个适配器；
后来换成宿主+模块的写法 → 42 套，见上面"文档：测试套件计数更正"），
README / MANUAL / RELEASE / `adapters/README` 里"25/25"与"38 个套件"的旧计数
一并更正；手册 4.9 的峰值表按重测数据重写（含 14 个适配器套件）。静态池边界
重测（Linux / gcc 13，39 套）：**32 KiB → 37/39**（`file`、`ftp` 被拒）、
**64 KiB → 38/39**（只剩 `file`）、**1.5 MiB → 39/39**。`file` 这一套的门槛是
它自己造成的：`ncl_file_read_all()` 要把 1.06 MiB 的文件整块读进池里比较，而默认
尺寸类区占池的 1/4，大块只能从通用区拿，所以池要 ≥ 4/3 × 1.06 MiB。把尺寸类区
关掉（`-MemClassBytes 0`）后实测 **1.125 MiB 池 39/39**；设备端照常用流式读写
（16 KiB 一块）就不必为这一套开大池。

### 新增：FANUC 适配器做成可装载的模块（插件化）

FANUC 现场要的是"一个进程、一条链路、一个设备"，但 3.4.0 那版是把它做成了
一个**单独的采集器**（`ncl_fanuc_collector`），点表**编译在程序里**，于是"换品牌"
就得换程序。现在反过来：**程序就是宿主 `ncl_adapter`**（它本来就是一台完整的
NC-Link 设备：MQTT + REST + 采样 + 审计），厂商适配器是启动时从目录里装载的模块，
**点表写在配置文件里**：

```sh
ncl_adapter -c conf/fanuc.json            # 一直跑，Ctrl+C 退出
ncl_adapter -c conf/fanuc.json --once     # 轮询一遍全部点位就退出（自检）
ncl_adapter -c conf/fanuc.json --plugins  # 列出装载到的模块与协议，然后退出
```

- **模块 ABI**（`nclink_adapter/ncl_module.h`）：模块只导出一个入口
  `const ncl_adapter_module_desc *ncl_adapter_module(void)`，结构里带 ABI 代次、
  协议名、版本、说明、驱动工厂与可选别名。命名约定
  `ncl_driver_<协议名>.dll`（POSIX：`libncl_driver_<协议名>.so`），配置里写
  协议名即可，装载器自己补文件名；写文件名/带路径的名字也认。
- **装载器**：扫描目录、按名字装载、去重、校验 ABI 代次与必需字段、把驱动工厂
  登记到宿主自己的注册表（模块不碰自己的注册表副本——它链的是静态核心，有
  自己的一份）、登记别名；失败时把平台自己的原因（`LoadLibrary`/`dlerror` 原文）
  一起报出来，而不是变成没头没脑的"协议未注册"。
- **宿主侧**：`-P/--plugin-dir <目录>`（默认 `<root>/plugins`）、
  `--plugin <名字|文件>`（可重复，最多 8 个）、`--plugins`（列表）；配置里
  `"plugins": {"load": ["focas"]}`（也接受数组或
  `{"dir":…, "load":[…], "auto":true}`）。装载完成后再核对配置里用到的协议，
  缺了就直接说"协议 "focas" 未注册：<目录> 里没有 ncl_driver_focas.dll"。
- **点表进配置文件**（`conf/fanuc.json`）：19 条点位照 01 册 §2.3 实证的布局写全
  （`STATINFO` 的 ODBST 拆分、`ACTF`/`ACTS` 每轴一个 float、`RDCOUNT`、
  `EXEPRGNAME2`），改点表就是改 JSON。删掉了编译在程序里的点表
  （`ncl_fanuc.h` + `src/app/fanuc.c`）与 `src/main_fanuc.c`：
  `--axes`/`--all-items`/`--no-poll`/`--host` 一并变成"配置里改"。
  `RDLIFE`/`RDPARAM`/`RDMACRO`/`RDTOFS`/`RDPROGDIR3` 的布局仍未核对，默认不写进
  点表；要试就在 `points` 里加一条并先别开采样（`"sample": false`）。
- **两个真机上会咬人的细节修掉了**：
  - 地址解析把尾部的数字串当**偏移**（`"D100"` 是区 `D` 偏移 100），于是
    `{"area":"EXEPRGNAME2"}` 到手是区 `EXEPRGNAME` + 偏移 2，`RDPROGDIR3`
    同理。驱动在项表里查不到这个名字时会把那个数字再拼回去（项表说了算），
    两种写法都能用。
  - 字符串点位按**点声明长度**取值（`cnc_exeprgname2` 的应答是 `name[36]` 加两个
    long，原来会把尾巴上的 8 字节也拼成字符串），并在第一个 `NUL` 处截断、
    去掉尾部空格（FOCAS 的字符数组就是这个形状）。
- **发布包**：`tools/make_fanuc_release.ps1` 改成打"组装好的程序"：
  `dist\nclink-fanuc-adapter-<版本>-win-x64\`（+ zip + `.sha256`）——
  `bin/ncl_adapter.exe`、`plugins/ncl_driver_focas.dll`、`conf/fanuc.json` 与
  `conf/mqtt.cfg` 样例、站端手册（`adapters/FANUC-ADAPTER.md` → 包内
  `README.md`）、`run-once.ps1` / `run.ps1` / `list-plugins.ps1`、`SHA256SUMS.txt`；
  组装后跑一道"不许带本机构建路径"的检查。加 `-WithProtocolDocs` 才把 01 册与
  `adapters/README.md` 放进 `docs/`（内部资料版）。
- **静态内存构建不要混用模块**：模块自带一份核心库拷贝，两边各有一块内存池，
  谁也释放不了对方的内存块；要用模块就用默认堆构建
  （`-DNCLINK_BUILD_PLUGINS=OFF` 把驱动放回内置注册表也是选择）。

### 文档：测试套件计数更正

适配器层把 `fanuc_collector` 一套换成 `adapter_plugin` 一套（装载模块 → 登记协议
与别名 → 用配置里的 8 个点位组装设备 → 读假 FANUC 机床的应答块 → 模型里的值，
含加载器的失败路径），核心库那边多了 `library` 一套（`ncl_library_*`：名字 →
文件名、装载夹具模块、取符号、错误文案），全量 **42 个测试套件**
（26 个核心 + 16 个适配器）。README / MANUAL / RELEASE / `adapters/README` 里旧计数
一并更正；**静态池的那几组边界仍是这一套之前的 39 套口径**（本轮只重跑了默认堆：
Windows/MSVC **42/42**、MinGW/gcc 16.2 **42/42**），手册 4.9 已注明。

`build-linux.sh` 也跟着改：目标从 `$OUT/bin/ncl_fanuc_collector` 换成插件
`$OUT/plugins/libncl_driver_focas.so`（MinGW：`ncl_driver_focas.dll`），并把测试要的
夹具模块（`ncl_test_module.*` 与两个被拒绝的模块）一起编出来。

### 变更：适配器自己管 MQTT 会话（配置里的 `"mqtt"`）

`ncl_adapter` 原来是"半个网关"：驱动、模型、采样、REST 都在，但 `ncl_server`
建起来时没有 MQTT 客户端，`ncl_server_subscribe()` 必然返回
`NCL_ERR_INVALID_ARG`，采样上报和应答都没地方发。现在适配器配置多了 `mqtt`
一段，适配器**自己建客户端、连 broker、订阅本 SN 的请求主题**、并把采样与应答
都走同一个会话：

```json
"mqtt": {
  "url": "tcp://127.0.0.1:1883",
  "username": "", "password": "",
  "clientId": "V2AABBCCDD1",        // 默认取设备 SN
  "keepAliveSeconds": 60,
  "automaticReconnect": true,
  "reconnectDelayMs": 1000, "reconnectMaxDelayMs": 30000,
  "offline": false
}
```

- 配置里没有 `mqtt`（或 `"offline": true`）＝ 离线：驱动照读、REST 照开，
  只是不上总线。库层面保持离线默认，测试与不带 broker 的部署不受影响。
- 两个主机程序都加了 broker 参数：`ncl_adapter -b <URL>` / `-b -`（`-` ＝ 离线，
  省略 ＝ 读 `<conf>/mqtt.cfg`，也就是设备端示例用的那一份）。
- **broker 没起来不是致命错误**：`ncl_adapter_broker_poll()` 在主机自己的循环里
  以 1 s 起、上限 30 s 的退避重试，连上后自动补订阅；断线交给库的自动重连
  （避免两处同时连）。这条在换实现时很重要——盒子比 broker 先上电是常态。

### 修复：FOCAS 点位可以取块内字节偏移（`area` 的 `"@<字节>"`）

`focas` 驱动一直把 `address->bit` 当"块内负载字节偏移"，但通用地址模型里有
`if (bit >= 0) dtype = BIT` 这条硬规则（`test_driver.c` 明确断言"位索引胜出"），
所以**从配置根本给不出字节偏移**：`STATINFO` 只能读负载第 0 字节，`ACTF` 只能读
第一个轴。现在点位把偏移写进 `area`：

```json
{"path": "/CNC/STATUS@ALARM", "addr": {"area": "STATINFO@12", "dtype": "int16"}}
{"path": "/CNC/AXIS@1/POSITION", "addr": {"area": "ACTF@4", "dtype": "float32"}}
```

名字里没有 `@`、或 `@` 后面不是十进制数的（别的协议那种 `AXIS@0/SCREW`）按原样
处理，地址模型与 `bit` 的语义都没动。

## 3.4.0

文件通道换成**显式握手**：字节仍然走 FTP、MQTT 只传 `/temp/<名字>` 令牌，但设备不再
从 `conf/mqtt.cfg` 猜对端。版本号推进到 **3.4.0**（`NCL_VERSION`、CMake 工程版本与
包名一致）。

### 变更（破坏性）

- **方法调用可以异步了**（GB/T 41970-2022 的 `Method/Status`、`Method/Result` 两对，
  之前只有 `Method/Call` 一对）：
  - 请求带 `async: true`（报文键就是 `async`；历史版本里拼成 `aysnc`，解析时兼容）
    → 设备把方法丢进共享线程池执行，**立刻**回 `code=OK` + `handler`（方法句柄，
    代表本次调用的那个线程/任务）；
  - 客户端拿句柄查进度：`Method/Status/Request|Response`（`id`/`@id`/`handler` +
    `process`/`status`/`code`，`status ∈ executing|waiting|stopped|sleep`）；
  - 查结果：`Method/Result/Request|Response`（`id`/`@id`/`handler` + `code`/`return`/
    `result`，`result ∈ finished|cancel|error`）。未完成时回 `code=PENDING` 且不带
    `result`；完成后第一次查询带 `return` 与 `result` 并释放句柄，之后同句柄是 NG。
  - 设备端工具仍是普通函数（异步调度、句柄、状态/结果登记都在库里）；可选
    `ncl_server_report_method_progress()` 上报 `process`/`status`。
  - 报文层：`MethodCallRequest` 增加 `async`、`MethodCallResponse` 增加 `handler`；
    新增 4 个消息类型与 `ncl_message_*handler/request_id/async/status/process/result/
    return` 一组访问器；`ncl_general.h` 补状态与结果取值常量。
  - 客户端 API：`ncl_client_method_call_async()`、`ncl_client_method_status()`、
    `ncl_client_method_result()`；设备端 `ncl_server_subscribe()` 从 6 条主题加到 8 条，
    客户端响应主题同样 6 → 8。
  - **线格式决定**：方法入参的键从 `params` 改成 **`args`**（与既有 Java 客户端一致；
    解析时兼容旧的 `params`）。
- **移除 `Edge/*` 主题**：4 个前缀常量（`NCL_TOPIC_EDGE_GET_REQUEST_PREFIX`、
  `..._GET_RESPONSE_PREFIX`、`..._REGISTER_PREFIX`、`..._MESSAGE_PREFIX`）与 4 个构造函数
  （`ncl_topic_edge_get_request/_get_response/_register/_message`）删除，
  客户端也不再订阅 `Edge/Get/Response/<sn>`。边缘接口暂不使用；需要时按规范自行拼主题。
- **删掉"按 conf/mqtt.cfg 猜对端"那条隐式路径**。3.3.0 以前设备端文件工具默认按
  "broker 的主机名 + 2323 + admin/123456"去拨 FTP——只有对端恰好跑在 broker 那台机器
  上才成立。现在对端只有两个来源：
  - 上位机用 **`file/openFileChannel`** 握手把端点交过来（推荐）；
  - 设备自己用 `ncl_server_set_file_peer()` 钉一个静态对端（要求对端自己跑 FTP 服务端、
    目录布局是 `/<sn>/...`）。
  两者都没有时，设备端文件方法答新增的 **`NoFileChannelException`
  （`NCL_ERR_NO_CHANNEL`，-14）**，不再有 `127.0.0.1` 兜底。
- **客户端管理器不再在 `init` 时顺手起本机 FTP 端点**。要传文件就先
  `ncl_client_open_file_channel()`；它按需把端点拉起来（`ncl_client_holder_start_ftp*()`
  仍是显式起端点的入口，用于换端口/根目录/账号）。

### 新增
- **文件通道握手**（`file/openFileChannel` / `file/closeFileChannel`，`file` 工具的第
  6、7 个方法）：参数 `host`/`port`/`user` 必填，`password`、`channelId`（租约名）、
  `path`（远端前缀，默认设备 SN）、`force`（顶替已有通道）可选。租约名相同的重复握手是
  幂等 no-op（应答 `reused=true`），断线重连后重试不会打断正在传的传输；租约名不同又
  没给 `force` 则拒绝（`NG`，理由里带当前租约名）。设备侧查状态：
  `ncl_server_file_channel_is_open()` / `ncl_server_file_channel_id()`。
- **客户端侧 API**：`ncl_client_open_file_channel(client, options)`（`options` 可为
  `NULL`，结构体 `ncl_file_channel_options` 覆盖 host/port/user/password/channelId/
  path/force/timeout）、`ncl_client_close_file_channel()`（幂等）、
  `ncl_client_file_channel_is_open()`、`ncl_client_file_channel_id()`。默认路径下库给
  每条通道在端点上加**一个临时账号**（随机口令，只有设备知道），关闭通道时撤销它——
  撤一条通道不影响同一端点上别的对端。
- **配置文件 `conf/ftp.txt`（可省）**：`host` / `port` / `advertisePort` / `root` /
  `userName` / `password` / `path` / `force`，键全可选，缺文件/缺键/空值都退回默认，
  文件写坏只记一条 WARNING。每次开通道与 `start_ftp*()` 都重新读，改完不用重启；
  优先级 **函数参数 > conf/ftp.txt > 推导默认**。读写接口
  `ncl_file_channel_config_read/_write/_free()`。
- **地址推导**：默认交给设备的地址是"到 broker 的本机地址"
  （`ncl_socket_local_ip_toward()`：connected UDP 套接字问路由表，不发包）；broker 在
  本机时结果是回环地址，改取本机第一个非回环 IPv4，最后才 `127.0.0.1`。跨网段、端口
  映射、走 VPN 的部署用 `options.host` 或 `conf/ftp.txt` 显式指定。
- **FTP 服务端多账号**：`ncl_ftp_account` +
  `ncl_ftp_server_add_account()` / `ncl_ftp_server_remove_account()` /
  `ncl_ftp_server_account_count()`；每个账号可带自己的根目录与写权限，撤销账号会断开
  同名会话（文件通道的临时账号就建在这上面）。
- **`ncl_client_holder_server_uri()`**：返回进程级客户端实际建连用的 broker URL
  （`init_ex` 里存下来的那份，不一定是磁盘上的 `conf/mqtt.cfg`）。
- **托管绑定**：原生垫片新增 `nclshim_client_file_channel_open` / `..._open_ex` /
  `..._close` / `..._is_open` 与便利入口 `nclshim_client_ensure_file_channel`；
  C# / Java / Python 的 `DeviceClient` 加 `OpenFileChannel` / `CloseFileChannel` /
  `FileChannelIsOpen`，并且上传/下载/列目录/建目录/删文件/带文件参数的方法调用这些
  便利方法会**按需自动握一次手**（C API 保持显式，不做隐式网络动作）。
- **托管绑定同步跟上异步方法调用**：垫片新增
  `nclshim_client_method_call_async` / `..._method_status` / `..._method_result`、
  `nclshim_server_report_method_progress`，以及三个离线驱动入口
  （`nclshim_server_invoke_method_call_async/_status/_result`，供自检不接 broker 用）。
  Python 的 `DeviceClient.method_call_async/method_status/method_result`、
  `Server.invoke_method_call_async/invoke_method_status/invoke_method_result/
  report_method_progress`，C# 的 `NclDeviceClient.MethodCallAsync/MethodStatus/
  MethodResult`、`NclServer.InvokeMethodCallAsync/InvokeMethodStatus/InvokeMethodResult/
  ReportMethodProgress`，Java 的 `DeviceClient.methodCallAsync/methodStatus/methodResult`、
  `Server.invokeMethodCallAsync/invokeMethodStatus/invokeMethodResult/reportMethodProgress`。
  Python 自检加了异步端到端用例（47 项）、C# 106 项、Java 107 项均 0 失败。
- **Go 绑定补齐方法调用面**：`bindings/go/method.go` 的 `MethodCall` /
  `MethodCallCheck` / `MethodCallAsync` / `MethodStatus` / `MethodResult`，以及
  `method_server.go` 的离线驱动 `Server.InvokeMethodCallAsync` /
  `InvokeMethodStatus` / `InvokeMethodResult` / `ReportMethodProgress`；单测
  `TestAsyncMethodCall` 覆盖"立刻拿句柄 → 查状态 → 轮询结果 → 句柄释放 → 失败方法
  NG+error → 同步调用无 handler"。Windows（mingw + cgo）与 Linux（容器）两侧
  `go test ./...` 全绿。
- **跨语言异步实测**：Go 设备端示例（`bindings/go/example/device`，新增 `slow`
  方法）连 EMQX，Python 客户端用 `bindings/python/examples/async_client.py` 异步调用
  `/plc/slow` —— 实测输出：受理 `code=OK` + handler → 状态 `executing` →
  结果 `code=OK` / `result=finished` / `return={"done":true,...}`。
- **文件传输改成流式 + 可续传**（`ncl_ftp_client_upload()` /
  `ncl_ftp_client_download()`，设备端 file 工具直接用这两个）：
  - **流式**：上传按 256 KiB 从本地文件读着发（对端没有就 `STOR`）、下载按 64 KiB 收着写盘，
    内存不随文件大小增长 —— 512 MiB 的文件只需要 256 KiB + 64 KiB 两个缓冲，
    **20 MiB 静态池构建**也能传 64 MiB 文件（旧实现是整文件读进内存，那份构建跑不了）。
    服务端同样改成流式收写（`REST` 对 `STOR` 生效 = 从该偏移覆盖写，收完按实际长度截断）。
  - **续传**：上传先问对端 `SIZE`，已有前缀就 `APPE` 续；下载按本地已有大小 `REST` 续。
    单次调用内失败重试 3 次、每次从断点继续；测试里用
    `NCL_TEST_FTP_ABORT_AFTER` 把数据连接在第 3 MiB 掐断，断言传输仍成功、校验一致、
    线上字节数 ≥ 文件大小且 < 2×（即"续传"而不是"重传"）。
  - **计数**：`ncl_ftp_client_bytes_sent/received()`、
    `ncl_ftp_server_bytes_sent/received()`、`ncl_server_file_tool_bytes_sent/received()`
    （工具级，跨重连不归零）。
- **被动模式可按通道要求**：`ncl_file_channel_options.passive` / `conf/ftp.txt` 的
  `"passive": true` → 握手里带 `passive` 参数 → 设备改用 PASV 传输。设备在 NAT /
  容器 / 防火墙后面时必需（主动模式要 FTP 服务端反向连回设备）。
  设备侧也可直接 `ncl_server_file_tool_set_passive()`。
- **流式 SHA-256**（`ncl_sha256_new/update/finish/free()`）与流式文件复制
  （`ncl_file_open_read/read_chunk/close_read`、`ncl_file_open_write/write_chunk/close_write`、
  `ncl_path_same_file()`）；`ncl_file_checksum()` / `ncl_file_copy()` 内部也改成流式，
  于是"校验一个 512 MiB 文件"不再需要 512 MiB 内存。
- **`examples/ncl_file_bench.c`**：文件通道吞吐 / 跨主机测试台（`device` / `client`
  两个角色，可指定 host/port/passive），进度与结论见 **TRANSFER_PERF.md**
  （同机 16 MiB~512 MiB 上传 128~512 MiB/s、下载 106~140 MiB/s；跨容器
  上传 786~901 MiB/s、下载 136~155 MiB/s；续传行只传一半且逐字节一致）。

### 修复
- **`ncl_client_ll()` 以前把设备回的 NG 当成功**：空目录和被拒绝都返回 `NCL_OK`，
  调用方分不出来。现在应答项 `code != OK` 记日志并返回 `NCL_ERR`。
- **设备没取到文件时上传会报成功**：设备的 file 工具在拿不到文件时按协议回
  `code=OK` + `value=false`，客户端工具只看 code，于是"传输失败"被当成"上传成功"。
  现在客户端会检查那个布尔值并报失败。

### 文档
- 手册 5.10 重写成"握手 → 传输 → 收租约"三段，并补了 `conf/ftp.txt` 一节；5.9 补 FTP
  多账号；2.4.3/2.4.4/2.4.5（Java/Python/C#）与 3.2 客户端最小程序、7 章排错、附录 D
  目录布局同步；附录 A/B 由 `tools/gen_api_index.py` 重生成（错误码表补
  `NoFileChannelException` 文案）。四个绑定的 README、垫片头注释也跟上。

### 实测（本次发布前）

| 项 | 结果 |
|----|------|
| Windows x64（MSVC 14.44.35207，Release） | **25/25**；`file` 套件 **271 项断言**（新增：握手 6 个用例、`conf/ftp.txt` 4 个用例、**64 MiB 大文件**、**对端已有前半/本地已有前半**两种续传、**数据连接第 3 MiB 被掐断**后的续传重试） |
| Windows 其他变体 | x86、静态内存、TLS、静态内存+TLS、x86 静态内存各 **25/25** |
| Linux（gcc 13.4 / Debian bookworm，容器内） | 默认堆 / TLS / 静态内存 / 静态内存+TLS 各 **25/25** |
| mingw-w64（gcc 16.2.0，UCRT + posix threads） | 非 TLS 与 TLS 各 **25/25**（含 `test_cpp`） |
| 内存检查 | ASan + LeakSanitizer（6 个套件）**0 发现**；ThreadSanitizer **0 数据竞争** |
| 托管绑定自检 | C# 106 项、Java 107 项、Python 46 项，**0 失败**；对真 broker（EMQX）C# **135 项**、Java **15 项**、Python 46 项 0 失败（含文件通道握手全流程） |
| Go 绑定 | Linux 容器与 Windows（cgo + mingw）`go test ./...` 通过；`-tags nclink_tls` 两侧通过 |
| 异步方法调用 | `test_server` 的 `test_async_method_call`：立刻回句柄（`code=OK`+`handler`）、按句柄查状态、结果先 `PENDING` 后 `finished`（带 `return` 值）、句柄取走后查不到、失败方法给出 `NG`+`error`、同步调用不带 `handler`、结束时无悬挂句柄 |

## 3.3.0

### 新增
- **发布包同时提供两种构建**：`lib/<平台>/` 仍是默认（分配走 C 运行库堆）版，新增
  `lib/<平台>-staticmem/` 是**静态内存版**（库内分配全部走 `.bss` 里的固定池，本包按默认
  20 MiB 编译，不调用 `malloc`）；示例可执行文件同样给两份（`examples/bin/*-staticmem/`）。
  另有 **静态内存 + TLS** 的组合（`lib/*-staticmem-tls/`，池仍只覆盖库自身、OpenSSL 走系统堆）。
  打包脚本 `make_release.ps1` 一并收集，并新增产出 `dist/<包名>.zip.sha256`；版本号推进到
  **3.3.0**（`NCL_VERSION`、CMake 工程版本与包名一致）。

- **并发（多线程）压测套件 `mem_mt`（`tests/test_mem_mt.c`）**：多线程共享同一个池做随机
  流量（随机尺寸、随机分配/释放/重分配），每块带图案并在释放前校验；线程之间还通过环形
  队列**传递所有权**（A 分配写图案、B 收到校验再释放——正是"收包线程分配、调用方释放"的
  形状）；主线程在它们运行期间**不停调用 `ncl_mem_check()`**。为让自检可用于并发，
  `ncl_mem_check()` 现在还会**从块本身重算**账目恒等式（`已用 + 空闲 + 块头×块数 == 池大小`）
  并与计数器比对，全部在同一把锁下完成，因此每次观测都是一致快照、非零即真缺陷。
  收尾断言：池回到一整块空闲、`in_use 0`，且"申请最大空洞大小必须成功、比它大一个对齐
  单位必须被拒"。
  实测：单轮 64 KiB 池 4 线程 × 40000 次操作 = **160000 次操作 / 22838 次跨线程交接 /
  0 违规**；10 分钟并发长跑（8 线程 × 64 KiB/512 KiB/20 MiB）= **约 5.63 亿次操作、
  1.44 亿次分配、8030 万次跨线程交接、7034 轮、0 失败**；见手册 4.9.4。
- **ThreadSanitizer 门禁**：`./tools/asan-linux.sh --tsan --docker` 专压上述并发用例
  （TSan 与容器 ASLR 冲突，脚本自动把 `vm.mmap_rnd_bits` 降到 28，需要 `--privileged`）。
  6 线程 × 20000 次操作 **0 数据竞争**；`--docker` 的 ASan 套件也已把 `test_mem_mt` 纳入。
- **长跑（soak）工具 `tools/soak-linux.sh`**：为每个池尺寸各编一份库 + 压测程序，并行跑满
  指定时长（默认 1 小时），支持 `NCL_SOAK_MT=1` 切换到并发版；逐操作校验不变量、逐轮验证
  "排空后回到一整块空闲"，任一条不满足即非零退出。首次实测（18 逻辑核，7 个尺寸并行，
  3600 s，exit=0）：**260389 轮、约 52.1 亿次操作、22.0 亿次分配、0 失败**；形状拒绝约占
  拒绝的 60%（合成流量特征），最坏最大连续空洞约为池的 1/128～1/50。表见手册 4.9.3。

- **静态池的小对象尺寸类**：实测请求分布极度偏小（**98% 以上的请求 ≤128 字节**，33～64
  字节一档约占一半），所以池在首次使用时把自己切成"尺寸类区域 + 通用区"。尺寸类为
  {16, 32, 48, 64, 96, 128, 192, 256}（**步长不用 2 的幂**：C 类只在请求 > C−32 时划算，
  插 48/96 是为了压掉 65～96 字节那段亏损窗口），类内**无块头**、靠"指针落在哪个区域"
  判定类别，分配/释放 O(1) 且**类内结构上不可能外部碎片**；类用完**自动回落通用区**，
  因此配小了只损失速度、不产生新失败模式。开关 `NCLINK_MEM_CLASS_BYTES`（`-MemClassBytes`，
  `-1`＝池的 1/4，`0`＝关闭）。
  实测收益（64 KiB 池，24 个套件，**实际占用＝载荷+块头**的峰值，开/关对照）：
  server 36160→25584（**−29.2%**）、message 24064→16256（**−32.4%**）、
  client 20032→13920（−30.5%）、rest 25872→18000（−30.4%）、model 16304→10720（−34.2%）、
  event 25504→15344（−39.8%）、schema 7456→4032（−45.9%）、json 2800→1568（−44%）、
  mem_mc 50608→40736（−19.5%）；`ftp` 只 −1.9%（几乎全是 16 KiB 大块，本就不进类）。
  池大小边界随之改善：**32 KiB 池 22/25 → 23/25**（`message` 现在通过），64 KiB 仍 25/25。
  另新增两个统计口径：`footprint_bytes` / `peak_footprint_bytes`（载荷 + 块头）与每类的
  `class_size/class_bytes/class_live/class_free`——`in_use_bytes` 只算载荷、天然偏向通用区，
  比较"省没省"要看 footprint。

- **静态内存（无堆）构建**：库内 471 处分配点全部改道 `ncl_mem_alloc()` /
  `ncl_mem_calloc()` / `ncl_mem_realloc()` / `ncl_mem_free()`（`src/core/ncl_mem.c`
  是唯一知道内存从哪来的文件；默认实现转发给 C 运行库，行为与开销不变）。
  `NCLINK_STATIC_MEM=ON` 时改由**静态数组里的一个固定池**供给：库不再调用
  `malloc`，池耗尽返回 `NULL` 并由上层翻成 `NCL_ERR_NOMEM`，绝不回退到堆。
  池默认 **20 MiB**（`NCLINK_MEM_POOL_BYTES` / `NCL_MEM_POOL_BYTES`，池在 `.bss`
  里，不占可执行文件体积），先跑通再按量到的峰值往下压。配套选项还有
  `NCLINK_MEM_SINGLE_THREAD`（去锁）、
  `NCLINK_MEM_STRICT`（外来指针释放即 abort）、`NCLINK_MEM_REPORT`（退出时打印峰值）；
  `build.ps1` 是 `-StaticMem / -MemPoolBytes / -MemReport`，`build-linux.sh` 是
  `NCL_STATIC_MEM=1 NCL_MEM_POOL_BYTES=...`。新增运行时统计
  `ncl_mem_get_stats()` / `ncl_mem_mode()` 与测试套件 `tests/test_mem.c`。
  **碎片**：池用最佳适配 + 释放时双向合并，`test_pool_fragmentation` 逐轮断言"空闲块
  数回到 1、最大连续空闲块逐字节等于轮次前"（长期块 + 6×300 次混合尺寸 churn）；
  拒绝时的空闲快照（`failure_free_bytes` / `failure_largest_free_bytes`）用来区分
  "池小了"和"空间被形状打散"。这一改有实测依据：64 KiB 池 + 首适配时 file 用例出现过
  一次真实的形状拒绝（总空闲 21152 / 最大连续 10592 / 请求 16384），换最佳适配后
  0 拒绝。实测（Windows x64 / MSVC）：**64 KiB 池全量测试 25/25 通过**，32 KiB 时
  22/25（message、ftp、file 是"总空闲都不够"的尺寸问题）；设备端常见组合的峰值在
  32 KiB 上下。细节见手册 4.9。
- **深挖碎片时修掉两个真缺陷**（都在静态池里，只有紧池 + 自检能稳定暴露）：
  ① `realloc` 原地扩张合并后没更新"后继块的前驱链接"，留下陈旧链接，之后释放会往错
  地址合并；② 切分块产生的空闲尾块没有与后面的空闲块合并，出现**两个空闲块永久并排**
  （空间是空的，却是碎的）。合并逻辑统一改成"向前合并到底"。
- 新增自检 `size_t ncl_mem_check(void)`：走一遍池，校验前驱链接、相邻空闲块、覆盖范围，
  返回问题条数（默认构建恒 0）。就是它把上面第 ② 条从"偶发崩溃"钉成了可复现的用例。
- 新增蒙特卡洛压测套件 `tests/test_mem_mc.c`（`mem_mc`）：随机尺寸跨三个数量级、随机
  分配/释放/重分配、每块带图案校验、每次操作后校验池不变量与账目恒等式、拒绝必须正当
  （把"总空闲够却失败"单独计为形状拒绝）、**同一种子跑两遍必须完全一致**（漂移检测）。
  它上线即抓到第三个缺陷：切分后若后继块是已分配的，`merge_forward` 提前返回，导致后继
  块的前驱链接陈旧（一个"任何尺寸变化都必须修后继链接"的不变量，现在由
  `pool_fix_follower()` 统一保证）。
  同序列对照（64 KiB，4 种子 × 20000 操作）：最佳适配 20 次拒绝（其中形状 7 次 / 35%），
  首适配 22 次（形状 13 次 / 59%），最坏最大连续空洞 5504 B vs 3920 B——最佳适配的收益
  第一次有了量化数字。新增开关 `NCLINK_MEM_FIRST_FIT`（`-MemFirstFit`）用于复现对照。
- `ncl_mem_stats` 追加 `meta_bytes`（块头总开销）与 `size_hist[16]`（请求尺寸直方图），
  `-MemReport` 退出时一并打印：实测 **98% 以上的请求 ≤ 128 字节**，而 `ncl_strbuf` /
  向量 / 模型 map 都是翻倍增长，单次最大块需求可达最终大小的 1.5 倍以上——这两条是
  "要不要上尺寸类"与"池该留多少余量"的直接依据。
- `ncl.hpp`、`examples/`、`tests/` 里释放库指针的地方统一改用 `ncl_free_safe()`，
  与静态池构建的所有权约定一致（此前直接用 libc `free()`）。

### 修复（Linux + ASan 复验发现）

- **MQTT 客户端重连会漏掉已结束的收包线程对象**：`ncl_mqtt_client_connect()` 在
  reader 因服务端 DISCONNECT/网络故障自行退出后再次被调用时，会直接覆盖
  `client->reader`，那个已结束但从未 join 的线程对象就漏了（24 字节；POSIX 上还会一
  并占着已结束线程的栈直到 join）。设备反复重连就是长期增长。现在重连前先 join 掉陈旧
  的 reader。由 LeakSanitizer 在 `test_mqtt_client` 抓到（MSVC ASan 无泄漏检测，
  所以此前看不见）。
- **`test_mqtt_client` 的假 broker 停机写法是 use-after-free**：`broker_stop()` 直接
  `ncl_socket_close(listener)` 去打断 `accept()`，而 close 会立即释放对象 —— 另一线程
  的 `accept()` 正在读它。库自己在 `http_server.c` 的注释里给出的正确写法是
  shutdown → join → close，`fake_nclink_server.c` 也是这么写的。已按同样形状修正。
- **`test_message` 的 `check_wire()` 泄漏解析结果**：`ncl_message_from_json()` 收的是
  `const ncl_json *`（借用），测试把 `ncl_json_parse_cstr()` 的结果直接传进去却没释放
  —— 每次调用漏一个 JSON 根对象，一次运行累计 269 个对象。已补 `ncl_json_free()`。
- 测试侧的分配器混用：`tests/` 里由 libc `malloc` 申请、却被 `ncl_free_safe()` 释放的
  缓冲（假服务器收包缓冲、TLS/broker 大载荷、ptrvec 元素）改回 libc `free()` —— 静态池
  构建下池会（按设计）拒收外来指针，于是这些缓冲全部泄漏（LeakSanitizer 报了 170 处）。
  `test_mem` 里两个"超大请求"探针只在池构建下运行（堆构建下那是 libc 的
  calloc 溢出/malloc 过大，ASan 直接 abort）。
- 新增 `tools/asan-linux.sh`（ASan + LeakSanitizer，`--docker` 可在 Windows 跑）。

- **`ncl_rest_attach()` 的上下文没人释放**（每次 attach 8 字节，`test_rest` 报 16 字节 /
  2 处）：REST 用同一个上下文注册 4 条路由，"谁负责回收"此前没有约定。新增
  `ncl_http_server_own_context(server, ctx, free_fn)`，**按上下文登记一次**（不是每个路由
  一个析构器——那个共用上下文会被释放 4 次），由 `ncl_http_server_free()` 在路由释放之后
  释放一次；同一指针重复登记返回 `NCL_ERR_EXISTS`，登记失败时调用方仍持有它。不登记
  上下文的既有写法（栈上/静态对象）行为逐位不变。`ncl_rest_attach()` 已改用它。
  `tests/test_http.c` 新增用例覆盖"恰好释放一次／重复登记被拒／参数校验"；验收标准是
  `tools/asan-linux.sh` 归零，现已达成：`asan-linux: 0 suite(s) with sanitizer findings`。

### 文档

- 手册补上"被别的程序集成"这一面：**2.5「集成到自己的工程」从三条扩到七条**——两个堆的
  释放边界、一个进程一份库（客户端 holder / 环境根 / logger / 线程池都是进程级全局）、
  安装根别靠 cwd（库里会建 `bin/sn.txt`、`conf/mqtt.cfg`、`log/out.txt`、`uploadFile/`）、
  池按量到的峰值开；顺带修掉 2.5 第 2 条里重复的 `ncl_net_ip_map_json()` 片段。
- **新增 4.9.5「集成约束」**：外来指针静默丢弃（默认）vs `NCL_MEM_STRICT` abort、
  库无法共用宿主的池（替换 `src/core/ncl_mem.c` 是唯一出路）、静态版仍走系统堆的路径
  （线程栈 / DNS / OpenSSL / CRT）、容量量法（`peak_footprint_bytes`、16 KiB 连续块、
  `failure_*` 快照、小池压 NOMEM）、一把全局锁与 `ncl_mem_check()` 的代价、进程级单例与
  退出顺序，末尾附一张"验收清单"表，可直接搬进宿主 CI。

### mingw 复验（Windows 目标的 GCC 16）

- **`build-linux.sh` 在 mingw 目标下会自动补 Windows 系统库**（`-lws2_32 -liphlpapi
  -lwinmm`）。此前按 `tools/stage-go-libs.sh` 里写的
  `CC=<mingw>/gcc AR=<mingw>/ar sh build-linux.sh build-mingw` 跑，库能出来，但示例与
  测试在链接期报一片 `undefined reference to __imp_WSAGetLastError` 之类——MSVC 靠源码
  里的 `#pragma comment(lib, ...)`，mingw 没有这个机制，于是那条"官方配方"实际是半截的。
  判据取 `$CC -dumpmachine`，不影响非 mingw 的构建。
- **三处测试缺 include 被 GCC 14+ 抓出来**（隐式声明在新版 GCC 里是 error，不再是
  warning）：`tests/fake_nclink_server.c` 少了 `<stdio.h>`（`snprintf`，连带
  `test_client` / `test_server` 编不过）、`tests/test_message.c` 少了
  `nclink/ncl_env.h`（`ncl_file_read_all`）、`tests/test_topic.c` 少了
  `nclink/ncl_common.h`（`ncl_free_safe`）。补齐后 mingw 侧 **25/25**。
- 复验结果：库 / 示例 / 测试在 **mingw-w64 gcc 16.2.0（UCRT、posix-threads）下全量
  25/25 通过**；**Go 绑定的 `go test ./...` 在 Windows 上用 cgo + mingw 也跑通**
  （此前只有 Linux gcc 那一遍的记录）。
- 发布包与文档跟上：`build-mingw/libnclink_core.a` 现在随包提供
  （`lib/windows-amd64-mingw/`）；RELEASE 里"未带 mingw 库"的两处改成实况；手册 2.2 补了
  mingw 这条已验证链路，2.4.2 的 Go 链接说明也点明系统库由脚本自动补。
- **mingw 的 TLS 变体也补齐了**：`build-mingw-tls/libnclink_core.a`（`NCL_WITH_TLS=1`，
  OpenSSL 3.6.1 来自 Strawberry Perl 的头文件与导入库）在 mingw 侧同样 **25/25**，
  Go 绑定的 `go test -tags nclink_tls ./...` 在 **Windows 上也跑通**。该变体与 Linux 的
  TLS 版口径一致：**动态依赖 OpenSSL**（链接要导入库、运行要 `libssl-3-x64*.dll` /
  `libcrypto-3-x64*.dll`），因为 MSVC 那份用的静态 OpenSSL 在 Windows 的 Go 工具链下
  用不了。包内 `lib/windows-amd64-mingw/` 现在同时放非 TLS 与 TLS 两份 `.a`，
  RELEASE 的限制条目改成"依赖 OpenSSL 3 导入库与 DLL"这条实况。

## 3.2.0

三种托管绑定补齐 HTTP/REST、设备端与文件通道，并加上 TLS 选项；顺带修掉
一批文件通道与连接路径上的库问题。

版本号从 3.1.0 起：`NCL_VERSION`、CMake 工程版本与发布包名都跟到 **3.2.0**。

### 修复

- **设备端释放时会踩到正在跑的采样任务（随机段错误）**：`ncl_server_free()` 先释放
  工具绑定表与模型、**最后**才停采样任务；采样线程的每一轮 collect 都要查绑定
  （`ncl_server_lookup()`），于是它可能读到刚释放的表 —— 命中就表现为
  `ncl_server_lookup` 里的 SIGSEGV。窗口只有微秒级，所以是"偶发"（Go 绑定的分配器
  复用得快，13/20 次复现；纯 C 基本靠运气）。现在 `ncl_server_free()` 先把采样任务
  停下并 join，再释放任何东西；`tests/test_server.c` 加了"带着在跑的通道直接 free"
  的用例守住这条顺序。

- **没编 TLS 时报的是笼统的 `NCL_ERR`**：`ssl://` 连不上时只看到 "连接失败"，看不出
  是"库没带 TLS 编"。现在客户端管理器在建连接前先判断 URL 与 `ncl_socket_tls_available()`，
  直接返回 `NCL_ERR_NOT_SUPPORTED`（-8）并记一条明确的日志。

- **主机名解析出多个地址时，第一个卡住的地址会吃光整个超时**：`ncl_socket_connect()`
  按 `getaddrinfo()` 的顺序逐个试，但每个都给完整超时。Windows 上 `localhost` 会先
 解析出 `::1`，而本机 IPv6 环回上没监听时那个 connect **不会立刻被拒**，于是白等
  一整个超时（默认 5 s）才轮到 `127.0.0.1`——表现就是"连 localhost 要 5 秒"，
  文件通道里更要命（设备侧 FTP 连接等 5 s，方法调用那头早就超时了）。现在非最后一个
  地址只试 300 ms（`NCL_SOCKET_STAGGER_MS`），最后一个地址拿剩下的全部预算；只有
  一个地址时行为不变。实测 `ncl_server_file_tool_detect("localhost", ...)` 由
  **5032 ms → 313 ms**。
- **方法名少了前导斜杠就不认**：文件头一直写着 `"/plc/getValue"` 与 `"plc/getValue"`
  都接受，但 `ncl_server_dispatch()` 只在**以 `/` 开头**时才拆 `<工具>/<方法>`，
  `"plc/getValue"` 被当成一个裸方法名 → "未找到方法"。现在两种写法都拆。
- **设备端文件工具的对端目录用了 `bin/sn.txt` 的 SN**：客户端把文件放在
  `<当前目录>/<它寻址的 SN>/` 下，而设备端却按 `bin/sn.txt` 里的 SN 去取 —— 只要
  `bin/sn.txt` 与设备端实际用的 SN 不同（显式指定 SN 的设备端就是这种情况），
  上传必然 550。现在一律用服务器自己的 SN（协议里的那个）。
- **`{"@file": ...}` 标记对象在 Windows 上废掉**：临时文件名取的是路径的 basename，
  而 `remote_basename()` 只认 `/`，Windows 本机路径（`C:\...`）整条被当成了文件名，
  复制必然失败（`fileKeys` 也不会出现在应答里）。现在两种分隔符都认。
- **工具返回的文件与文件工具的镜像目录对不上**：方法结果里的文件被复制到
  `<root>/temp/`，而文件工具的 `read`（客户端按 `/temp/<名字>` 来取）找的是
  `<root>/uploadFile/temp/`；于是"工具返回文件"只能命中"客户端本地已有一份"的
  特殊情况，真取字节就 404。现在落到 `<root>/uploadFile/temp/`，与镜像一致。
- **Java 设备端连不上 `ssl://`（连编译都过不了）**：README 与端到端用例都用
  `new Server(sn, model, broker, user, pass, sink, TlsOptions)`，但 `com.nclink.Server`
  只有 6 个参数的构造，`Native.serverCreateEx` 与 JNI 那侧的实现也缺 ——
  `javac` 直接失败。三处都补齐，与 C# / Python 的同一形状（设备端与客户端现在都能
  走 TLS）。
- **Java 绑定选不到带 TLS 的原生库**：`Native` 的加载顺序里"当前目录 →
  `bindings/java/native/bin/`"写死在 `-Djava.library.path` 前面，于是按 README 指到
  `bin-tls/` 也没用（永远先命中不带 TLS 的那份，`tlsAvailable()` 报 false）。现在
  显式给的 `-Djava.library.path` 优先于仓库里的默认位置。
- **托管绑定的客户端方法调用（`methodCall`）把应答文本当成了 JSON 句柄**：库返还的是
  **应答报文的 JSON 文本**（`char*`），C# 的 `NclDeviceClient.MethodCall` 与 Python 的
  `DeviceClient.method_call` 直接把它包成 JSON 对象 → 一读就炸
  （Python 报 `ValueError: ... is not a valid JsonType`，C# 读到野指针）。Java 那边
  是 `Json.parse(out[0])`，一直是对的。现在两边都用
  `NclJson.TakeText()` / `Json.parse(take_text(...))` 解析；离线自检覆盖不到这条路径，
  所以顺带加了"对真 broker"的可选端到端用例（见下）。
- **垫片 `nclshim_server_create()` 的"模型默认走内置模型"没实现**：文件头的注释一直
  这么写，但代码把 `model_json == NULL` 直接当"空模型"传给 `ncl_server_create()`，
  于是"不传模型"的设备端没有模型——采样通道、按路径应答都无从谈起（Java / Python
  绑定绕过了它：它们自己先把内置模型序列化出来再传）。现在垫片真的会把内置模型
  （`ncl_root_node_parse(NULL)`）序列化后传进去，C# / Java / Python 的行为一致。

### 构建与发布

- **TLS 选项贯通到三份托管绑定**（之前只有 C API 能设 CA / 双向证书 / 关校验 / SNI）：
  库新增 `ncl_client_holder_options` + `ncl_client_holder_init_ex()`，垫片新增
  `nclshim_open_ex()`、`nclshim_tls_available()`，以及设备端那条路
  `nclshim_server_create_ex()`（设备直连 `ssl://` broker）。托管侧：C#
  `Nclink.Init(uri, user, pass, NclTlsOptions)` / `Nclink.TlsAvailable`、
  Java `Nclink.init(..., TlsOptions)` / `Nclink.tlsAvailable()`、
  Python `nclink.init(..., tls=TlsOptions(...))` / `nclink.tls_available()`，
  设备端同理各带一个 TLS 参数。
- **文件通道的对端可配**（原来固定按 conf/mqtt.cfg 推"broker 主机 + 2323 + admin/123456"，
  对端不跟 broker 同机就用不了）：库新增 `ncl_server_set_file_peer()`（设备端指定对端
  FTP 的 host/port/账号）与 `ncl_client_holder_start_ftp_ex()`（本机 FTP 端点换端口 /
  根目录 / 账号）；垫片与三份绑定各包一层（C# `NclServer.SetFilePeer` +
  `Nclink.StartFileServer(port, root, user, pass)`、Java 与 Python 同名）。
- **垫片加 TLS 构建开关**：`bindings/native/build-shim.ps1 -Tls`（连 `build-tls` 的库、
  带上 OpenSSL 导入库，并把运行期要用的两个 DLL 拷到输出目录），README 里写了完整步骤。
- **C# 补上 `LoadModel` / `ClearModel`**（垫片里唯一没被 C# 包的一个函数）：
  客户端可以装上自己那份模型，路径 ↔ id 与采样补齐都靠它。

- **文件通道进了三份托管绑定**（`ncl_file` 早就在库里，垫片里新加一组
  `nclshim_*file*`）：客户端侧 `upload_file` / `download_file` / `list_files` /
  `make_directory` / `delete_file` / `method_call_file`（带文件参数的方法调用）+ 本地
  小工具（`need_compression` / `total_chunks` / `checksum` / `attribute` → `FileInfo`），
  C# 是 `NclDeviceClient.UploadLocalFile / DownloadTo / ListFiles / MethodCallFile`
  + `Nclink.StartFileServer()`，Java 与 Python 同一套形状；三个设备端示例都挂上了
  `register_file_tool()`（客户端传文件用得到）+ 容忍式 `start_ftp()`。
- **HTTP / REST 端点进了三份托管绑定**（`ncl_rest_attach` 早就在库里）：
  `start_http(port, with_config)` + 自定义路由（`method` 支持 `"*"`、`/api/` 开头是
  前缀匹配）+ `request_count` / `set_cors`；C# 是 `NclServer.StartHttp()` /
  `NclHttpEndpoint.Route()`，Java 是 `Server.startHttp()` / `HttpEndpoint.route()`，
  Python 是 `Server.start_http()` / `HttpEndpoint.route()`。端点内容全在库里：
  `GET /api/schema`（OpenAPI 3.0）、`GET /swagger-ui`、`POST /api/<工具>/<方法>`
  （等价于 methodCall，走 Result 信封），配置端点（SN / 模型 / 驱动 / mqtt.cfg /
  服务器列表）。
- **C# 绑定补齐设备端**：`NclServer`（工具注册 / 路径绑定 / 采样通道 / 事件推送 /
  离线 dispatch / 自研传输）+ `NclHttpEndpoint` + `Nclink.Parse()` / `NclMessage`
  （报文解码，`AsSample()` / `AsEvent()` 拿快照），`NclOperation` / `NclToolBinding` /
  `NclToolHandler` 与 Java / Python 一套语义；新增设备端示例
  `samples/Nclink.Demo.Device`（`broker` 写 `-` 就是离线：出站报文走自研传输打到
  控制台，REST 端点照常可用）与一键构建 `bindings/csharp/build.ps1`。
- **C# 自检工程** `tests/Nclink.SelfTest`（106 项，不需要 broker）：JSON / 模型 /
  报文解析 / 设备端（离线 dispatch、工具注册、采样通道、事件、自研传输、关闭语义）/
  HTTP（REST、配置端点、swagger-ui、自定义路由、错误路径、幂等关闭）；net472 与
  net8.0 两个目标都跑通。Java 自检补上 HTTP 与文件小工具用例（75 → 107 项），
  Python 32 → 46 项。
- **可选的真 broker 端到端用例**：`NCLINK_TEST_BROKER=tcp://host:port` 一设，Python
  （`tests/test_broker_e2e.py`）与 C#（自检里的 `Broker` 一节）就把设备端与客户端
  放进**同一个进程**、报文真的过一遍 MQTT：probe、路径绑定取值/写值、`methodCall`、
  采样上报、事件推送，以及整条**文件通道**（上传 / 列目录 / 下载 / 建目录 / 删文件 /
  带文件参数的方法调用 + 工具返回文件）。Java 也补了 `com.nclink.BrokerE2E`
  （`build.ps1` 里设了环境变量就会跑）。C# 132 项 / Java 12 项 / Python 46 项，实测
  Mosquitto 2 与 EMQX 5.8.9 各跑一遍都 0 失败（methodCall 的应答解析、文件通道的
  几个 bug 都是这么抓到的）。
- **可选的真 TLS broker 端到端用例**：再有 `NCLINK_TEST_TLS_BROKER=ssl://host:port`
  与 `NCLINK_TEST_TLS_CA=<pem>`，三份绑定就多跑一段"设备端与客户端都过 `ssl://`"的
  用例（C# 134 项 / Java 13 项 / Python 46 项，对 Mosquitto 的 8883/18832 TLS 监听
  与 EMQX 都 0 失败）：正例之外还有"不给 CA 必须被证书校验挡下"的反例；库没编 TLS 时
  明确的 `NCL_ERR_NOT_SUPPORTED` 由离线用例守着。
- **Go 绑定补齐设备端**：新增 `bindings/go/server.go`（`NewServer` / `RegisterTool` +
  `Binding` / `RegisterBuiltinTool` / `RegisterFileTool` / `Subscribe` / `InitSamples` /
  `AddSample` / `RemoveSample` / `PushEvent` / 离线 `Dispatch` 与 `Invoke*` /
  `StartHTTP` + `Route`）与 `nclink_thunks.c`（cgo 不能把 Go 函数指针交给 C，而 C API
  是**用函数指针认工具方法**的，所以每种方法一个 C 跳板；回调句柄另起一次分配，因为
  cgo 只允许把"不含 Go 指针的 Go 内存"交给 C），示例 `example/device` 既能离线跑
  （出站报文打到控制台）也能过 broker；离线自检不需要 broker。
- **Go 绑定的 `-tags nclink_tls` 之前等于没开**：那个 tag 只加了 `-lssl -lcrypto`，
  链的还是非 TLS 的 `libnclink_core.a`，`ssl://` 永远返回 `NCL_ERR_NOT_SUPPORTED`。
  现在该 tag 链 `libnclink_core_tls.a`（`tools/stage-go-libs.sh` 已经会暂存这份），
  并补了 `nclink.TLSAvailable()` 供调用方先问一句。
- **借用视图的 `Dispose` 改成空操作**（C#）：`NclJson` 的下标/成员视图、设备端的
  `NclServer.Model` 这类借用对象以前 `Dispose` 会把句柄置空、之后再用就抛
  `ObjectDisposedException`；现在与 Java / Python 一致——借用的东西不归你管，
  `Dispose` 什么都不做。
- Python 设备端示例的收尾计数：关闭后再读 `sample_upload_count` / `event_count` 会抛
  `ClosedException`，现在先读计数再关。

### 采样与报文

- `ncl_message_sample_value_at()`：**按行**取某列的值。行数 = 数据最多的那一列的
  点数（最细的那根时间轴），更粗的列在覆盖该行的段里反复取第一个点 —— 1 ms 的功率
  列与 0.25 ms 的振动列可以放在同一张表里逐行读，消费端不用自己判断谁粗谁细。
- `ncl_message_sample_point_count()` 的语义明确为"行数 = 最多那列的点数"（此前要求
  各列点数相同，现在不同也能读）。
- **完整性口径放宽到只看外层**：表头项数 == 列数、各列槽位数一致即可，内层（每槽
  装几个点）各列自便，每槽点数抖动也照常上报。整列 `[]` 仍按"本周期无数据"处理，
  由 `ncl_message_sample_normalise()` 按列换成 `null`（不再要求"换完能回到统一
  标量形状"）。
- 设备端/客户端示例按新口径消费：逐列打印编码与点数，再按行打前 8 行。

### 设备端示例（ncl_device_demo）

- **默认一直运行**：省略运行秒数（或写 0）就一直跑到 Ctrl+C / SIGTERM；给正数则跑完
  自动退出（脚本、冒烟）。Ctrl+C 走正常清理路径（停采样、停 FTP/HTTP、断开 MQTT），
  退出码 0；SIGINT/SIGTERM 的处理函数只置标志，主循环 100 ms 一跳，响应不迟。
- 默认模型的采样整理成**两个通道**：
  - 通道 0 `sample_channel0`（1 s / 1 s，机床运行状态八项）：加工计件、进给倍率、
    当前加工程序名、当前刀号、主轴转速、设备状态、加工模式、报警号；
  - 通道 1 `EdgeSersors`（`sampleInterval` 1 ms / `uploadInterval` 100 ms，十二项）：
    5 轴的功率与振动（振动每槽 4 点 = 0.25 ms）；主轴 S 上挂**两路**传感器，用数据项的
    `number` 区分。一条报文 100 个槽位（功率列 100 点、振动列 400 点）。
- **数据项支持 `number`**：一个部件挂多路同类传感器时就是"同 `type`、不同 `number`、
  不同 id"的几个数据项，路径变成 `/<父路径>/<type>@<number>`（`/AXIS@S/POWER@1`、
  `/AXIS@S/POWER@2`），工具绑定、采样表头、按路径查询都按这条完整路径走；没有
  `number` 的项就是单路，路径不带后缀。`tests/test_model.c` 增补了路径、按路径反查、
  同一个通道两路传感器各成一列，以及 `number` 的序列化字段顺序（在 `dataType` 之前）。
- 主轴转速按"轴 + 物理量"写在主轴 S 轴上：`/AXIS@S/SPEED`（`SPEED` 数据项），不再
  是设备级自成一类的 `SPINDLE_SPEED`；示例补上对应工具绑定与模拟值。
- 三个示例（设备端、C 客户端、C++ 客户端）都会打印轴上的量（`路径 含义`），
  `SPEED` 读作"转速"：`/AXIS@S/SPEED 主轴转速`。
- 模拟产件数与主循环计数改成 64 位，长时间运行（现场是"一直跑"）不会溢出。
- 日志同时写 `<root>/log/out.txt`（UTF-8，10 MB 轮转）与控制台（stderr，真控制台
  走 `WriteConsoleW`，中文在任何代码页下都对）。

### 修复

- **`ncl_mqtt_client_disconnect()` 断开前先把套接字里在途的字节读干净**。
  之前是"发完 DISCONNECT 直接 shutdown(SD_BOTH) + closesocket"：Windows 上若关闭时
  还有没读走的接收数据（例如刚到的 SUBACK），close 会走 **RST** 而不是 FIN，对端收到
  RST 时会把它**还没读**的数据一起丢掉 —— 刚发过去的 DISCONNECT 就这样消失，broker
  只看到"连接被重置"（表现为测试里 `disconnect_count` 一直是 0，约 10~25% 偶发；
  真机上就是 broker 日志里的"客户端非正常断开"）。现在断开路径先把在途字节读掉
  （最多 8 KB、每次 recv 1 ms），再 FIN 收尾，broker 能正常读到干净的 DISCONNECT。
  实测：`mqtt_client` 套件由 40 次里 10 次失败 → **40/40 通过**，全套 22/22 连跑 5 轮通过。

### 平台与定时精度

- **短等待不再被 Windows 的时钟粒度拖住**（默认 ~15.6 ms）。`ncl_cond_wait_timeout()`
  与 `ncl_sleep_millis()` 对 ≤ 100 ms 的等待改走高精度计时器：优先 Win10 1803+ 的
  `CreateWaitableTimerEx`（`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`，精度 ~0.5 ms，
  计时器按线程持有并用 `WaitForMultipleObjects` 与"被唤醒"事件一起等），老系统上
  回退到 `timeBeginPeriod(1)`（~1.7 ms）。Windows 侧条件变量随之改成"计数信号量 +
  等人数"实现，语义不变（没有等待者时信号同样丢弃）。
  本机实测：`ncl_sleep_millis(1)` 由 14.1 ms → **1.56 ms**，
  `ncl_cond_wait_timeout(_, _, 1)` 由 15.5 ms → **1.56 ms**，`ncl_sleep_millis(100)`
  不变；示例的 `EdgeSersors`（1 ms 槽位 / 100 ms 上报）从 ~1.4 s 一条变成 ~222 ms
  一条（剩下的开销是每槽 12 次完整 Query）。Windows 链接多一个 `winmm`
  （MSVC 由源码里的 `#pragma comment` 自动带上，MinGW 需 `-lwinmm`；Go 绑定的 cgo
  LDFLAGS 已加）。

### 构建与发布

- `build.ps1 -Arch x86 -BuildDir build-x86`：新增 **32 位（Win32/x86）** 构建
  （库、示例、测试都是 x86，同一个 Ninja 工程换 `vcvars32` 即可）。
- 发布包新增 `lib/windows-x86-msvc/`、`examples/bin/{windows-x64-msvc,windows-x86-msvc,
  linux-x86_64-gcc}/`：**编好的示例可执行文件**随包交付，拿到即可跑（Windows 版是
  `/MD`，需要 VC++ 2015-2022 x64/x86 运行库；Linux 版需要 glibc 2.31+）。
- 包内同时带上语言绑定源码（`bindings/go`、`bindings/csharp`，都只放源码）。

### 文档

- 手册（`MANUAL.md` / `MANUAL.docx`）跟改：两个采样通道与各自的实测输出、按行消费、
  亚毫秒采样、设备端"一直运行 + Ctrl+C"、日志与编码说明。
- `RELEASE.md` 的包内清单、ABI 表与验证状态按本次实测更新。
## 3.0.0

首个 C 版本，零第三方依赖（仅可选的 zlib），Windows（MSVC）与 Linux（gcc）
双平台编译并跑通全部测试。

以 **MIT License** 授权（见 `LICENSE`），所有源文件带
`SPDX-License-Identifier: MIT` 头。

### MQTT 修复与互操作验证

- 新增 `tests/test_broker.c` 与 `tools/interop.sh`：对真实 broker（EMQX /
  Mosquitto，Docker 一键起）验证 QoS 0/1/2、通配订阅、40 KB 报文、退订、空闲
  保活、会话被顶替与重连后的订阅恢复。首次运行即发现并修掉下面两个问题。
- **修复：会话被顶替（0x8E）后不再自动重连**。此前两端都会立刻重连，导致同一
  clientId 的两个连接互相顶替、无限循环；现在把 0x8E 如实上报给回调并停止
  重连，由应用显式决定是否抢回身份（`ncl_mqtt_client_connect()`）。
- **修复：重连后的订阅恢复不再阻塞接收线程**。此前恢复订阅调用的是同步
  `subscribe()`，而 SUBACK 只能由接收线程处理，于是每恢复一个订阅就白等一个
  超时（10 s × 订阅数），期间报文不收、保活不发（会被 broker 以 0x8D 断开）。
  现在恢复订阅改为只发不等的异步路径，并在每次 connect 后恢复（显式重连不再
  静默丢订阅）。
- **修复：对端关闭连接立刻感知**。此前把"套接字被对端关闭"与"空闲超时"当成同
  一种情况，只能等保活看门狗（最多 2 × keepAlive）才发现掉线；现在 EOF/部分
  报文都会立即结束会话，不会在死连接上滞留，也不会因半截报文而错帧。
- **修复：服务器 DISCONNECT 立即结束会话**。协议规定服务器发完 DISCONNECT 就
  关连接，客户端不再继续读，直接进入断开/重连流程（0x8E 则按上面所述不重连）。
- `tests/test_mqtt_client.c` 的假 broker 改为可接受多次连接，并新增两个回归用例：
  「断开后自动重连并恢复订阅、且恢复过程不阻塞接收线程」与「服务器 0x8E 停止
  自动重连」——不装 Docker 也能挡住这两个问题。

### TLS（可选）

- 新增可选的 TLS 传输：`ncl_socket_connect_tls()` / `ncl_socket_tls_available()`
  与 MQTT 客户端上的 `ssl://`、`tls_ca_file`、`tls_verify_peer`、
  `tls_server_name`、`tls_client_cert/key`。默认构建仍然零依赖，
  用 `-DNCLINK_WITH_TLS=ON`（CMake）或 `NCL_WITH_TLS=1 ./build-linux.sh` 打开，
  链接 `-lssl -lcrypto`。校验链与主机名默认开启（IP 与 DNS 名都支持），
  可显式关掉用于自签调试。
- 新增 `tests/test_tls.c`（第 21 个套件，未启用 TLS 时自动跳过）：内置 TLS
  服务端，覆盖握手、CONNECT/SUBSCRIBE、8 KB 报文跨记录、服务端推送、陌生 CA
  与错误主机名必须失败、`verify_peer=false` 必须成功。
- `tools/interop.sh` 增加 Mosquitto 的 TLS 监听（18832），互操作套件再对
  `ssl://` 跑一遍（本机实测 44 检查全通过）。
- Windows 也可用：`.\build.ps1 -Tls`（或 `-DNCLINK_WITH_TLS=ON -DOPENSSL_ROOT_DIR=…`）
  自动定位 OpenSSL 3 并编译，两个平台的 TLS 构建均 21/21 通过；发布包附带
  `lib/windows-x64-msvc-tls/` 与 `lib/linux-x86_64-gcc-tls/`，Windows 版运行时
  需要 OpenSSL 3 的 DLL（`libssl-3-x64.dll`、`libcrypto-3-x64.dll`）。

### 协议与基础

- JSON DOM（有序对象、空值省略、忽略未知字段、数字保留原文）、
  字符串/容器、平台抽象（线程/互斥/条件变量/时间/熵）、日志、
  运行环境与路径（`conf/`、`bin/`、`log/`）、线程池与 TTL 缓存。
- NC-Link 常量与校验、全部主题构造与 `sn` 提取、18 种消息与 4 种消息项
  （字段顺序按规范固定）、完整数据模型（节点树、路径规则、采样通道绑定、
  映射表）、hex 与可选 zlib 编解码。
- MQTT 5.0：报文编解码逐字节对齐 OASIS 规范；传输层含 QoS 0/1/2、保活、
  自动重连与订阅恢复；TCP 套接字层（Winsock/BSD 双实现）。
- 日志的控制台镜像在 Windows 上按"真控制台 / 管道"分别解码（`WriteConsoleW`
  与本地 ANSI 代码页），中文不再乱码；`NCL_CONSOLE_ENCODING=utf8` 可强制按
  UTF-8 写（`ncl_console_write()`）。日志文件始终是 UTF-8。

### 客户端与服务端

- `ncl_client`：请求/响应关联、5 分钟响应缓存、getValue/getLength/setValue
  （含索引与区间）/probe/methodCall；`ncl_client_holder`：按 SN 的客户端表、
  30 分钟空闲过期、MQTT 连接与 FTP 端点生命周期。
- `ncl_server`：工具注册与 `<operation>#<path>` 绑定、Query/Set/MethodCall/Probe/Ping
  分发、线程池异步处理、采样通道（定时采集 + 聚合上报）、事件推送、
  参数 JSON Schema 校验（`check` 语义）、无 broker 的发布钩子。
- HTTP/REST：HTTP/1.1 基础层（解析、路由、应答、CORS，含前缀兜底路由）、
  统一应答封装、OpenAPI 3.0 生成、`/api/schema`、`/swagger-ui`、
  12 个配置接口，以及 `POST /api/<工具>/<方法>` 工具入口。
- 配置：SN、模型、驱动、服务器列表、`conf/mqtt.cfg`、`ipConf.json` 的读写。

### 文件传输

- 自带 FTP 服务端与客户端（RFC 959/2389 子集：主动/被动数据连接、
  STOR/RETR/LIST/MKD/RMD/DELE/RNFR/SIZE/MDTM 等），登录根隔离。
- 文件属性与工具函数（SHA-256 校验和、分片数、压缩判定）、
  FTP 文件工具两端、`file` 工具（`/CONTROLLER/FILE`）、
  methodCall 的 `@file` 标记与 `fileKeys` 替换；设备端与客户端两个 FTP 端点。

### 校验与事件

- JSON Schema 校验器（draft-07 子集，含自带正则引擎），用于 methodCall 的
  参数校验与 `check` 快速失败。
- 事件：`ncl_server_push_event()` 发布 + `ncl_client_subscribe_events()` /
  `ncl_client_set_event_handler()` 接收（主题 `Event/<sn>`）。
- 采样：客户端订阅 `Sample/<sn>/#` 与回调；**亚毫秒采样**支持"值本身是数组"
  （外层 1 ms 槽位、内层批次），报文不加字段；发布前校验**内外层都要对齐**，
  不完整即丢弃。
- 采样补齐：设备端省掉表头 `paths`、或用 `[]` 给"本周期该项没有数据"占位时，
  客户端先用设备模型补回规范形状再交给回调（`ncl_message_sample_normalise()`，
  见 4.5）；通道查不到、项数对不上、形状统一不了就原样交过去，绝不猜对应关系。

### 文档与工程

- `MANUAL.md` / `MANUAL.docx` 使用手册（构建、核心概念、逐模块 API、
  典型任务、排错表、API 索引）、`README.md` 工程说明、`RELEASE.md` 发布包说明。
- `examples/`：设备端与客户端两个可运行示例（模型、工具与 schema、采样、
  事件、文件、参数校验全覆盖）。
- 设备端示例可以指向一个**空目录**首次启动：目录不存在就建，缺什么按出厂默认值
  补齐 —— 随机 9 位 SN（`bin/sn.txt`）、默认机床模型（`conf/model/nclink.json`）、
  本机 broker 配置（`conf/mqtt.cfg`，`tcp://127.0.0.1:1883` 匿名）；已存在的
  文件一律不动，换模型/换 broker 直接改文件即可（见手册 3.4）。
- `tests/`：19 个测试套件（含假 MQTT broker 与许可头检查），Windows ctest 与
  Linux `build-linux.sh` 均 19/19；ASan 构建全绿。
- `tools/`：markdown → docx 转换与校验、发布打包脚本。

### 未实现部分

HTTP multipart、驱动层 Modbus/串口、`Edge/*` 主题暂未提供，调用时会返回明确错误码。
（`ssl://` 在本版本已提供，见 TLS 一节：需要按 `NCLINK_WITH_TLS=ON` 构建；
没带 TLS 编译时用 `ssl://` 会返回 `NCL_ERR_NOT_SUPPORTED`。）
