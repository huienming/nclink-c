# 10 · 新代 SYNTEC（RemoteCNC / OpenCNC）★ 实现规格书

> **证据**：🟢 全实证 —— 用户提供控制器升级包（`Syntec.RemoteCNC.WinCE.dll` / `Syntec.OpenCNC.dll` 反解）
> **定位**：国产 CNC 中 **API 层次最完整**的一家（读+写+参数+PLC+伺服调谐全覆盖）

---

## 1. 速查

| 项 | 值 |
|---|---|
| 端口 | **TCP 8000**（RemoteCNC 默认；⚠️ 实测另见到 5566-5572，见 §8 坑 1） |
| 承载 | 控制器内置 **OCAPI Server**（`OCAPIServer_WinCE.exe`） |
| API | `Syntec.Remote.SyntecRemoteCNC` 类，**150 方法**（.NET，控制器侧 WinCE） |
| 双模式 | **RemoteServer**（单连接）· **MultiTCP**（多链路） |
| 构造参数 | `new SyntecRemoteCNC(host, timeout)`；超时单位秒 |
| 同时支持 | Modbus 主机（`MODBUS_FC01~FC16`）· EtherNet/IP（`EnIP_*`）· CAN · **内置 OPC UA 服务器** |
| 完整表 | 150 方法（原始素材未随本目录提供） |

---

## 2. 连接建立

```
① TCP connect(host, 8000)
② 构造 SyntecRemoteCNC 对象（可传 TimeOut）
③ 校验 isConnected；失败读 get_SeriesNo 确认型号
④ 业务调用（见 §4）
⑤ Close()；对象 Dispose()
```

**会话属性**：`Host` · `Timeout` · `SeriesNo`（型号）· `MainBoardPlatformName`（主板平台）· `CncOption`（选配）· `PassSeed`

---

## 3. 帧格式

⚠️ **本协议为 .NET 对象 API（RemoteCNC 库），非裸字节协议**。控制器侧由 OCAPI Server 承载，PC 侧调用官方 `Syntec.RemoteCNC`（RemoteCNCAPI 1.x/3.x）。
如需自实现裸协议：抓包观察 `RemoteServer`/`MultiTCP` 通道（尚未逆向，见 §9 待办）。

### 3.1 设备侧报文（🟢 本地仿真抓取，2026-09）——请求形状已拿到

新代那一路**不在 Go 网关的协议包里**，但网关带 `/SYNTEC/CNC/*` 12 条路由；把
`Open/TCP` 指向假机床（`tools/site-probe/syntec_probe.sh`，端口 8000，另接住
5566-5572）逐项 POST，设备侧请求就落到了手上：**每项自己开一条 TCP 连接，请求固定
36 字节、全小端**。

```
[0..3]   18 00 00 00      0x18 = 24（= 36-12，像是"正文长度"）
[4..5]   10 00            常数 0x10 = 16
[6..7]   00 00            逐项标志（PROGRAM = f1 05、WARNING = 01 01）
[8..9]   c8 00            常数 0x00c8 = 200
[10..11] 00 07            逐项码（多数 0x0700；PROGRAM = 071e、WARNING = 0701）
[12..15] 00 00 00 00      会话/连接号
[16..19] 请求号            逐项不同（见下表）
[20..23] 04 00 00 00      类型 = 4
[24..27] 参数 A            逐项不同
[28..31] 参数 B            逐项不同
[32..35] 标志             1 / 0
```

| 项 | [6..7] | [10..11] | 请求号 | 参数 A | 参数 B | 标志 |
|---|---|---|---|---|---|---|
| `STATUS` | `0000` | `0007` | `0x0407` | 8 | 4 | 1 |
| `PART_COUNT` | `0000` | `0007` | `0x041a` | 8 | 1000 | 0 |
| `LINE_NUMBER` | `0000` | `0007` | `0x0407` | 8 | 10 | 1 |
| `PROGRAM` | `f105` | `1e07` | `0x048c` | 0x0204 | 1 | 1 |
| `FEED_SPEED` | `0000` | `0007` | `0x041a` | 8 | 700 | 0 |
| `SPDL_SPEED` | `0000` | `0007` | `0x041a` | 8 | 771 | 0 |
| `FEED_OVERRIDE` | `0000` | `0007` | `0x0407` | 8 | 19 | 1 |
| `SPDL_OVERRIDE` | `0000` | `0007` | `0x0407` | 8 | 21 | 1 |
| `WARNING` | `0101` | `0701` | `0x0428` | 0x1c78 | 40 | 115 |

完整帧（`STATUS`，36 字节）：

```
18 00 00 00 10 00 00 00 c8 00 00 07 c8 00 00 00
07 04 00 00 04 00 00 00 08 00 00 00 04 00 00 00 01 00 00 00
```

### 3.2 应答形状（🟢 2026-09，`tools/site-probe/syntec_reply_probe.sh`）

反汇编 `(*SyntecCnc).GetResponse`（`0x64d1e0`）与 `RRegister`（`0x64d6b4`）：
应答被切成 **`[0..19]` 头 + `[20..]` 正文**（`GetResponse` 的偏移参数就是 20），
正文比偏移还短就报 `error response length`。**所以最小应答 = 请求回声 + 正文**。

各数据项在正文里的读法（`mock.py` 用 `EREP:20:<hex>` 就能试）：

