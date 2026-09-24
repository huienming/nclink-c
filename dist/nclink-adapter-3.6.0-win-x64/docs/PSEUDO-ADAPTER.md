# 伪机床（pseudo）· 现场说明

一台**不接硬件**的数控机床：点位模型照 [`plugins/syntec.c`](syntec.c)（新代）摆，值由模块
内置的模拟器算出来。整条链路——模型 → 采样通道 → 绑定 → 请求应答 → 上位机——不用等机床
到现场就能跑通，而且**客户端一行代码都不用改**：它看到的路径、类型、周期与一台新代机床
一模一样。

```
（没有机床）──▶ plugins/ncl_driver_pseudo.dll ──▶ ncl_server
                 内置模拟器（相位时钟）            ├── 采样上报（MQTT）
                                                 ├── 请求应答（Query / Set / Method）
                                                 └── HTTP + Swagger
```

**它适合**：上位机与客户端的联调、自动化用例、演示与培训、把"有没有机床"从开发进度里拿掉。
**它不能**：替代真机验收。协议帧、控制器行为、点位是否真的存在，这些只有机床能回答 ——
伪机床证明的是"你这一侧的逻辑对不对"，不是"这台机床会不会这么答"。

装法就是普通适配器：模块名 `pseudo`，配置里写 `"plugins": ["pseudo"]`，样例见
`conf/pseudo.json`。

```powershell
bin\ncl_server.exe -r <包根> -c conf\pseudo.json -P plugins --offline --once   # 自检：72/72，0 失败
bin\ncl_server.exe -r <包根> -c conf\pseudo.json -P plugins --model           # 看它对外发布的模型
bin\ncl_server.exe -r <包根> -c conf\pseudo.json -P plugins --port 8080       # 常驻跑（REST + 轮询）
```

## 1. 点位模型（与新代逐条对齐）

| 类别 | 路径 | 类型 | 说明 |
|---|---|---|---|
| 采样 4 项 | `/STATUS` | string | `running` / `holding` / `free`（新代的字面量） |
| | `/PART_COUNT` | i64 | 加工件数，每个循环走完 +1 |
| | `/CONTROLLER/PROGRAM` | string | 当前程序名（循环之间轮换） |
| | `/CONTROLLER/WARNING` | list | 报警；空表 = 没有报警 |
| 覆盖量 | `/CONTROLLER/LINE_NUMBER` | string | 程序行号，形如 `N2010` |
| | `/FEED_OVERRIDE` | i64 | 进给倍率 %（**可写**） |
| | `/SPINDLE_OVERRIDE` | i64 | 主轴倍率 %（**可写**） |
| | `/FEED_SPEED` | f64 | 进给速度 mm/min（= 基准 × 倍率） |
| | `/SPINDLE_SPEED` | f64 | 主轴转速 rpm（= 基准 × 倍率） |
| 轴（九个字母 × 六格 = 54 条） | `/AXIS@<轴>/SCREW/POSITION` | f64 | 实际位置（机械坐标） |
| | `/AXIS@<轴>/SERVO_DRIVER/POSITION` | f64 | 指令位置（比实际稍靠后那一点） |
| | `/AXIS@<轴>/MOTOR/POSITION` | f64 | 机械坐标（与 SCREW 同源） |
| | `/AXIS@<轴>/MOTOR/VARIABLE@ABSOLUTE` | f64 | 工件坐标系（机械坐标 + 零点偏置） |
| | `/AXIS@<轴>/MOTOR/VARIABLE@RELATIVE` | f64 | 相对当前程序段起点 |
| | `/AXIS@<轴>/MOTOR/VARIABLE@DISTANCE` | f64 | 到当前程序段终点的剩余量 |
| 配置表 | `/CONTROLLER/PARAMETER` | HASH | `get_keys` / `get_value` / `get_attributes` / `set_value` |
| | `/CONTROLLER/TOOL` | LIST | 刀补表：`get_length` / `get_value` / `set_value` / `get_attributes` |
| | `/CONTROLLER/REGISTER@R\|I\|O\|C\|S\|A` | LIST | PLC；R/I/C/S 可写，O/A 只读（同新代） |
| | `/CONTROLLER/VARIABLE` | LIST | 变量表（程序里的 `#号`），空号答 `type 0` |
| 方法 | `pseudo/SESSION` | — | 会话状态 + 这台假机床的遥控器（第 3 节） |

