# 16 · MTConnect 实现规格书

> **证据**：🟢参考实现DMG MORI 驱动 + `mtconnect 0.3.3` 包 · 🔵 公开标准（ANSI/MTC1.4）
> **定位**：设备数据"只读"标准接口，**HTTP/XML，实现最简单**，欧美机床普遍支持

---

## 1. 速查

| 项 | 值 |
|---|---|
| 端口 | **HTTP 7878**（DMG MORI 常用；标准无固定端口） |
| 传输 | **HTTP GET**（RESTful，无会话） |
| 数据格式 | **XML**（Agent 输出）/ JSON（部分实现） |
| 架构 | `设备 → MTConnect Adapter → MTConnect Agent(HTTP) → 客户端` |
| 轮询 vs 流 | `/probe` · `/current`（轮询）· `/sample?interval=&heartbeat=`（**流式**） |
|参考实现| `mtconnect`（Python） |

---

## 2. 接口清单（HTTP 路径）

| 路径 | 说明 |
|---|---|
| `/probe` | **设备能力描述**（有哪些数据项 DataItem，含 device/model 元信息） |
| `/current` | **当前快照**（所有 DataItem 的最新值） |
| `/sample` | **流式采样**（长连接，按 interval 推送） |
| `/sample?from=&count=` | 历史数据（Agent 缓冲区内） |
| `/assets` | 资产（刀具库、程序等） |
| `/assets/{assetId}` | 单个资产 |

**认证**：HTTP Basic 或自定义头（取决于 Agent 配置）。

---

## 3. XML 结构（核心）

### 3.1 `/probe` 响应骨架

```xml
<MTConnectDevices xmlns="urn:mtconnect.org:MTConnectDevices:1.4">
  <Header creationTime="..." sender="..." instanceId="..." version="1.4"/>
  <Devices>
    <Device id="d1" name="Machine" uuid="...">
      <Description>DMG MORI DMU 50</Description>
      <DataItems>
        <DataItem id="x1" category="SAMPLE" type="POSITION" subType="ACTUAL" name="Xabs" units="MILLIMETER"/>
        <DataItem id="r1" category="EVENT" type="EXECUTION" name="exec"/>
        <DataItem id="a1" category="EVENT" type="ALARM" .../>
      </DataItems>
      <Components>
        <Axes><Linear id="X" name="X">
          <DataItems>
            <DataItem id="xp" category="SAMPLE" type="POSITION" subType="ACTUAL" units="MILLIMETER"/>
            <DataItem id="xl" category="SAMPLE" type="LOAD"/>
          </DataItems>
        </Linear></Axes>
        <Controller>
          <DataItems><DataItem id="mode" category="EVENT" type="CONTROLLER_MODE"/></DataItems>
        </Controller>
      </Components>
    </Device>
  </Devices>
</MTConnectDevices>
```

### 3.2 `/current` 响应骨架

```xml
<MTConnectStreams xmlns="urn:mtconnect.org:MTConnectStreams:1.4">
  <Header .../>
  <Streams>
    <DeviceStream name="Machine" uuid="...">
      <ComponentStream component="Linear" name="X">
        <Samples>
          <Position dataItemId="xp" timestamp="2026-09-18T10:00:00.000Z" sequence="1">123.456</Position>
        </Samples>
        <Events>
          <Execution dataItemId="r1" timestamp="..." sequence="2">ACTIVE</Execution>
        </Events>
      </ComponentStream>
    </DeviceStream>
  </Streams>
</MTConnectStreams>
```

---

## 4. 关键概念

| 概念 | 说明 |
|---|---|
| **DataItem** | 数据项（`id` 唯一，`type` + `subType` 决定语义） |
| **category** | `SAMPLE`（数值采样）· `EVENT`（状态/事件）· `CONDITION`（报警/状态条件） |
| **sequence** | 全局单调序号 —— **断线重连靠它续传**（`/sample?from=<lastSeq>`） |
| **timestamp** | UTC ISO8601（`Z` 结尾） |
| **heartbeat** | 流式模式下无数据时的心跳间隔 |

### 4.1 常用 DataItem 类型

| type | subType | 说明 |
|---|---|---|
| `POSITION` | `ACTUAL` / `COMMANDED` / `MACHINE` / `RELATIVE` | 坐标 |
| `EXECUTION` | — | 运行状态（ACTIVE/READY/STOPPED/INTERRUPTED） |
| `CONTROLLER_MODE` | — | 模式（AUTOMATIC/MANUAL/MDI） |
| `PATH_FEEDRATE` | `ACTUAL` / `OVERRIDE` / `PROGRAMMED` | 进给 |
| `ROTARY_VELOCITY` | `ACTUAL` / `OVERRIDE` | 主轴转速 |
| `LOAD` | — | 负载（% 或 N·m） |
| `TEMPERATURE` | — | 温度 |
| `TOOL_NUMBER` / `TOOL_ID` | — | 刀具 |
| `PART_COUNT` | `ALL` / `GOOD` / `BAD` / `REMAINING` | 计数 |
| `PROGRAM` | — | 当前程序名 |
| `BLOCK` | — | 当前行号 |
| `AVAILABILITY` | — | 可用性（AVAILABLE/INTERRUPTED） |
| `ALARM`（CONDITION） | — | 报警（含 nativeCode/severity） |
| `MESSAGE` | — | 操作员消息 |

---

## 5. 采集流程

```
① GET /probe            → 解析 DataItem 清单（构建点位模型）
② GET /current          → 首帧快照（记录 sequence）
③ GET /sample?interval=100&heartbeat=10000&from=<seq>
                        → 长连接流式接收（推荐）
   或 轮询 /current（interval 200-1000ms）
④ 断线：记录最后 sequence，重连用 /sample?from=<seq> 续传（不丢数据）
⑤ 报警：CONDITION 类 item（Fault/Warning/Normal 三态）
```

---

## 6. 实现坑

1. **`/current` 只返回"变化过"的数据项** —— 不要假设每帧都有全部点位；维护本地缓存。
2. **sequence 是全局单调的**，断线后必须从上次 sequence 续传，否则丢数据或重复。
3. **timestamp 是 UTC**（设备本地时区需另取）；跨时区展示要显式转换。
4. **不同 Agent 版本命名空间不同**（`urn:mtconnect.org:MTConnectStreams:1.4`）——解析时按命名空间匹配，别硬编码前缀。
5. **单位（units）必须从 DataItem 读取**（MILLIMETER vs INCH；DEGREE vs RADIAN）。
6. **报警是 CONDITION 类型**，有 Fault/Warning/Unavailable/Normal 四态，与 EVENT 不同。
7. **流式连接会被中间设备（代理/防火墙）超时断开** —— 客户端要能自动重连 + 续传。

---

## 7. 参考实现

| 来源 | 说明 |
|---|---|
| `mtconnect-0.3.3` | Python 客户端 |
| 参考实现侧 | DMG MORI 驱动（端口 7878）· `dmg_mori_mtconnect_driver.json` |
| 公开规范 | MTConnect Standard v1.4/v2.0（mtconnect.org） |
