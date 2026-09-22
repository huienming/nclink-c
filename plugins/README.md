# 适配器：一个文件一台机床

这个目录放厂商适配器的**源码**：一个 `.c` 一个适配器，编成可动态装载的模块
（`<build>/plugins/ncl_driver_<名字>.dll|.so`），由唯一的那台设备程序 `ncl_server`
启动时从 `<root>/plugins` 装载。**点位表在代码里**；配置文件只有连接参数、设备信息
与采样周期。

分工一句话：

| 谁 | 管什么 | 在哪 |
|---|---|---|
| **client**（协议实现） | 跟机床说什么字节：帧、会话、错误码，以及**有名字的语义函数**（`ncl_focas_part_count()`） | `clients/<协议>/` |
| **adapter**（本目录） | 这台机床对外有哪些量：哪条路径绑哪个函数 | `plugins/<名字>.c` |

协议字节一个字都不进适配器。反过来，点位表也不进配置——**改点位要重编模块**，这是有意的：
点位的名字与地址由编译器检查，模型路径不会因为手写 JSON 打错而漂。

---

## 1. 三种写法（按"知识在哪"选）

### ① 绑定：client 给了语义函数就绑，一行一个点（推荐）

```c
#include "nclink/ncl_tool.h"

#include "nclink/clients/focas.h"     /* 现场只看这一个头 */

static void *fanuc_open(const ncl_json *params, char **err)
{
    ncl_focas_config config;

    ncl_focas_config_default(&config);
    config.host = ncl_tool_param_str(params, "host", "");
    return ncl_focas_open(&config, err);   /* 返回值就是"实例" */
}

static void fanuc_close(void *ctx) { ncl_focas_close((ncl_focas *)ctx); }

NCL_TOOL_BEGIN("fanuc", "FANUC 数控机床", "MACHINE", 1000, 1000,
               fanuc_open, fanuc_close)
    NCL_DATAITEM_STR_SAMPLED("/STATUS",             ncl_focas_status)
    NCL_DATAITEM_I64_SAMPLED("/PART_COUNT",         ncl_focas_part_count)
    NCL_DATAITEM_STR_SAMPLED("/CONTROLLER/PROGRAM", ncl_focas_program_name)
    NCL_DATAITEM_F64("/AXIS@X/POSITION@REAL",
                     ncl_focas_axis_position, NCL_FOCAS_AXIS_X)
NCL_TOOL_END()

NCL_TOOL_MODULE("1.0.0", "FANUC 数控机床适配器")
```

三条约定，都是为了少想：

1. **实例就是 `open()` 返回的那个指针**，所以绑定宏里不写实例名 —— 也就不可能绑到一个
   没有实例的函数上。"必须有一个 client 实例"是结构上的，不是纪律。
2. **函数名即语义**：`ncl_focas_part_count()` 读回来的就是加工件数。哪个 item、哪一块、
   怎么解，是 client 的事，不是现场的事。
3. **失败带原因**：语义函数返回 `ncl_err`，应答就是 `code=NG` 加一句话
   （`/MACHINE/BROKEN 取值失败：NCL_ERR_CLOSED`）。要协议自己的原因（`EW_PROTOCOL -17`）
   就让 client 留一个 `xxx_last_error()`，再写覆盖档把它带上。

### ② 覆盖：要自己的解释时才写函数

同一个实例，把它读回来的东西重新解释：

```c
static ncl_err status_dispatch(void *ctx, const ncl_tool_point *self,
                               ncl_operation op, const ncl_json *params,
                               ncl_json **result, char **reason)
{
    ncl_focas *focas = (ncl_focas *)ctx;
    double a = 0.0; double b = 0.0;

    (void)op; (void)params;
    (void)ncl_focas_axis_position(focas, NCL_FOCAS_AXIS_X, &a);
    (void)ncl_focas_axis_position(focas, NCL_FOCAS_AXIS_Y, &b);
    return ncl_tool_reply_text(result, a > b ? "X 在前" : "Y 在前");
}

NCL_DATAITEM_SAMPLED("/STATUS", status_dispatch, NULL)
```

**绑定与覆盖混在同一张表里是常态**：能直接叫出名字的量绑，需要推导/换算/拼接的量自己写。
`plugins/focas.c` 就是混用的活样例（20 个点位里，18 个是绑定，2 个方法是覆盖）。

方法的两种写法：`NCL_METHOD(路径, 函数, 数据)` 自己写；`NCL_METHOD_CALL(路径, 函数)`
绑 client 的方法函数（签名 `ncl_err f(实例, params, result, reason)`）。

