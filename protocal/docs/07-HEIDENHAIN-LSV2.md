# 07 · 海德汉 HEIDENHAIN（LSV2）实现规格书

> **证据**：🟢 全实证 —— 参考实现二进制（LSV2 驱动）+ `pyLSV2 1.7.0`（45 条命令全带语义与权限说明）
> **定位**：iTNC530 / TNC7 系列的标准远程协议，**文本命令式**，实现难度低

---

## 1. 速查

| 项 | 值 |
|---|---|
| 端口 | **TCP 19000** |
| 传输 | TCP 长连接；单会话独占 |
| 报文 | `[4 字节大端 payload 长度] + [4 字节命令名] + [payload]` |
| 命令 | **45 条 CMD / 23 条 RSP**（字符串命令，如 `C_FL` / `T_OK`） |
| 缓冲区 | 默认 256 字节（可协商放大） |
| 登录 | 分级：`INSPECT < FILE < MONITOR < DIAGNOSTICS < PLCDEBUG`（部分命令需特定权限） |
|参考实现| `pyLSV2`（Python，MIT） |
| 版本要求 | `R_DP`/`R_DR` 等需 iTNC530 ≥ 34049x 03 / 60642x 01 |

---

## 2. 连接建立

```
① TCP connect(host, 19000)
② 无握手
③ 按需登录：发 A_LG + "\0" + 用户名 + "\0" (+ 口令 + "\0")
   —— 不登录也能用部分命令（见 §4 权限列）
④ 保活：周期性 R_ST（请求远程状态）
⑤ 登出：A_LO + 用户名
```

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

---

## 8. 参考实现

| 来源 | 说明 |
|---|---|
| `pyLSV2`（参考架 `pylsv2-1.7.0-py3-none-any.whl`） | 完整实现：`low_level_com.py`（分帧）+ `client.py`（业务）+ `const.py`（命令表） |
| 数据 | CMD 45 条带说明 · RSP 23 条 · LSV2StatusCode 99 错误码 · MemoryType/ExecState/PgmState/ControlType（原始素材未随本目录提供） |
| 参考实现侧 | `驱动定义目录/` 中 LSV2 驱动定义（端口 19000） |
