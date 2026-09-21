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

自检输出长这样（每个点位一行，同时写进 `log\out.txt`；下面这段是拿**一台 0i-MD（3 轴）**
真机跑出来的，原样贴的）：

```
2026-09-21 21:00:35.092 INFO    [23612] /MACHINE/STATUS = "free"
2026-09-21 21:00:35.097 INFO    [23612] /MACHINE/WORK_MODE = "manual"
2026-09-21 21:00:35.102 INFO    [23612] /MACHINE/AXIS@X/POSITION@REAL = 0.0
2026-09-21 21:00:35.102 INFO    [23612] /MACHINE/AXIS@Z/POSITION@REAL = 100.0
2026-09-21 21:00:35.149 INFO    [23612] /MACHINE/MOTOR@S1/SPEED = 2201.0
2026-09-21 21:00:35.088 INFO    [23612] /MACHINE/WARNING = []                # 没报警就是空数组
2026-09-21 21:00:35.102 WARNING [23612] /MACHINE/AXIS@A/POSITION@REAL = <读取失败>（…NotFoundException）
2026-09-21 21:00:35.149 WARNING [23612] /MACHINE/CONTROLLER/TOOL = <待抓包>（…UnavailableException）
自检：43 个点位（32 个可读，11 个待抓包），11 个读取失败
```

`--probe` 打的是单点结果（客户端视角，带 OK/NG 与原因）：

```
> .\bin\ncl_server.exe -c conf\fanuc.json --probe /MACHINE/PART_COUNT
/MACHINE/PART_COUNT = 1234          # 退出码 0；读不到时打印 NG 与原因，退出码 1
> .\bin\ncl_server.exe -c conf\fanuc.json --probe /MACHINE/WARNING
/MACHINE/WARNING: NG —— /MACHINE/WARNING：还读不了（UnavailableException）
```

读到不出来的点位有两种，自检分得很清楚：

- **`<待抓包>`**：这一份 client 还没有实现那条调用，函数回 `NCL_ERR_UNAVAILABLE`
  （"还读不了"）—— **不计数**，退出码不受它影响。现场不会因为"我们还没实现"而以为机床坏了。
- **`<读取失败>`**：调用发出去了，机床说"没有 / 不支持 / 读不到"，或者这台机器没有那根轴。
  这个**计数**，退出码变成 1。

**"读取失败"里有一部分是机床本身的性质，不是故障**（上面那段样例就是）：点位表按 **5 轴**
出厂配置声明，3 轴机床上的 `AXIS@A` / `AXIS@C` 几条会明确回 `NotFoundException`
（"这台机床没有这根轴"）—— 现场把点位表里那几行删掉就不再出现（第 5 节）。`PART_COUNT`、
`CONTROLLER/COORDINATE` 这些同理，看机床给不给（第 7 节那张表）。

退出码：只要有**一个读取失败**就是 1；全是"可读 + 待抓包"就是 0。**机床不在线时**，
自检在第一个传输层错误后停止，不会把每个点位都各等一次超时。
**自检全过不等于点表全对**：数值是否合理要看第 5 节的表。

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

> **文件/程序不在点位表里**：文件的链路是 **client → adapter → 机床**，前两段由
> 文件工具（`/CONTROLLER/FILE` 的对象操作 + `call` 里带通道参数的传输）做；**最后
> 一段（adapter → 机床）**由这份模块注册 `ncl_file_backend`，内部调 FOCAS 的程序上下行
> （`cnc_dwnstart4` 三件套；上行与目录/删除的应答还待核）。所以本站点把文件交给文件
> 工具那套流程即可，不用再学一套 FOCAS 的文件方法。

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
  绑定与覆盖可以混在同一张表里 —— 本文件对应的模块就是混用的：**43 个点位全是绑定**
  （其中 11 个绑的是 client 里"还读不了"的函数，见第 7 节 7.2），2 个方法是覆盖。
- 改点位 ＝ 改这个 `.c` 并重编模块；**采样周期与上报周期在配置和模型文件里，现场改不用重编**。

出厂点位（5 轴 + 1 主轴，名字全部来自数据字典；**完整对照表见 32 册 §5.2**）。
下表 **✔ = 现在真读得到**（真机验过值）、**🟡 = 这一份 client 还没实现**（答"还读不了"，不计数）、
**⛔ = 机床不提供**（调用被机床拒，或这台机器没有那根轴 —— 属于现场性质，不是故障）：

