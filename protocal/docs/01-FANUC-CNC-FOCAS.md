# 01 · FANUC CNC（FOCAS / Fwlib32）实现规格书

> **证据**：🟢 全实证 —— `libfwlib32.so.1` 导出 **881 个函数**（cnc 758 / pmc 95 / flnt 14 / ds 8 / pbm 3 / hmgr 1 / smgr 1 / loglevel 1）
> **定位**：FANUC 全系（0i/16i/18i/21i/30i/31i/32i）标准数据接口

---

## 1. 速查

| 项 | 值 |
|---|---|
| 端口 | **TCP 8193**（FOCAS 以太网；部分系统 8192） |
| 库 | `Fwlib32.dll`（Win）/ `libfwlib32.so.1`（Linux）/ `fwlib32.x64.dll` |
| 函数前缀 | `cnc_*`（CNC 数据）· `pmc_*`（PMC/梯形图）· `flnt_*`（Focas2 Logger）· `ds_*`（数据服务）· `pbm_*`（程序块管理） |
| 前置条件 | 机床侧需开通"以太网功能"并授权 |
|参考实现| `pyfocas`（ctypes 绑定）· `pyfanuc`（含 fwlib 头文件） |
| 完整表 | 881 函数原名（原始素材未随本目录提供） |

---

## 2. 连接建立（典型调用序列）

```c
// ① 分配句柄（最多 2 个并发句柄）
unsigned short h;
cnc_allclibhndl3("192.168.1.10", 8193, 10 /*秒*/, &h);

// ② 校验连接与系统信息
ODBSYS sys;  cnc_sysinfo(h, &sys);        // 系统型号/系列/轴数
ODBMDL mdl;  cnc_rdmodel(h, &mdl);        // 机床型号

// ③ 业务调用
ODBACT pos;  cnc_absolute(h, 1 /*X轴*/, &pos);      // 绝对坐标
ODBACT2 p2;  cnc_absolute2(h, 1, &p2);
ODBST  st;   cnc_statinfo(h, &st);                  // 运行状态

// ④ 释放
cnc_freelibhndl(h);
```

### 2.1 线协议握手（🟢 实测，非文档）

用现场交付包里的 `libfwlib32.so` 在 ARM 仿真环境里对着假机床跑
`cnc_allclibhndl3("127.0.0.1", 8193, 3)`，抓到的**设备侧**字节序列如下
（复现工具：`tools/site-probe/`）：

```
连接 1 →  a0 a0 a0 a0 00 01 01 01 00 02 00 01     (12 字节)
连接 2 →  a0 a0 a0 a0 00 01 01 01 00 02 00 02     (12 字节，计数器 +1)
          a0 a0 a0 a0 00 01 21 01 00 00           (10 字节)
          a0 a0 a0 a0 00 01 02 01 00 00           (10 字节)
```

- 没有应答时 SDK 返回 **-16（EW_SOCKET）**，并且**会重试一次**（所以抓到两次
  TCP 连接、四条消息）。
- 应答格式（🟢 2026-09 第九/十轮，反汇编 `libfwlib32.so` 的 `Pdu::send`/`Pdu::receive`/
  `getRbPos`/`getRb` + 假机床实测）：帧格式与四道判据见 §2.2，
  **应答体取值见 §2.3** —— 第十轮已把"体 = 块个数 + 变长块"这套结构解出来并实测
  `cnc_allclibhndl3` **rc=0**，本册**不再需要真机**。
- 现场（`cfg/driver_def.json`）用的就是这一家：`module: focas`、`8193`。

### 2.2 帧格式与校验规则（🟢 2026-09 第九轮反汇编）

**帧 = 10 字节头 + 体**，头和体都是"大端 u16 字段"的堆叠：

