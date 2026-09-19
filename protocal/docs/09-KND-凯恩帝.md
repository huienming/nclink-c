# 09 · 凯恩帝 KND（REST API + DLL 双通道）实现规格书

> **证据**：🟢 REST 端点面（参考实现实测调用）· 🔵 公开文章（DLL 通道）
> **定位**：国产 CNC 中**唯一原生提供 HTTP REST 接口**的厂商（集成最省事）

---

## 1. 速查

| 项 | 值 |
|---|---|
| 通道 A | **HTTP REST**（KAPI，端口常配 80） |
| 通道 B | **DLL**（厂商动态库，Windows） |
| API 版本 | `/api/v1.2`（另有 v1.0 / v1.1；官方《KND-REST-API 参考手册 V1》） |
| 数据格式 | JSON |
| 认证 | 登录接口取 token（或 Basic） |
| 参考实现侧 | `/KND/*` 端点（直接透传 KND REST） |

---

## 2. 连接建立

```
① 机床侧开启网络功能（KND 面板参数）
② HTTP 请求：POST/GET http://<ip>/api/v1.2/<资源>
③ 需要鉴权的接口先调登录，拿 token 后放请求头
④ 采集：轮询各资源接口（无推送，除非用 DLL 回调）
```

---

## 3. 端点族（REST）—— 现场实测表

> **证据升级**：下表来自现场交付包里真正在跑的映射层（`lua_mod/knd_mod.lua`、
> `lua_mod/func_mod.lua` 的 `register_entries{ ["get_value@<模型项>"] = {url=…} }`），
> 以及它们的基类 `knd_mod_base.lua` / `func_mod_base.lua`（`base_url` 由配置给，
> 请求是 `GET <base_url><url>`）。**早期按"官方 REST 规范"猜的 `/api/v1.2/*`
> 路径在交付包里没有任何一处出现，已作废。**

| 端点 | 返回字段 | 说明 |
|---|---|---|
| `GET /getValue` | `run-status` | **运行状态**：0=free、1=holding、2=running |
| `GET /status` | `not-ready-reason` | 就绪原因位域，`bit0 = 1` 即"未就绪/急停" |
| `GET /progs/cur` | `number` | 当前（主/子）程序号 |
| `GET /progs/exec-status` | `P` | 执行中的行号 |
| `GET /workcounts/total` | `count` | 工件计数 |
| `GET /workcountgoals/total` | `count` | 计划件数 |
| `GET /cycletime` | `total` / `cur` | 累计加工时间 / 当前加工时间 |
| `GET /overrides/feed` | `ov` | 进给倍率，**×100** |
| `GET /overrides/rapid` | `ov` | 快移倍率，**×100** |
| `GET /overrides/jog` | `ov` | 点动倍率，**×100** |
| `GET /overrides/handle` | `ov` | 手轮倍率，**×100** |
| `GET /sp/overrides/1` | `ov` | 主轴倍率（主轴 1），**×100** |
| `GET /sp/speeds/1` | `speed` | 主轴转速（主轴 1） |
| `GET /coors/machine` | 轴名键 `X Y Z A B C U V W` | **机械坐标**，一次取全部轴 |
| `GET /plc/vm/TL0` | 数组，`[1]` | 刀具号（PLC 变量表 TL0） |
| `GET /alarms/` | 报警**类别名**键 | 见下 |

**响应形态**：JSON，且是**扁平对象**——取哪个量就用哪个键，例如
`GET /workcounts/total` → `{"count": 1234}`；`GET /coors/machine` →
`{"X": 1.234, "Y": ..., "Z": ...}`。
（**没有** `{"code":0,"data":...}` 这层信封；`error`/`error-message` 两个键表示失败。）

**报警键 → 报警号**（映射层固定按这个顺序编号 `100%02d`，组 `{number, text}`）：

```
prm-switch  reboot  plc  ps  over-travel  over-heat  mem
servo  servo-bus  over-workarea  io-bus  io-module  manufacture  forbid-move
```