| 项 | 正文读法 | 实测 |
|---|---|---|
| `PART_COUNT`（寄存器 1000） | `[0..1]` = u16 | 回 1234 / 7 / 65535 原样得同值 ✅ |
| `LINE_NUMBER`（寄存器 10） | `[0..1]` = u16 | 4321 ✅ |
| `SPDL_SPEED`（寄存器 771） | `[0..1]` = u16 | 4321 ✅ |
| `FEED_OVERRIDE`（寄存器 19）· `SPDL_OVERRIDE`（寄存器 21） | `[0..1]` = u16 | 4321 ✅ |
| `STATUS`（状态索引 4） | `[0..1]` = u16 枚举 → `0/1/4` = `free`、`2` = `running`、`3` = `holding`、其余 `unknown` | 0..5 实测 ✅ |
| `PROGRAM` | 正文整段当**字符串** | `EREP:20:4f31303030` 回出对应文本 ✅（编码按机器） |
| `FEED_SPEED` | **算出来的**：先读寄存器 700、再读状态 12 与 76；状态 76 == 70 时 = `float64(寄存器700)`；否则按状态 12 查**单位换算表**（`>>5` 取整数档、`&31` 取小数档；档位 (0,0) 的系数 = 1.0） | 700=4321 → **4321**、700=1234 → **1234** ✅（三帧按寄存器号分别回，见下） |
| `WARNING` | 正文为空 → `[]` ✅（有报警时的条目布局未试） | — |

**一项要多帧的怎么摆**：`FEED_SPEED` 会连发三帧（寄存器 700 / 状态 12 / 状态 76），
而**每一帧都是一个新连接**（实测 39 连接 / 38 请求），`SEQ:`（按连接内的请求序号）
不管用——用 `mock.py` 新加的 **`MAP:`**（按请求里的字节挑应答）：

```
MAP:28:2:bc02=<寄存器700 的应答>|0c00=<状态12 的应答>|4c00=<状态76 的应答>|<兜底>
```

（`[28..29]` 是请求里的寄存器号：700 = `bc 02`、12 = `0c 00`、76 = `4c 00`。）

**九项至此全部闭环**（请求 + 应答），新代这一家可以整机仿真了。

> 会话模型：`Open/TCP` 拿不到 `connectionId`（`None`），但**每个数据项调用会自己建连接**，
> 一个数据项内部可能连发多帧（如 `FEED_SPEED` 三帧），每帧都是新连接。

> 顺带一个观察：`Open/TCP` 返回的 `connectionId` 是 `None`（Open 本身没成功），
> 但每个数据项调用仍会**自己建连接**——这一家的会话模型与 M70/精雕不同。

---

## 4. API 全表（150 方法，按功能分组）

### 4.1 会话 / 系统（19）

```
.ctor(TimeOut)  Dispose  Finalize  Close  isConnected
get_Host set_Host set_Timeout get_SeriesNo get_MainBoardPlatformName get_CncOption get_PassSeed
RequestUTChannel  Read_UTChannelState  SetCncUseTime  isUSBExist  ClearCache
isDipoleSupported  IsNeedUpdateVersion  RemoteProgExecute
```

### 4.2 状态 / 坐标 / 进给（9）

```
READ_status      READ_information   READ_position    READ_spindle
READ_gcode       READ_othercode     READ_time        READ_part_count
WRITE_relpos     READ_state_variable
```

### 4.3 报警（2）
```
READ_alm_current   READ_alm_history
```

### 4.4 工件坐标（6）
```
READ_work_coord_axis  READ_work_coord_all  READ_work_coord_scope  READ_work_coord_single  READ_work_coord_count
WRITE_work_coord_all  WRITE_work_coord_single
```

### 4.5 宏 / 参数（7）
```
READ_macro_all   READ_macro_scope   READ_macro_single   READ_macro_variable   WRITE_macro_all   WRITE_macro_single
READ_param_max   READ_param_data    READ_param_schema   WRITE_param_single
```

### 4.6 PLC（20）★ 闭环关键
```
READ_plc_ibit  READ_plc_obit  READ_plc_cbit  READ_plc_sbit  READ_plc_abit
READ_plc_register  READ_plc_timer  READ_plc_counter  READ_plc_type  READ_plc_type2  READ_plc_addr  READ_plc_ver
WRITE_plc_ibit  WRITE_plc_cbit  WRITE_plc_sbit  WRITE_plc_register  WRITE_plc_addr
UPLOAD_plc_file   DOWNLOAD_plc_ladder
```

### 4.7 程序 / 文件（12）
```
UPLOAD_software  UPLOAD_nc_mem  UPLOAD_param_file  DOWNLOAD_nc_mem  DOWNLOAD_work_record
READ_nc_OPLog  WRITE_nc_main  READ_nc_pointer  READ_nc_current_block  READ_nc_freespace  READ_nc_mem_list  DEL_nc_mem
```
进度属性：`isFileUploadDone` · `FileUploadTotalProgress` · `FileUploadNowProgress`

### 4.8 刀补（7）
```
READ_offset_title  READ_offset_count  READ_offset_all  READ_offset_scope  READ_offset_single
WRITE_offset_all   WRITE_offset_single
```

### 4.9 伺服 / 调谐 / DAQ（关键子集）

```
READ_SerialStateVar_*        # 伺服状态变量
WRITE_PutSerialParamValue    # 写伺服参数
READ_GetSampleTime  WRITE_SetSampleDivider
PutDAQChSetting  CTRL_EnableSampleData  CTRL_Listener_*
SSV_*  SerialParam*  AxisParamGetTypeOfChArray
```