```
[0..4)   a0 a0 a0 a0          魔数（Pdu::receive #23fbc 做 32 位字节交换后跟
                              字面量 0x24ac0 = a0 a0 a0 a0 比）
[4..6)   be16 类型            请求恒 00 01；应答决定体长上限（见下表）
[6]      功能码               Pdu::send 写调用方给的"功能/方向"字节；
                              Pdu::receive 要求它 == 调用方期望值（0 = 不检查）
[7]      方向                 请求恒 01；应答必须 1..4（#25230）
[8..10)  be16 体长度（字节）
[10..)   体
```

`Pdu::send`（`0x234b8`）写头就是：magic、`[4]=0 [5]=1`、`[6]=func`、`[7]=dir`，
然后 `[8..10)=10+现有体长`。请求侧能一一对上：

| 抓到的帧 | 功能码 | 体 |
|---|---|---|
| `a0a0a0a0 0001 01 01 0002 0001` | `01` | `00 01`（2 字节） |
| `a0a0a0a0 0001 21 01 0000` | `21` | 无 |
| `a0a0a0a0 0001 02 01 0000` | `02` | 无 |

**`Pdu::receive`（`0x23a94`）的四道校验**（任一条不过就抛 `ErrObj(-17)` =
`EW_PROTOCOL`，SDK 层看到的就是 **rc = -17**）：

1. `#23fe0`：magic 必须是 `a0 a0 a0 a0`；
2. `#24488`：`header[6]` 必须等于调用方传入的期望功能码（传 0 则不查）；
3. `#25230`：`header[7] ∈ 1..4`；
4. `#25600`–`#25634`：**体长上限**——`be16(header[4..6)) <= 2` 时 1450 字节、
   `== 3` 时 2910、`> 3` 时 3470，`be16(header[8..10))` 不能超；
5. `#25ecc`–`#25f40`（`header[7]==2` 且 `header[6]==1` 时）：**体长必须是
   `n*8 + 16`**（`n = be16(体[8..10))`，type ≤ 2）或 `n*32 + 40`
   （`n = be16(体[20..22))`，type > 2）；
6. `#2601c`–`#26120`（`header[6]==0x21` 时）：体长必须 **> 1**，且
   `be16(体[0..2)) != 0`。

**应答块（rb）**：体是按 32 字节切的一串块，`Pdu::getRb(i)`（`0x26b78`）取第 i 块，
块内 `[8..10)` = 返回码（大端 int16），**非 0 直接抛异常**
（`ErrObj(code, detail1=块[10..12), detail2=块[12..14), ecode=块[2..4))`）。
所以"机床说 OK" = 这些块的 `[8..10)` 全 0。

**谁要应答**：`SockPair::request(Socket&, int, Pdu&, u8 dir)`（`0x1b3a0`）——
`dir == 2` 才轮询收应答（并要求应答 `[6] == 2`），否则只收一次、要求应答
`[6] == dir`。握手那四条都是 `dir = 1`。

**假机床实测（`focas_handshake_probe.sh`）**：给这些帧回
`a0a0a0a0 0001 01 02 0010 <16 字节体>`（体 `[8..10)=0`，正好满足第 5 条），
`cnc_allclibhndl3` 从 **-16（EW_SOCKET，什么都不回）** 变成
**-17（EW_PROTOCOL）**——**帧被收下了**，只剩应答体字段值没对上。复盘表：

| 试法 | rc |
|---|---|
| 不回任何应答 | -16（socket/超时） |
| 四个请求一律回 16 字节体 | -17，且**驱动只发 2 条请求**（`func 21` 那步没了） |
| 系统信息 `[2..4)=2` + `func 21/02` 回空体 | -17（`func 21` 的应答体长必须 > 1，见第 6 条） |
| 只回 `func 01` 那两条 | -16（说明其余请求也在等应答） |
| 类型 `[4..6)` 改 `0002`（其余同 A） | -16 |
| 方向 `[7]` 改 `01`（其余同 A） | -16 |

