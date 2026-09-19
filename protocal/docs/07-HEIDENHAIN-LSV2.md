# 07 · 海德汉 HEIDENHAIN（LSV2）实现规格书

> **证据**：🟢 全实证 —— 参考实现二进制（LSV2 驱动）+ `pyLSV2 1.7.0`（45 条命令全带语义与权限说明）+ **本包设备侧实测（§2.1，握手与 9 个方法已闭环）**
> **定位**：iTNC530 / TNC7 系列的标准远程协议，**文本命令式**，实现难度低

---

## 1. 速查

| 项 | 值 |
|---|---|
| 端口 | **TCP 19000** |
| 传输 | TCP 长连接；单会话独占 |
| 报文 | `[4 字节大端 payload 长度] + [4 字节命令名] + [payload]` |
| 命令 | **45 条 CMD / 23 条 RSP**（字符串命令，如 `C_FL` / `T_OK`） |
| 缓冲区 | 默认 256 字节；握手用 `C_CC` 协商（本包实测 `max_block_length=4096` → `C_CC 00 07`） |
| 登录 | 分级：`INSPECT < FILE < MONITOR < DIAGNOSTICS < PLCDEBUG`（部分命令需特定权限） |
|参考实现| `pyLSV2`（Python，MIT） |
| 版本要求 | `R_DP`/`R_DR` 等需 iTNC530 ≥ 34049x 03 / 60642x 01 |

---

## 2. 连接建立

```
① TCP connect(host, 19000)
② 握手（🟢 本包实测，见 §2.1）：A_LG 登录 → R_VR ×7 读版本 → R_PR 读系统参数
   → C_CC 协商缓冲区 → 再 A_LG 登录 FILETRANSFER
③ 按需登录：发 A_LG + "\0" + 用户名 + "\0" (+ 口令 + "\0")
   —— 不登录也能用部分命令（见 §4 权限列）
④ 保活：周期性 R_ST（请求远程状态）
⑤ 登出：A_LO + 用户名
```

### 2.1 设备侧实测（🟢 2026-09，`tools/site-probe/lsv2_probe{1..5}.sh`）

把网关注到假机床（`mock.py` 监听 19000）后 `POST /LSV2/Open/TCP`，网关自己把
整段握手走完；按 pyLSV2 的约定逐条答上（`A_LG` → `T_OK`、`R_VR` → `S_VR` 文本、
`R_PR` → `S_PR` + 120 字节系统参数、`C_CC` → `T_OK`）后 **Open/TCP 直接返回
`success: true` + connectionId**，整段是：

```
→ A_LG "INSPECT\0"                       16B  （只读登录）
↓ T_OK
→ R_VR 01 / 02 / 03 / 04 / 05 / 06 / 07  9B×7 （CONTROL/NC_VERSION/PLC_VERSION/
                                                 OPTIONS/ID/RELEASE_TYPE/SPLC_VERSION）
↓ S_VR "<字符串>\0"                      每项一条
→ R_PR                                   8B   （系统参数，应答 120 字节）
↓ S_PR + 120B  （pyLSV2 misc.decode_system_parameters 的 "!14L8B8L2BH4B2L2HL"，
                 其中 max_block_length 在偏移 98、2 字节大端 → 决定下面的 C_CC 值）
→ C_CC 00 07                             10B  （协商 4096 字节缓冲）
↓ T_OK
→ C_CC 00 13                             10B  （第二档协商）
↓ T_OK
→ A_LG "FILE\0"                          13B  （文件传输登录）
```

`max_block_length` 给 0 时网关直接回 `unknown buffer size`（Open/TCP 失败）；
给 4096 时协商值变成 `00 07`（`ParCCC` 枚举）。

**逐项 telegram（🟢 实测，参数码就是 pyLSV2 的 `ParRRI`）**：