### ③ 协议：clients/ 里没有的协议，才写 client

见 [`clients/README.md`](../clients/README.md)。最小面是 `read_batch` 加
`create/open/close/destroy`，其余回调留 `NULL`，骨架替你答 `NCL_ERR_NOT_SUPPORTED`。

---

## 2. 声明语法速查

### 2.1 NCL_TOOL_BEGIN：名字、设备类型、周期

```c
NCL_TOOL_BEGIN("fanuc", "FANUC 数控机床", "MACHINE", 1000, 1000,
               fanuc_open, fanuc_close)
```

| 参数 | 说明 |
|---|---|
| 工具名 | 模型里采样通道的名字，方法调用地址的前缀（`fanuc/RESET`） |
| 描述 | `--plugins` 与 schema 里显示的一行 |
| **设备类型** | 这台设备在模型里是什么（表 1：`MACHINE`/`ROBOT`/…）。**只在这里写一次**，点位路径里不重复 |
| 采样/上报周期 | 默认采样通道的周期（ms）；0 = 这个声明不生成采样通道 |
| open/close | 连接开一次、关一次；`open()` 返回的东西就是每个绑定函数的实例 |

### 2.2 路径规则

**路径相对设备节点**，最后一段是数据对象（数据项或配置项），前面每一段都是组件（可嵌套）：

```
"/STATUS"                  → 设备上的数据项            /MACHINE/STATUS
"/CONTROLLER/PROGRAM"      → 组件 CONTROLLER 下的数据项 /MACHINE/CONTROLLER/PROGRAM
"/AXIS@X/POSITION@REAL"    → 组件 AXIS(number=X) 下的数据项 POSITION(number=REAL)
"/CONTROLLER/SUB@1/PARAM"  → 组件 CONTROLLER → 子组件 SUB(number=1) 下的配置项
```

- 每一段都能用 `type@number` 区分（`AXIS@X`、`SUB@1`、`POSITION@REAL`）。
- 运行时（模型里的路径、采样通道、REST 地址）是**绝对路径** `/MACHINE/...`，由设备段拼出来；
  客户端看到的、`ncl_host_point_path()` 返回的都是绝对路径。
- 点位**名字**从相对路径推（`@`→`_`、`/`→`.`）：`/AXIS@X/POSITION@REAL` → `AXIS_X.POSITION_REAL`，
  方法调用地址 `fanuc/AXIS_X.POSITION_REAL`。设备段不参与命名。
- 配置里的 `device.type` 可以不写；写了必须与这里一致（不一致启动就报错）。

### 2.3 两族宏与形状

数据对象只有两种（册 3 §5.4/§5.5）：**数据项**进模型的 `dataItems`（可采样），
**配置**进 `configs`（参数、坐标系、刀具表、元信息，**不得作为采样数据源**，所以没有 `_SAMPLED`）。
名字就是 `NCL_<族>_<形状>`：

| 你要的 | 数据项 | 配置 |
|---|---|---|
| 绑只读函数 | `NCL_DATAITEM_I64/_F64/_BOOL/_STR(路径, 函数[, 现场参数])`、`NCL_DATAITEM_JSON(路径, 函数)` | `NCL_CONFIG_I64/_F64/_BOOL/_STR(…)`、`NCL_CONFIG_JSON(路径, 函数)` |
| 绑只读函数并进采样 | `NCL_DATAITEM_I64_SAMPLED/…` | —— 配置不许采样 |
| 绑读写 | `NCL_DATAITEM_I64_RW(路径, 取, 置)`（`_F64/_STR` 同理） | `NCL_CONFIG_I64_RW(…)` |
| 自己写函数 | `NCL_DATAITEM[_SAMPLED\|_RW\|_OPS](路径, 函数, 数据)` | `NCL_CONFIG[_RW\|_OPS](…)` |
| 方法 | `NCL_METHOD(路径, 函数, 数据)` / `NCL_METHOD_CALL(路径, 函数)` | |

绑定函数的签名按类型固定（宏名里的类型就是出参类型）：

```c
ncl_err get(void *instance, long long *value);          /* NCL_*_I64   */
ncl_err get(void *instance, double    *value);          /* NCL_*_F64   */
ncl_err get(void *instance, bool      *value);          /* NCL_*_BOOL  */
ncl_err get(void *instance, char *out, size_t cap);     /* NCL_*_STR   */
ncl_err set(void *instance, 同类型的值);                 /* *_RW 的第二个函数 */
ncl_err get(void *instance, long long arg, 出参);         /* 给了现场参数时（轴号…）*/
```

