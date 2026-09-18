# 24 · 博世力士乐 Bosch Rexroth（OPC UA）实现规格书

> **证据**：🟡 项目案例实证（五轴加工中心 OPC UA 采集项目）+ 🟢 可复用现有 OPC UA 实现栈
> **定位**：**最容易补的缺口** —— 走标准 OPC UA，**零新协议开发**

---

## 1. 速查

| 项 | 值 |
|---|---|
| 系统 | **Rexroth IndraControl** 系列工业控制器（MTX 数控 / ctrlX CORE） |
| 协议 | **OPC UA**（原生支持，标准服务器接口） |
| 端口 | **TCP 4840**（OPC UA Binary 默认） |
| 通信口 | 多路以太网，实测用 **X7E5** 口做采集（详见机床手册确认） |
| 节点模型 | 统一 **NodeID** 标识；数据类型/访问权限标准化 |
| 安全 | X.509 证书认证 · Sign & Encrypt 加密传输 |
| 实现库 | `asyncua`（Python，本仓库 `supp/asyncua-2.0.1`）—— **与 04/19 册共用** |

---

## 2. 连接建立

```python
from asyncua import Client
async with Client(url="opc.tcp://192.168.1.10:4840") as client:
    # ① 探测命名空间
    idx = await client.get_namespace_index("urn:Rexroth:...")   # 以实际 URI 为准
    # ② Browse 定位数据节点（机床数据按 轴/通道/状态 组织）
    # ③ 读值 或 建订阅
    objs = client.get_objects_node()
    children = await objs.get_children()
```

**推荐流程**：首连 → `Browse` 全树 → 导出节点清单到配置 → 后续按配置读/订阅（**不要硬编码 NodeID**）。

---

## 3. 数据访问方式

| 方式 | 说明 | 适用 |
|---|---|---|
| **Read** | 单点/批量读 | 低频、一次性 |
| **Subscription（推荐）** | 服务端按间隔推送变化值 | 高频采集（坐标/状态） |
| 历史访问 | 若服务器开 HA 功能 | 追溯 |

采集建议：
```
订阅周期：坐标/进给 100-200ms · 状态/模式 500ms · 报警 1000ms
断线重连：自动重连 + 重建订阅（OPC UA 订阅不会自动恢复）
会话保活：KeepAlive（默认会话超时 60 分钟）
```

---

## 4. 与其它 OPC UA 设备的关系（复用说明）

本机已有 **3 册 OPC UA 规格书**，实现层完全共用：

| 册 | 设备 | 差异 |
|---|---|---|
| 04 | 西门子 840D sl | 节点命名空间/树结构不同 |
| 19 | 沈阳 i5 | 同上 |
| **24（本册）** | 博世力士乐 IndraControl | 同上 |

> **一套 OPC UA 客户端 + 每设备一份"节点路径配置"** 即可覆盖三者 —— 这正是 OPC UA 标准化的价值。**补本册的实际工作量 ≈ 0（只需现场 Browse 得到节点清单）**。

---

## 5. 实现坑

1. **节点命名空间厂商私有** —— 必须现场探测；不同固件版本节点路径可能变化。
2. **OPC UA 功能需许可**（ctrlX CORE 上 OPC UA 是扩展功能，需授权；IndraControl 视型号）。
3. **安全策略**：若启用了 Sign & Encrypt，客户端必须导入/信任证书，否则握手失败。
4. **X7E5 等口的选择** —— 部分控制器多网口分工不同（诊断/实时/通用），需按手册选采集口。
5. **订阅丢失静默**：网络抖动后订阅可能失效但 TCP 还连着 —— 需定期校验（读一个已知节点 or 订阅心跳）。
6. **写操作**：OPC UA 可写节点受权限控制，工业现场默认只读。

---

## 6. 参考实现

| 来源 | 说明 |
|---|---|
| `supp/asyncua-2.0.1` | Python OPC UA 客户端（与 04/19 册同一套） |
| 案例 | CSDN《创新五轴加工中心博世力士乐系统 OPCUA 数据采集项目案例》（IndraControl + X7E5 口） |
| 官方 | ctrlX CORE OPC UA Server 文档 · IndraControl 通信手册 |
