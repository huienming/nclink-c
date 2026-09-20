# FANUC 数控机床适配器 · 现场手册

`ncl_adapter` —— 把一台 FANUC 机床接进 NC-Link 的现场程序。它**本身就是一台 NC-Link
设备**：`ncl_server` 加它周围的传输与端点；机床侧的 FOCAS 适配器不是编进程序里的，
而是启动时从 `plugins\` 装载的一个模块。**一个进程、一条链路、一个设备**。

点位（模型路径 → FOCAS 数据项）**声明在适配器模块里**（`adapters/plugins/focas.c`），
一个点位一行；配置只管机床地址、采样周期、broker 与要装载的模块。加/改一个点位＝改那一个
.c 文件并重编模块；**采样周期与上报周期在生成的模型文件里，现场可调不用编译**（第 5 节）。

```
FANUC CNC ──FOCAS(8193)──▶ ncl_driver_focas.dll（插件） ──▶ ncl_adapter（宿主：ncl_server）
                                                              ├── 采样上报（MQTT）
                                                              ├── 请求应答（MQTT：Query / Set / Method / Ping）
                                                              └── HTTP + Swagger（/swagger-ui、/api/*）
```

版本 **3.4.0**（配合 NC-Link 协议 3.0.0；协议依据：`docs/01-FANUC-CNC-FOCAS.md`）。

---

## 1. 包内清单

```
bin/ncl_adapter.exe            设备程序（宿主）：一台 NC-Link 服务端 + REST + 轮询调度
plugins/ncl_driver_focas.dll   FANUC 适配器模块（FOCAS over TCP）——启动时动态装载
conf/fanuc.json                设备配置：机床地址、采样周期、broker、要装载的模块
                               （点位表在模块里，见第 5 节）
conf/mqtt.cfg                  MQTT broker 配置样例
run-once.ps1                   自检：轮询一遍全部点位，打印到屏幕并写日志
run.ps1                        常驻运行（Ctrl+C 退出）
list-plugins.ps1               列出装载到的适配器模块与协议（排查"模块没装上"）
README.md                      本文件
LICENSE                        MIT
SHA256SUMS.txt                 包内每个文件的 SHA-256

docs/                          **只在内部资料版里**（打包时加 -WithProtocolDocs）：
docs/FANUC-CNC-FOCAS.md        协议依据：帧格式、握手、应答块取值规则（含假机床实测记录）
docs/adapters-README.md        驱动/适配器完整说明（点表写法、审计、各协议清单、模块 ABI）
```

运行环境：Windows x64（Windows 10/11、Server 2016 及以上）。程序只依赖
**VC++ 2015–2022 x64 运行库**（`vcruntime140.dll`、`msvcp140.dll`）；没有的话先装
"Microsoft Visual C++ Redistributable (x64)"。交换机/防火墙要放开到机床 **TCP 8193**
与到 broker 的端口（默认 1883）。

---

## 2. 装到现场

安装根目录（下文记作 `<root>`）决定 `conf/ bin/ plugins/ log/` 的位置，用 `-r/--root`
指定，默认是当前目录。推荐：

```
D:\fanuc\              <- <root>
├── bin\               SN 从这里生成（bin\sn.txt），删掉会重新生成
├── plugins\           适配器模块（ncl_driver_focas.dll）；这是"这台机器会说哪种机床"的地方
├── conf\              fanuc.json / mqtt.cfg
└── log\out.txt        运行日志（UTF-8）
```

1. 把本包解开放到 `<root>`（例如 `D:\fanuc\`）。**`plugins\` 必须跟 `bin\` 一起放**：
   程序默认从 `<root>\plugins` 装载模块，找不到就说"协议 focas 未注册"。
2. 改 `conf/fanuc.json`：
   - `tools[0].parameters.host` ＝ 机床 IP（`port` 默认 8193，另有 `timeoutMs`、
     `connectTimeoutMs`、`retries`、`negotiate`）；
   - `sample.intervalMs` / `sample.uploadMs` ＝ 采样与上报周期；
   - `plugins.load` ＝ 要装载的模块名（默认 `["focas"]`）。
3. 改 `conf/mqtt.cfg` 里的 `url` 为 broker 地址；或者用命令行的 `-b`（优先级更高）。
4. 设备 SN：`bin\sn.txt` 不存在时自动生成一个（`V2` + 9 位十六进制）。**同一台机床
   要固定用同一个 SN**，删掉 sn.txt 会变成一台"新设备"。也可以在配置里写一句
   `"sn": "V2XXXXXXXXX"`（顶层）直接指定。

---

## 3. 三步跑起来

```powershell
cd D:\fanuc

# ① 看模块装载结果（"这台机器会说 focas"）
.\list-plugins.ps1

# ② 自检：轮询一遍全部点位（不接 broker，读完就退出）
.\bin\ncl_adapter.exe -c conf\fanuc.json --once

# ②b 只看一个点位（排查某个点位时最省事；走的是和上位机一样的绑定）
.\bin\ncl_adapter.exe -c conf\fanuc.json --probe /MACHINE/PART_COUNT

# ③ 接 broker 跑起来（Ctrl+C 退出）
.\bin\ncl_adapter.exe -c conf\fanuc.json -b tcp://10.0.0.9:1883

# ④ 或者用包里的脚本
.\run-once.ps1                          # 自检
.\run.ps1 -Broker tcp://10.0.0.9:1883   # 常驻
.\run.ps1 -Broker tcp://10.0.0.9:1883 -Raw   # 审计里带原始报文
```

`list-plugins.ps1` 会列出 `plugins\` 里装载到的模块、协议名、别名与版本，然后退出
（等价于 `ncl_adapter.exe --plugins`）。

自检输出长这样（每个点位一行，同时写进 `log\out.txt`）：

```
2026-09-20 12:57:57.517 INFO [46812] /MACHINE/STATUS@MANUAL = 0
2026-09-20 12:57:57.518 INFO [46812] /MACHINE/STATUS@RUN = 1
2026-09-20 12:57:57.532 INFO [46812] /MACHINE/PART_COUNT = 1234
2026-09-20 12:57:57.532 INFO [46812] /MACHINE/PROGRAM@NAME = "O1234"
2026-09-20 12:57:57.535 INFO [46812] /MACHINE/AXIS@0/POSITION = 12.345
自检：19 个点位，0 个读取失败
```

`--probe` 打的是单点结果（客户端视角，带 OK/NG 与原因）：

```
> .\bin\ncl_adapter.exe -c conf\fanuc.json --probe /MACHINE/PART_COUNT
/MACHINE/PART_COUNT = 1234          # 退出码 0；读不到时打印 NG 与原因，退出码 1
```

读到不出来的点位会打 `<读取失败>`；**只要有一个点位没读到，退出码就是 1**，所以
`run-once.ps1` 可以直接用来看"这台机器接好了没有"。**自检全过不等于点表全对**：
数值是否合理要看表（见第 5 节）。机床不在线时，自检在第一个传输层错误后停止，
不会把每个点位都各等一次超时。

### 常用参数

| 参数 | 说明 |
|---|---|
| `-r, --root <目录>` | 安装根目录：`conf/ bin/ plugins/ log/` 都在它下面（默认当前目录） |
| `-c, --config <文件>` | 设备配置，默认 `<root>\conf\adapter.json`（FANUC 现场用 `conf\fanuc.json`） |
| `-b, --broker <URL>` | broker；`-` ＝ 不接；省略 ＝ 读 `<root>\conf\mqtt.cfg` |
| `-P, --plugin-dir <目录>` | 模块目录，默认 `<root>\plugins` |
| `--plugin <名字\|文件>` | 额外装载一个模块（可重复）；名字 = `<目录>\ncl_driver_<名字>.*` |
| `--plugins` | 列出已装载的模块与协议，然后退出 |
| `--port <端口>` | REST 端口，默认 8080，`0` ＝ 随机 |
| `--interval <毫秒>` | 轮询周期，默认 1000 |
| `--once` | 轮询一遍并打印，然后退出（自检；有读不到的点位时退出码为 1） |
| `--probe <路径>` | 只读一个点位并打印（走和客户端一样的绑定），然后退出 |
| `--stats` | 跑完 `--once` 再打印审计计数（§6），然后退出 |
| `--raw` | 审计里带上每次请求的原始报文 hex（§6） |
| `--offline` | 不连 MQTT，只跑 REST 与轮询 |
| `--operator <名字>` | 写审计里的操作者（默认不写） |

---

## 4. 数据怎么被读走

**周期采样（推荐）**：采样通道每 `sample.intervalMs` 毫秒读一遍参与采样的点位，按
`sample.uploadMs` 聚合成 NC-Link `Sample` 报文发到 broker。上位机订阅上报主题即可。

**按需读**：上位机对某个点位发 `Query`，设备侧实时读机床再回。每个点位都注册了
`get_value#<模型路径>`；`/MACHINE/SESSION`、`/MACHINE/ITEMS` 两个方法可以问会话状态与点表。

**本地看**：浏览器打开 `http://<运行机器的IP>:8080/swagger-ui`，能直接调
`get_value`、看模型当前值；`/api/schema` 是 OpenAPI 3.0 文档。

请求主题是 `<SN>` 相关的 8 条（Query/Set/Method/Ping 各自的 Request），启动日志里会
打印自己的 SN。**写操作不支持**（见第 7 节）。

---

## 5. 点位表（在模块里：`adapters/plugins/focas.c`）

点位**声明在适配器模块里**：一个点位 ＝ 一条 FOCAS 地址 + 一行声明。模型路径、采样与否
都在那一行上写着，**改点位＝改这个 .c 文件并重编模块**；采样周期与上报周期在配置和模型
文件里，现场改不用重编。

```c
static const focas_point k_status_run = {"STATINFO@2", 0, NCL_DTYPE_INT16, 1};
static const focas_point k_part_count = {"RDCOUNT", 0, NCL_DTYPE_INT32, 1};

NCL_POINT_SAMPLED_ARG("/MACHINE/STATUS@RUN", focas_dispatch, &k_status_run)
NCL_POINT_SAMPLED_ARG("/MACHINE/PART_COUNT", focas_dispatch, &k_part_count)
```

`focas_point` 四个字段就是原来的 `addr`：`area`（FOCAS 数据项）、`offset`（应答块序号）、
`dtype`、`length`（元素个数）。三种声明宏：

| 宏 | 含义 |
|---|---|
| `NCL_POINT_SAMPLED_ARG(路径, 函数, 地址)` | 可读，**并进采样通道** |
| `NCL_POINT_ARG(路径, 函数, 地址)` | 只按需读（Query），不参与周期采样 |
| `NCL_METHOD_NAMED(路径, 函数, 参数, 名字)` | 方法调用（不进模型取值） |

`area` 的写法是 FOCAS 特有的（照抄即可）：

- `"STATINFO@12"`、`"ACTF@4"`：`@` 后面是**块内字节偏移**，用来取多值载荷里的某一个
  （`cnc_statinfo` 的 9 个状态量、`cnc_actf` 的每个轴一个 float）。
- 不带 `@` 的整个数据项（`"RDCOUNT"`、`"EXEPRGNAME2"`）就是"取这个块的全部载荷"。
- `offset` 是**应答块序号**（`STATINFO` 一次请求三块：块 1 保留、块 2 自动方式、
  块 0 是那九个状态量，所以取 `@AUTO`/`@DUMMY` 时用 `offset: 1` / `offset: 2`；
  一般的单块数据项用 `offset: 0`）。
- **项名以数字结尾时把它和 `offset` 一起写全**，例如 `"RDPROGDIR3"` + `"offset": 0`；
  只写 `{"area": "EXEPRGNAME2"}` 也能用（驱动会把被拆开的数字再拼回去），但显式写
  `offset` 最不容易误读。

出厂 19 个点位（3 轴）：

| 模型路径 | FOCAS 数据项 | 含义 | 采样 |
|---|---|---|---|
| `/MACHINE/STATUS@MANUAL` `@EDIT` | `STATINFO@0` `@4` | 手动 / 编辑方式 | ✔ |
| `/MACHINE/STATUS@RUN` `@MOTION` `@MSTB` | `STATINFO@2` `@6` `@8` | 运行 / 轴运动 / 移动中 | ✔ |
| `/MACHINE/STATUS@EMERGENCY` | `STATINFO@10` | 急停 | ✔ |
| `/MACHINE/STATUS@ALARM` | `STATINFO@12` | 报警中（0 = 无报警） | ✔ |
| `/MACHINE/STATUS@SPINDLE` `@OPERATOR` | `STATINFO@14` `@16` | 主轴 / 操作相关状态 | ✔ |
| `/MACHINE/STATUS@DUMMY` `@AUTO` | `STATINFO` + `offset` 1 / 2 | 保留字段 / 自动方式 | ✔ |
| `/MACHINE/PART_COUNT` | `RDCOUNT` | 加工计数（int32） | ✔ |
| `/MACHINE/PROGRAM@NAME` | `EXEPRGNAME2` | 当前加工程序名（36 字节字符串） | ✔ |
| `/MACHINE/AXIS@k/POSITION` | `ACTF@<4k>` | 第 k 轴位置（k=0 即第一轴，一般是 X） | ✔ |
| `/MACHINE/AXIS@k/SPEED` | `ACTS@<4k>` | 第 k 轴速度 | ✔ |
| `RDLIFE` `RDPARAM` `RDMACRO` `RDTOFS` `RDPROGDIR3` | 同名项 | 刀具寿命 / 参数 / 宏变量 / 刀补 / 程序目录（**字段布局待真机核对**） | ✘ |

- 最后一行**默认不写进点表**：按需读一个没核对过的字段可以，每秒往总线上报一个没人
  核对过的名字不行。要试就照着加一条 `focas_point` + 一行 `NCL_POINT_ARG`（只按需读，
  别用 `NCL_POINT_SAMPLED_ARG`）。
- 3 轴写 6 条轴点位（位置 + 速度各 3 条）；要加轴就照 `k_axisN_position` / `k_axisN_speed`
  往下加。**轴点位的路径尾段会重名**（`POSITION`×3、`SPEED`×3），所以它们用
  `NCL_POINT_SAMPLED_NAMED` 显式给名字（`"AXIS0.POSITION"`…），方法调用地址就是
  `focas/AXIS0.POSITION`；名字在同一个 tool 里必须唯一，重复会被宿主在装载时拒绝。
- `/MACHINE/SESSION`、`/MACHINE/ITEMS` 两个方法由 `NCL_METHOD_NAMED` 声明（会话状态、数据项
  清单），它们只作为方法调用，不进取值模型、也不参与采样。

---

## 6. 加一台别的品牌 / 别的协议

"这台机器会说哪种机床"由 `plugins\` 目录决定，不是编译期定的：

```
plugins\
├── ncl_driver_focas.dll      ← 协议名 "focas"，别名 "fanuc"
├── ncl_driver_modbus.dll     ← 协议名 "modbus_tcp"
└── ...
```

- 命名约定：`ncl_driver_<协议名>.dll`（Linux/macOS 是 `libncl_driver_<协议名>.so`）。
  配置里写协议名即可，程序自己补文件名；也可以直接写文件名或路径。
- 配置里 `plugins.load` 是数组（要装载哪些模块），也可以写成
  `"plugins": { "dir": "plugins", "load": ["focas"], "auto": true }`：`auto` 表示
  "先扫描整个目录"。
- `--plugin <名字>` 可以在命令行补一个模块（最多 8 个），`-P/--plugin-dir` 换目录。
- 换别的品牌/协议 ＝ 换一个适配器模块：配置里 `tools[0].name` 换成模块声明的工具名，
  `parameters` 换成那台设备的连接参数。多数品牌要**新写一个模块**（一个 .c：连接 +
  一个 dispatch + 点位声明 + `NCL_TOOL_MODULE`），写法见内部资料版的
  `docs/adapters-README.md`。

```powershell
.\bin\ncl_adapter.exe -c conf\modbus.json -b tcp://10.0.0.9:1883
```

模块是一个独立的动态库，与程序之间有固定的装载约定：导出 `ncl_adapter_module()`，
把**点位声明**交给宿主（ABI 2，现代适配器），或者把**驱动工厂**交给宿主（ABI 1，
老式驱动模块，宿主同样支持——配置里那套 `drivers[]` + `points[]` 就是给它用的）。两条纪律：

- **模块必须和程序用同一套头文件编译**：模块自带一份核心库的拷贝，装载时程序会核对
  ABI 代次（不一致会明确拒绝并给出原因），但跨版本的模块请重新编译。
- **静态内存构建（`NCL_STATIC_MEM`）不要混用模块**：两边各有一块内存池，谁也释放不了
  对方的内存块。要用模块就用默认堆构建。

协议清单与各协议的配置样例见内部资料版的 `docs\adapters-README.md`。

---

## 7. 已知限制

- **只读**：01 册只抓到读的报文，写操作（MDI、启程序、写刀补）没有实现，调
  `set_value` 会明确返回"不支持"。要写就走机床自己的通道。
- **五处字段布局待真机核对**：`RDLIFE` / `RDPARAM` / `RDMACRO` / `RDTOFS` /
  `RDPROGDIR3`（要用就自己加点位，别开采样）。
- **坐标缩放**：位置/速度按 01 册 §2.3 实测的 **float 数组**读。Fwlib32 手册里
  `cnc_actf` 的 `ODBACT` 还有 `data + dec`（小数点位数）形态，若真机上是这种形态，
  数值会明显偏大/偏小——用 `--once --raw` 抓一次原始报文再定（`log\out.txt` 里有
  hex）。这条列在 31 册 §1 #7 一起核对。
- **不做**：PMC 梯形图、报警文本（`cnc_rdalmmsg2`）、程序上传/下载（`cnc_upload4`
  等）、伺服波形、Focas2 Logger。需要的话按 01 册继续扩驱动（改 `plugins` 里的模块）。
- 采样与轮询**各读一遍机床**（模型值由轮询刷新，采样通道负责上报）：3 轴一轮约
  19 次交换。嫌报文多就把 `--interval` 调大。

---

## 8. 排错

| 现象 | 原因 / 处理 |
|---|---|
| 启动就报 `工具 "focas" 未装载：<目录> 里没有 ncl_driver_focas.dll` | `plugins\` 没跟 `bin\` 一起放，或缺了模块文件；用 `--plugins` 看装载结果 |
| 启动就报 `协议 "xxx" 未注册：…` | 老式写法（配置里 `drivers[]` + `points[]`）才需要注册协议；先看第 6 节，或 `--plugins` 看装载结果 |
| `模块 ... 的 ABI 是 N，本宿主只认 M` | 模块与程序不是同一次构建的产物，换配套的模块 |
| 日志 `cnc_allclibhndl3 ... -16`（连接超时） | 机床没开以太网功能、IP/端口不对、被防火墙挡；先用 `ping` 与 `telnet <ip> 8193` 确认 |
| 日志 `-17`（协议/握手类） | 协商没通过。FOCAS 需机床侧授权"以太网功能"；先试 `--raw` 抓帧，把 `log\out.txt` 给开发 |
| 单个点位读失败但其它正常 | 该点位的 `area`/`dtype`/`offset` 不对：第 5 节的表里对一下，或先用 `--probe <路径>` 单独试这一个点位 |
| 想单独确认一个点位 | `--probe /MACHINE/PART_COUNT`：走客户端一样的绑定，打印值或 NG 与原因 |
| 数值明显不对（比如位置是 12345 而不是 12.345） | 见第 7 节的坐标形态说明，用 `--raw` 抓一次 |
| `MQTT 暂未连上 / broker 未就绪` | broker 没起或地址不对。**不影响读机床**：程序会 1 s→30 s 退避重试，连上自动补订阅 |
| REST 没起来（端口被占？） | 换 `--port 8081` |
| 想确认真的发上去了 | `--stats` 打审计计数；上位机订阅 `Sample` 主题；或开 `--raw` 看每次请求的 hex |
| 想清空重来 | 删掉 `<root>\log\out.txt`；想换设备身份就删 `<root>\bin\sn.txt`（谨慎） |

日志是 UTF-8，用 VS Code / Notepad++ 打开不会乱码；控制台（GBK）里中文可能显示
成乱码，属正常现象。