| 模型路径 | 读法（client） | 含义 | 状态 / 采样 |
|---|---|---|---|
| `/MACHINE/STATUS` | `ncl_focas_status()`（ODBST 的 RUN / EMERGENCY 两位推三态） | 运行状态：`running`/`free`/`holding` | ✔ 采样 |
| `/MACHINE/WORK_MODE` | `ncl_focas_mode()`（同一个 ODBST 的 aut / manual 两位） | 工作模式：`manual`/`auto`（表 8） | ✔ 按需读 |
| `/MACHINE/PART_COUNT` | `ncl_focas_part_count()`（`RDCOUNT`） | 加工件数 | ⛔ 采样（机床 rc=6） |
| `/MACHINE/LINE_NUMBER` | `ncl_focas_line_number()`（`cnc_rdseqnum`） | 程序行号（文本 `N1234`） | ✔ 按需读 |
| `/MACHINE/TOOL_NUMBER` | `ncl_focas_tool_number()` | 当前刀具号 | 🟡 按需读（还没有可靠来源） |
| `/MACHINE/FEED_SPEED` | `ncl_focas_feed_speed()`（每轴 `cnc_actf` 取最大） | 合成进给速度 | ✔ 按需读 |
| `/MACHINE/FEED_OVERRIDE` | `ncl_focas_feed_override()`（面板信号 `0x5d` @0xa） | 进给倍率 | ✔ 按需读 |
| `/MACHINE/SPINDLE_OVERRIDE` | `ncl_focas_spindle_override()` | 主轴倍率 | 🟡 按需读（现代系列没有那一格） |
| `/MACHINE/WARNING` | `ncl_focas_alarm()`（`cnc_rdalmmsg2`，`0x23`） | 报警（JSON 数组：`number`/`type`/`text`）；**没报警是空数组** | ✔ 采样 |
| `/MACHINE/MODEL`、`/VERSION` | `ncl_focas_model()/_version()`（会话探针的 ODBSYS） | 型号（如 `0M D4G3`）、系统版本（`28.0`） | ✔ configs |
| `/MACHINE/MANUFACTURER` | `ncl_focas_manufacturer()`（常量） | 厂商：`FANUC` | ✔ configs |
| `/MACHINE/CONTROLLER/PROGRAM` | `ncl_focas_program_name()`（`EXEPRGNAME2`） | 主程序名 | ✔ 采样 |
| `/MACHINE/CONTROLLER/PROGRAM_NUMBER` | `ncl_focas_program_number()`（`cnc_rdprgnum`） | 当前程序号 | ✔ 按需读 |
| `/MACHINE/CONTROLLER/SUBPROGRAM` | `ncl_focas_subprogram_number()` | 子程序号 | 🟡 按需读（这套 SDK 没有 `cnc_rdexecprog3`） |
| `/MACHINE/CONTROLLER/TOOL`（list） | `ncl_focas_tool_list()` | 刀具列表 | ⛔ configs（机床 rc=1/3） |
| `/MACHINE/CONTROLLER/TOOLPARAM`（JSON） | `ncl_focas_tool_param_table()`（`cnc_rdtofs` + `cnc_rdlife`） | 刀具参数 | 🟡 configs（**单条刀补已通**：`ncl_focas_tool_offset()`，寿命机床 rc=6） |
| `/MACHINE/CONTROLLER/VARIABLE`（list） | `ncl_focas_variable_table()`（`cnc_rdmacror`） | 运行变量（宏变量） | 🟡 configs（**单条已通**：`ncl_focas_macro_variable()`；整表机床 rc=2） |
| `/MACHINE/CONTROLLER/PARAMETER`（dict） | `ncl_focas_parameter_table()`（`cnc_rdparanum` + `cnc_rdparar`） | 参数表 | 🟡 configs（**单条已通**：`ncl_focas_parameter()`；整表机床拒） |
| `/MACHINE/CONTROLLER/COORDINATE`（JSON） | `ncl_focas_work_offsets()`（`cnc_rdwkcdshft`） | 工件坐标系（x/y/z…，表 9） | ⛔ configs（type 0..20 全被拒） |
| `/MACHINE/AXIS@X\|Y\|Z/POSITION@REAL`、`@CMD` | `ncl_focas_axis_position()` / `_cmd()`（`cnc_rdposition`，8 字节记录） | 线性轴位置（mm，实际/目标） | ✔ 按需读 |
| `/MACHINE/AXIS@A\|C/ANGLE@REAL` | 同一个 `ncl_focas_axis_position()` | 旋转轴角度 | ⛔ 这台机器没有 A/C 轴（回 `NotFoundException`；3 轴机上删掉这几行） |
| `/MACHINE/AXIS@k/SPEED` | `ncl_focas_axis_feedrate()`（`cnc_actf`，每轴 8 字节记录） | **实际进给速度**（mm/min，表 4 的 SPEED） | ✔ 按需读（这台机器 `0x24` 只回一根轴） |
| `/MACHINE/AXIS@k/PATH_LEFT_LENGTH` | `ncl_focas_axis_distance()`（`cnc_rdposition` 的剩余那一块） | 剩余进给 | ✔ 按需读 |
| `/MACHINE/AXIS@k/TORQUE`、`/CURRENT`、`/TEMPERATURE` | `ncl_focas_axis_torque/_current/_temperature()` | 扭矩 / 电流 / 温度 | 🟡 按需读（机床那两条不是被拒就是桩） |
| `/MACHINE/AXIS@k/TYPE`（`linear`/`rotary`） | `ncl_focas_axis_type()`（`cnc_rdaxisname` 的轴名 + 命名约定） | 轴类型 | ✔ configs（真机 `X` → `linear`） |
| `/MACHINE/MOTOR@S1/SPEED` | `ncl_focas_spindle_speed()`（`cnc_acts`） | 主轴转速（units rpm；表 2 没有 SPINDLE，主轴按 MOTOR 归置） | ✔ 按需读 |