采样/上报周期都是 **1000 ms**，设备类型 `MACHINE`，采样通道的 `id` 就是 `pseudo`。

**轴表由配置给**（`axes`）：没列进去的轴照**新代的规矩**回 `NotFound` —— 不给数、也不去读
别的轴。样机默认把九个轴全打开（自检 72/72 全绿）；把 `axes` 收窄成 `"XYZC"` 之后，A/B/U/V/W
那 30 条会逐条报 `NotFound`，自检把它们列成读取失败 —— 这**正是**真机上没配那些轴时的样子。

## 2. 配置项（`tools[].parameters`，全都可以不写）

| 键 | 默认 | 作用 |
|---|---|---|
| `cycleMs` | `8000` | 一个加工循环多长（ms）：位置、状态、计件都跟着它走 |
| `axes` | `"XYZC"` | 这台"机床"有哪几个轴（大小写都行） |
| `seed` | `0` | 轨迹错开量：同配置 + 同 seed = 同一条轨迹（要复现就靠它） |
| `frozen` | `false` | `true`：把时钟钉在循环中段（phase=0.5、第 4 个循环），取值与时间无关，用例可以断言精确值 |
| `programs` | `["O0001"]` | 程序名表，循环之间轮换 |
| `partCount` | `0` | 起始计件 |
| `feedRate` / `spindleRpm` | `800` / `1200` | 100% 时的进给（mm/min）与转速（rpm） |
| `feedOverride` / `spindleOverride` | `100` / `100` | 初始倍率 % |
| `status` | 空 | 把状态钉死（`running`/`holding`/`free`）；空 = 跟着相位走 |
| `alarmEvery` / `alarmFor` | `0` / `1` | 每 `alarmEvery` 个循环里，末尾 `alarmFor` 个循环报一次警 |
| `alarms` | `[{"number":1201,"text":"伺服轴过载（伪）"}]` | 报警条目（`number` + `text`） |
| `params` | 12 条（1001..1012） | 系统参数：`[{"no":1001,"title":"X 轴行程","value":800000}, ...]` |
| `tools` | `8` | 刀补条数（号从 1 起） |
| `registers` | `{"R":64,"I":32,"O":32,"C":32,"S":32,"A":16}` | 各族容量 |
| `variables` | `64` | 变量个数（`#1..#8` 默认有值，其余按"空号"答） |
| `latencyMs` | `0` | 每次取值先睡这么久（喂"取值慢/丢拍/超时"那条路） |
| `fail` | 无 | `{"code":-5,"count":2,"message":"..."}`：前 `count` 次取值失败（`-1` = 一直失败） |
| `unavailable` | `[]` | `["/CONTROLLER/LINE_NUMBER"]`：这些路径照实回"还读不了"（与待抓包同一个待遇） |

三个注入开关（`latencyMs` / `fail` / `unavailable`）就是**故障注入**：客户端的 NG 分支、
超时与重试、`UnavailableException` 那些平时要拔网线才能验的路径，改一行配置就能验。

## 3. `/SESSION`：通过协议本身遥控这台假机床

调用地址 `pseudo/SESSION`（方法调用，参数都在 params 里）。不传参数就是查状态：

```json
Method/Call  {"method": "pseudo/SESSION", "params": {}}
→ {"open": true, "lastError": "", "kind": "pseudo", "status": "running",
   "cycle": 4, "phase": 0.5, "partCount": 11, "program": "O1000",
   "axes": "XYZC", "cycleMs": 8000, "frozen": false, "reads": 37,
   "alarm": false, "feedOverride": 100, "spindleOverride": 100}
```