### 4.10 其它
```
READ_time  WRITE_remoteDate  WRITE_remoteTime  READ_diskCFreeSpace  READ_CncFreeSpace
READ_debug_variable  READ_system_variable  WRITE_MakerConfigInfo  READ_MakerConfigInfo
READ_DeviceInfo  SynDevice/SynModule/SynUnit（设备树）
```

---

## 5. 地址与数据类型

| 数据区 | API | 说明 |
|---|---|---|
| PLC 寄存器 | `READ_plc_register` / `WRITE_plc_register` | R 值 |
| PLC 位 | `READ_plc_{i,o,c,s,a}bit` | 输入/输出/线圈/特殊/辅助 |
| PLC 定时器/计数器 | `READ_plc_timer` / `READ_plc_counter` | — |
| 宏变量 | `READ_macro_variable` | 编号寻址 |
| CNC 参数 | `READ_param_data` + `READ_param_schema` | **schema 先取参数模型再取值** |
| 工件坐标 | `READ_work_coord_axis(single/scope)` | 按轴/按范围 |
| 刀补 | `READ_offset_single` | 按刀号 |

**注意**：多数 `READ_*_scope` 系列为**范围读**（起始+长度），单点读优先用 `_single`。

---

## 6. 典型流程

### 6.1 采集循环（推荐）

```
session = new SyntecRemoteCNC(host, timeout=5)
loop every 200ms:
    status = session.READ_status()          # 运行状态（含 ALARM/EMG 字段）
    pos    = session.READ_position()        # 坐标（含进给/主轴字段）
    if status.alarm:  alm = session.READ_alm_current()
loop every 1s:
    cnt = session.READ_part_count()
    t   = session.READ_time()
```

### 6.2 程序下发（危险操作，需确认）

```
session.WRITE_nc_main(程序号)              # 指定主程序
session.UPLOAD_nc_mem(本地文件)            # 上传程序
wait until session.isFileUploadDone
session.DOWNLOAD_nc_mem(远程名)            # 取回程序
```

### 6.3 闭环控制（PLC 交互）

```
val = session.READ_plc_register(addr)
session.WRITE_plc_register(addr, value)
bit = session.READ_plc_cbit(addr)
session.WRITE_plc_cbit(addr, 1)
```

---

## 7. 同包附带能力（可直接用，无需额外授权）

| 能力 | 接口 | 用途 |
|---|---|---|
| **Modbus 主机** | `MODBUS_Init/DeInit` + `MODBUS_FC01..FC16` | 控制器可做主站连外部设备 |
| **EtherNet/IP** | `EnIP_Connection` / `SendExpMsgReq` / `RecvExpMsgRsp` | 与 AB PLC 交互 |
| **CAN 总线** | `CAN_Connect/Start/Polling` + `CAN_GetVendorObjDictionary` | 现场总线 |
| **OPC UA 服务端** | `OPCUA_SDK.dll` + `OPCUAServer_WINCE.exe` | 控制器**直接对外提供 OPC UA**（可绕过 RemoteCNC 采集） |
| **云上传** | `CloudAPI.UploadFileParam` | 参数上云 |
| **加密/校验** | `DES_CRC16` / `Encryption` / `Decryption` | 报文校验 |
| **伺服参数字典** | `StdRes/DriverParam/*.xml` | 安川Σ/三菱J4/汇川DA200/台达ASDA 参数模型 |

---

## 8. 实现坑

1. **端口疑点（重要）**：RemoteCNC API 走 **8000**，但实测扫描记录里还有 **5566-5572** —— 说明存在**不走 RemoteCNC API** 的实现（可能走 SyntecLayer / 私有层协议）。自实现时先确认目标机床开放哪个端口。
2. **参数读取要先取 schema**：`READ_param_schema` 返回参数模型（类型/范围），否则 `READ_param_data` 的类型解释会错。
3. **文件操作必须等进度**：`UPLOAD_*` 是异步的，必须轮询 `isFileUploadDone`，否则后续调用会冲突。
4. **`DEL_nc_mem` / `WRITE_nc_main` 是破坏性操作** —— 必须白名单 + 二次确认。
5. **伺服调谐（`SSV_*`/`SerialServo`）会改变机床动态特性**：采样类（DAQ）可放开，写入类必须工程权限 + 记录。
6. **多路径机床**：`READ_work_coord_axis` 等按轴寻址时注意多路径主轴的轴号偏移。

---

## 9. 参考实现与待办

| 项 | 说明 |
|---|---|
| 官方 SDK | `SyntecRemoteAPI`（1.x 用于旧控制器 / 3.x 新版）；包内含 `SyntecRemoteExample_KrnlAPI_*.sln` 示例 |
| 素材 | 固件履历 + 解出的 29 个 API 二进制（原始素材未随本目录提供） |
| 完整方法表 | 150 方法全签名 · OpenCNC 4206 方法（原始素材未随本目录提供） |
| **待办** | ① 抓 `RemoteServer`/`MultiTCP` 通道报文，逆向裸协议（可绕开 SDK 分发限制）② 确认 5566-5572 通道的协议本质 |

---

## 10. 线协议解剖（进行中）

> 本节是**逐条从交付包里的 .NET 程序集元数据读出来的**，不是推测。读到哪写到哪，
> 每条都标了出处；未落实的部分明确标"待补"，不用它去写代码。

### 10.1 两端各是谁（出处：程序集清单）