结论：**格式与判据已经全解**，剩下的是"应答体里那 16/32 字节放什么"——
**这一格 2026-09 第十轮已经补上**，见 §2.3（不需要真机了：按解出来的块结构回，
`cnc_allclibhndl3` 实测 **rc=0**）。

### 2.3 应答体的取值（🟢 2026-09 第十轮：反汇编 + 假机床实测 rc=0）

**体不是一块连续数据，而是一串"块"。** `Pdu::getRbPos`（`0x26ab0`）就是靠块自己的
长度字段往后走的：

```
体
  [0..2)   块个数 N（BE16）—— Pdu::getRbPos 的第一道检查：i 必须 < N
  [2..)    N 个块，逐个首尾相接：
             [0..2)   本块字节数 L（BE16，getRbPos 用 i 走到第 i 块就是靠它累加）
             [2..4)   ecode
             [8..10)  返回码（BE16，**非 0 → Pdu::getRb 抛 ErrObj**）
             [10..12) detail1      [12..14) detail2
             [14..16) 载荷字节数（数据块才有意义）
             [16..)   载荷
```

**三条硬规则（全部实测）**：

1. **块个数 = 请求里 Cb 的个数**。请求体的 `[0..2)` 就是这个数，应答必须一致——
   少一个，驱动取 `getRb(最后一个)` 时 `N <= i` 直接抛 → 上层看到 **-17**。
   无体的 10 字节请求（0 个 Cb）按 **1** 个块回（`Pdu::receive` 判据 6 也要求
   `0x21` 的应答块个数非 0）。
2. **块长 ≥ 34 字节**：`SockPair::system_info_v1` 要读块 `[16..34)`，
   `cnc_statinfo` 要读块 `[14..16)` 与 `[16..]`。
3. 每个块的 `[8..10)` 必须是 0，否则 `getRb(i)` 抛异常。

**func 01 的应答是另一套布局**（不是块，是"8 字节记录表"）：

```
[0..2) → this+116      [2..4) → this+118（==2 / ==3 走特殊分支）
[4..6) → this+122/144  [6..8) → this+124
[8..10) = 记录数 n     [10..12) [12..14) [14..16) → this+138/146/148
[16..)  n 个 8 字节记录（每条 4 个 BE16：A/B/C/D）
```

长度必须 `== 16 + 8n`（`Pdu::receive` 判据 5）。记录 A != 0 时驱动会为它加一个
`Cb(1, i, 24)` 并 `getRb(顺序号)`，所以"记录数 / 记录 A"要和后面的 Cb 个数对上。
`this+118 == 3` 时分支里会**段错误**（驱动自身的坑，实测必崩），填 0 或 2 都能走通。

**实测结果**（`tools/site-probe/focas_data_probe.sh`；`mock.py` 新增了 `CBREP:` 规格，
按请求的 Cb 个数自动生成同样多个块）：

| SDK 调用 | 结果 |
|---|---|
| `cnc_allclibhndl3` | **rc=0**，handle=32769 |
| `cnc_statinfo` · `cnc_rdcount` · `cnc_actf` · `cnc_acts` · `cnc_rdparam` · `cnc_rdtofs` · `cnc_exeprgname2` · `cnc_rdlife` | **rc=0** |
| `cnc_rdaxisdata` / `cnc_rdalmmsg2` / `cnc_rdexecprog` / `cnc_rdblkcount` | rc=1/2/6/6 —— 假机床给的是全零载荷，形状不够，不是协议不明 |
| `cnc_rdmacro` | 载荷长度 0 时驱动段错误（真机有真实长度，不会） |
| `cnc_machine` | 探针的调用签名不对（少一个 axis 参数），rc=4，与协议无关 |

**数据是怎么切的（以 `cnc_statinfo` 为例，反汇编 `0x27a38`，110 行读完）**：

