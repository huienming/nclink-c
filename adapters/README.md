# 适配器（厂商协议驱动）

把设备自带的通信协议接进 NC-Link 的一层。核心库（`src/`）只认 NC-Link 主题与
消息；适配器负责另一侧——按厂商协议跟机床/PLC/机器人说话，把读到的数据挂到
设备模型上，把方法调用转成协议命令。

协议规格书在 [`../protocal/docs/`](../protocal/docs/)：先读
`00-通用-实现约定.md`（统一地址模型、错误分级、重连、批量合并、审计），
再读具体协议分册。编码顺序见 `protocal/docs/README.md` 的优先级排序。

## 目录

```
adapters/
├── include/nclink_adapter/   # 对外接口（驱动实现者要 include 的头）
│   ├── ncl_driver.h          # ncl_driver_ops / ncl_address / ncl_driver_result
│   ├── ncl_driver_manager.h  # 驱动配置、点位表、path→驱动 分派
│   └── ncl_adapter.h         # 守护进程：配置 → 设备
├── src/core/                 # 与协议无关的骨架（类型、错误分级、地址解析、注册表）
├── src/registry/             # 配置加载 + 点位表 + 最长前缀分派
├── src/app/                  # 守护进程主体（模型生成、操作注册、轮询）
├── src/main.c                # ncl_adapter 可执行文件
├── drivers/<协议>/           # 每个协议一个目录：帧构造/解析 + 会话状态
└── tests/                    # 黄金报文 + mock 靶机的集成测试
```

构建产物是 `libnclink_drivers.a`（CMake 目标 `nclink::drivers`）与可执行文件
`ncl_adapter`，与 `nclink::core` 分开：设备端不带任何厂商驱动时可以直接不编译
这一层（`-DNCLINK_BUILD_ADAPTERS=OFF`）。

## 驱动接口

一个驱动 = 一张 `ncl_driver_ops` 表：

| 回调 | 语义 |
|---|---|
| `protocol` | 配置里写的协议名（`"mock"`、`"modbus_tcp"`…） |
| `create` | 用配置的 `parameters` 对象初始化 |
| `open` / `close` / `is_connected` | 会话生命周期（连接 + 握手 / 断开） |
| `read_batch` / `write_batch` | 统一地址（`area` + `offset` + `bit` + `length` + `dtype`）的批量读写 |
| `read_raw` / `write_raw` | 逃生舱：直发原始报文（诊断、厂商私有命令） |
| `call` | 方法类操作（启程序、MDI、刀补…），对应 NC-Link 的 Method 调用 |
| `attach_event` | 报警、程序结束一类的事件回调 |
| `destroy` | 释放私有状态与驱动结构本身 |

三条共同的约定：

1. **统一地址模型**：`ncl_address` 只有 `area / offset / bit / length / dtype`
   五个字段，七种类型 `bit/byte/int16/int32/float32/float64/string`。协议里的
   怪类型（MELDAS 的 10 字节 double、FOCAS 的 4 字节串）在驱动内部消化。
2. **三类错误分开**：`NCL_DRV_ERR_TRANSPORT`（超时、断开 → 重连）/
   `NCL_DRV_ERR_PROTOCOL`（协议错误码 → 按码表映射）/
   `NCL_DRV_ERR_BUSINESS`（数据无效 → 上报，不改协议状态）。调用方用
   `ncl_driver_error_tier()` 判断该不该重连。
3. **会话按需打开**：`ncl_driver_read_one()` / `ncl_driver_write_one()` 会在
   未连接时先 `open()`。实现了 `open` 的驱动必须同时实现 `is_connected`。

响应统一走 `ncl_driver_result`：`code / success / value / message / raw`，
其中 `raw` 是原始应答字节，供审计与排障（`00-通用-实现约定.md` §6）。

## 写一个新驱动

1. 建 `drivers/<协议>/`，实现 `ncl_driver_ops` 里的回调。
2. 提供一个工厂 `ncl_driver *ncl_<协议>_create(void)`，头文件放在同目录。
3. 在 `src/core/driver.c` 的 `ncl_driver_register_builtin()` 里注册；
   第三方驱动也可以自己调用 `ncl_driver_register_protocol()` 挂进注册表。
4. 在 `tests/` 里加：报文构造/解析的黄金样本（真实抓包做 case）＋ 用
   `mock` 或自建靶机跑一遍连接与读写。

`drivers/mock/` 是最小样板：内存点位模型 + 错误注入 + 事件触发，没有一行
网络代码，测试里可以直接用（`ncl_mock_driver_create()`）。

## 配置与守护进程

一条「链路」= 一个驱动实例。配置文件（单个文件、目录里的多个 `*.json`，或
直接内嵌 `drivers` 数组都行）：

```json
{
  "id": "plc1",                     // 链路 id（唯一）
  "path": "/PLC1",                  // 这条链路负责的模型路径前缀；"/" 为兜底
  "type": "modbus_tcp",             // 驱动注册表里的协议名
  "parameters": { "host": "10.0.0.5", "port": 502, "unit": 1 },
  "points": [
    { "path": "/PLC1/STATUS", "addr": "D100" },
    { "id": "POWER", "addr": {"area": "D", "offset": 101},
      "dtype": "float32", "writable": true }
  ]
}
```

- 点位路径可写绝对路径（`/PLC1/STATUS`）或相对路径（`STATUS`），内部统一
  按「链路前缀之后的相对路径」存表；查找时先取最长前缀，再回退到 `"/"`
  那条兜底链路（`/PLC10` 不会被 `/PLC1` 抢走）。
- `writable` 默认 **false**（§7：默认只读，写能力要显式开）；`sample`
  默认 true，设 false 可把高频/大流量点位排除在采样通道之外。

适配器的配置：

```json
{
  "sn": "V2AABBCCDD1",
  "driverDir": "conf/driver",        // 或 "driverFile"，或内嵌 "drivers"
  "model": "conf/model.json",        // 可选；不给就按点位表生成
  "device": { "type": "MACHINE", "id": "01", "name": "数控机床" },
  "methods": [ { "path": "/PLC1/START", "operation": "startProgram" } ],
  "sample": { "intervalMs": 1000, "uploadMs": 1000 }
}
```

守护进程为每个点位注册一条操作：`get_value#<路径>` 读、`set_value#<路径>` 写
（只有 `writable` 的点位才注册），`call#<路径>` 走驱动自己的方法。核心的采样
任务发的就是普通 Query，所以采样通道不需要额外机制。

```sh
./ncl_adapter -c conf/adapter.json            # MQTT + REST + 轮询
./ncl_adapter -c conf/adapter.json --once     # 读一遍全部点位就退出（自检）
```

自动生成的模型里，每个数据项的 `source` 就是点位路径的父级，因此
**模型路径与配置里的点位路径严格一致**，配置和模型不会漂移。
可运行的配置样例见 `tests/test_adapter.c` 里的 `kConfig`。

## 测试

```sh
.\build.ps1                      # Windows：配置 + 编译 + 28 个测试套件
sh build-linux.sh build-linux    # Linux：同样全跑一遍
```

适配器层的测试三件套：`tests/test_driver.c`（驱动接口：注册表、地址解析、
错误分级、mock 的读写/位寻址/批量/事件/原始报文）、
`tests/test_driver_manager.c`（配置加载、点位表、前缀分派）、
`tests/test_adapter.c`（配置 → 设备：生成模型、操作、读写、方法、轮询）。
协议驱动的测试以两段为主：报文级的黄金样本（字节级 diff），以及对着 mock
靶机的连接—读写—重连流程。