| 参数 | 作用 |
|---|---|
| `{"status":"holding"}` | 把状态钉死（空串交回给相位）—— 上位机的状态分支、报警联动都能随手验 |
| `{"frozen":true}` | 钉住时钟（位置/计件不再走），用例要精确值就用它 |
| `{"reset":true}` | 回到第 0 个循环、计件回起始值、清掉注入的报警 |
| `{"seed":42}` | 换一条轨迹 |
| `{"partCount":100}` | 改起始计件 |
| `{"alarm":{"number":1201,"text":"..."}}` / `{"alarm":null}` | 注入 / 清掉报警 |
| `{"feedOverride":50}` / `{"spindleOverride":120}` | 拨倍率（与写那两个点位等效） |
| `{"fail":{"code":-5,"count":3,"message":"..."}}` / `{"fail":null}` | 注入 / 停掉失败 |

这一条是"假机床能做真机床做不了的事"：**自动化用例从客户端那一侧就能把这台机器拨到
RUN / HOLD / ALARM / 慢响应**，不用人去改配置、不用重启服务。

## 4. 值是怎么算出来的

一条**相位时钟**：`cycleMs` 一循环，循环内进度 p ∈ [0,1)。

* 轴位置：三角波（p 与轴序号、`seed` 错开），所以采样帧画出来是**连续的曲线**而不是常数；
  指令位置取 p 稍靠后的一点（伺服跟着指令走）；一个循环分四段，`RELATIVE`/`DISTANCE`
  在段内连续、段边界跳一下。行程与零点偏置按轴序号推（第 i 根轴 100+40i mm）。
* 状态：p < 0.9 `running`、< 0.95 `holding`、否则 `free`。
* 计件：`partCount` 起始值 + 走完的循环数；程序名每个循环换一个。
* 倍率：写进去立刻生效，`FEED_SPEED`/`SPINDLE_SPEED` 跟着变（基准 × 倍率）。
* 写进去的东西（倍率、参数、刀补、寄存器、变量）都落在模拟器的内存里，下一次读回来就是
  新值 —— 写后复核那条路能验。

`frozen: true` 时钉在 phase = 0.5、第 4 个循环：状态 `running`、计件 = 起始 + 4、行号
`N2010`、X 轴在行程顶点（50.0）、Y 轴 39.2 —— 测试用例就是拿这几个数当锚点的
（见 `plugins/tests/test_host_tool.c`）。

## 5. 与新代**有意不同**的两处

1. `/AXIS@<轴>/SERVO_DRIVER/POSITION`：新代那边这条还"待抓包"（回 `NCL_ERR_UNAVAILABLE`），
   伪机床给值 —— 客户端要验的是"拿到指令位置怎么用"，不是"这台机床抓没抓到帧"。
2. `/FEED_OVERRIDE`、`/SPINDLE_OVERRIDE`：新代只读，伪机床**可写** —— 上位机通过协议
   本身就能把倍率拨到任意值。**权限仍然在适配器外面控**（和别的适配器一样）。

另外两点如实说明：

* **审计里没有帧**。伪机床没有线上字节，所以它不实现 `last_raw` 钩子 —— 审计照常记录
  "谁在什么时候问过哪条路径、结果是什么"（见 §6 那套），但没有 `--raw` 的报文 hex，
  不会伪造一串 JSON 冒充报文。
* **不能和静态内存版混**（这是模块设计的已知边界）：静态内存构建里模块与宿主各有一块
  内存池，谁也释放不了对方的内存块。

## 6. 排错

| 现象 | 先看哪 |
|---|---|
| 自检里某条轴报 `NotFound` | 那根轴没在 `axes` 里。要么加上它，要么当"这台机床没这轴"照收 |
| 某条点位回"还读不了（UnavailableException）" | 它在 `unavailable` 列表里（配置写的就是这个意思） |
| 取值回 NG 且理由里带一句话 | `fail` 注入还没用完（`count` 次）；`/SESSION` 查 `lastError` 能看到上一次的原话 |
| 位置/计件一直不动 | `frozen: true`（钉住时钟）或 `status` 被钉死了 |
| 采样里数值一直一样 | 同上；另外只有采样通道那四项会进采样，别的点位要主动问 |
| 模块没被装载 | `plugins/ncl_driver_pseudo.dll` 是否在 `plugins\`、配置里 `"plugins": ["pseudo"]` 是否写了 |
