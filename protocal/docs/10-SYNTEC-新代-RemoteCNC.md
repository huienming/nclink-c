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
即小端）。**它是给哪一段算的、放在报文哪个位置，待补**——只在文件类路径上被引用，
业务读写路径目前没看到。

### 10.3 下一步（按此顺序补齐，每步都留出处）

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
