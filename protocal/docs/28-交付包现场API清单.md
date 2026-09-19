# 28 · 交付包现场 API 清单（iNC-BOX-200 现场实现对照）

> **证据**：🟢 交付包二进制（符号表 / 字符串 / 结构体 tag）· 🔵 交付包 Lua 源码
> **来源**：`D:\03-开发代码\incbox200\INCBOX200\app1\`（现场设备上跑的一整套）
> **用途**：本册是**"现场到底调了哪些 API、映射了哪些数据项"的一手清单**，
> 用来补 08/09/11/20 那些"🔴 缺资料"的分册，并给本地适配器（`adapters/`）作对照。
> **不替代**：它只给出**端点面与数据项面**；报文/请求体的**字节布局**仍需实机抓包，
> 交付包里没有源码，只有二进制与字符串。
>
> **续篇**：**[29 册](29-现场模型与驱动定义.md)** 把同一批现场文件读得更深——
> `cfg/models/*.json`（每个品牌的数据项全集）、`cfg/driver_defs/*.json`（连接参数与
> 端口）、插件 `.dynsym` 的 C++ 方法签名、以及供应商 SDK（FOCAS 的 `libfwlib32.so`、
> 华数的 `libHsc3Api.so`）。那一册给出的结论是：**数据面已经齐了**，只有自研
> HTTP/socket 的那几家还差"设备侧请求形状"，而且那一块可以在本地用 qemu 仿真抓包补。

---

## 1. 交付包布局

```
INCBOX200/
├── cfg/                      # 现场配置（driver_def.json 只挂了 focas + sharedfs 两条）
│   ├── driver_def.json       # connections[] + drivers{path → client}
│   ├── model.json            # NC-Link 设备模型（dataItems 的类型就是标准数据项）
│   └── nclink_cfg.json
└── app1/
    ├── nclink-service/       # ② 协议服务（ARM32 ELF）+ 20 个设备 .so
    │   ├── nclink-service      ARM 32-bit, EABI5, stripped, ld-linux-armhf
    │   ├── libfocas.so / libfwlib32.so / libmc.so / libs7.so / libfins.so …
    │   └── lua/ + lua/lua_mod/  # ③ 设备映射层（mod_create / mod_call / mod_destroy）
    ├── hp2x/hp2x_box200      # ① 设备协议网关（Go, GoFrame），监听 :33123
    │   ├── config.yaml         # :81 web / :33123 协议服务 / :20031 第三方接入
    │   └── warning/            # 报警文本配置（如 mitsubishi_cnc_m80.conf）
    ├── rmc/rmc_box200        # 管理服务（web UI）
    └── frp/frpc              # 内网穿透
```

启动方式（`nclink-service.sh`）：

```sh
./nclink-service -M ../../cfg/model.json -D ../../cfg/driver_def.json \
                 -C ../../cfg/nclink_cfg.json -L ../../log/nclink-service.log
```

`driver_def.json` 的形状（现场那份只配了 FOCAS + 共享文件系统）：

```json
{
  "connections": [
    { "id": "focas-client-1", "module": "focas",
      "parameters": { "ipAddress": "192.168.1.100", "port": 8193 } },
    { "id": "fs-client-1", "module": "sharedfs",
      "parameters": { "path": "../../gcode" } }
  ],
  "drivers": {
    "/":                 { "client": "focas-client-1" },
    "/CONTROLLER/FILE":  { "client": "fs-client-1" }
  }
}
```

**`module` 就是要加载的 .so**；`drivers` 是"模型路径前缀 → 连接"的挂载表，和本地
适配器的点位前缀分派是同一个思路。

---

## 2. 三层架构（谁负责什么）

| 层 | 实体 | 职责 | 证据 |
|---|---|---|---|
| ① 协议网关 | `hp2x_box200`（Go） | 直接讲厂商协议（GSK/SYNTEC/LSV2/Brother/M70/MTConnect/Modbus/S7/KEDE/JINGDIAO/安川天机/FANUC 机器人…），对外只暴露**本机 HTTP/JSON** `127.0.0.1:33123` | 🟢 符号 `hp2x/protocols/*` |
| ② 协议服务 | `nclink-service` + `lib*.so` | 插件式：一个设备 = 一个 `.so`；NC-Link 协议栈 + 模型生成在这一层 | 🟢 20 个 `.so` 导出同一套 ABI |
| ③ 映射层 | `lua/lua_mod/*.lua` | 把**NC-Link 操作 + 模型路径**（`get_value@/STATUS`）映射到①/②的端点，并做数值规整（取整、×100、位掩码） | 🟢 现场 Lua 源码 |

`:33123` 上收到的是 `POST <url>`，体为 JSON，回包形状两种：

```json
{"code": 0, "data": { "success": true, "value": 123 }}
```

（hp2x 网关：`code != 0` 视为协议失败，`data.success = false` 视为本次读数失败；
`Open/TCP` 的 `data` 里给的是 `connectionId`，后续请求都要带着它。）

---

## 3. 模块 ABI 与端点前缀（🟢 `nm -D` / `strings` 实测）

每个设备 `.so` 导出**同一套四个符号**（stripped 也在动态符号表里）：

```
create   call   destroy   get_version
```

`libfins.so`、`libHsc3Api.so`、`libCommApi.so`、`libLogApi.so`、`libbase.so` 例外：
它们没有这四个导出（是给别的模块调用的**库**，不是插件）。

| 模块 | 端点前缀 | 说明 |
|---|---|---|
| `libbase.so` | `/CONTROLLER/*`、`/STATUS`、`/PART_COUNT`… | **标准数据项表**（见 §5），不含协议 |
| `libgsk-http.so` | `/GSK/CNC/*` | 广州数控 GSK |
| `libkede-http.so` | `/KEDE/CNC/GNC62/*` | 科德 GNC62 |
| `libmitsubishi-http.so` | `/Mitsubishi/CNC/M70/*`、`/CONTROLLER/FILE` | 三菱 M70 |
| `libmc.so` | `/Mitsubishi/Plc/MC/*` | 三菱 PLC（MC） |
| `libs7.so` | `/S7/*` | 西门子 S7 |
| `libmodbus.so` | `/Modbus/*` | Modbus（TCP/RTU/ASCII） |
| `libhsr3.so` | `/STATUS`、`/TYPE`、`/CONTROLLER/*` | 华数 HSR3（CODESYS 平台，带固件升级） |
| `libi5-opcua.so` | `/CONTROLLER/*` | 沈阳 i5（open62541 OPC UA） |
| `libsinumerik-arm[-readvar].so` | `/Nck/*`、`/Methods/ReadVar` | 西门子 840D（OPC UA） |
| `libfocas.so` | `/CONTROLLER/CONSOLE`、`/CONTROLLER/FILE`、`/CONTROLLER/PROGRAM_DATA` | FANUC FOCAS（底层调 `libfwlib32.so`） |
| `libsharedfs.so` | （无端点） | 共享目录文件访问，挂 `/CONTROLLER/FILE` |
| `libcamera.so` | （无端点） | 相机：`CAMERA::getFeature` / `CAMERA::getCV2` |
| `libHsc3Api.so` / `libCommApi.so` | （库） | Hsc3 通信栈：`Hsc3::Comm::{BaseClient,ReceiverTcp,ReceiverUdp,BroadcastClient,DataFilter}` |
| `libLogApi.so` | （库） | `Hsc3::Log::LogApi`（`setLogLevel` / `writeLogInfo`） |
| `libbase64.so` / `libiconv.so` / `liblua*.so` / `libmosquitto.so` / `libpython3.5m.so` | （库） | 编码/解释器/MQTT |

> 现场**没有**独立的 LSV2/SYNTEC/Brother/MTConnect 插件 `.so`：这几个协议在
> `hp2x_box200` 里（见 §4），`nclink-service` 侧只留 Lua 映射。

---

## 4. 设备协议清单（🟢 Go 符号表 `hp2x/protocols/*`）

Go 二进制没有 strip 符号，包名就是"现场支持了哪些协议"的最硬证据：

```
hp2x/protocols/{anchuan/robot, brother/cnc, debug, fanuc/robot, gsk/cnc,
                jingdiao/cnc, kede/cnc/gnc62, lsv2, mitsubishi/cnc/m70,
                mitsubishi/plc/melsec, mitsubishi/plc/slmp, mtconnect,
                siemens/plc/s7v1, siemens/plc/s7v2, syntec}
```

按设备列出方法（只摘**对外语义**，省略 `init`/`decode*` 等内部函数）：

| 设备 | 方法（= 现场实现的能力面） |
|---|---|
| SYNTEC 新代 | `GetStatus` `GetProgramName` `GetLineNumber` `GetPartCount` `GetFeedSpeed` `GetFeedOverride` `GetSpindleSpeed` `GetSpindleOverride` `GetWarning` `GetWarningContent` `GetResponse` `NcStateGetValue` `RRegister` |
| GSK 广州数控 | `OpenTCP` `GetInit` `GetStatus` `GetLineNumber` `GetProgram` `GetPartCount` `GetFeedSpeed` `GetFeedOverride` `GetRapidOverride` `GetSpindleSpeed` `GetSpindleOverride` `GetToolNumber` `GetToolParam` `GetWarning` `GetCoordinate` `GetAxisActPosition` `GetAxisActSpeed` `GetAxisCmdPosition` `GetAxisCmdSpeed` `GetAttr` `GetCd` `GetLs` `GetC04` `GetRemove` `SendFile` `ReceiveFile` `Close` |
| 科德 GNC62 | `STATUS` `PROGRAM` `LINE_NUMBER` `PART_COUNT` `FEED_SPEED` `FEED_OVERRIDE` `SPDL_SPEED` `SPDL_OVERRIDE` `RAPID_OVERRIDE` `AXIS_ACT_POSITION` `TOOL_PARAM` `WARNING`；请求族：`NewCommonRequest` `NewAxesRequest` `NewFeedRequest` `NewSpindleRequest` `NewCounterRequest` `NewRecordRequest` `NewAlarmRequest` `NewNcdaRequest`（+ `getUid` 会话号） |
| 精雕 JINGDIAO | `Bind` `GetStatus` `GetProgramName` `GetLineNumber` `GetPartCount` `GetFeedSpeed` `GetFeedOverride` `GetSpindleSpeed` `GetSpindleOverride` `GetWarning` `GetResponse` |
| 三菱 M70（CNC） | `Connect` `GetStatus` `GetProgramName` `GetLineNumber` `GetPartCount` `GetFeedSpeed` `getPauseStatus` `getRelativePosition` `GetTimePowerOn` `GetTimeMachining` `GetTimeCumulative` `GetWarning` `GetFileList` `ReadFile` `WriteFile` `RemoveFile` |
| 三菱 PLC | `melsec`（MC 帧）+ `slmp`（`Read`/`Read2`/`Write`/`Write2`、`GetDeviceFromString`、`GetDeviceType`、`GetSubcommand`、`SLMPErr`、`InvalidDataLengthErr`） |
| 海德汉 LSV2 | `GetVersion` `GetSystemParameter` `GetAxesLocation` `GetExecutionStatus` `GetProgramStatus` `GetProgramStack` `GetOverrideInfo` `GetSpindleToolStatus` `GetErrorMessages` `ReadPLC` `GetFileList` `GetFileInfo` `GetDirectoryContent` `GetDirectoryInfo` `ReceiveFile` `SendFile` `ChangeDirectory` `MakeDirectory` `DeleteFile` `DeleteEmptyDirectory`；底层 `Telegram2`/`Telegram3`、`sendReciveAck`、`sendReciveBlock`、`read_Big_H/L/l`、`read_Lit_H/L/d`、`read_Latin1`、`IsItnc`、`login/logout` |
| 西门子 S7（plc） | `s7v1`：`Connect` `RequestConnection` `SetupConnection` `ReadBytes` `WriteBytes` `ReadTPKT` `ReadTPDU` `ReadTSDU`；**含 S7-200 Smart 专用握手**（`GetCOTPConnectionRequestS7200Smart`） |
| 西门子 840D（NCU） | `s7v2`：`NckName` `NckNo` `NckVer` `PlcType` `Mode` `Execution` `Program` `ToolNo` `PartCount` `FeedSet/FeedActual/FeedOverride` `SpeedSet/SpeedActual/SpeedOverride` `S1Load` `CycleTime` `LastRunTime` `CoordinateMachine/Absolute/Relative/Name` `Alarm` |
| Brother 兄弟 | `OpenTCP` `GetCurProgName` `GetToolList` `GetMaintenanceData` `LS` `CD` `PWD` `MakeDir` `Delete` `SendFile` `ReceiveFile` |
| FANUC 机器人 | `OpenTCP` `GetInit/GetInit2` `GetData` `GetHexResponse` `ReadRDI/RDO` `ReadSDI/SDO` `ReadGI/GO` `ReadUI/UO` `ReadSI/SO` `ReadRShort` `PrvReadBit` `PrvReadUWord` `ReadPMCR2` `GetAlarmList` |
| 安川（天机）机器人 | `OpenUdp` `ReadValue` `GetWarn` `Close` |
| MTConnect | `OpenTCP` `GetData` |
| Modbus | `OpenTCP` `OpenRTU` `OpenASCII` `Function` |
| 调试 | `OpenTCP` `Send` `ShowClients` `Close`（原始字节收发，排障用） |

> `debug.Send` 的默认样例是一串原始报文，说明现场排障就是"拿调试端点直发字节"。

`hp2x/httphandlers/*` 是上述协议之上的 HTTP 壳，与协议包一一对应；现场还挂了
`/Anchuan` `/Debug` 两个名字。没有 httphandler 的协议（melsec/slmp/s7v1/s7v2/
siemens NCU/kede）说明它们的入口在 `nclink-service` 那一侧（`*.so`）。

---

## 5. 已映射的数据项（🔵 现场 Lua 源码 + 🟢 `libbase.so` 字符串 + 模型）

### 5.1 标准数据项（模型里 `dataItem.type` 用的就是这些名字）

`libbase.so` 里那一串路径就是现场认的**标准项全集**：

```
/STATUS  /PART_COUNT  /TOTAL_PART_COUNT  /PLANNED_COUNT
/FEED_SPEED  /FEED_OVERRIDE  /RAPID_OVERRIDE  /JOG_OVERRIDE  /HANDWHEEL_OVERRIDE
/SPINDLE_SPEED  /SPINDLE_OVERRIDE
/RUN_TIME  /CUT_TIME  /POWER_ON_TIME  /ESP_STATE
/CONTROLLER/LINE_NUMBER  /CONTROLLER/PROGRAM  /CONTROLLER/SUBPROGRAM
/CONTROLLER/TOOL_NUMBER  /CONTROLLER/TOOL_PARAM  /CONTROLLER/WARNING
```

再加两种带限定的写法（Lua mod 里到处都是）：

| 写法 | 含义 | 例 |
|---|---|---|
| `/VARIABLE@<名字>` | 厂商私有量，模型里是 `{"type":"VARIABLE","number":"<名字>"}` | `/VARIABLE@CUT_TIME`、`/VARIABLE@PROCESS_TIME_RECORD` |
| `/AXIS@<轴>/<部件>/<量>` | 轴级量，轴名或轴号 | `/AXIS@0/SCREW/POSITION`、`/AXIS@C5/MOTOR/VARIABLE@LOAD` |

`cfg/model.json` 里 `dataItems[].type` 出现的值：`STATUS`、`FEED_SPEED`、`FEED_OVERRIDE`、
`SPINDLE_OVERRIDE`、`SPINDLE_SPEED`、`RAPID_OVERRIDE`、`PART_COUNT`、`VARIABLE`（配 `number`）、
`POSITION`……与上面一一对应。

### 5.2 每台设备的映射（🔵 `lua_mod/*_mod.lua` 的 `["get_value@路径"] → url`）

#### KND（`knd_mod.lua`、`func_mod.lua`，走**机床自带 HTTP**，见 §6.9）

| 模型项 | 端点 | 取值字段 | 规整 |
|---|---|---|---|
| `/STATUS` | `GET /getValue` | `run-status` | 0=free 1=holding 2=running |
| `/VARIABLE@ESP_STATE` | `GET /status` | `not-ready-reason` | `& 0x01` → true |
| `/CONTROLLER/PROGRAM`、`/CONTROLLER/SUBPROGRAM` | `GET /progs/cur` | `number` | 取整转字符串 |
| `/CONTROLLER/LINE_NUMBER` | `GET /progs/exec-status` | `P` | — |
| `/PART_COUNT` | `GET /workcounts/total` | `count` | 取整 |
| `/VARIABLE@PLANNED_COUNT` | `GET /workcountgoals/total` | `count` | — |
| `/VARIABLE@RUN_TIME` | `GET /cycletime` | `total` | — |
| `/VARIABLE@CUT_TIME` | `GET /cycletime` | `cur` | — |
| `/FEED_OVERRIDE`、`/SPINDLE_OVERRIDE`、`/RAPID_OVERRIDE`、`/JOG_OVERRIDE`、`/HANDWHEEL_OVERRIDE` | `GET /overrides/feed`、`/sp/overrides/1`、`/overrides/rapid`、`/overrides/jog`、`/overrides/handle` | `ov` | ×100 |
| `/SPINDLE_SPEED` | `GET /sp/speeds/1` | `speed` | — |
| `/TOOL_NUMBER` | `GET /plc/vm/TL0` | `[1]` | — |
| `/CONTROLLER/WARNING` | `GET /alarms/` | 按**报警类别键**取字符串 | 键→编号 `100%02d`，组 `{number,text}` |
| `/AXIS@<n>/SCREW|MOTOR/POSITION` | `GET /coors/machine` | 轴名键 `X..W`（n=0..8） | — |

报警类别键（顺序即编号）：`prm-switch, reboot, plc, ps, over-travel, over-heat, mem,
servo, servo-bus, over-workarea, io-bus, io-module, manufacture, forbid-move`。

#### LSV2（`lsv2_mod.lua` / `lsv2_mod_plc.lua`）

| 模型项 | 端点 |
|---|---|
| `/STATUS` | `/LSV2/GetProgramStatus`（+`GetExecutionStatus`） |
| `/CONTROLLER/PROGRAM` | `/LSV2/GetProgramStack` |
| `/CONTROLLER/LINE_NUMBER` | `/LSV2/GetProgramStack` |
| `/CONTROLLER/WARNING` | `/LSV2/GetErrorMessages` |
| `/FEED_SPEED` `/SPINDLE_SPEED` `/FEED_OVERRIDE` `/SPINDLE_OVERRIDE` | `/LSV2/GetOverrideInfo` |
| `/AXIS@0..2/SCREW/POSITION` | `/LSV2/GetAxesLocation` |
| `/CONTROLLER/TOOL_PARAM`（+`get_length`） | `/LSV2/GetSpindleToolStatus` |
| （PLC 区） | `/LSV2/ReadPLC` |
| 文件 | `/LSV2/{GetFileList,GetDirectoryContent,GetDirectoryInfo,ReceiveFile,SendFile}` |

#### 三菱 M70（`libmitsubishi-http.so` 端点，handler 名与之一一对应）

`GetStatus` `GetProgramName` `GetLineNumber` `GetPartCount` `GetFeedSpeed` `GetWarning`
`GetRelativePositionX/Y/Z` `GetTimePowerOn` `GetTimeMachining` `GetTimeCumulative`
`GetFileList` `ReadFile` `WriteFile` `RemoveFile` `Open/TCP` `Close`

#### GSK（`gsk_mod.lua` + `libgsk-http.so`）

`/STATUS` `/CONTROLLER/LINE_NUMBER` `/FEED_OVERRIDE` `/FEED_SPEED` `/PART_COUNT`
`/CONTROLLER/PROGRAM` `/CONTROLLER/WARNING` `/SPINDLE_SPEED` `/SPINDLE_OVERRIDE`
；文件：`Open/TCP` `Close` `Init` `LS` `Attr` `CD` `Remove` `SendFile` `ReceiveFile`。
`libgsk-http.so` 另有 `RAPID_OVERRIDE`、`TOOL_NUMBER`（mod 没映射，端点存在）。

#### SYNTEC（`syntec_mod.lua`）

`/STATUS` `/CONTROLLER/LINE_NUMBER` `/FEED_OVERRIDE` `/FEED_SPEED` `/PART_COUNT`
`/CONTROLLER/PROGRAM` `/CONTROLLER/WARNING` `/SPINDLE_SPEED` `/SPINDLE_OVERRIDE`
；`/STATUS` 与 `/CONTROLLER/PROGRAM` 的结果会 `srv_shr_set` 写进共享内存给别的任务用。

#### Brother / MTConnect（DMG MORI）/ 840D NCU / 安川与发那科机器人

| 设备 | 映射的项（要点） |
|---|---|
| Brother（`brother_mod.lua`） | `/STATUS` `/PART_COUNT` `/FEED_SPEED` `/FEED_OVERRIDE` `/RAPID_OVERRIDE` `/SPINDLE_OVERRIDE` `/CONTROLLER/{PROGRAM,PROGRAM_NUMBER,TOOL_NUMBER,TOOL_PARAM,WARNING,COORDINATE,MAINTENANCE_NOTICE}` `/VARIABLE@{CUT_TIME,OPERATE_TIME,POWER_ON_TIME,PROCESS_TIME_RECORD,TOTAL_PART_COUNT}`；文件：`/CONTROLLER/FILE` 的 `get_attributes/get_keys/add/delete/set_value` |
| DMG MORI（`dmg_mori_mtconnect_mod.lua`，走 MTConnect） | `/STATUS` `/PART_COUNT` `/RAPID_OVERRIDE` `/SPINDLE_OVERRIDE` `/CONTROLLER/{PROGRAM,LINE_NUMBER,TOOL_NUMBER,WARNING}` + 一整套 `/CONTROLLER/VARIABLE@X` 与 `X2` 双通道（BLOCK/EXECUTION/MODE/OPTIONAL_STOP/RESET/DRYRUN/CUTTING/AXES/PATH_FEEDRATE/POWER_STATE/DATETIME/ESTOP/PART_COUNT/SPINDLE_ROTATING…）+ 轴级 `/AXIS@*/MOTOR/VARIABLE@{LOAD,MODE,SPEED}`、`/AXIS@*/SCREW/POSITION` |
| 840D NCU（`s7_ncu_mod.lua`） | `/STATUS` `/PART_COUNT` `/FEED_SPEED` `/FEED_OVERRIDE` `/SPINDLE_SPEED` `/SPINDLE_OVERRIDE` `/CONTROLLER/{PROGRAM,TOOL_NUMBER,WARNING}` `/CONTROLLER/VARIABLE@{MODE,NCK_NAME,NCK_NO,NCK_VER}` `/VARIABLE@{CYCLE_TIME,FEED_SET,LAST_RUN_TIME,PLCTYPE,S1LOAD,SPEED_SET}` |
| 埃夫特/机器人（`efort_mod.lua`、`kuka_mod.lua`、`nachi_mod.lua`、`efort_task.lua`、`kuka_task.lua`、`nachi_task.lua`） | `/STATUS[@FSK]` `/CONTROLLER/{PROGRAM,VERSION,WARNING}` `/VARIABLE@{AUTO_OVERRIDE,CT,NG,TOTAL_PART_COUNT,WT,WITH_LINE_STATUS}`，6 轴各 `/AXIS@n/MOTOR/{CURRENT,POSITION,TORQUE,VARIABLE@VOLTAGE}`（埃夫特走 Modbus 端点 `/Modbus/Function`；库卡/那智走 `/Foxconn/Chengdu/Robot/Nachi/*`） |

> 机器人这一族用的是**同一套模型项**（`/STATUS`、`/CONTROLLER/*`、`/AXIS@n/…`），
> 说明 NC-Link 模型对机器人和机床是统一的——本地适配器应保持同样的项名。

---

## 6. 端点与参数细节（🟢 字符串 + struct tag 实测）

### 6.1 端口默认值（从结构体 tag 的 `d:"<默认>"` 取）

| 协议 | 端口 | 备注 |
|---|---|---|
| FOCAS | 8193 | 现场 `driver_def.json` 就是这个 |
| S7 | 102 | `rack`/`slot` 默认 `-1`（自动） |
| Modbus | 502 | `slaveId` 默认 1 |
| 三菱 M70（MELDAS） | 683 | |
| 三菱 PLC（MC/SLMP） | 6000 / 5534 | tag 原文 `dc:"Port 6000,5534"`，默认 6000 |
| MTConnect | 7878 | |
| LSV2 | 19000 | |
| 其它文档里出现过的 | 10000 / 10030 / 10040 / 33999 / 60008 / 62937 / 5566 | 对应 Brother/AI/机器人/私有端口 |
| hp2x 网关 | 33123 | |
| hp2x 第三方接入 | 20031 | `/push`、`/pop` |

### 6.2 S7

`cpu` ∈ `S7 200 Smart, S7 200, S7 300, S7 400, S7 1200, S7 1500`；
`rack` `Slot 0x00-0x0F`；`dbNumber` 注释原文
`"for DB1 this value is 1, for T45 this value is 45"`（**T/C 也走 DB 号字段**）；
`memoryType` ∈ `MARKER,INPUT,OUTPUT,COUNTER,TIMER,BYTE,WORD,DWORD,STRING,INPUT_WORD,OUTPUT_WORD`；
`deviceType` ∈ `Input,Output,Memory,DataBlock,Timer,Counter`；
`index` = `Address of the first byte to read`，`size` = `Length of data to read`。

### 6.3 Modbus

`functionCode`（默认 1）、`slaveId`、`address`、`quantity`、`values`（写，`Bytes value`）。
串口形态另有 `parity`（`N - None(0), E - Even(1), O - Odd(2)`，注释还提醒
「无校验必须用 2 位停止位」）、`baudRate`、`dataBits`（5/6/7/8）、`stopBits`。

### 6.4 LSV2 与 840D 版本信息

结构体里出现 `lsv2_version`、`lsv2_version_flags`、`lsv2_version_flags_ex`、
`hdh_bin_version`/`hdh_bin_revision`、`iso_bin_version`/`iso_bin_revision`、
`nc_version`、`plc_version`、`splc_version`、`pw_encryption_key` —— 说明现场**登录/
口令加密**与**版本探测**都是单独一步（对应 07 册 §7.4 的登录流程）。
`selector` 字段的默认样例给了两个值：`d:"12"`、`d:"70"` —— 07 册 §7 缺的
`R_RI` 选择码，可以拿这两个当**实机验证起点**（仍不是全量码表）。

`warning/mitsubishi_cnc_m80.conf` 里出现 `setasgs`，样例：
`[\"SETASG 1 1000 ALM[1] 1\", \"SETASG 1001 50 POS[0] 0.0\"]` ——
现场用三菱的 **SETASG** 命令把报警号/坐标映射成"报警文本"与"事件"，这也是
模型里 `CONTROLLER/WARNING` 那条数据的来源。

### 6.5 SYNTEC 的报文样例

结构体 tag 里留了一条报文样例（`hexData`，36 字节）：

```
18000000 1000 0101 c8000000 28040000 04000000 781c0000 28000000 00000000
```

前 4 字节 `0x18 = 24` 正是"包头之后的长度"，与 10 册 §10.3 的 `Length` 语义一致；
但 `0101` 这两个"填充字节"非零，说明**填充字段是复用/带标记的**，10 册里
「2 字节填充」的说法要按这条样例修正（10 册 §10.3 已按实测口径描述，此处留作实机对照）。

---

## 7. 与本地规格书 / 适配器的差异（缺口闭合）

| # | 之前状态 | 现场证据 | 现在的结论 |
|---|---|---|---|
| 08 GSK | 🔵 只有 `GSKRM.dll`，端点面空白 | §3/§5.4：13 个端点 + 9 个模型项 + 6 个文件端点 | **端点面已补齐**；报文布局仍缺（GSK 网关在 `libgsk-http.so` 内） |
| 09 KND | 🟡 猜的 `/api/v1.2/*` | §5.2：**真实端点** `/getValue` `/status` `/progs/cur` `/coors/machine` … | **改按现场端点表实现**（09 册 §3 已按本册更新） |
| 11 Brother | 🟢 10 命令，无映射 | §5.5：12 个 CNC 端点 + 模型项 + 文件操作 | 补齐项映射 |
| 20 科德 GNC62 | 🔴 缺 | §4：12 个量 + 8 个请求族 + `getUid` 会话号 | 🟡→🟢 **协议骨架已明**（请求族名就是命令分类） |
| 20 精雕 JD50 | 🔴 缺 | §4：`Bind` + 10 个量 + 11 个端点 | 🟡→🟢 能力面已明 |
| 海康相机 | 🔴 缺 | `libcamera.so`：`CAMERA::getFeature`/`getCV2` | 🟡 有库、无协议细节 |
| 14 华数 HSR | 🔵 ~170 方法 | `libHsc3Api.so`/`libCommApi.so`（Hsc3 通信栈）+ `libhsr3.so`（`/STATUS`、`/TYPE`、`/CONTROLLER/*`、固件升级） | 补齐"现场实际用到的那部分" |
| 17 KUKA / 18 埃夫特 / 那智 / 安川天机 | 🟡/🔴 | §4/§5.5：机器人统一模型项；天机 `OpenUdp/ReadValue/GetWarn`；库卡/那智走 `/Foxconn/Chengdu/Robot/Nachi/*`；FANUC 机器人 21 个读点 | 机器人项名统一，逐个补 |
| 04 840D | 🟢 OPC UA | `libsinumerik-arm*.so` 节点：`/Nck/State/numAlarms`、`/Methods/ReadVar`；`s7v2` 26 个量 | 两条通道都在现场用 |
| 19 沈阳 i5 | 🟢 OPC UA | `libi5-opcua.so`：`/CONTROLLER/VARIABLE@DOOR1_OPEN` 等 | 节点命名风格确认 |
| 01 FOCAS | 🟢 881 函数 | `libfocas.so` + 官方 `libfwlib32.so`，`/CONTROLLER/{CONSOLE,FILE,PROGRAM_DATA}` | 现场就是"包官方库"，不是自实现 |
| 15 Modbus | 🟢 | `/Modbus/Open/ASCII` 端点存在 | **ASCII 确实在用**（本地文档标"暂缓"可降级为"现场在用"） |
| 16 MTConnect | 🟢 | `/MTConnect/Open/TCP|Data|Close` | 一致 |

**仍然缺的**（不是没资料，是这批二进制里没有）：所有 `.so` 都是 stripped 的
ARM ELF，`hp2x_box200` 是 Go（只有符号名，没有结构体布局）。
因此**报文级布局**（如 GNC62 的 `NewAxesRequest` 具体字节、GSK 的帧头、
现场 LSV2 的 `GetOverrideInfo` 选择码）**仍需一次抓包或实机验证**。

---

## 8. 收尾：怎么用这份清单

1. **对齐项名**：本地适配器的点位/模型项必须用 §5.1 的名字，否则模型对不上。
2. **按 §5.2 的表逐设备补点数**：每条"模型项 → 端点/命令 → 字段 → 规整"都是现场跑过的，
   可以直接当验收用例。
3. **报文级实现**仍走各分册（08/09/10/11/20…），本册只保证"**调什么**"对。
4. **缺的批量补法**：拿一台现场设备，用 `debug` 端点（`/Debug/Send`）或本仓库的
   `raw` 逃生舱直发字节，抓一次真机报文，回填到对应分册的"字节布局"表。