| 角色 | 文件 | 说明 |
|---|---|---|
| 控制器侧服务端 | `Shared/dotnet/OCAPIServer_WinCE.exe` | .NET 程序集，含 `OCAPIServer.TCPServer` / `OCAPIServer.TCPService`，端口默认 8000 |
| 客户端 API | `Shared/dotnet/Syntec.RemoteCNC.WinCE.dll` | .NET 程序集，`Syntec.Remote.SyntecRemoteCNC`（150 方法） |
| 客户端底层 | `Shared/dotnet/Syntec.OpenCNC.dll` | .NET 程序集，`Syntec.OpenCNC.OcApiTCP`（65 方法：`Connect` / `InternalConnect` / `Disconnect` / `TCPFile*` / `Install` / `OCAPIInit`） |
| PC 侧原生 | `Shared/Windows/OCApi.dll`、`OCKrnl.dll` 等 | 原生 PE（非 .NET），`OcApiTCP` 的会话很可能落在这里 |

> 说明：`Syntec.OpenCNC.dll` 里 `OcApiTCP` 没有 `Socket`/`TcpClient` 调用，会话在原生侧；
> 但**服务端** `OCAPIServer_WinCE.exe` 有 `Socket::Select` + `ReceivePackets`，
> 所以"报文长什么样"从服务端程序集就能读全。

### 10.2 报文是结构体（出处：`OCAPIServer_WinCE.exe` 元数据字段表）

服务端的收发走 `ByteArrayToStructure` / `StructureToByteArray`，也就是把 C 结构体
直接按字节搬。已读出的结构体（字段名照抄）：

```
CTCPCMD_PacketStart          : Length, CmdID, Reserved          # 每个包的包头
CTCPFunctionCmdSend_Header   : uFuncID, uSerial, Reserved, IHeader
MMI_Request_KrnlAPI          : uFuncID, dwCode, dwSizeIn, dwSizeOut, pBufferIn
MMI_Request_FileSendStart    : uFuncID, nFilePathLength, szFilePath
MMI_Request_FileSending      : uFuncID, nFileLength, pBufferIn
MMI_Request_FileRecvStart    : uFuncID, nFilePathLength, szFilePath
MMI_Request_FileRecving      : uFuncID, nFileOffset, nReqLength
MMI_Request_GetAllFileList   : uFuncID, nDirPathLength, szDirPath
MMI_Request_Install          : uFuncID, nMethod
MMI_Request_FileExist        : uFuncID, nFilePathLength, szFilePath
MMI_Request_DirExist         : uFuncID, nDirPathLength, szDirPath
MMI_Request_DirCreate        : uFuncID, nDirPathLength, szDirPath
MMI_Request_FileNew          : uFuncID, nFilePathLength, szFilePath
MMI_Request_FileDelete       : uFuncID, nFilePathLength, szFilePath
MMI_Request_FileCopy         : uFuncID, nTwoFilePathLength, szTwoFilePath
MMI_Request_FileMove         : uFuncID, nTwoFilePathLength, szTwoFilePath
MMI_Request_RemoteProgExecute: uFuncID, nPathLength, nArgumentsLength, lpszProgFullPath
MMI_Request_NcShutdown       : uFuncID, dwMilliseconds
MMI_Request_NcRestartCNC     : nDelayTimeMilliseconds, nTimeOutMilliseconds, bForce
MMI_Request_NcRequestUpdate  : uFuncID, nLength, pBufferIn
MMI_Request_ResMgr_RemoteLookup: uFuncID, nLength, lpszKey
MMI_Request_OnEventCallParams: nFuncID, nEventID, nParam
Krnl_Response_NcRequestUpdate: hr, nLength, pBufferOut
```

两个命令枚举（同处读出）：`FileTransferCmd` = FileSendStart / FileSending /
FileRecvStart / FileRecving / GetAllFileList / Install / FileExist / DirExist /
FileNew / FileDelete / FileCopy / FileMove / DirCreate；`AlarmCmd` 里含
`TCPALARM_OnEventCall`。

另有 CRC：客户端程序集里有 256 项 CRC-16 表、多项式 **0xA001**（`BitConverter` 取字节，
即小端）。**2026-09 结清**：它**只在文件传输路径被引用**（`TCPFile*` /
`MMI_Request_FileSending` 那一族），**业务读写路径不带校验**——所以驱动实现里
只有"传程序文件"要做 CRC，坐标/状态/计数那些 `KrnlAPI` 调用不用。位置与覆盖范围
按文件块（`nFileOffset` + `pBufferIn`）算，属文件类实现细节，不影响数据采集。

### 10.3 下一步（**已全部完成**，见 §10.4–§10.11；本节留作存档）

> 四步都做完了：1 → §10.4（12 字节包头 + 结构体镜像）、§10.9（`uFuncID` 即 `CmdID`）；
> 2 → §10.10/§10.11（结构体与枚举数值全取到）；3 → §10.7 + §10.11（命令号表，
> 两条独立路径互证）；4 → 靶机与驱动骨架现在可以直接写。
> 下面原文保留，便于回溯当时的推断顺序。

1. 读 `OCAPIServer.TCPService::ReceivePackets` 与 `ByteArrayToStructure` 的 IL：
   确定 `Length` / `CmdID` / `Reserved` 的宽度与字节序、包头→命令体的分派方式、
   超时与重发（`TCPRetrying` / `TCPRetried` 事件在客户端侧）。