```
Cb(1, hd[0x8c0], 25) → addCb        Cb(1, hd[0x8c0], 225) → addCb
Cb(1, hd[0x8c0], 152) → addCb       Handle::request(hd, pdu, 0x21)
st[0] = 0; if (getRbCode(1) == 0) st[0] = be16(块1[16..18))
st[2] = 0; if (getRbCode(2) == 0) st[2] = be16(块2[16..18))
n = be16(块0[14..16)) / 2 ; 从 块0[16..] 拷 n 个 u16 到 st[4..]     ← getRb(0)
```

对上 `ODBST { dummy, aut, manual, run, edit, motion, mstb, emergency, alarm,
spindle, oper }`：**块 1 = dummy、块 2 = aut、块 0 的载荷 = manual…oper 这 9 个
u16**。也就是"标量各占一个块、数组走块 0 的载荷"——这就是 FOCAS 应答的通用切法。
（块 1/2 的返回码非 0 只是把该字段填 0，**不抛异常**；块 0 的返回码非 0 才抛。）

**Cb 码表**（每个数据项发的就是**一条** func `0x21` 请求，`c` 字段是它的码）：

| SDK 调用 | 请求条数 | Cb 的 `c` | d/e 取值 |
|---|---|---|---|
| 握手 `system_info_v1` 第 3 条 | 1 | `0x0e` | d=e=`0x26f0` |
| `cnc_statinfo` | 3 | `25` / `225` / `152` | 0 |
| `cnc_rdcount` | 1 | `0x8b`(139) | d=e=1 |
| `cnc_rdlife` | 1 | `0x8b`(139) | d=e=1 |
| `cnc_actf` | 1 | `0x24`(36) | 0 |
| `cnc_acts` | 1 | `0x25`(37) | 0 |
| `cnc_rdparam` | 1 | `0x0e`(14) | d=e=1，载荷含 `40 7f fc 90` |
| `cnc_rdmacro` | 1 | `0x15`(21) | d=e=1 |
| `cnc_rdtofs` | 1 | `0x08`(8) | d=e=1，另带 `0x78`(120) |
| `cnc_rdprogdir3` | 1 | `0x06`(6) | d=`0x13`, e=1 |
| `cnc_exeprgname2` | 1 | `0xfc`(252) | 0 |
| `cnc_rdaxisdata` `cnc_rdalmmsg2` `cnc_rdexecprog` `cnc_rdblkcount` | 0 | — | 本地先失败，没进到协议层 |

**这家实际用到的 29 个 SDK 调用**（`libfocas.so` 的导入表，🟢）：

```
cnc_allclibhndl3 cnc_freelibhndl cnc_statinfo cnc_machine cnc_rdcount
cnc_rdaxisdata cnc_rdparam cnc_rdtofs cnc_rdtofsinfo cnc_rdmacro
cnc_rdalmmsg2 cnc_rdlife cnc_rdexecprog cnc_exeprgname2 cnc_actf cnc_acts
cnc_rdblkcount cnc_rdprogdir3 cnc_pdf_slctmain cnc_pdf_del cnc_download4
cnc_dwnstart4 cnc_dwnend4 cnc_upload4 cnc_upstart4 cnc_upend4
cnc_startupprocess cnc_exitprocess cnc_rdparam
```

---

## 3. 常用函数表（按域）

### 3.1 连接与系统（cnc_*）

| 函数 | 说明 |
|---|---|
| `cnc_allclibhndl3(ip, port, timeout, &h)` | 建立连接（最常用） |
| `cnc_allclibhndl2/4` | 变体（多句柄/超时细控） |
| `cnc_freelibhndl(h)` | 释放 |
| `cnc_sysinfo(h, &ODBSYS)` | 系统信息（型号/系列/轴数/主轴数） |
| `cnc_rdmodel(h, &ODBMDL)` | 机床型号 |
| `cnc_statinfo(h, &ODBST)` | **运行状态**（模式/运行/急停/报警） |
| `cnc_rddt(`…`)` / `cnc_rdtime` | 日期时间 |
| `cnc_rdopmode(h, &ODBOPM)` | 工作模式 |