| 方法 | 登录 | 设备侧 telegram | 应答 | 实测结果 |
|---|---|---|---|---|
| `GetVersion` | INSPECT | 上面那 7 条 `R_VR` | `S_VR` + 文本 | ✅ `{control:"TNC640", nc_version:"340595-07", …}` 七项全出值 |
| `GetSystemParameter` | INSPECT | `R_PR` | `S_PR` + 120/124B | ✅ 29 个字段（`max_block_length` 等） |
| `GetAxesLocation` | **DNC** | `R_RI` + `00 16`（22） | `S_RI` | ✅ `{axes:{X:1.234, Y:-5.678, Z:9.5}}` |
| `GetExecutionStatus` | DNC | `R_RI` + `00 17`（23） | `S_RI` + u16 | ✅ `{state:1, text:"MDI"}` |
| `GetProgramStack` | DNC | `R_RI` + `00 18`（24） | `S_RI` + u32 + 两个 NUL 串 | ✅ `{line:300, main_pgm:"MAIN.H", current_pgm:"SUB.H"}` |
| `GetOverrideInfo` | DNC | `R_RI` + `00 19`（25） | `S_RI` + 3×u32 | ⚠️ 出值但**整除截断**（见 §7.8） |
| `GetProgramStatus` | DNC | `R_RI` + `00 1a`（26） | `S_RI` + u16 | ✅ `{state:2, text:"FINISHED"}`（2 = `PgmState.FINISHED`） |
| `GetErrorMessages` | DNC | `R_RI` + `00 1b`（27）→ 循环 `00 1c`（28） | `S_RI` + 报文；**结束时回 `T_ER` + `00 39`** | ✅ `{errors:[{Class:1,Group:2,Number:3,Text:"TEXT"}]}` |
| `GetSpindleToolStatus` | DNC | `R_RI` + `00 33`（51） | `S_RI` + u32 + 2×u16 + 2×double(**小端**) | 形状对（`Number/Index/Axis`），`Length/Radius` 用 `<d` |
| `GetDirectoryInfo` | FILE | `R_DI` | `S_DI` | ✅ `{Path:"TNC:/", FreeSize:65536, DirectoryAttributes:["READ","WRIT"], Attributes:"AAAA…="}`（§2.2） |
| `GetDirectoryContent` | FILE | `R_DR` + `00`（`ParRDR.SINGLE`）+ 收一条回一条 `T_OK` | `S_DR` 多条，最后用 `T_FD` 收尾 | ✅ `{fileInfos:[{Name:"TEST.H",Size:12345,…}]}`（§2.2） |
| `GetFileInfo` | FILE | `R_FI` | `S_FI` | ✅ `{Name:"TEST.H", Size:12345, Timestamp:"2021-01-14T08:25:36Z", Attributes:98, IsDirectory:true}` |
| `ChangeDirectory` | FILE | `C_DC` + `00` | `T_OK` | ✅ `value: None`（成功） |
| `GetFileList` | FILE | `C_DC` + 路径串（`TNC:\0`）→ `R_DI` → 递归 `walkDir` | `T_OK`/`S_DI`/`S_DR` | 请求链已抓到（walk 是递归的，按 C_DC 换目录逐层取） |
| `ReadPLC` | **PLCDEBUG** | `A_LG "PLCDEBUG\0"` 之后**不再发报文** | — | 🔴 **这条路径在本包里没跑通**（`values` 恒为 `[]`；没有 connectionId 时还 panic） |

注：`R_RI` 的参数码与 pyLSV2 `ParRRI` 完全对上（16=22 AXIS_LOCATION、17=23 EXEC_STATE、
18=24 SELECTED_PGM、19=25 OVERRIDE、1a=26 PGM_STATE、1b=27 FIRST_ERROR、1c=28 NEXT_ERROR、
33=51 CURRENT_TOOL），这是**独立第三方实现与本包网关互为佐证**的一条。

**报文分帧的两个坑**（第一轮就踩了）：

1. **块名在偏移 4..8、载荷从偏移 8 开始** —— `[4B 长度][4B 块名][载荷]`，
   别把偏移 8 当成块名（用 `mock.py` 的 `MAP:4:6:…` 才挑得中）。
2. **`MAP` 的键长要等于"块名 + 参数"的实际字节数**：`A_LG` 16B → 键含用户名，
   `R_VR` 9B → 键 5 字节，`R_PR` 8B → 键 4 字节，`R_RI` 10B → 键 6 字节。

### 2.2 文件类应答（🟢 2026-09 第七轮，`lsv2_probe6.sh`）

三条应答的载荷布局（与 pyLSV2 `misc.decode_directory_info` / `decode_file_system_info`
逐字节对得上）：

