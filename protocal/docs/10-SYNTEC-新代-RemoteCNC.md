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