2. 读各 `MMI_Request_*` 结构体的字段签名与 `StructLayout`/`MarshalAs`：
   算出每个包的**精确字节长度与偏移**（`n*Length` 与 `sz*` 是变长还是定长数组）。
3. 读 `SyntecRemoteCNC` 的 `READ_status` / `READ_position` 之类的薄壳，
   把它们映射到的 `uFuncID` 取出来（业务命令号）。
4. 有了 1–3 就能写一个假 OCAPIServer 靶机，跑通握手 + `READ_status`，再实现驱动。

### 10.4 分帧机制（已确证，出处：`OCAPIServer_WinCE.exe` 的 IL）

**一个包 = 12 字节包头 + `Length` 字节内容**，两端都用"结构体 ↔ 字节"直接搬：

```
CTCPCMD_PacketStart   (12 字节，LayoutKind.Sequential 默认对齐)
    0  4  Length      u4      内容长度（不是总长）
    4  2  CmdID       u2      命令号
    6  2  ——          ——      CmdID 后的 2 字节填充（默认对齐产生）
    8  4  Reserved    u4
```

依据：

1. `TCPService::RecvPacketStart` 里 `Socket::BeginReceive(buffer, 0, **12**, None, ...)`
   —— 包头固定 12 字节（取自 `m_HeaderBuf`）。
2. `TCPService::ReceivePackets` 的调用序列是
   `RecvPacketStart(&len)` → `RecvPacketContent(len)`，
   而 `RecvPacketContent(size)` 内部按 `size - 已收` 继续 `BeginReceive`
   到 `m_WorkBuffer` —— 即 **`len` 就是"包头之后的内容字节数"**。
3. `TCPService::StructureToByteArray(structure, size)` 是
   `new byte[size]` + `Marshal.StructureToPtr` + `Marshal.Copy` + `FreeHGlobal`，
   反向的 `ByteArrayToStructure` 同类 —— 报文体就是 C 结构体的字节镜像。
4. `TCPService::CheckAndCreateBuffer(ref buffer, size)`：缓冲区不够长就
   `new byte[size]`，说明内容长度完全由包头决定，收包侧不预设上限。
5. `TCPService::Run` 的主循环：`Select(timeout)` → `ReceivePackets()` →
   `ProcessPacket()` → `SendResponse()`；超时参数是秒（`m_nTimeOut * 1000`）。

函数体的前 4 字节是 `CTCPFunctionCmdSend_Header`
（`uFuncID u2 | uSerial u1 | Reserved u1 | IHeader u4`，共 8 字节，无填充），
其后才是该功能自己的 `MMI_Request_*`。

**下一步**：把业务薄壳（`READ_status` / `READ_position`…）对应的 `uFuncID`
取出来，并从 `ProcessPacket` 的分派表读出命令号全集；再确认 32/64 位指针宽度
（客户端是 x86，指针 4 字节）。

### 10.5 命令号与服务分层（名字已确证，数值待取）

服务端把连接**按用途分成 5 个服务**（枚举 `OCAPIServer.EServiceName`）：

```
Dipole · Alarm · Update · FileTransfer · AutoConnect
```

每个服务有自己的分派方法，各自按包头的命令号跳转（IL 里就是 `switch`）：

| 服务 | 分派方法 | case 数 |
|---|---|---|
| 文件传输 | `TCPFileTransferService::DispatchPacketByFunctionID` | 17 |
| 固件更新 | `TCPUpdateService::ReceiveUpdate` | 21 |
| Dipole（直连/中继） | `TCPDipoleService::DispatchPacketByFunctionID` | 3 |
| 服务查找 | `ServiceManager::GetService` | 4 |

> 这一层结构对上 §1 里"实测另见到 5566-5572"那条坑：**不是一个端口一个协议，
> 而是同一套 TCP 包跑在多个服务端口上**（`EInternetProtocol` 另有 TCP/UDP 之分）。

命令号枚举（成员名已读出，**数值待取**）：

```
EFunctionID      : TCP_NcShutdown, TCP_NcRestartCNC, TCP_NcRequestUpdate,
                   TCP_NcStartControlSystem, TCP_ResMgr_RemoteLookup,
                   TCP_RemoteProgExecute, TCP_KrnlAPI
FileTransferCmd  : FileSendStart, FileSending, FileRecvStart, FileRecving,
                   GetAllFileList, Install, FileExist, DirExist, FileNew,
                   FileDelete, FileCopy, FileMove, DirCreate
AlarmCmd         : TCPALARM_OnEventCall
TCPError         : ShutDown, UnKnownErr, S_OK, CloseConnection,
                   ReceiveLengthTooShort, WSAEINTR, WSAECONNRESET …
```

命令号的数值下一次直接**从 IL 的比较指令里取**（`ldc.i4 N` → 比较 → `switch`），
不再走 `Constant` 表：那个堆在 dnfile 里解出来的编码索引对不上（同一批 49 行里
多行解出同一个 field rid），会得到假值，不如从代码里读。

### 10.6 命令号已取到的部分（出处：分派方法的 IL）

分派方法的开头是 `命令号 - 基数; switch`，基数就是该服务命令号的起点：

| 服务 | 代码里的基数 | 推出 |
|---|---|---|
| 文件传输 `TCPFileTransferService` | `arg - 1`，17 个 case | 命令号 **1..17**（`FileTransferCmd` 13 项都落在这个区间） |
| `TCPUpdateService::ProcessStringDevice` | `arg - 1`，3 个 case | 命令号 1..3 |
| Dipole `TCPDipoleService` | `arg - 178`，3 个 case | 命令号 **178..180** |