```
S_DI = free_size(u32 大端) | 32 × 4 字节属性串 | 32 字节属性位图 | 路径串（NUL 结尾）
       → {Path, FreeSize, DirectoryAttributes[], Attributes(base64 的 32 字节)}
S_FI = size(u32) | timestamp(u32) | attributes(u32) | 名字串（NUL 结尾）
       → {Path, Name, Size, Timestamp, Attributes, IsFile, IsDirectory, IsWriteProtected}
S_DR = 同 S_FI，一条一个文件项
```

实测（`lsv2_probe6.sh`，S_DI 给 `free_size=65536`、属性串 `READ`/`WRIT`、路径 `TNC:\0`）：

```
GetDirectoryInfo   → {Path:"TNC:/", FreeSize:65536,
                      DirectoryAttributes:["READ","WRIT"], Attributes:"AAAA…="}
GetFileInfo        → {Name:"TEST.H", Size:12345, Timestamp:"2021-01-14T08:25:36Z",
                      Attributes:98, IsDirectory:true, IsFile:false,
                      IsWriteProtected:false}          # 98 = 0x62 = 0x02|0x20|0x40
GetDirectoryContent→ {fileInfos:[{…同上一条…}]}
ChangeDirectory    → None（成功）
```

**块传输的真实节奏**（`R_DR` 那条，🟢 实测）：网关发 `R_DR 00` → 收一条 `S_DR` →
**回一条 `T_OK` 请求** → 再收 `S_DR` → …；控制侧用**非 `S_DR` 的响应**（`T_FD`）结束整段。
探针里只要让 `MAP` 把 `T_OK` 请求答成 `T_FD`，就只回一条 `S_DR` 且干净收尾。

两个已记下的细节：

