# 22 · Universal Robots 优傲（RTDE + Dashboard）实现规格书

> **证据**：🟢 全实证 —— 官方 `ur_rtde 1.6.5` 源码（C++ 头文件 + Python 绑定）
> **定位**：协作机器人**全球第一**（UB 系列 e-Series/CB 系列），接口开放，**官方库开源，零授权成本**

---

## 1. 速查

| 端口 | 用途 | 协议 |
|---|---|---|
| **30004** | **RTDE**（实时数据交换，主采集口） | 二进制（TCP） |
| **29999** | **Dashboard**（控制/查询，文本命令） | ASCII 文本 + `\n`（TCP） |
| 30001 | Primary Interface（URScript 主接口） | ASCII 文本 |
| 30002 | Secondary Interface（URScript 副接口） | ASCII 文本 |
| 30003 | **Realtime Interface**（125Hz 老接口，固定 1108 字节包） | 二进制 |
| 30020 | Interpreter Mode | ASCII 文本 |

| 项 | 值 |
|---|---|
| 字节序 | **大端**（网络序，RTDE 负载） |
| 推荐库 | **`ur_rtde`**（官方维护，C++/Python）|
| 采集频率 | RTDE 支持 125Hz / 500Hz（e-Series） |
| 前置 | 机器人装 **RTDE 程序**（Polyscope 里加载 `rtde.urp`）并运行 |

---

## 2. RTDE 协议（端口 30004）

### 2.1 命令码（单字节 ASCII）

| 码 | ASCII | 名称 | 用途 |
|---|---|---|---|
| 86 | `V` | `RTDE_REQUEST_PROTOCOL_VERSION` | 协商协议版本 |
| 118 | `v` | `RTDE_GET_URCONTROL_VERSION` | 取控制器版本 |
| 77 | `M` | `RTDE_TEXT_MESSAGE` | 文本消息（双向） |
| 85 | `U` | `RTDE_DATA_PACKAGE` | **数据包（接收/发送）** |
| 79 | `O` | `RTDE_CONTROL_PACKAGE_SETUP_OUTPUTS` | **订阅输出（机器人→上位机）** |
| 73 | `I` | `RTDE_CONTROL_PACKAGE_SETUP_INPUTS` | 声明输入（上位机→机器人） |
| 83 | `S` | `RTDE_CONTROL_PACKAGE_START` | **启动数据流** |
| 80 | `P` | `RTDE_CONTROL_PACKAGE_PAUSE` | 暂停数据流 |

### 2.2 报文结构

```
[长度 2字节大端] [命令 1字节] [负载 N字节]

SETUP_OUTPUTS 请求负载：
  [变量名列表: 每个 "name" + \0，以空串 \0 结束] [输出频率 double(8, 大端)]

SETUP_OUTPUTS 响应：
  [1字节 结果: 'U'=成功 'E'=失败] [变量类型串: 每个 "TYPE" + \0]

类型串: VECTOR6D / VECTOR3D / DOUBLE / INT32 / UINT32 / UINT64 / BOOL / VECTOR6INT32 …

DATA_PACKAGE（响应）：
  [按订阅顺序的二进制数据，无分隔]  —— 依 §2.3 类型解析
```

### 2.3 RTDE 变量表（84+ 订阅项）

| 类别 | 变量 | 类型 |
|---|---|---|
| **关节** | `actual_q` `target_q` | VECTOR6D |
| | `actual_qd` `target_qd` | VECTOR6D |
| | `actual_qdd` `target_qdd` | VECTOR6D |
| | `actual_current` `target_current` | VECTOR6D |
| | `actual_moment` `target_moment` | VECTOR6D |
| | `actual_current_as_torque` | VECTOR6D |
| **TCP** | `actual_TCP_pose` `target_TCP_pose` | VECTOR6D |
| | `actual_TCP_speed` `target_TCP_speed` | VECTOR6D |
| | `actual_TCP_force` | VECTOR6D |
| **状态** | `robot_mode` | INT32 |
| | `safety_mode` | INT32 |
| | `runtime_state` | UINT32 |
| | `robot_status_bits` | UINT32 |
| | `safety_status_bits` | UINT32 |
| | `joint_mode` | VECTOR6INT32 |
| **时间/速度** | `timestamp` | DOUBLE |
| | `actual_execution_time` | DOUBLE |
| | `speed_scaling` | DOUBLE |
| | `target_speed_fraction` | DOUBLE |
| | `actual_momentum` | DOUBLE |
| **电气** | `joint_temperatures` | VECTOR6D |
| | `actual_joint_voltage` | VECTOR6D |
| | `actual_main_voltage` `actual_robot_voltage` `actual_robot_current` | DOUBLE |
| **IO** | `actual_digital_input_bits` | UINT64 |
| | `actual_digital_output_bits` | UINT64 |
| | `standard_analog_input0/1` `standard_analog_output0/1` | DOUBLE |
| **寄存器** | `output_int_register_0..47` | INT32（各 1） |
| | `output_double_register_0..47` | DOUBLE（各 1） |
| | `output_bit_registers0_to_31` / `32_to_63` | UINT32 |
| （输入侧对应 `input_int_register_{0..47}` / `input_double_register_*` / `input_bit_register_*`） |
| **负载/力** | `payload`（DOUBLE）· `payload_cog`（VECTOR3D）· `payload_inertia`（VECTOR6D）· `ft_raw_wrench`（VECTOR6D） |
| **其它** | `actual_tool_accelerometer`（VECTOR3D）· `joint_control_output`（VECTOR6D） |