几点现场要知道的：

- 进采样通道的就是表里 ✔ 的四个：**设备状态、加工计件、程序名称、报警**。要上报别的
  （某个轴的位置、速度）就把那一行的 `NCL_DATAITEM(...)` 换成 `NCL_DATAITEM_SAMPLED(...)`
  重编模块；不想上报就去掉 `_SAMPLED`。通道本身开在模型里（`configs` 里的采样通道），
  周期在配置/模型文件里调。
- **机床没有的轴**：读那一条会明确回 `NotFoundException`（"这台机床没有这根轴"）——
  client 先读一次轴名表问"这台几根轴"，再逐个轴比。**不会给 0、更不会给垃圾值**：
  3 轴机上照 5 轴表读第 4、5 根，读到的是应答载荷后面的填充（真机上出现过 6e8 这种数），
  所以这一层闸门是必须的。5 轴是出厂配置，3 轴机把 A/C 那几行删掉即可。
- 模型树是标准的 **`MACHINE → CONTROLLER / AXIS@X → 数据对象`**：相对路径里最后一段是数据
  对象，前面每一段都是组件（可以嵌套），组件与数据对象都能用 `@number` 区分
  （`AXIS@X`、`POSITION@REAL`）。
- 模型里每个数据项/组件的 **`name` 是可读名**（照数据字典的含义列）：`/MACHINE/STATUS` 叫
  "运行状态"、`/MACHINE/AXIS@X/POSITION@REAL` 是 `AXIS@X` 组件（"X 轴"）下的"位置（实际）"。
  **名字与路径无关**：路径由 `type` 与 `number` 拼出来，上位机按路径问、按名字显示。
- 表里绝大多数点位是 **`dataItems`**（可采集）；刀具列表是 **`configs`**（不常变、可查询，
  按册 3 表 1 注 b **不得作为采样数据源**）。参数 `PARAMETER`、坐标系 `COORDINATE`、
  宏变量 `VARIABLE` 这些也是 configs；它们现在属于"声明了但读不到"（第 7 节那张表），
  要动就照 `NCL_CONFIG*` 写一行（详见 `docs/plugins-README.md`）。
- `/MACHINE/SESSION`、`/MACHINE/ITEMS` 两个方法是**现场调试用**的（会话状态、client 的
  item 表），不进模型、不参与采样。