- 路径里的反斜杠会被换成 `/`（`TNC:\` → `TNC:/`）；`Attributes` 是 32 字节位图，JSON 里是 base64。
- `IsWriteProtected` 的位**不在** `{0x02,0x20,0x40}` 里（喂 `0x62` 时为 `false`，
  而 `IsDirectory` 已由 `0x20` 命中为 `true`）——现场若要看写保护，按真机对一次这一位。

---

## 3. 帧格式（字节级）

### 3.1 请求

```
偏移  长度  内容            示例              说明
0     4     payload 长度    12 00 00 00       **大端**（struct.pack("!L")），不含命令名 4 字节
4     4     命令名          "C_FL"            4 个 ASCII 字符
8     N     payload（可选） 文件名 + \0 ...    多数命令要求 NUL 结尾的字符串
```

### 3.2 响应

```
偏移  长度  内容            说明
0     4     payload 长度    大端
4     4     响应名          T_OK / T_ER / S_XX / M_CC …
8     N     payload        错误码 / 数据
```

### 3.3 完整帧示例（读文件）

```
→  0c 00 00 00  52 5f 46 4c   (R_FL)  74 65 73 74 2e 68 00     "R_FL" + "test.h\0"
←  30 00 00 00  53 5f 46 4c   3c 21 2d 2d ...                  "S_FL" + 文件数据块
←  ...                                                          （循环接收直到 T_FD）
```

---

## 4. 命令表（45 条 CMD，含权限）

### 4.1 登录

| 命令 | 说明 | 权限 |
|---|---|---|
| `A_LG` | 登录（后随用户名 + 可选口令） | 无需 |
| `A_LO` | 登出（后随可选用户名） | 任意已登录 |

**登录名**：`INSPECT`（只读诊断）· `FILE`（文件操作）· `MONITOR`（屏幕监视）· `DIAGNOSTICS`（诊断，可发按键）· `PLCDEBUG`（PLC 调试，可写机器参数）
> 登录名来源：`pyLSV2/const.py` 的 `Login` 类 + 命令文档字符串中的 `requires XXX login priviliege`

### 4.2 控制类（C_*）

| 命令 | 说明 | 权限 |
|---|---|---|
| `C_CC` | 设置系统命令 | — |
| `C_DC` | **切换工作目录**（后随 NUL 结尾字符串） | FILE |
| `C_DT` | 设置日期时间 | DIAGNOSTICS |
| `C_DD` | 删除目录 | FILE |
| `C_DM` | 新建目录 | FILE |
| `C_EK` | **发送按键码**（模拟面板按键） | DIAGNOSTICS |
| `C_FA` | 修改文件属性 | FILE |
| `C_FC` | 本地文件复制（源 + NUL + 目标 + NUL） | FILE |
| `C_FD` | **删除文件** | FILE |
| `C_FL` | **上传文件到控制器**（NUL 结尾文件名） | FILE |
| `C_FR` | 本地文件移动 | FILE |
| `C_LK` | **锁定/解锁键盘输入** | DIAGNOSTICS |
| `C_MC` | **设置机器参数**（flags + 名称 + 值） | PLCDEBUG |
| `C_ST` | 设置状态（仅当前登录） | 任意 |

### 4.3 读取类（R_*）

| 命令 | 说明 | 权限 |
|---|---|---|
| `R_CD` | 字符集 | MONITOR |
| `R_DI` | **目录信息** | FILE |
| `R_DP` | 从数据路径读数据（iTNC530 34049x03+） | — |
| `R_DR` | **目录内容** | FILE |
| `R_DT` | 日期时间 | DIAGNOSTICS |
| `R_FI` | **文件信息**（NUL 结尾文件名） | FILE |
| `R_FL` | **从控制器下载文件** | FILE |
| `R_FO` | 字体定义 | MONITOR |
| `R_LB` | 日志缓冲 | DIAGNOSTICS |
| `R_MB` | **读 PLC 内存**（4 字节地址 + 1 字节长度） | INSPECT |
| `R_MC` | 读机器参数（NUL 结尾参数号/路径） | — |
| `R_MP` | 读机器参数 | INSPECT |
| `R_PD` | 托盘定义 | FILE 或 MONITOR |
| `R_PR` | **读控制参数** | INSPECT |
| `R_RI` | **读控制器状态**（后随 16 位选择码）★采集主命令 | — |
| `R_RS` | 寄存器状态 | INSPECT |
| `R_SD` | **屏幕截图** | FILE |
| `R_SE` | 屏幕窗口元素信息 | MONITOR |
| `R_SP` | 屏幕调色板 | MONITOR |
| `R_SS` | 当前活动屏幕 | INSPECT |
| `R_ST` | **远程状态**（保活用） | 任意 |
| `R_SW` | 屏幕窗口信息 | MONITOR |
| `R_VR` | **控制器版本信息** ★首连必发 | INSPECT |
| `R_WD` | 窗口定义 | MONITOR |

### 4.4 响应（23 条 RSP）

| 响应 | 含义 |
|---|---|
| `T_OK` | 事务完成 |
| `T_ER` | **错误（后随错误码）** |
| `T_FD` | **文件传输完成** |
| `T_BD` | 文件传输出错（后随更多数据） |
| `M_CC` | 操作完成（耗时操作） |
| `S_DI` / `S_DR` | `R_DI` / `R_DR` 的应答（后随数据） |
| `S_DP` | `R_DP` 应答（后随数据值） |
| `S_FI` | `R_FI` 应答（后随数据） |
| `S_FL` | **文件数据块**（`R_FL` 应答，循环接收） |
| `S_MB` | `R_MB` 应答（后随实际数据） |
| `S_MC` / `S_PR` | 参数读取应答（后随数据） |
| `S_RI` | `R_RI` 应答（后随数据）★ |
| `S_ST` | 远程状态应答 |
| `S_VR` | 版本应答（后随数据）★ |
| `NONE` / `UNKN` | 内部使用（无响应/未知响应） |

---

## 5. 数据模型（枚举，来自 `pyLSV2/const.py`）

### 5.1 存储器类型（`R_MB` / `R_PR` 用）

| 枚举 | 值 | 枚举 | 值 |
|---|---|---|---|
| `MARKER`（M） | 1 | `WORD` | 7 |
| `INPUT`（I） | 2 | `DWORD` | 8 |
| `OUTPUT`（O） | 3 | `STRING` | 9 |
| `COUNTER` | 4 | `INPUT_WORD` | 10 |
| `TIMER` | 5 | `OUTPUT_WORD` | 11 |
| `BYTE` | 6 | `OUTPUT_DWORD` / `INPUT_DWORD` | 12 / 13 |

### 5.2 机床类型（`ControlType`）

```
MILL_NEW=1  MILL_OLD=2  LATHE_NEW=3  LATHE_OLD=4  TNC7=5  MILLPLUS=6  UNKNOWN=-1
```

### 5.3 执行状态（`ExecState`）★ 采集用

```
MANUAL=0   MDI=1   PASS_REFERENCES=2   SINGLE_STEP=3   AUTOMATIC=4   UNDEFINED=5
```

### 5.4 程序状态（`PgmState`）★ 采集用

```
STARTED=0  STOPPED=1  FINISHED=2  CANCELLED=3  INTERRUPTED=4  ERROR=5  ERROR_CLEARED=6  IDLE=7  UNDEFINED=8
```

### 5.5 通道类型（`ChannelType`）

```
TYPE0..TYPE4 = 0..4, TYPE5 = 10, UNKNOWN = -1
```

---

## 6. 错误码（`LSV2StatusCode`，99 个，节选）

| 码 | 名称 | 码 | 名称 |
|---|---|---|---|
| 0 | `LSV2_OK` | 12 | `LSV2_TIMEOUT3` |
| 1 | `LSV2_TIMEOUT` | 16 | `LSV2_NO_MESSAGE` |
| 2 | `LSV2_NO_ENQ` | 5 | `LSV2_TO_LONG` |
| 3 | `LSV2_TIMEOUT2` | 6 | `LSV2_WRONG_BBC`（**校验错**） |
| 4 | `LSV2_WRONG_CHAR` | 7 | `LSV2_NO_EOT` |

> 全集 99 项（原始素材未随本目录提供）。
> `T_ER` 响应后随的错误码需查 `pyLSV2/locales/*/error_text.po`（德/英文本）。

---

## 7. 实现要点

1. **长度字段是大端**（`struct.pack("!L")`），与 PLC 类协议相反，容易写错。
2. **payload 里的字符串必须 NUL 结尾**，长度含 NUL。
3. **缓冲区 256 字节限制**：大文件传输靠 `S_FL` 分块循环，不要一次发完。
4. **权限分级**：先 `R_VR` 探版本，再按需 `A_LG` 登录 —— 不登录只能做有限操作。
5. **`R_RI` 是采集主命令**（读控制器状态），16 位选择码决定读哪类信息。
6. **`C_EK` 可模拟按键、`C_MC` 可改机器参数** —— 生产禁用，必须权限墙。
7. **iTNC530 与 TNC7 命令集有差异**（`R_DP`/`R_DR` 有最低版本要求）。
8. **倍率（`R_RI` 25）被整除截断**：报文里是"百分数 ×100"的大端 u32，本包网关
   按**整数除法**换算（喂 `150/250/350` → 回 `1/2/3`），所以 50% 会变成 0、
   99.5% 变成 0 —— 现场"倍率显示为 0"先看这一条（§2.1 实测）。
9. **报警列表靠 `T_ER + 00 39`（`T_ER_NO_NEXT_ERROR`=57）收尾**：假机床如果一直回
   `S_RI`，网关会**无限循环**地重复 `R_RI 00 1c`（第四轮就是这样卡到超时的）。
10. **`max_block_length` 为 0 时 Open/TCP 直接失败**（`unknown buffer size`）——
    应答 `R_PR` 时必须把偏移 98 那 2 个字节填对（本包实测 4096 → `C_CC 00 07`）。
11. **`ReadPLC` 本包不可用**（PLCDEBUG 登录之后就不再发报文，返回空数组；
    `lsv2_mod_plc.lua` 那套 `plc_feed_speed=4448`/`plc_spindle_speed=18376`/
    `plc_status=4352` 的地址采不到值）。

---

## 8. 参考实现

| 来源 | 说明 |
|---|---|
| `pyLSV2`（参考架 `pylsv2-1.7.0-py3-none-any.whl`） | 完整实现：`low_level_com.py`（分帧）+ `client.py`（业务）+ `const.py`（命令表） |
| 数据 | CMD 45 条带说明 · RSP 23 条 · LSV2StatusCode 99 错误码 · MemoryType/ExecState/PgmState/ControlType（原始素材未随本目录提供） |
| 参考实现侧 | `驱动定义目录/` 中 LSV2 驱动定义（端口 19000） |
| 上游源码（本轮对照用） | `github.com/drunsinn/pyLSV2`（master：`pyLSV2/{low_level_com,client,const,misc,dat_cls}.py`）——§2.1 的握手/应答结构都按它对照验证过 |
| 现场 Lua 驱动 | `app1/nclink-service/lua/lua_mod/lsv2_mod.lua`（NC-Link 项 → LSV2 方法的映射，含 `/STATUS` 状态枚举 0=running、2/3/7/8=free、其余 holding）与 `lsv2_mod_plc.lua`（PLC 内存版，本包 `ReadPLC` 不通） |