`_JSON` 给的不是标量，是 client 交出来的一个 JSON（报警的 `{"number","text"}`、刀具表
这样的表）；适配器里照样只写"这条路径绑哪个函数"。

**一个概念一个名字**：`NCL_DATAITEM_F64(路径, 函数)` 与
`NCL_DATAITEM_F64(路径, 函数, NCL_FOCAS_AXIS_X)` 都叫 `NCL_DATAITEM_F64`，宏按参数个数选形状。

### 2.4 操作位、可写必然可读

- `_RW` 就是 `get_value | set_value`；集合类（文件 dict、刀具表 list）用 `_OPS` 写全
  `get_length / get_keys / get_attributes / add / delete`（册 5 表 11 / 表 13）。
  没声明的操作，宿主照表 2 回 `Unsupported Operation`，根本不会调到你的函数。
- **可写必然可读**：`set_value` / `add` / `delete` 都要求 `get_value` —— 审计要记写之前的
  旧值。真只写的东西（密码、复位脉冲）写成 `NCL_METHOD`，值走调用参数。
- **配置不许进采样通道**：手写表把 `config` 与 `sampled` 同时置位，校验直接拒。

### 2.5 协议调用还没抓到帧：`NCL_ERR_UNAVAILABLE`

这种点位**没有第二种声明形状** —— 照常写一行绑定，函数挂在 client 那边；"还没实现"
由那个函数的返回值说，不由点位表说：

```c
/* plugins/focas.c：报警照常绑，函数在 client 里 */
NCL_DATAITEM_JSON_SAMPLED("/WARNING", ncl_focas_alarm)
```

```c
/* clients/focas/focas_values.c：帧还没抓到，所以现在这样回 */
ncl_err ncl_focas_alarm(ncl_focas *focas, ncl_json **value)
{
    *value = NULL;
    return not_yet(focas, "报警", "cnc_rdalmmsg2，01 册 §2.3 / 31 册");  /* → NCL_ERR_UNAVAILABLE */
}
```

于是：模型里看得见它、客户端问它有明确答复（`NG` + "还读不了（UnavailableException）"，
不是"没有这个点位"）、自检把它算成"待抓包"而不是失败、轮询第一次问过之后就不再碰它、
§6 审计里没有它（根本没问过机床）；带 `_SAMPLED` 的会占住采样通道的位置（那一列暂时是
`null`）。**抓包补上时只改 client 里那个函数体** —— 点位表一行都不用动，这也是"知识留在
client"的同一件事。

### 2.6 参数有三条通道，别混

| 参数 | 从哪来 |
|---|---|
| 配置里的 `parameters`（IP/端口/超时…） | `open(params, err)` 拿一次，返回值传给每个点位 |
| 客户端的请求参数（Query 的 params、Set 的 `"value"`、方法调用的 arguments） | 函数的 `params` |
| **点位自己的数据**（寄存器地址、协议项名、映射表条目） | 声明时挂在点位上，函数从 `self->arg` 取 |

---

## 3. 模块：怎么被装载

- 文件放 `plugins/` 就会被编成 `ncl_driver_<文件名>.dll|.so`（`file(GLOB plugins/*.c)`），
  **不用改 CMake**；工具名与文件名一致，所以配置里写工具名即可。
- 最后一行 `NCL_TOOL_MODULE("版本", "描述")` 导出模块入口 —— 宿主就靠它拿到声明。
- 装载器核对 **ABI 代次**（当前 3）：代次不对、或文件根本没导出入口，都会**用平台自己的
  原因拒收**（不会变成"协议未注册"这种没头没脑的话）。一代模块（交驱动工厂的老式模块）
  被明确拒收并提示改写成声明。
- `-DNCLINK_BUILD_PLUGINS=OFF` 得到一台不装任何厂商适配器的设备程序：照样起、照样提供
  REST 与文件工具，只是没有点位。

## 4. 配置文件：只剩参数、设备、采样与 broker

```json
{
  "tools": [ { "name": "fanuc", "parameters": { "host": "192.168.1.100", "port": 8193 } } ],
  "device": { "type": "MACHINE", "id": "01", "name": "FANUC 数控机床" },
  "sample": { "intervalMs": 1000, "uploadMs": 1000 },
  "plugins": { "load": ["fanuc"] },
  "model":  "conf/model/01.json"
}
```

- 点位**不在**这里；`tools[].parameters` 就是 `open()` 收到的那个对象。
- `device.type` 只用来跟声明核对；`id` / `name` 填模型里的设备节点。
- `mqtt` 一段可以不写：不给就是离线（照样读、照样 REST，不上总线）。优先顺序
  **命令行 `-b` → 配置里的 `mqtt` → `<root>/conf/mqtt.cfg`**。