### 3.1 与模型项的对应（现场映射，可直接当验收用例）

| NC-Link 模型项 | 端点 | 规整 |
|---|---|---|
| `/STATUS` | `/getValue` → `run-status` | 0/1/2 → `free/holding/running` |
| `/VARIABLE@ESP_STATE` | `/status` → `not-ready-reason` | `& 0x01` → bool |
| `/CONTROLLER/PROGRAM`、`/CONTROLLER/SUBPROGRAM` | `/progs/cur` → `number` | 取整、转字符串 |
| `/CONTROLLER/LINE_NUMBER` | `/progs/exec-status` → `P` | — |
| `/PART_COUNT` | `/workcounts/total` → `count` | 取整 |
| `/VARIABLE@PLANNED_COUNT` | `/workcountgoals/total` → `count` | — |
| `/VARIABLE@RUN_TIME` / `/VARIABLE@CUT_TIME` | `/cycletime` → `total` / `cur` | — |
| `/FEED_OVERRIDE` `/SPINDLE_OVERRIDE` `/RAPID_OVERRIDE` `/JOG_OVERRIDE` `/HANDWHEEL_OVERRIDE` | `…/overrides/*` → `ov` | ×100 |
| `/SPINDLE_SPEED` | `/sp/speeds/1` → `speed` | — |
| `/TOOL_NUMBER` | `/plc/vm/TL0` → `[1]` | — |
| `/CONTROLLER/WARNING` | `/alarms/` | 键→`{number,text}` 数组 |
| `/AXIS@0..8/SCREW|MOTOR/POSITION` | `/coors/machine` → 轴名 | 轴号 0..8 对应 `X Y Z A B C U V W` |

> 现场实现是"**一次取一个量**"，且请求必须串行（见 §5.4）。本地适配器若要批量
> 合并，只能合并**同一个端点**的多次取值（如 9 个轴一次 `/coors/machine`）。

---

## 4. DLL 通道

| 项 | 说明 |
|---|---|
| 形态 | 厂商动态库（随系统光盘/代理提供） |
| 调用 | C/C++/C# 直接链接；`__stdcall` 约定 |
| 优势 | 数据更全（部分数据 REST 不暴露）、可注册回调 |
| 劣势 | Windows 绑定、需分发授权 |

**双通道选择建议**：
- 采集（读）→ **优先 REST**（跨平台、无依赖）
- 高频/回调/特殊数据 → DLL

---

## 5. 实现要点与坑

1. **版本敏感**：v1.0/v1.1/v1.2 的端点与字段不同，**实现时必须先探测版本**（取一个版本接口或从响应头判断）。
2. **鉴权**：部分固件要求先登录；token 有效期短，需自动续期。
3. **无推送机制**（REST 通道）—— 高频采集靠轮询，建议坐标类 100-200ms、状态类 500ms、报警 1s。
4. **HTTP 并发限制**：机床内置 HTTP 服务并发能力弱，**请求必须串行化 + 限速**（建议 ≤ 10 QPS）。
5. **参数写入高危**：REST 写接口与 DLL 写接口都要权限墙。
6. **端口可配**：不要假设 80。
7. **JSON 字段命名**：不同固件版本字段大小写/命名不一致，解析要容错。

---

## 6. 参考实现

| 来源 | 说明 |
|---|---|
| 现场交付包 | `app1/nclink-service/lua/lua_mod/{knd_mod,func_mod,knd_mod_base,func_mod_base}.lua`（端点表与字段名的出处，见 [28 册](28-交付包现场API清单.md) §5.2） |
| 现场交付包 | `app1/hp2x/hp2x_box200`：KND 之外还有 GSK/精雕/科德/SYNTEC/LSV2… 同一套端点风格 |
| 官方手册 | 《KND-REST-API 参考手册 V1》（book118，**付费**） |
| 采集实践 | 公开文章：KND 数据采集（DLL + REST 双通道对比） |