- `/MACHINE/PROGRAM@DOWNLOAD`、`/MACHINE/PROGRAM@UPLOAD` 两个方法是**程序上下行**
  （动作，不是数据对象 —— 标准里"文件"是 `FILE`（dict），"把一段程序下发/取回"是调用）：
  参数给 `data`（程序文本）/`name`（要取的程序名）与 `type`（0 NC 程序、1 刀补、2 参数…）。
  下行已通（`cnc_dwnstart4` 三件套，见 01 册 §2.4）；**上行现在回"还读不了"** ——
  请求码已核（0x15/0x18），真机上 `cnc_upstart4` rc=0 而取数据那一步被机床拒（rc=10），
  换一台愿意给数据的机床就能补上。
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

**这一节按真机（0i-MD，3 轴）实测写的**；内部资料版（`docs/FANUC-CNC-FOCAS.md` §2.8）
有逐条的证据与判据。

### 7.1 机床自己不提供的（不是我们的帧错 —— 官方 SDK 用同样参数也被拒）

现场看到下面这些点位读到"不支持/失败"，先看这张表，别去查程序：

| 点位 | FOCAS 调用 | 机床回的 |
|---|---|---|
| `/MACHINE/PART_COUNT` | `cnc_rdcount`（`0x8b` d=e=0） | rc=6 |
| `/MACHINE/CONTROLLER/TOOL` | `cnc_rdtooldata` / `cnc_rdtoolrng` | rc=1 / rc=3 |
| `/MACHINE/CONTROLLER/COORDINATE` | `cnc_rdwkcdshft`（type 0..20 全试） | rc=1 |
| `/MACHINE/AXIS@A`、`@C` 那几条（3 轴机上） | `cnc_rdposition` | 明确回 `NotFoundException`（"没有这根轴"） |
| （client 里没绑点位的几个量） | 刀具寿命 `cnc_rdlife` / 扭矩 `cnc_loadtorq` / 刀具组数 `cnc_rdngrp` | rc=6 / rc=4 / rc=6 |

另外有两个"机床回了、但内容是桩"的：**操作面板信号**（`0x5d`）在这台机器上 32 字节里除
`@2 = 0xffff` 全是 0 —— 所以 `FEED_OVERRIDE` 会读成 0%（不是 100%），**不是偏移读错**；
**伺服那一类**（`cnc_rdaxisdata` 的 `cls=2`）回的也不是数据（`08 00 09 00` 这种）。

### 7.2 这一份 client 还没实现的（答"还读不了"，自检里是 `<待抓包>`）

- **写操作一律不做**：MDI、启程序、写刀补/参数/宏变量都没有实现，`set_value` 明确回
  "不支持"。要写就走机床自己的通道。
- `/MACHINE/TOOL_NUMBER`：**还没有可靠来源**。模态那条 `0x96` 只报 G 码组（24 组全扫过），
  `cnc_rdexecprog` 在这台机器上回的是**整段程序**（里面 T 码出现多次）—— 与其编个数，
  不如如实说"读不到"。
- `/MACHINE/CONTROLLER/SUBPROGRAM`：这套官方 SDK **没有导出** `cnc_rdexecprog3`。
- `/MACHINE/CONTROLLER/TOOLPARAM`、`/PARAMETER`、`/VARIABLE`：**单条**已经能读
  （`ncl_focas_tool_offset()` / `ncl_focas_parameter()` / `ncl_focas_macro_variable()`），
  但整表要的范围调用（`cnc_rdparanum` / `cnc_rdparar` / `cnc_rdmacror`）在这台机器上被拒
  （数量 0 / 带崩 SDK / rc=2），所以表格形态先不声明。
- `/MACHINE/AXIS@k/TORQUE`、`/CURRENT`、`/TEMPERATURE`：来源要么被机床拒、要么是桩（7.1）。
- 程序上行（`/MACHINE/PROGRAM@UPLOAD`）：见第 5 节末。
- 采样与轮询**各读一遍机床**：轮询刷新模型里的值（"还读不了"的点位第一次问过之后就不再碰），
  采样通道按 `sample.intervalMs` 读通道里的点位并按 `uploadMs` 上报。

### 7.3 现场要知道的几条口径

