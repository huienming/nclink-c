# 04 · 西门子 SINUMERIK 840D sl（OPC UA）实现规格书

> **证据**：🟢参考实现`libsinumerik-arm(-readvar)` + OPC UA 节点表 · 端口 4840
> **定位**：840D sl / 828D 的标准数据接口（**OPC UA 是西门子官方推荐方式**）

---

## 1. 速查

| 项 | 值 |
|---|---|
| 端口 | **TCP 4840**（OPC UA binary） |
| 协议 | **OPC UA**（Binary over TCP，非 HTTP） |
| 端点 | `opc.tcp://<ip>:4840` |
| 认证 | 匿名 / 用户名密码 / 证书（840D sl 默认允许匿名读） |
| 命名空间 | 西门子专用（机床数据按 NCK/通道/轴 树状组织） |
|参考实现| `asyncua`（Python，参考架 `asyncua-2.0.1`） |
| 参考实现侧 | `/Siemens/840D/*` + `libsinumerik-arm` 库 |

---

## 2. 连接建立（asyncua 示例）

```python
from asyncua import Client
async with Client(url="opc.tcp://192.168.1.10:4840") as client:
    # 浏览根节点找西门子命名空间
    root = client.get_root_node()
    objects = client.get_objects_node()
    # 840D 的机床数据通常在 Sinumerik 命名空间下
```

---

## 3. 节点模型（840D 数据组织）

```
Root
└ Objects
  └ Sinumerik (或 NCK 命名空间)
    ├ NCK/Configuration/…
    ├ Channel/…
    └ Axes/Axis1..n/…
       ├ ActualPosition   ← 实际位置
       ├ SetpointPosition
       ├ Feedrate
       └ …
```

**常用机床数据（Machine Data）**：
| 数据 | 说明 |
|---|---|
| `ActualPosition` / `actpos` | 轴实际位置 |
| `SetpointPosition` | 设定位置 |
| `ActualFeed` / `feed` | 实际进给 |
| `ActualSpeed` / `spindle speed` | 主轴转速 |
| `ProgramName` / `PartProgram` | 当前程序 |
| `ToolNumber` | 刀具号 |
| `MachineMode` | 运行模式 |
| `Alarm[..]` | 报警 |

---

## 4. 通道 B：`libsinumerik-arm` / `-readvar`（私有库）

| 项 | 说明 |
|---|---|
| 形态 | 参考实现内的原生库（ARM） |
| 能力 | 读 NCK 变量（`readvar`） |
| 优势 | 数据更全（含 OPC UA 未暴露的 NCK 变量） |
| 劣势 | 厂商 SDK，需授权 |

**西门子变量服务（VARIABLE SERVICE）**：840D 还支持基于 S7 通讯的变量服务（通过 `NC VAR SELECTOR` 选择器），是 `-readvar` 库的实现原理。

---

## 5. 实现坑

1. **840D sl 的 OPC UA 需要授权选项**（"OPC UA Server" 许可，部分机床未开通）。
2. **节点路径随版本变化**（840D sl 4.5/4.7/4.8 命名空间结构有差异）—— 必须先 `/browse` 探测，不要硬编码 NodeId。
3. **订阅（Subscription）优于轮询**：OPC UA 支持数据变化推送，用 `create_subscription(period)` 比轮询省带宽。
4. **匿名访问权限**：默认可能只读；写需证书 + 授权。
5. **会话超时**：OPC UA 会话默认 60 分钟，长连接要保活（`KeepAlive`）。
6. **大端/小端**：OPC UA Binary 编码规定，用库即可，别手写。
7. **与 S7 层的关系**：840D 的 PLC 部分可用 S7comm（见 03 册）读，NCK 部分用 OPC UA / 变量服务 —— **两者互补**。

---

## 6. 参考实现

| 来源 | 说明 |
|---|---|
| `asyncua-2.0.1` | Python OPC UA 客户端（异步，功能全） |
| 参考实现侧 | `libsinumerik-arm.so` / `libsinumerik-arm-readvar.so` + `/Siemens/840D/*` 驱动 |
| 西门子文档 | SINUMERIK 840D sl OPC UA Server 手册（节点清单） |