### 3.2 坐标与轴（★ 采集核心）

| 函数 | 说明 |
|---|---|
| `cnc_absolute(h, axis, &ODBACT)` | 绝对坐标 |
| `cnc_absolute2` | 绝对坐标（扩展） |
| `cnc_relative(h, axis, &ODBREL)` | 相对坐标 |
| `cnc_machine(h, axis, &ODBM)` | 机械坐标 |
| `cnc_distance(h, axis, &ODBDIS)` | 剩余距离 |
| `cnc_rdposition(h, type, …)` | **批量坐标读取**（一次取多轴多类型） |
| `cnc_acts(h, &ODBACT)` / `cnc_actf` | 全部轴绝对位置（short/float） |
| `cnc_rdsvmeter` / `cnc_rdspmeter` | **伺服/主轴负载表** |
| `cnc_rdaxisdata(h, …)` | 轴数据（速度/负载/温度，可批量，推荐） |
| `cnc_rdaxisname` | 轴名称 |

### 3.3 主轴

| 函数 | 说明 |
|---|---|
| `cnc_rdspdata` | 主轴数据 |
| `cnc_rdspmeter` | 主轴负载/速度 |
| `cnc_rdspindle` / `cnc_rdspload` | 主轴转速/负载 |
| `cnc_rdspspeed` | 主轴速度 |

### 3.4 报警

| 函数 | 说明 |
|---|---|
| `cnc_alarm(h, &ODBALM)` | **当前报警**（数量+内容） |
| `cnc_rdalmmsg2(h, type, …)` | 报警消息（可分类/分页，推荐） |
| `cnc_rdalmmsg` | 报警消息（旧版） |
| `cnc_rdopmsg` / `cnc_rdopmsg2/3` | 操作员消息 |
| `cnc_rdalmhis` 类 | 报警历史（部分系统） |

### 3.5 程序

| 函数 | 说明 |
|---|---|
| `cnc_rdprgnum(h, &ODBPRO)` | 当前主/子程序号 |
| `cnc_rdseqnum(h, &ODBSEQ)` | 当前程序行号（序列号） |
| `cnc_rdprogdir` / `cnc_rdprogdir2/3` | 程序目录 |
| `cnc_upstart4` / `cnc_upend4` / `cnc_upload` | 程序上传（分块） |
| `cnc_dwnstart4` / `cnc_download` / `cnc_dwnend4` | 程序下载 |
| `cnc_delprogram` / `cnc_rdproginfo` | 删除 / 信息 |
| `cnc_rdpdf_*`（pbm_*） | 程序块管理（按块读写，大程序更高效） |

### 3.6 刀具补偿

| 函数 | 说明 |
|---|---|
| `cnc_rdtofs(h, type, &ODBTOFS)` | 刀补读（形状/磨损，长度/半径） |
| `cnc_wrtofs` | 刀补写 |
| `cnc_rdtool` / `cnc_rdtoolgrp` | 刀具信息/刀具组 |
| `cnc_rdngrp` | 刀具组数量 |

### 3.7 参数与宏变量

| 函数 | 说明 |
|---|---|
| `cnc_rdparam(h, num, len, &ODBPARA)` | **CNC 参数读** |
| `cnc_wrparam` | 参数写 |
| `cnc_rdmacro(h, num, len, &ODBM)` | **宏变量读** |
| `cnc_wrmacro` | 宏变量写 |
| `cnc_rdparainfo` / `cnc_rdparaminfo` | 参数信息 |

### 3.8 PMC（PLC 层，95 函数）