- `model` 指一份模型文件时以它为准（现场要单独调采样周期就存一份）。

## 5. 跑起来

```sh
ncl_server -c conf/fanuc.json                 # 一直跑：MQTT + REST + 轮询，Ctrl+C 退出
ncl_server -c conf/fanuc.json --plugins       # 列装载到的模块与它们的工具，然后退出
ncl_server -c conf/fanuc.json --once          # 每个点位读一遍（自检），退出
ncl_server -c conf/fanuc.json --probe /STATUS # 单点试读，退出
ncl_server -c conf/fanuc.json --model         # 把这份声明生成的设备模型打出来，退出
ncl_server -c conf/fanuc.json -b tcp://10.0.0.9:1883
ncl_server -c conf/fanuc.json --offline       # 或 -b -：不接 broker
ncl_server -c conf/fanuc.json --stats --raw   # 跑完自检再打 §6 审计计数（含原始报文）
```

`-P/--plugin-dir` 换模块目录，`--interval` 轮询周期，`--port` REST 端口，`--root` 安装根目录。
broker 没起来不致命：宿主按退避重试（1 s 起、上限 30 s），连上后补订阅。

## 6. 自检与审计（§6）

- **审计由宿主记，适配器作者不写审计代码**：`ncl_tool_register()` 收一个审计回调，core 里的
  shim 在每次点位调用前后把「路径、操作、结果、耗时」交出去，写操作还会先读一次旧值。
  模块唯一可选的是交出原始帧：`NCL_TOOL_END_WITH_RAW(fn)` 一行。
- 日志落 `<root>/log/out.txt`；写操作恒 `INFO`，请求行按结果分 `DEBUG` / `WARNING`；
  `--raw` 打开时日志里带交换的报文 hex。
- 自检口径：`--once` 会把"还读不了"（`NCL_ERR_UNAVAILABLE`）的点位列成待抓包（不是失败），
  读失败才计失败。

## 7. 构建与测试

```sh
.\build.ps1                     # Windows：配置 + 编译 + 全部测试（当前 42 套）
sh build-linux.sh               # Linux：同样全跑一遍
```

- 适配器模块跟着默认构建一起出（`<build>/plugins/`）。
- 加一套适配器的测试：放 `tests/` 里，用 `ncl_tool_*` 与 `ncl_host_*` 的公开 API 驱动，
  不碰内部结构（`tests/test_bind.c` 是模板）。
- 静态内存版（`NCL_STATIC_MEM`）与插件模块**不能混**：两边各有一块内存池，谁也释放不了
  对方的内存块 —— 这是模块设计的已知边界。

## 8. 排错

| 现象 | 先看哪 |
|---|---|
| `--plugins` 里没有你的模块 | 文件是否在 `<root>/plugins`、名字是否是 `ncl_driver_<工具名>.<后缀>`、ABI 报错原文 |
| 启动报"点位路径的设备段与配置的 device.type 不一致" | 路径不要写设备段；要换机型改 `NCL_TOOL_BEGIN` 那个参数 |
| 客户端问点位回 `Unsupported Operation` | 点位没声明那个操作位（`_RW` / `_OPS`） |
| 点位回 NG 且理由是 `取值失败：NCL_ERR_*` | 语义函数报的错；看 client 的 `last_error()` 与机床侧 |
| 点位回 NG "还读不了（UnavailableException）" | client 里那个函数回 `NCL_ERR_UNAVAILABLE`：协议调用还没实现（见 2.5） |
| 采样的那一列一直是 `null` | 那个点位的协议调用还没实现（见 2.5），或 client 读失败（看日志） |
| 写被拒 | 点位只读（没有 `_RW`），或机床侧不支持写 |

## 9. 例子索引

| 想看的 | 文件 |
|---|---|
| 绑定 + 覆盖混用、真实协议 | `plugins/focas.c`（FANUC，20 点位 + 2 方法） |
| 只读适配器、九项现场闭环 | `plugins/syntec.c`（新代，9 点位 + 1 方法；形状见 10 册 §3.1/§3.2） |
| 最小适配器（零协议代码） | `tests/module_tool_basic.c`（夹具，98 行） |
| 绑定层本身的测试 | `tests/test_bind.c` |
| 各协议怎么说话 | [`clients/README.md`](../clients/README.md) |
| FANUC 现场手册（随包发布） | [`FANUC-ADAPTER.md`](FANUC-ADAPTER.md) |