- **点位表按 5 轴声明**：3 轴机上 A/C 那几条会回"没有这根轴"（7.1 第四行）——
  把点位表里那几行删掉即可，这是预期行为，不是故障。
- **私有位不进模型**：FANUC 的 ODBST 位域（手动、自动、编辑、移动、急停、主轴…）
  在数据字典里**没有名字**，所以模型里一个都不出现；`STATUS` 是 RUN/EMERGENCY 两位推出来的
  三态，`WORK_MODE` 由 aut/manual 两位推出来（表 8 的 `manual`/`auto`）。要看原始位就用方法
  `focas/ITEMS`（驱动自己的项表，不进模型）。
- **倍率的口径**：`FEED_OVERRIDE` 走操作面板信号（`0x5d`）的 `feed_ovrd`，码 × 10 = 百分比；
  `cnc_rddynamic2` 的 `ODBDY2` **没有倍率字段**（真机抄包确认），别去找。
- **计时器有台机床是小端**：`cnc_rdtimer` 的载荷在这台仿真机上按小端写，分钟数于是读成天文
  数字（官方 SDK 也一样）—— client 按**官方口径（大端）**读，遇到这种机床就是把机床的毛病
  照实报出来。真要这个量先跟机床厂对一下。
- **不做**：PMC 梯形图、伺服波形、Focas2 Logger。需要的话按 01 册继续扩驱动。

---

## 8. 排错

| 现象 | 原因 / 处理 |
|---|---|
| 启动就报 `工具 "focas" 未装载：<目录> 里没有 ncl_driver_focas.dll` | `plugins\` 没跟 `bin\` 一起放，或缺了模块文件；用 `--plugins` 看装载结果 |
| 启动就报 `协议 "xxx" 未注册：…` | 老式写法（配置里 `drivers[]` + `points[]`）才需要注册协议；先看第 6 节，或 `--plugins` 看装载结果 |
| `模块 ... 的 ABI 是 N，本宿主只认 M` | 模块与程序不是同一次构建的产物，换配套的模块 |
| 日志 `cnc_allclibhndl3 ... -16`（连接超时） | 机床没开以太网功能、IP/端口不对、被防火墙挡；先用 `ping` 与 `telnet <ip> 8193` 确认 |
| 日志 `-17`（协议/握手类） | 协商没通过。FOCAS 需机床侧授权"以太网功能"；先试 `--raw` 抓帧，把 `log\out.txt` 给开发。**注意**：机床用"方向 3"的帧回"没有这个数"（宏变量/刀补/参数里不存在的号），那一类 client 已经翻成 `NotFoundException`，不是协议错 |
| 单个点位读失败但其它正常 | 先看第 7 节那张"机床不提供"的表（`PART_COUNT`、`COORDINATE`、3 轴机上的 `AXIS@A/@C` 都在里面）；不在表里再用 `--probe <路径>` 单独试这一个点位 |
| 自检里有点位是 `<待抓包>` | 那个点位的协议调用在这一份 client 里还没实现：函数回 `NCL_ERR_UNAVAILABLE`（"还读不了"），不是机床的问题，**不计失败**。清单看第 7 节 7.2 |
| 想单独确认一个点位 | `--probe /MACHINE/PART_COUNT`：走客户端一样的绑定，打印值或 NG 与原因 |
| 数值明显不对（比如位置是 12345 而不是 12.345） | 拿 `--once --raw` 抓一次原始报文（`log\out.txt` 里有 hex），对着 `docs/FANUC-CNC-FOCAS.md` §2.8.1 的"每轴 8 字节记录（data@0 + dec@6）"核 |
| `MQTT 暂未连上 / broker 未就绪` | broker 没起或地址不对。**不影响读机床**：程序会 1 s→30 s 退避重试，连上自动补订阅 |
| REST 没起来（端口被占？） | 换 `--port 8081` |
| 想确认真的发上去了 | `--stats` 打审计计数；上位机订阅 `Sample` 主题；或开 `--raw` 看每次请求的 hex |
| 想清空重来 | 删掉 `<root>\log\out.txt`；想换设备身份就删 `<root>\bin\sn.txt`（谨慎） |

日志是 UTF-8，用 VS Code / Notepad++ 打开不会乱码；控制台（GBK）里中文可能显示
成乱码，属正常现象。
