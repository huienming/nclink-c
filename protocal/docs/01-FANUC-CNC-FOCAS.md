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
- 四条消息的应答内容仍未确证（要么真机抓一次，要么按 Fwlib32 手册补全字段）；
  这是本册目前唯一还缺的字节级证据。
- 现场（`cfg/driver_def.json`）用的就是这一家：`module: focas`、`8193`。

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
