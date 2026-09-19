# 设备协议实现规格书 · 总索引

> 用途：**按协议分册，供编码实现**。每册含：连接握手、字节级帧格式、命令码表、地址/数据类型、读写流程、错误码、实现坑。
> 来源：实测证据（二进制符号 / 头文件 / 抓包）＋ 厂商交付包（新代 / 三菱 / EZSocket）＋ 开源实现（pylsv2/snap7/pymcprotocol/…）＋ 第三方商业库对照
> 参考实现：下文出现的 wheel / 源码包名保存在本地参考架（`protocal/supp/` 的原始目录），未随本仓库提供。
> 证据分级：**🟢 实证**（二进制符号/头文件/抓包）· **🔵 常量**（协议常量/文档）· **🟡 公开**（文章/二手）· **🔴 缺**

---

## 一、分册目录

| # | 协议分册 | 端口 | 传输 | 证据 | 现成参考实现 |
|---|---|---|---|---|---|
| 00 | [通用-实现约定](00-通用-实现约定.md) | — | — | — | — |
| 01 | [FANUC CNC（FOCAS/Fwlib32）](01-FANUC-CNC-FOCAS.md) | 8193 | TCP | 🟢 881 函数 | pyfocas / pyfanuc |
| 02 | [FANUC 机器人（RMI + 老接口）](02-FANUC-机器人-RMI.md) | 8193 | TCP | 🟢 24 指令 | fanuc_rmi / fanucpy |
| 03 | [西门子 S7 PLC（S7comm）](03-SIEMENS-S7-PLC.md) | 102 | TCP/ISO-TSAP | 🟢 | python-snap7 |
| 04 | [西门子 840D sl（OPC UA）](04-SIEMENS-840D-OPCUA.md) | 4840 | TCP | 🟢 | asyncua |
| 05 | [**三菱 CNC M70/M80（MELDAS/GIOP）**](05-MITSUBISHI-CNC-M70-MELDAS.md) | 683 | TCP | 🟢 39接口/271方法 | EZSocket / 裸协议 |
| 06 | [三菱 PLC（MC/SLMP）](06-MITSUBISHI-PLC-MC-SLMP.md) | 5534 | TCP/UDP | 🟢 | pymcprotocol / melsec_mc_protocol |
| 07 | [海德汉 HEIDENHAIN（LSV2）](07-HEIDENHAIN-LSV2.md) | 19000 | TCP | 🟢 45命令 | pyLSV2 |
| 08 | [广州数控 GSK](08-GSK-广州数控.md) | 6000 | TCP | 🟡 | GSKRM.dll |
| 09 | [凯恩帝 KND（REST + DLL）](09-KND-凯恩帝.md) | 80 | HTTP | 🟢 面 | KAPI |
| 10 | [**新代 SYNTEC（RemoteCNC）**](10-SYNTEC-新代-RemoteCNC.md) | 8000 | TCP | 🟢 150方法 | SyntecRemoteAPI |
| 11 | [兄弟 Brother](11-BROTHER-兄弟.md) | 10000 | TCP | 🟢 10命令 | — |
| 12 | [欧姆龙 Omron（FINS）](12-OMRON-FINS.md) | 9600 | TCP/UDP | 🟢 19函数 | omronfins |
| 13 | [安川 Yaskawa（HSES）](13-YASKAWA-HSES.md) | 10040 | UDP | 🟢 173方法 | underautomation / HSES |
| 14 | [华数机器人 HSR（Hsc3）](14-HSR-华数-Hsc3.md) | 23234 | UDP | 🟢 ~170方法 | Hsc3Api |
| 15 | [通用 Modbus](15-MODBUS.md) | 502 | TCP/RTU/ASCII | 🔵 | pymodbus |
| 16 | [MTConnect](16-MTCONNECT.md) | 7878 | HTTP | 🟢 | mtconnect |
| 17 | [库卡 KUKA](17-KUKA.md) | 7000 | TCP | 🟡/✅ | KUKAVARPROXY |
| 18 | [埃夫特 Efort](18-EFORT-埃夫特.md) | 502/定制 | TCP | 🟡 | 商业库 ER7BC10 |
| 19 | [沈阳 i5（OPC UA）](19-沈阳i5-OPCUA.md) | 4840 | TCP | 🟢 | asyncua |
| 20 | [缺口：科德/精雕/海康](20-缺口-科德-精雕-海康.md) | — | — | 🔴 | — |
| 21 | [品牌盘点与缺口分析](21-品牌缺口盘点.md) | — | — | ★ | 商用清单差集分析 |
| 22 | [**Universal Robots（RTDE+Dashboard）**](22-UniversalRobots-RTDE.md) | 30004/29999 | TCP | 🟢 官方库全反解 | ur_rtde（官方） |
| 23 | [博世力士乐 Rexroth（OPC UA）](23-Rexroth-OPCUA.md) | 4840 | TCP | 🟡（复用 OPC UA 栈） | asyncua |
| 24 | [宝元 LNC（UDP+Modbus）](24-宝元LNC.md) | 1700 | UDP | 🟡 三路已确认 | ReCON（官方） |
| 25 | [哈斯 Haas（MDC）](25-哈斯Haas.md) | 参数143 | TCP | 🟡 参数+数据点已确认 | — |
| 26 | [**马扎克 MAZAK（Smart/Smooth）**](26-马扎克MAZAK.md) | 50100 | TCP | 🟡 免授权直连 | 纯 Socket |
| 27 | [**发格 Fagor（宏程序上报）**](27-发格Fagor.md) | 8899(自定) | TCP | 🟡 官方宏函数 | 无授权 |
| 28 | [**交付包现场 API 清单**](28-交付包现场API清单.md) | — | — | 🟢 现场二进制/Lua | 现场 iNC-BOX-200 的模块与数据项对照 |
| 29 | [**现场模型与驱动定义（cfg + 插件符号）**](29-现场模型与驱动定义.md) | 按品牌 | — | 🟢 现场 cfg/Lua/.dynsym | 逐品牌的模型项、连接参数、插件方法与项键；缺口的真正闭合 |
| 30 | [**外部资料：这批数据到底是什么（NC-Link 与西门子）**](30-外部资料-NC-Link与西门子.md) | — | — | 🟡 公开问答 · 🔵 团体标准 | NC-Link（T/CMTBA 1008.1…7-2020）出处、数据项对照、progStatus/opMode 取值表 |