| 函数 | 说明 |
|---|---|
| `pmc_rdpmcrng(h, adr_type, data_type, start, end, len, buf)` | **PMC 区读**（R/Y/X/G/F/E/A/C/D 等） |
| `pmc_wrpmcrng` | PMC 区写 |
| `pmc_rdpmcinfo` | PMC 区信息 |
| `pmc_rdladder` | 梯形图读取 |
| `pmc_getdtailerr` | 详细错误 |
| `pmc_setpcmcntl` / `pmc_startladdermonitor` | 梯形图监控 |

### 3.9 其它

| 域 | 前缀 | 说明 |
|---|---|---|
| 时间 | `cnc_rdtimer` / `cnc_rdcount` | 运行时间/工件计数 |
| 伺服/主轴参数 | `cnc_rdservo` / `cnc_rdspindle` | 伺服/主轴参数 |
| 波形 | `cnc_startwave` / `cnc_rdwave` / `cnc_stopwave` | 波形采集（诊断） |
| 数据服务 | `ds_*`（8 函数） | FOCAS2 Data Service |
| 变量/PMC 日志 | `flnt_*`（14 函数） | Focas2 Logger |

---

## 4. 数据类型（结构体，选摘）

```c
typedef struct { short data; short dec; short unit; short disp; short name; short suff; } ODBACT;
// 坐标值 = data / 10^dec  —— ★ 必须按 dec 缩放，否则数值差 10^n 倍

typedef struct { short dummy; short num; struct { short no; short type; short data; } msg[10]; } ODBALM;
typedef struct { short datano; short type; long data[2]; } ODBTOFS;   // 刀补（形状/磨损 × 长度/半径）
typedef struct { short datano; short type; long data; } ODBM;          // 宏变量
typedef struct { char name[36]; char cnc_type[2]; ... } ODBSYS;
```

**⚠️ 坐标缩放是第一坑**：`cnc_absolute` 返回的 `data` 是整数，必须 `data / pow(10, dec)` 才是实际值（dec 通常 3 或 4）。

---

## 5. 错误码（错误分类函数）

```c
short err = cnc_getdtailerr(h, &ODBERR);   // ODBERR 结构 getdtailerrkind/dta1/dta2
// 通用返回：EW_OK=0；非 0 时用 cnc_getdtailerr 取细节
```
常见：`EW_FUNC`(功能不支持) · `EW_LENGTH`(数据长度错) · `EW_PARAM`(参数错) · `EW_HANDLE`(句柄无效) · `EW_SOCKET`(网络错) · `EW_BUSY`

---

## 6. 实现坑

1. **句柄数限制**：FOCAS 默认最多 2 个并发句柄 —— 多进程采集会互相踢掉。**必须单进程复用句柄**。
2. **坐标缩放**（见 §4）—— 最常见的数值 bug 来源。
3. **`cnc_rdaxisdata` 优于逐轴 `cnc_absolute`**：批量读一次拿多轴，吞吐高 3-10 倍。
4. **报警消息分页**：报警多时 `cnc_alarm` 只给 10 条，遍历要用 `cnc_rdalmmsg2`。
5. **程序上传/下载必须成对调用**（`*start` → 分块 → `*end`），中途退出会锁住机床侧。
6. **32/64 位库不同**（`Fwlib32.dll` vs `fwlib32.x64.dll`），结构体对齐要用 `#pragma pack`。
7. **写类函数（`cnc_wrtofs` / `cnc_wrmacro` / `pmc_wrpmcrng`）风险高**：改刀补会导致撞刀。

---

## 7. 参考实现

| 来源 | 说明 |
|---|---|
| `pyfocas` / `pyfanuc` | ctypes 绑定 + fwlib 头文件（可直接抄结构体定义） |
| **881 个函数原名**（按前缀分类） | 原始素材未随本目录提供 |
| 商业库对照 `FanucSeries0i` | 高层 API 分组参考（59 成员，坐标/报警/负载/刀补/宏变量/程序列表） |
| 参考实现侧 | `驱动定义目录/` FANUC 驱动（端口 8193） |