### 2.4 连接流程（伪代码）

```python
from rtde_receive import RTDEReceiveInterface
rtde_r = RTDEReceiveInterface("192.168.1.10")          # 自动完成 V/O/S 协商
for _ in range(10):
    q  = rtde_r.getActualQ()            # 关节角 [6]
    p  = rtde_r.getActualTCPPose()      # TCP 位姿 [x,y,z,rx,ry,rz]
    v  = rtde_r.getActualTCPSpeed()
    f  = rtde_r.getActualTCPForce()
    mode = rtde_r.getRobotMode()        # 机器人模式
    sf   = rtde_r.getSpeedScaling()
    di   = rtde_r.getActualDigitalInputBits()
    time.sleep(0.1)
```

**手动实现顺序**：`TCP connect 30004` → `V`(协商版本) → `v`(取版本) → `O`(SETUP_OUTPUTS，列变量+频率) 读响应类型串 → `S`(START) → 循环收 `U` 数据包 → `P`(PAUSE) → 断开。

---

## 3. Dashboard 协议（端口 29999）

**格式**：纯文本命令 + `\n`，响应一行文本。

| 命令 | 说明 |
|---|---|
| `power on` / `power off` | 上电/下电 |
| `brake release` | 松开抱闸 |
| `play` / `stop` / `pause` | 程序控制 |
| `programState` | **程序状态**（STOPPED/PLAYING/PAUSED） |
| `robotmode` | **机器人模式** |
| `safetymode` | **安全模式** |
| `running` | 是否运行中 |
| `load <文件名.urp>` | 加载程序 |
| `get loaded program` | 当前程序名 |
| `get serial number` / `get robot model` | 序列号/型号 |
| `polyscope version` | Polyscope 版本 |
| `is in remote control` | 是否远程控制 |
| `unlock protective stop` | **解除保护性停止** |
| `close safety popup` / `close popup` | 关弹窗 |
| `restart safety` | 重启安全 |
| `popup <msg>` | 弹窗 |
| `add to log <msg>` | 写日志 |
| `set user role <role>` | 权限（locked/none/operator/programmer/restricted） |
| `quit` / `shutdown` | 退出/关机 |

> 共 32 个 API 方法（`ur_rtde` 的 `DashboardClient`）。

---

## 4. 实现坑

1. **必须先在机器人上运行 RTDE 程序**（Polyscope 加载 `rtde.urp` 并 PLAY）——否则 30004 连上后 SETUP 会失败。
2. **SETUP_OUTPUTS 必须在 START 之前**，且变量名拼写完全一致（响应会回类型串，要逐字符比对，长度不符即失败）。
3. **数据包无分隔符**：必须按类型串顺序和宽度精确解析（VECTOR6D=48 字节，DOUBLE=8，INT32=4，UINT64=8）。
4. **大端**（网络字节序）—— 与 PLC 类协议一致，但和多数机器人协议不同。
5. **robot_mode / safety_mode 是枚举**，需按 §5 映射后再用。
6. **高频采集**：RTDE 标称 500Hz（e-Series）/ 125Hz（CB3），实际受网络与解析速度限制；建议 100-200Hz 起。
7. **Dashboard 会阻塞**（发命令后等一行响应），不要与 RTDE 循环混在同一线程。
8. **写类操作**（`power off` / `brake release` / `unlock protective stop`）**有安全风险** —— 生产默认只读。

---

## 5. 枚举映射

### robot_mode（`getRobotMode()`）
| 值 | 含义 |
|---|---|
| -1 | ROBOT_MODE_NO_CONTROLLER |
| 0 | ROBOT_MODE_DISCONNECTED |
| 1 | ROBOT_MODE_CONFIRM_SAFETY |
| 2 | ROBOT_MODE_BOOTING |
| 3 | ROBOT_MODE_POWER_OFF |
| 4 | ROBOT_MODE_POWER_ON |
| 5 | **ROBOT_MODE_IDLE**（待机） |
| 6 | **ROBOT_MODE_BACKDRIVE**（拖动） |
| 7 | **ROBOT_MODE_RUNNING**（运行） |
| 8 | ROBOT_MODE_UPDATING_FIRMWARE |

### safety_mode
| 值 | 含义 |
|---|---|
| 1 | NORMAL |
| 2 | REDUCED |
| 3 | PROTECTIVE_STOP |
| 4 | RECOVERY |
| 5 | SAFETY_STOP |
| 6 | SYSTEM_THREE_POSITION_ENABLING_DEVICE |
| 7 | ROBOT_EMERGENCY_STOP |
| 8 | VIOLATION |
| 9 | FAULT |

---

## 6. 参考实现

| 来源 | 说明 |
|---|---|
| `supp/ur_rtde-1.6.5`（本仓库） | **官方 C++ 库 + Python 绑定**：`include/ur_rtde/rtde.h`（协议常量）· `robot_state.h`（变量表）· `dashboard_client.h`（32 命令）· `src/robot_state.cpp`（变量类型表） |
| 提取产物 | 84 变量+类型 · 32 Dashboard API（原始素材未随本目录提供） |
| 官方文档 | UR RTDE Guide（变量清单/版本差异）· `ur_rtde` 官方文档站 |
| 备注 | CB 系列（3.x）与 e-Series 的变量集有差异，以控制器版本为准 |