---

## 二、按实现优先级排序（建议编码顺序）

### 第一批：通用 + 高价值（1-2 周可落地）
1. **Modbus**（15）—— 覆盖面最广，几乎所有设备都支持
2. **三菱 MC/SLMP**（06）—— 国内机床 PLC 主流
3. **FINS**（12）—— 欧姆龙全系
4. **S7comm**（03）—— 西门子全系
5. **MTConnect**（16）—— HTTP/XML，最易实现

### 第二批：CNC 深度接入（2-4 周）
6. **三菱 M70 MELDAS**（05）—— ★ 资料最全，裸协议可用
7. **新代 RemoteCNC**（10）—— ★ 150 方法全覆盖
8. **海德汉 LSV2**（07）—— ★ 45 命令全带权限说明
9. **FANUC FOCAS**（01）—— 需 whitelist 但函数表全

### 第三批：机器人 + 长尾
10. **FANUC 机器人 RMI**（02）· **安川 HSES**（13）· **KUKA**（17）· **埃夫特**（18）· **华数**（14）
11. **GSK**（08）· **KND**（09）· **Brother**（11）· **OPC UA**（04/19）

### 第二批补齐（2026-09 已完成）★
Universal Robots RTDE · 力士乐 Rexroth · 宝元 LNC · 哈斯 Haas —— 见 22-25 册

### 已评估否决
| 品牌 | 否决理由 |
|---|---|
| **华中数控 HNC** | hncAPI 为 **Windows-only DLL** 且**必须部署厂商上位机软件** |
| **大隈 OKUMA** | **DevelopKit 是 Windows 程序**；跨平台免授权方案清单不含大隈；OSP 系统封闭 |
| （马扎克 640/MATRIX） | 只能 Windows 装插件 UDP 上报（**Smart/Smooth 系列不受此限，已入库 26 册**） |

> 收录准则（三条硬门槛：跨平台 / 免授权 / 可自实现）见 [21 册 §4.5](21-品牌缺口盘点.md)

### 缺口（需外部资源）

