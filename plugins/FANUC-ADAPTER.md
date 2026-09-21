# FANUC 数控机床适配器 · 现场手册

`ncl_server` —— 把一台 FANUC 机床接进 NC-Link 的现场程序。它**本身就是一台 NC-Link
设备**：`ncl_server` 加它周围的传输与端点；机床侧的 FOCAS 适配器不是编进程序里的，
而是启动时从 `plugins\` 装载的一个模块。**一个进程、一条链路、一个设备**。

点位（模型路径 → FOCAS 数据项）**声明在适配器模块里**（`plugins/focas.c`），
一个点位一行；配置只管机床地址、采样周期、broker 与要装载的模块。加/改一个点位＝改那一个
.c 文件并重编模块；**采样周期与上报周期在生成的模型文件里，现场可调不用编译**（第 5 节）。

```
FANUC CNC ──FOCAS(8193)──▶ ncl_driver_focas.dll（插件） ──▶ ncl_server（唯一的设备程序）
                                                              ├── 采样上报（MQTT）
                                                              ├── 请求应答（MQTT：Query / Set / Method / Ping）
                                                              └── HTTP + Swagger（/swagger-ui、/api/*）
```

版本 **3.4.0**（配合 NC-Link 协议 3.0.0；协议依据：`docs/01-FANUC-CNC-FOCAS.md`）。

---

## 1. 包内清单

```
bin/ncl_server.exe            设备程序（宿主）：一台 NC-Link 服务端 + REST + 轮询调度
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
docs/plugins-README.md        适配器完整说明（写法、审计、模块 ABI）（点表写法、审计、各协议清单、模块 ABI）
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
   程序默认从 `<root>\plugins` 装载模块：找不到模块时装载器会说清楚是哪个文件、平台报的什么原因。
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
.\bin\ncl_server.exe -c conf\fanuc.json --once

# ②b 只看一个点位（排查某个点位时最省事；走的是和上位机一样的绑定）
.\bin\ncl_server.exe -c conf\fanuc.json --probe /MACHINE/PART_COUNT

# ②c 看一眼设备发布的模型（就是下面第 5 节那张表；存下来可以现场调采样周期）
.\bin\ncl_server.exe -c conf\fanuc.json --model > conf\fanuc-model.json

# ③ 接 broker 跑起来（Ctrl+C 退出）
.\bin\ncl_server.exe -c conf\fanuc.json -b tcp://10.0.0.9:1883

# ④ 或者用包里的脚本
.\run-once.ps1                          # 自检
.\run.ps1 -Broker tcp://10.0.0.9:1883   # 常驻
.\run.ps1 -Broker tcp://10.0.0.9:1883 -Raw   # 审计里带原始报文
```

`list-plugins.ps1` 会列出 `plugins\` 里装载到的模块、协议名、别名与版本，然后退出
（等价于 `ncl_server.exe --plugins`）。

自检输出长这样（每个点位一行，同时写进 `log\out.txt`）：

```
2026-09-20 14:04:22.719 INFO [50060] /MACHINE/STATUS = running
2026-09-20 14:04:22.719 INFO [50060] /MACHINE/PART_COUNT = 1234
2026-09-20 14:04:22.719 INFO [50060] /MACHINE/CONTROLLER/PROGRAM = "O1234"
2026-09-20 14:04:22.719 WARNING [50060] /MACHINE/WARNING = <待抓包>（/MACHINE/WARNING：还读不了（UnavailableException））
2026-09-20 14:04:22.719 INFO [50060] /MACHINE/AXIS@X/POSITION@REAL = 12.345
2026-09-20 14:04:22.723 WARNING [50060] /MACHINE/AXIS@X/POSITION@CMD = <待抓包>（/MACHINE/AXIS@X/POSITION@CMD：还读不了（UnavailableException））
自检：20 个点位（13 个可读，7 个待抓包），0 个读取失败      # 退出码 0
```

`--probe` 打的是单点结果（客户端视角，带 OK/NG 与原因）：

```
> .\bin\ncl_server.exe -c conf\fanuc.json --probe /MACHINE/PART_COUNT
/MACHINE/PART_COUNT = 1234          # 退出码 0；读不到时打印 NG 与原因，退出码 1
> .\bin\ncl_server.exe -c conf\fanuc.json --probe /MACHINE/WARNING
/MACHINE/WARNING: NG —— /MACHINE/WARNING：还读不了（UnavailableException）
```

读到不出来的点位会打 `<读取失败>`；**只要有一个点位没读到，退出码就是 1**，所以
`run-once.ps1` 可以直接用来看"这台机器接好了没有"。**"待抓包"不算读取失败**：那 7 个点位
（报警、5 条目标位置、刀具列表）的 FOCAS 调用还没抓到帧（见第 7 节），client 里对应的函数
现在回 `NCL_ERR_UNAVAILABLE`（"还读不了"），自检给它们打 `<待抓包>`，**不计数**，退出码照旧是
0 —— 现场不会因为"我们还没抓包"而以为机床坏了。
**自检全过不等于点表全对**：
数值是否合理要看表（见第 5 节）。机床不在线时，自检在第一个传输层错误后停止，
不会把每个点位都各等一次超时。

### 常用参数

| 参数 | 说明 |
|---|---|
| `-r, --root <目录>` | 安装根目录：`conf/ bin/ plugins/ log/` 都在它下面（默认当前目录） |
| `-c, --config <文件>` | 设备配置，默认 `<root>\conf\device.json`（FANUC 现场用 `conf\fanuc.json`） |
| `-b, --broker <URL>` | broker；`-` ＝ 不接；省略 ＝ 读 `<root>\conf\mqtt.cfg` |
| `-P, --plugin-dir <目录>` | 模块目录，默认 `<root>\plugins` |
| `--plugin <名字\|文件>` | 额外装载一个模块（可重复）；名字 = `<目录>\ncl_driver_<名字>.*` |
| `--plugins` | 列出已装载的模块与协议，然后退出 |
| `--model` | 打印这份声明生成的**设备模型 JSON**（设备对外发布的就是它），然后退出；采样周期要现场调就存成文件、用配置里的 `"model"` 指过去 |
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

**调周期／换通道**：设备发布的模型是一份 JSON（设备 → 组件 → 数据项，外加一个采样通道
`configs[0]`，通道里的 `ids` 就是参与采样的点位、`sampleInterval`/`uploadInterval` 是周期）。
用 `--model` 把它打出来存成文件（例如 `conf\fanuc-model.json`），在配置里写一行
`"model": "conf\\fanuc-model.json"` 指过去，之后**改这个文件就行，不用重编模块**：
第 5 节里"哪些点位进默认采样通道"的现场口径就在它的 `ids` 里。

请求主题是 `<SN>` 相关的 8 条（Query/Set/Method/Ping 各自的 Request），启动日志里会
打印自己的 SN。**写操作不支持**（见第 7 节）。

---

## 5. 点位表（在模块里：`plugins/focas.c`）

点位**声明在适配器模块里**，而且大部分点位**不用写函数** —— 直接把 client 的语义函数绑到模型路径上：

```c
NCL_TOOL_BEGIN("focas", "FANUC FOCAS / Fwlib32 over TCP, read only", "MACHINE",
               1000, 1000, focas_open, focas_close)
    NCL_DATAITEM_STR_SAMPLED("/STATUS",             ncl_focas_status)
    NCL_DATAITEM_I64_SAMPLED("/PART_COUNT",         ncl_focas_part_count)
    NCL_DATAITEM_STR_SAMPLED("/CONTROLLER/PROGRAM", ncl_focas_program_name)
    NCL_DATAITEM_F64("/AXIS@X/POSITION@REAL", ncl_focas_axis_position, NCL_FOCAS_AXIS_X)
    NCL_DATAITEM_F64("/AXIS@X/SPEED",         ncl_focas_axis_speed,    NCL_FOCAS_AXIS_X)
    ...
NCL_TOOL_END_WITH_RAW(focas_last_raw)
```

- **路径不带 `/MACHINE`**：设备段在 `NCL_TOOL_BEGIN` 的第三个参数里写一次（这里是
  `"MACHINE"`）；模型里对上位机发布的仍是绝对路径 `/MACHINE/STATUS`。换机型只改那一处。
- **"哪个 item、哪一块、怎么解"的知识在 client 里**（`clients/focas/focas_values.c`），
  现场只要读 `nclink/clients/focas.h` 那一个头：`ncl_focas_status()` 读回来就是三态，
  `ncl_focas_part_count()` 读回来就是加工件数。
- 需要自己的解释（报警拼文本、位置换算单位、两个量凑一个）时才写函数（覆盖档）；
  绑定与覆盖可以混在同一张表里 —— 本文件对应的模块就是混用的：20 个点位**全是绑定**
  （其中 7 个绑的是 client 里"帧待抓包"的函数，见第 7 节），2 个方法是覆盖。
- 改点位 ＝ 改这个 `.c` 并重编模块；**采样周期与上报周期在配置和模型文件里，现场改不用重编**。

出厂点位（5 轴 + 1 主轴，名字全部来自数据字典；**完整对照表见 32 册 §5.2**。
下表 ✔ = 现在真读得到，🟡 = 请求码/来源已核、应答（或字段布局）待真机核，
现在答"还读不了"）：

| 模型路径 | 读法（client） | 含义 | 状态 / 采样 |
|---|---|---|---|
| `/MACHINE/STATUS` | `ncl_focas_status()`（ODBST 的 RUN / EMERGENCY 两位推三态） | 运行状态：`running`/`free`/`holding` | ✔ 采样 |
| `/MACHINE/WORK_MODE` | `ncl_focas_mode()`（同一个 ODBST 的 aut / manual 两位） | 工作模式：`manual`/`auto`（表 8） | ✔ 按需读 |
| `/MACHINE/PART_COUNT` | `ncl_focas_part_count()`（`RDCOUNT`） | 加工件数 | ✔ 采样 |
| `/MACHINE/LINE_NUMBER` | `ncl_focas_line_number()`（`cnc_rdseqnum`） | 程序行号（文本 `N1234`） | ✔ 按需读 |
| `/MACHINE/TOOL_NUMBER` | `ncl_focas_tool_number()`（`cnc_rdgcode` 的模态 T 码） | 当前刀具号 | 🟡 按需读 |
| `/MACHINE/FEED_SPEED`、`/FEED_OVERRIDE`、`/SPINDLE_OVERRIDE` | `ncl_focas_feed_speed/_override()`（`cnc_rddynamic2`） | 合成进给、两个倍率 | 🟡 按需读 |
| `/MACHINE/WARNING` | `ncl_focas_alarm()`（`cnc_rdalmmsg2`） | 报警（JSON `number`/`text`）—— 占着通道，**现在取值为 `null`**（见第 7 节） | 🟡 采样（null） |
| `/MACHINE/MODEL`、`/VERSION` | `ncl_focas_model()/_version()`（`cnc_rdmodel` / `cnc_sysinfo` 的握手记录） | 型号、系统版本 | 🟡 configs |
| `/MACHINE/MANUFACTURER` | `ncl_focas_manufacturer()`（常量） | 厂商：`FANUC` | ✔ configs |
| `/MACHINE/CONTROLLER/PROGRAM` | `ncl_focas_program_name()`（`EXEPRGNAME2`） | 主程序名 | ✔ 采样 |
| `/MACHINE/CONTROLLER/PROGRAM_NUMBER` | `ncl_focas_program_number()`（`cnc_rdprgnum`） | 当前程序号 | ✔ 按需读 |
| `/MACHINE/CONTROLLER/SUBPROGRAM` | `ncl_focas_subprogram_number()`（`cnc_rdexecprog3`） | 子程序号 | 🟡 按需读 |
| `/MACHINE/CONTROLLER/TOOL`（list） | `ncl_focas_tool_list()`（`cnc_rdtooldata` 一族） | 刀具列表 | 🟡 configs |
| `/MACHINE/CONTROLLER/TOOLPARAM`（JSON） | `ncl_focas_tool_param_table()`（刀补 `cnc_rdtofs` + 寿命 `cnc_rdlife`） | 刀具参数（半径/长度/使用次数…） | 🟡 configs |
| `/MACHINE/CONTROLLER/VARIABLE`（list） | `ncl_focas_variable_table()`（`cnc_rdmacror`） | 运行变量（宏变量） | 🟡 configs |
| `/MACHINE/CONTROLLER/PARAMETER`（dict） | `ncl_focas_parameter_table()`（`cnc_rdparanum` + `cnc_rdparar`） | 参数表 | 🟡 configs |
| `/MACHINE/CONTROLLER/COORDINATE`（JSON） | `ncl_focas_work_offsets()`（`cnc_rdwkcdshft` 一族） | 工件坐标系（x/y/z…，表 9） | 🟡 configs |
| `/MACHINE/AXIS@X\|Y\|Z/POSITION@REAL`、`@CMD` | `ncl_focas_axis_position()`／`_cmd()`（`cnc_absolute`／`cnc_rdposition`，item 0x26） | 线性轴位置（mm，实际/目标） | 🟡 按需读 |
| `/MACHINE/AXIS@A\|C/ANGLE@REAL` | 同一个 `ncl_focas_axis_position()`（载荷 `unit=2` 是度） | 旋转轴角度 | 🟡 按需读 |
| `/MACHINE/AXIS@k/SPEED` | `ncl_focas_axis_feedrate()`（`cnc_actf`，每轴一个 float） | **实际进给速度**（mm/min，表 4 的 SPEED） | ✔ 按需读 |
| `/MACHINE/AXIS@k/PATH_LEFT_LENGTH` | `ncl_focas_axis_distance()`（`cnc_distance`，0x26 d=3） | 剩余进给 | 🟡 按需读 |
| `/MACHINE/AXIS@k/TORQUE`、`/CURRENT`、`/TEMPERATURE` | `ncl_focas_axis_torque/_current/_temperature()`（`cnc_loadtorq` / `cnc_rdaxisdata`） | 扭矩 / 电流 / 温度 | 🟡 按需读 |
| `/MACHINE/AXIS@k/TYPE`（`linear`/`rotary`） | `ncl_focas_axis_type()`（`cnc_rdaxisname` / `cnc_rdaxisdata` 的轴属性） | 轴类型 | 🟡 configs |
| `/MACHINE/MOTOR@S1/SPEED` | `ncl_focas_spindle_speed()`（`cnc_acts`） | 主轴转速（units rpm；表 2 没有 SPINDLE，主轴按 MOTOR 归置） | ✔ 按需读 |

几点现场要知道的：

- 进采样通道的就是表里 ✔ 的四个：**设备状态、加工计件、程序名称、报警**。要上报别的
  （某个轴的位置、速度）就把那一行的 `NCL_DATAITEM(...)` 换成 `NCL_DATAITEM_SAMPLED(...)`
  重编模块；不想上报就去掉 `_SAMPLED`。通道本身开在模型里（`configs` 里的采样通道），
  周期在配置/模型文件里调。
- **机床没有的轴**：读那一条会报错（client 在应答载荷不足时返回 `NCL_ERR_RANGE` 或
  `NCL_FOCAS_ERR_LENGTH`），不会给 0 之类的假值。5 轴是出厂配置，实际轴少的机床会看到
  对应点位读失败，这是预期行为。
- 模型树是标准的 **`MACHINE → CONTROLLER / AXIS@X → 数据对象`**：相对路径里最后一段是数据
  对象，前面每一段都是组件（可以嵌套），组件与数据对象都能用 `@number` 区分
  （`AXIS@X`、`POSITION@REAL`）。
- 模型里每个数据项/组件的 **`name` 是可读名**（照数据字典的含义列）：`/MACHINE/STATUS` 叫
  "运行状态"、`/MACHINE/AXIS@X/POSITION@REAL` 是 `AXIS@X` 组件（"X 轴"）下的"位置（实际）"。
  **名字与路径无关**：路径由 `type` 与 `number` 拼出来，上位机按路径问、按名字显示。
- 表里绝大多数点位是 **`dataItems`**（可采集）；刀具列表是 **`configs`**（不常变、可查询，
  按册 3 表 1 注 b **不得作为采样数据源**）。参数 `PARAMETER`、坐标系 `COORDINATE`、
  宏变量 `VARIABLE` 这些也是 configs，但 FOCAS 侧的帧还没核对字段布局（31 册 §1 #7），
  现在没有声明；要加就照 `NCL_CONFIG*` 写一行（详见 `docs/plugins-README.md`）。
- `/MACHINE/SESSION`、`/MACHINE/ITEMS` 两个方法是**现场调试用**的（会话状态、client 的
  item 表），不进模型、不参与采样。
- `/MACHINE/PROGRAM@DOWNLOAD`、`/MACHINE/PROGRAM@UPLOAD` 两个方法是**程序上下行**
  （动作，不是数据对象 —— 标准里"文件"是 `FILE`（dict），"把一段程序下发/取回"是调用）：
  参数给 `data`（程序文本）/`name`（要取的程序名）与 `type`（0 NC 程序、1 刀补、2 参数…）。
  下行已通（`cnc_dwnstart4` 三件套，见 01 册 §2.4）；**上行现在回"还读不了"** ——
  请求码已核（0x15/0x18），差应答里程序文本的切法，真机（或 NCGuide）抓一次就能补。
- 点位名字从路径自动推（`@`→`_`、`/`→`.`），方法调用地址是 `focas/AXIS_X.POSITION_REAL`
  这样；名字在同一个 tool 里必须唯一，撞了宿主在装载时就拒绝。

---
## 6. 加一台别的品牌 / 别的协议

"这台机器会说哪种机床"由 `plugins\` 目录决定，不是编译期定的：**换一个模块就是换一种机床**。

```
plugins\
├── ncl_driver_focas.dll      ← 工具名 "focas"
├── ncl_driver_modbus.dll     ← 工具名 "modbus"
└── ...
```

三步走：

1. **协议已经在 `clients\` 里**（Modbus / MC / FINS / S7 / MTConnect / MELDAS / LSV2 /
   SYNTEC / KND）：写一个 `plugins\<名字>.c`，在 `open()` 里构造对应的 client
   （`ncl_modbus_tcp_create()` 这样），然后把点位绑上去或写覆盖档。这些协议的语义层还在
   补（FOCAS 已经有了），寄存器类现在用"覆盖档 + 地址"的写法，见内部资料版的
   `docs\plugins-README.md` 与各协议笔记。
2. **协议不在 `clients\` 里**：先照 `docs\plugins-README.md` 加一个 client（最小面只有
   `read_batch` + `create/open/close/destroy`），再写第 1 步那个模块。
3. **编好模块放进现场 `plugins\`**：`ncl_driver_<工具名>.dll`（Linux/macOS 是
   `libncl_driver_<工具名>.so`）。配置里 `plugins.load` 写工具名即可，程序自己补文件名；
   也可以直接写文件名或路径；`--plugin <名字>` 能在命令行补一个（最多 8 个），
   `-P/--plugin-dir` 换目录。

```powershell
.\bin\ncl_server.exe -c conf\modbus.json -b tcp://10.0.0.9:1883
```

模块与程序之间的装载约定只有一条：导出入口 `ncl_adapter_module()`，把**点位声明**交给
宿主（ABI 3）。两条纪律：

- **模块必须和程序用同一套头文件编译**：装载时程序会核对 ABI 代次，不一致会明确拒绝并
  给出原因；跨版本的模块请重新编译。
- **静态内存构建（`NCL_STATIC_MEM`）不要混用模块**：两边各有一块内存池，谁也释放不了
  对方的内存块。要用模块就用默认堆构建。

协议清单、各协议的地址写法与坑，见内部资料版的 `docs\plugins-README.md`。

---
## 7. 已知限制

- **只读**：01 册只抓到读的报文，写操作（MDI、启程序、写刀补）没有实现，调
  `set_value` 会明确返回"不支持"。要写就走机床自己的通道。
- **五处字段布局待真机核对**：`RDLIFE` / `RDPARAM` / `RDMACRO` / `RDTOFS` /
  `RDPROGDIR3`（要用就自己加点位，别开采样）。
- **私有位不进模型**：FANUC 的 ODBST 位域（手动、自动、编辑、移动、急停、主轴、操作者…）
  在数据字典里**没有名字**，所以模型里一个都不出现 —— 模型里每个 `type` 都要能在 32 册
  里查到。需要什么状态就用标准名表达：`STATUS` 就是 RUN/EMERGENCY 两位推出来的三态；
  要看那些原始位，用方法 `focas/ITEMS`（驱动自己的项表，不进模型）。如果现场要"手动/自动"，
  加一条表 7 的 `WORK_MODE`（取值 `manual`/`auto`，表 8），由 ODBST 的手动/自动方式位推出来。
- **"帧待抓包/待核对"的点位（10 个）**：5 条目标位置（`POSITION@CMD`）、5 条实际位置
  （`POSITION@REAL`）、`/MACHINE/WARNING`（报警）、`/MACHINE/CONTROLLER/TOOL`（刀具列表，
  配置型）。2026-09 拿 FANUC 官方 SDK 把这些调用的**请求码**都核出来了
  （`cnc_rdposition`/`cnc_absolute` = item `0x26`、`cnc_rdalmmsg2` = `0x23`、
  `cnc_rdtooldata` 一族；表见 01 册 §2.4），差的只是**应答怎么切**；
  client 里对应的函数（`ncl_focas_axis_position()`、`ncl_focas_axis_position_cmd()`、
  `ncl_focas_alarm()`、`ncl_focas_tool_list()`）现在回 `NCL_ERR_UNAVAILABLE`。
  于是：点位照样声明、照样绑函数，模型里有它、`Query` 有明确答复（`NG` + "还读不了"）、
  自检报 `<待抓包>` 而不是失败、轮询与 §6 审计都不碰它，但**取不到值**，不给假值。
  **要抓哪一帧写在 client 那个函数的注释里**（`ncl_focas_last_error()` 里也带一句），
  真机抓一次补上时**只改那个函数体** —— 适配器那张点位表一行都不用动，清单在 31 册 §1 #8。
  差别只有一条：**报警（`/MACHINE/WARNING`）按现场口径已经占着默认采样通道** ——
  通道里现在有这一列，抓包补上之前每周期都是 `null`（不是"没有报警"，是"还没抓到帧"）。
- **进给速度改口径**：`/MACHINE/AXIS@k/SPEED` 现在绑 `ncl_focas_axis_feedrate()`
  （`cnc_actf`，item 0x24）—— 官方手册里 `cnc_actf` 是**轴的实际进给速度 F**、
  `cnc_acts` 是**主轴转速 S**，早先那一轮把两者当成"位置/速度"了（`POSITION@REAL`
  实际喂的是进给速度）。现在位置一栏绑的是真正的 `cnc_absolute`（帧待抓包），
  进给速度按表 4 的 `SPEED`（mm/min）报。主轴转速（`cnc_acts`）暂时没有模型项：
  表 2 的组件类型里没有 `SPINDLE`，口径定了再加点位（client 里的
  `ncl_focas_spindle_speed()` 已经能用）。
- **坐标缩放**：位置/速度按 01 册 §2.3 实测的 **float 数组**读。Fwlib32 手册里
  `cnc_actf` 的 `ODBACT` 还有 `data + dec`（小数点位数）形态，若真机上是这种形态，
  数值会明显偏大/偏小——用 `--once --raw` 抓一次原始报文再定（`log\out.txt` 里有
  hex）。这条列在 31 册 §1 #7 一起核对。
- **不做**：PMC 梯形图、程序上传/下载（`cnc_upload4`
  等）、伺服波形、Focas2 Logger。需要的话按 01 册继续扩驱动（改 `plugins` 里的模块）。
- 采样与轮询**各读一遍机床**：轮询刷新模型里的值（13 个可读点位每轮读一遍；那 7 个"还读不了"的
  点位第一次问过之后就不再碰 —— 宿主记得住，不必每秒再问一次），
  采样通道按 `sample.intervalMs` 读通道里的 4 个点位（状态 / 计件 / 程序名 / 报警）并按
  `uploadMs` 上报。嫌报文多就把 `--interval` 调大，把某个点位从 `NCL_DATAITEM_SAMPLED`
  换成 `NCL_DATAITEM`（只按需读），或者把采样周期调大。

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
| 自检里有点位是 `<待抓包>` | 那个点位的协议调用还没实现：client 的函数回 `NCL_ERR_UNAVAILABLE`，不是机床的问题，也不计失败。要抓哪一帧看第 7 节与 `clients/focas/focas_values.c` 里那个函数的注释 |
| 想单独确认一个点位 | `--probe /MACHINE/PART_COUNT`：走客户端一样的绑定，打印值或 NG 与原因 |
| 数值明显不对（比如位置是 12345 而不是 12.345） | 见第 7 节的坐标形态说明，用 `--raw` 抓一次 |
| `MQTT 暂未连上 / broker 未就绪` | broker 没起或地址不对。**不影响读机床**：程序会 1 s→30 s 退避重试，连上自动补订阅 |
| REST 没起来（端口被占？） | 换 `--port 8081` |
| 想确认真的发上去了 | `--stats` 打审计计数；上位机订阅 `Sample` 主题；或开 `--raw` 看每次请求的 hex |
| 想清空重来 | 删掉 `<root>\log\out.txt`；想换设备身份就删 `<root>\bin\sn.txt`（谨慎） |

日志是 UTF-8，用 VS Code / Notepad++ 打开不会乱码；控制台（GBK）里中文可能显示
成乱码，属正常现象。