待取：`EFunctionID`（`TCP_NcShutdown` … `TCP_KrnlAPI`，7 项）与
`TCPUpdateService::ReceiveUpdate` 的基数——那两处的 `switch` 前面还隔着别的指令，
要按跳转表反查 case 值，或直接读业务请求里设的 `uFuncID`。

### 10.7 命令号空间是全局的（Dipole 分派读通了）

`TCPDipoleService::DispatchPacketByFunctionID` 的形状是
`switch (命令号 - 178)` **再加**一条 `if (命令号 == 200)`，也就是
**命令号不是每个服务从 0 开始，而是全设备一套编号**：

```
文件传输服务 : 1 .. 17        （基数 1，17 个 case）
Dipole 服务  : 178, 179, 180   （基数 178，3 个 case）+ 200（单独的相等判断）
```

Dipole 的第 4 条命令（200）那条分支里调用的是 `TCPDipoleService::KrnlAPI`
（IL 偏移 180 处 `call TCPDipoleService::KrnlAPI`）——也就是承载
`MMI_Request_KrnlAPI{uFuncID, dwCode, dwSizeIn, dwSizeOut, pBufferIn}` 的处理函数。

> 读命令号的方法（已固定下来）：**先找分派方法的 `switch`，看它前面减掉的基数；
> 再看分派体里有没有 `ldc.i4 <值>; bne.un/beq` 这类"额外命令"比较**。
> 两条都读了，命令号才不漏。

### 10.8 `KrnlAPI` 是 P/Invoke：业务码来自原生 Krnl API

`OCAPIServer.TCPDipoleService::KrnlAPI` 的元数据是
`rva=0` + `mdPinvokeImpl=True`（`mdStatic`、`mdPrivate`）——**它没有 IL，是
对原生库的直接调用**。也就是说服务端把收到的
`MMI_Request_KrnlAPI{uFuncID, dwCode, dwSizeIn, dwSizeOut, pBufferIn}`
**原样递给本机的 Krnl/MMI 原生函数**，`dwCode` 是**原生 API 的函数码**，
不是协议层的枚举。

这条对实现的意义：

1. **分帧与命令号**（12 字节包头 + `CmdID` + 函数体）已经够写靶机与驱动骨架了；
2. **业务码表**（`READ_status` / `READ_position` 各自用哪个 `dwCode`）要么从
   客户端那 150 个薄壳里读（它们知道给每个 API 填什么码），要么从本机原生
   API 的导出表读（交付包里有 `OCUSER.dll`，166 个导出）；
3. 靶机先只答一路（`KrnlAPI`），把收发、序列号、超时跑通，再逐条补业务码。

### 10.9 `uFuncID` 就是命令号（`funcId` 悬念结清）

`OCAPIServer.TCPService::ProcessPacket` 的 IL 把两个方向都说清了：

```
local0 = 把 m_WorkBuffer 直接 marshal 成函数结构体
funcId = local0.uFuncID                    ← 取函数头里的 uFuncID
this.DispatchPacketByFunctionID(funcId, &out)   ← 按 uFuncID 分派（不是按 CmdID）
this.PreparePackets(out.size, funcId, m_nWtfReserved)   ← 回包：CmdID = funcId
```

而 `PreparePackets` 写包头是：

```
CTCPCMD_PacketStart.Length   = 分派返回的内容长度
CTCPCMD_PacketStart.CmdID    = 传进来的 funcId
CTCPCMD_PacketStart.Reserved = m_nWtfReserved
```

**结论**：包头里的 `CmdID` 与函数头里的 `uFuncID` 是**同一个命令号**，
服务端按 `uFuncID` 分派、回包时把它写回 `CmdID`。所以 §10.6/§10.7 里按基数
推出的那些数（文件传输 1..17、Dipole 178/179/180、200）**就是 uFuncID**，
不存在另一个"funcId 常量"要找。

旁证：`TCPAlarmService::PrepareAlarmPacket` 给
`MMI_Request_OnEventCallParams` 填的是 `nFuncID = 1`（报警回调这一路），
同样是"函数号 = 命令号"的用法。

实现因此改成：**两个字段默认填同一个命令号**，`"funcId"` 参数只在某台机床
要求不同值时覆盖（默认 0 = 用命令号）。

### 10.10 数据码空间（客户端程序集的枚举，名字已确证）

客户端 `Syntec.OpenCNC.dll` 里的枚举就是"要点什么数据"的码表，成员名全部可读：

```
Syntec.OpenCNC.EDevice_Type        L_REGISTER, GLOBAL_VARIABLE, R_REGISTER,
                                  SYSTEM_VARIABLE, I_BIT, O_BIT, C_BIT, S_BIT,
                                  A_BIT, STATE_VARIABLE, PARAM, COORD_VARIABLE,
                                  TIMER_STATE, TIMER_TYPE, TIMER_SETTING,
                                  TIMER_ELAPSE …
Syntec.OpenCNC.EOcVariantType      VACANT, INT, DOUBLE, STRING
                                  （对应 TOcVariant{nValType, nIntVal, DoubleVal}）
Syntec.OpenCNC.EventTriggerEnum.EDataType
                                  DT_MACHINEPOS, DT_SETTABLESET, DT_BASICOFFSET,
                                  DT_G92OFFSET, DT_HCSOFFSET, DT_HCSCHANGE,
                                  DT_SYSTIME, DT_FEEDBACK, DT_HOMING,
                                  DT_AXESALRM, DT_TOOLINFO, DT_COORD,
                                  DT_SERVOCMD, DT_DUALFEEDBACK, DT_GLOBALVAR,
                                  DT_SYSTEMVAR
```