| 缺口 | 现在缺什么 | 已补什么 |
|---|---|---|
| 科德 GNC62 | 只差设备侧 HTTP 请求形状 | 🟢 模型项 78 条 + 插件 23 个方法 + 14 个项键（[29 册 §4.3](29-现场模型与驱动定义.md)） |
| 精雕 JD50 | 只差设备侧请求形状 | 🟢 模型项 14 条 + Lua 映射（[29 册 §4](29-现场模型与驱动定义.md)） |
| 海康 HCNetSDK | 相机 SDK 头文件 | 🟡 现场是 `libcamera.so`（`CAMERA::getFeature/getCV2`），非 HCNetSDK |
| GSK | 只差设备侧 HTTP 请求形状 | 🟢 模型项 16 条 + 插件 22 个方法 + 13 个项键（[29 册 §4.3](29-现场模型与驱动定义.md)） |
| 马扎克 MAZAK | MT 授权 | — |
| 大隈 OKUMA | DevelopKit（Windows） | — |
| 发格 Fagor | FCOM SDK | — |

> KND 已从缺口移出：09 册 §3 已按现场映射层改成实测端点表（原 `/api/v1.2/*` 是猜的）。
>
> **"缺字节布局"这个说法要分开看**：FANUC FOCAS / 840D / i5 / 华数 是按**供应商 SDK 或
> OPC UA 节点集**接入的，语义面拿到就够了；真正只缺"设备侧请求形状"的只有
> GSK / 科德 / Mitsubishi-HTTP 与相机这几家自研 socket/HTTP 的，而且可以用
> qemu 在本地仿真抓包补上（[29 册 §6](29-现场模型与驱动定义.md)）。

### 品牌缺口（详见 [21 册](21-品牌缺口盘点.md)）★

**商用清单覆盖情况**：✅ 马扎克（26册，限 Smart/Smooth）· ✅ 发格（27册）· ⛔ 大隈（否决）· ⛔ 华中（否决）
**欧美/机器人空白**：力士乐 Rexroth（OPC UA）· 施耐德 Modicon · 罗克韦尔 AB 独立册 · **Universal Robots（协作第一）**
**已补齐**：UR RTDE · 力士乐 · 宝元 · 哈斯；**已否决**：华中数控（Windows-only + 需厂商软件）

---

## 三、跨协议通用设计（写给 CaNC 的统一数据面）

| 层 | 设计要点 | 参考来源 |
|---|---|---|
| **地址模型** | 统一 `DeviceAddress {区, 偏移, 位, 长度, 类型}`，各协议写适配器 | 商业库 `DeviceAddressDataBase` + 20 个子类 |
| **响应信封** | `{code, success, value, message}` 统一返回 |参考实现`T2Res` + 商业库 `OperateResult<T>` |
| **会话管理** | `Open(transport, host, port, timeout)` → `sessionId` → 业务调用 → `Close` |参考实现`/Open/TCP` 模型 |
| **连接模式** | 长连接 + 心跳 + 自动重连（超时分级：连接/读写/业务） | 三菱 `lTimeOut`、S7 PDU 协商 |
| **数据类型** | 统一 7 类：BIT/BYTE/INT16/INT32/FLOAT32/FLOAT64/STRING | 见各分册「数据类型」节 |
| **错误分级** | 传输层（超时/断开）· 协议层（错误码）· 业务层（数据无效） | ME_DEV_* / LSV2StatusCode / EZ_ERR_* |
| **批量读优化** | 连续地址合并读取；跨协议统一为 `ReadBatch(list<Address>)` | MC 批量 / FOCAS 多变量 |
| **测试** | 每协议一套虚拟靶机（商业库 47 个虚拟服务器可直接用） | 商业库 |

---

## 四、通用实现约定

见 [00-通用-实现约定.md](00-通用-实现约定.md)：错误处理、重连策略、字节序、超时参数、日志与审计、安全边界。

---

## 五、本目录与其它文档的关系

| 文档 | 作用 |
|---|---|

| `../协议与设备支持总清单.md` | 覆盖度与缺口总览 |
| **`本目录/*.md`** | **★ 编码落地规格书（按协议分册）** |

> 完整 API 表与系统性梳理等原始素材未随本目录提供。