配合已读出的结构体，业务访问的形状就清楚了：

```
TDevice{nDevType, nCoordID, nGroupID, nFormat, nNo}   ← "读哪个设备的什么"
TOcVariant{nValType, nIntVal, DoubleVal}             ← 值的类型与内容
MMI_Request_KrnlAPI{uFuncID, dwCode, dwSizeIn, dwSizeOut, pBufferIn}
```

也就是说一条业务读取 = **命令号（uFuncID）+ dwCode（原生函数码）+
payload（TDevice 之类的选择结构）**，payload 的布局就是上面这些结构体。

**还剩一步**：这些枚举的**数值**（成员名已确认，数值要从 IL 或常量表取；
`Constant` 表在 dnfile 里的父索引解析还没对上——`row.Type` 是元素类型、
父索引另有字段，下次直接修）。取到数值后，`EDevice_Type` + `EDataType` 就能
填进驱动的点位配置里用。

> 更省事的一条路：直接读客户端**某一个具体 API 的请求构造函数**
> （比如 `READ_position`），那一个方法里就同时有"哪个 uFuncID、哪个结构体、
> 哪个枚举值"，比逐个枚举去凑更快也更少歧义。

### 10.11 全部数值拿到（Constant 表按对字段读出来了）

上一节卡在枚举数值上，原因是把 `Constant` 行的 `Type`（**元素类型**，int32 = 8）
当成了父索引；父索引在 `Parent` 里，而且 dnfile 已经把它解析成行对象
（`row.Parent.row`）。改对之后一次取全：

**命令号（服务端 `EFunctionID`，就是报文里的 uFuncID）**

| 名称 | 值 | 名称 | 值 |
|---|---|---|---|
| `TCP_NcShutdown` | 87 | `TCP_ResMgr_RemoteLookup` | 178 |
| `TCP_NcRestartCNC` | 163 | `TCP_RemoteProgExecute` | 180 |
| `TCP_NcRequestUpdate` | 165 | `TCP_KrnlAPI` | **200** |
| `TCP_NcStartControlSystem` | 174 | | |

> 这张表**反证了 §10.7 从 IL 读出来的基数**：Dipole 分派是
> `switch(命令号 - 178)`，正对应 `TCP_ResMgr_RemoteLookup = 178`；那条
> `if (命令号 == 200)` 正是 `TCP_KrnlAPI`。两条独立路径得到同一结论。

**文件传输（服务端 `FileTransferCmd`）**

| 名称 | 值 | 名称 | 值 |
|---|---|---|---|
| `FileSendStart` | 1 | `FileExist` | 11 |
| `FileSending` | 2 | `DirExist` | 12 |
| `FileRecvStart` | 3 | `FileNew` | 13 |
| `FileRecving` | 4 | `FileDelete` | 14 |
| `GetAllFileList` | **8** | `FileCopy` | 15 |
| `Install` | **48** | `FileMove` | 16 |
| | | `DirCreate` | 17 |

> **声明顺序不等于取值**：`GetAllFileList` 是 8、`Install` 是 48。最初按声明
> 顺序填的表是错的——这种错误只有拿到数值才看得见，已按实测值改正。

**数据码（客户端 `EDataType`，KrnlAPI 的 `dwCode` 用它）**

```
DT_MACHINEPOS=0        DT_SETTABLESET=1     DT_BASICOFFSET=2    DT_G92OFFSET=3
DT_HCSOFFSET=4         DT_HCSCHANGE=5       DT_SYSTIME=6        DT_FEEDBACK=7
DT_HOMING=8            DT_AXESALRM=9        DT_TOOLINFO=10      DT_COORD=11
DT_SERVOCMD=12         DT_DUALFEEDBACK=13   DT_GLOBALVAR=14     DT_SYSTEMVAR=15
DT_DEBUGVAR=16         DT_IBIT=17           DT_OBIT=18          DT_CBIT=19
DT_SBIT=20             DT_ABIT=21           DT_SPINDLE_FEEDBACK_VEL=22
DT_SPINDLE_SERVOCMD_VEL=23                  DT_REGISTER=24      DT_DEVICEVAL=25
DT_ABSOLUTE_FEEDBACK=26                     DT_CNC_STATUS=41
DT_CNC_MAIN_PROGRAM=42 DT_PART_COUNT=43    DT_BUFFEROVERFLOW=500
```

**设备数据种类（客户端 `EDevice_Type`）**

```
L_REGISTER=0  GLOBAL_VARIABLE=1  R_REGISTER=2  SYSTEM_VARIABLE=3  I_BIT=4
O_BIT=5  C_BIT=6  S_BIT=7  A_BIT=8  STATE_VARIABLE=9  PARAM=10
COORD_VARIABLE=11  TIMER_STATE=12  TIMER_TYPE=13  TIMER_SETTING=14
TIMER_ELAPSE=15  COUNTER_STATE=16  COUNTER_TYPE=17  COUNTER_SETTING=18
COUNTER_COUNT=19  DEV_AX_VARIABLE=20  NOT_DEFINE=-1
```

另有 `EOcVariantType{VACANT=0, INT=1, DOUBLE=2, STRING=3}`（`TOcVariant.nValType`）
与 `EServiceName{Dipole=0, Alarm=1, Update=2, FileTransfer=3, AutoConnect=4}`。

这些数值已经落进代码（`ncl_syntec_cmd_lookup` / `ncl_syntec_data_code`），
点位配置可以直接用名字或数值：

```
{"area": "KrnlAPI", "offset": 43, "length": 4, "dtype": "int32"}   # 工件计数
{"area": "KrnlAPI", "offset": 41, "length": 4, "dtype": "int32"}   # CNC 状态
```

### 10.12 每个 API 用到的代码（从客户端 worker 桩里取）

客户端的 150 个 API 只是薄壳，真正干活的是 `_GB` 里的 worker；这些 worker 大多
是**七条指令的桩**（`ldc.i4 <码>` 之后转调），所以每个 API 用到的代码可以机械取出：

```
READ_part_count   → 1000, 1002, 1004       （总/好/坏计数）
READ_time         → 1010, 1011, 1012, 10020
READ_spindle      → 700, 771 （另有两次取数不带常量，应为轴号/通道号参数）
READ_status       → 1, 1, 5
READ_alm_current  → 0
READ_position     → 0, 1 （同上）
READ_useTime      → 无常量（取值由入参传入）
```

这些数看着是**设备号/数据号**（1000 段是计数与时间、700 段是主轴），与
`EDataType`、`EDevice_Type` 是**两套不同的编号**：`EDataType` 用于事件触发那一路，
这组是"读设备数据"那一路。**它到底是 `dwCode` 还是随包 `TDevice` 里的设备号，
需要一次实机验证**——`MMI_Request_KrnlAPI` 里两者都有。

取法可复现：读客户端 `SyntecRemoteCNC::READ_*` 的 IL → 看它调用了 `_GB` 的哪些
worker → 每个 worker 的 IL 里只有一个 `ldc.i4` 常量（脚本在参考架上，不进仓库）。

**适配器里的用法**：`part_count`(1000)、`part_count_good`(1002)、
`part_count_bad`(1004)、`spindle_700`(700)、`spindle_771`(771) 已经做成**具名
读数**（`adapters/drivers/syntec/syntec_codec.c` 的 `kReadings` +
`ncl_syntec_reading_lookup()`）：点位里区名直接写这些名字，命令号自动取
`KrnlAPI`(200)、`dwCode` 取上表的码，`length`/`dtype` 仍按点位写（计数是
4 字节 `int32`）。名字大小写与下划线不敏感，带不带 `READ_` 前缀都认。
解析顺序是「具名读数 → 命令名 → 十进制命令号」，所以旧写法（区名 `KrnlAPI`
+ 偏移当 `dwCode`）不受影响。主轴那两个码只给了两个数、没给含义，因此没有
给它们起语义化的名字：等一次实机读数看到值再定。

---

## 11 本仓库实现（2026-09 落地）

§3.1/§3.2 的九项已经按抓到的形状实现，**client → 适配器 → 目标**三段都在仓库里：

| 在哪 | 是什么 |
|---|---|
| `clients/include/nclink/clients/syntec.h`（items 一节） | 九项的表（flags / code / 请求号 / 参数 A / 参数 B / 标志）+ `ncl_syntec_item_frame()`（36 字节，逐字段写）+ 应答读法（`ncl_syntec_item_u16()` / `_text()` / `_empty()`）；九个语义函数 `ncl_syntec_status()` … `ncl_syntec_warning()`；会话 `ncl_syntec_open()` / `ncl_syntec_close()` |
| `clients/syntec/syntec_codec.c` | `kItems[]`：九项逐字段照 §3.1 的表；查找大小写 / 下划线不敏感、`READ_` 前缀可省 |
| `clients/syntec/syntec_driver.c` | 会话（TCP + uSerial 回显校验 + 重试）、九项取数（FEED_SPEED 三帧 700 → 12 → 76）、以及原来的 `ncl_driver` 门面 |
| `plugins/syntec.c` | 适配器：9 个点位 + `/SESSION` 调试方法 + 审计原始帧。绑定沿用 client 的语义函数，只有两处覆盖（`LINE_NUMBER` 落成 string、主轴转速归 `/MOTOR@S1/SPEED`） |
| `clients/tests/test_syntec_driver.c` | 对 mock 控制器：STATUS 请求**逐字节**对照本节那张完整帧；九项各读一次；FEED_SPEED 的 700/12/76 顺序；WARNING 空正文 = `[]`；非 (0,0) 单位档与**非空报警**如实回"还读不了"。最后一段把 `plugins/syntec.c` 当模块装载、由宿主读九个点位 —— 就是本节说的"整机仿真" |
| `conf/syntec.json` | 交付配置（只有 `host` 一定要改） |

**如实标注的两处缺口**（不是猜，是没抓到）：

1. **FEED_SPEED 的单位换算表**：只有档位 (0,0)（系数 1.0）实测过；状态 12 是别的档位时回
   `NCL_ERR_UNAVAILABLE`，理由写"单位换算表待抓包"。
2. **WARNING 的非空条目布局**：只实测过"没报警 → 空正文 → `[]`"；有报警时回
   `NCL_ERR_UNAVAILABLE`，理由写"非空报警条目布局待抓包"。

读一侧是完整的；写（宏 / 参数 / 刀补 / PLC 写 / 程序上下行）在 client 里没有对应调用，
适配器也就不声明 —— 现场网关那一侧的新代同样只有读 + Open/Close/GetResponse。
