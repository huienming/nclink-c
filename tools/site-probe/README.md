# site-probe · 把现场包的 ARM 代码跑起来看协议

现场交付包（iNC-BOX-200）里全是 **ELF32 ARM hard-float**：Go 网关
`hp2x_box200`、`nclink-service`、以及 20 多个厂商协议插件 `lib*.so`。
Docker Desktop 自带 `binfmt`，加 `--platform linux/arm/v7` 就能让它们在本机
跑起来；再把 `ipAddress` 指向我们自己的"假机床"，就能把**设备侧报文**抓下来
——不必等现场机床，也不必等厂商文档。这套东西就是干这个的。

```sh
cd tools/site-probe
docker build --platform linux/arm/v7 -t ncl-arm-probe .

# 只看 FANUC FOCAS 握手（不需要任何 FOCAS 头文件，走 dlsym）
docker run --rm --platform linux/arm/v7 \
  -v <现场包>/app1/nclink-service:/svc:ro -v $PWD:/work \
  ncl-arm-probe sh /work/focas_run.sh

# 直接驱动某个插件，让它去连我们监听的 6000
docker run --rm --platform linux/arm/v7 \
  -v <现场包>/app1/nclink-service:/svc:ro -v $PWD:/work \
  ncl-arm-probe sh /work/plugin_drive.sh libgsk-http.so 6000 /GSK/CNC/Open/TCP
```

## 已经拿到什么

1. **FOCAS2 握手字节**（🟢 实测，`focas_run.sh`）。`cnc_allclibhndl3()` 对假机床
   依次发（两次 TCP 连接、四条消息）：

   ```
   a0 a0 a0 a0 00 01 01 01 00 02 00 01     # 12 字节，第一次连接
   a0 a0 a0 a0 00 01 01 01 00 02 00 02     # 12 字节，第二次连接（计数器 +1）
   a0 a0 a0 a0 00 01 21 01 00 00           # 10 字节
   a0 a0 a0 a0 00 01 02 01 00 00           # 10 字节
   ```

   没回对时 `cnc_allclibhndl3` 返回 **-16（EW_SOCKET）**，且会重试一次。
   要往下拿数据调用（`cnc_statinfo` / `cnc_rdparam` …）得先把这四条回应答对；
   真实应答需要一台 FANUC 抓一次，或按 Fwlib32 手册补全。

2. **插件的 C 入口点与签名**（🟢 `.dynsym` + 反汇编）：

   ```c
   void *create(nlohmann::json *params, spdlog::logger *logger);   /* 见下 */
   int   call(void *instance, const char *method, const char *params,
              std::string *out);
   void  destroy(void *instance);
   const char *get_version(void);
   ```

   `create` 的第一个参数是 `nlohmann::json`（反汇编里直接 `operator[]`），
   `call` 把第二个参数当 `const char *` 造成 `std::string`。

3. **依赖顺序**：`libstdc++.so.6` → `libbase64.so` → `libLogApi.so`（还要
   `libboost_thread.so`）→ `libbase.so` → 具体插件。现场库是 C++ 但 **NEEDED 里
   没有 libstdc++**，所以必须先在宿主里把 libstdc++ 拉起来（现场是
   `nclink-service` 干的）。

## 还差什么

`plugin_drive.cpp` 现在能加载插件、解析出入口点，但 `create()` 会崩：
插件用的是它自己那份 nlohmann/spdlog（`../../thirdparty/nlohmann/json.hpp`），
与 Debian 的 3.11.2 头文件的模板实例（`std::less<void>` 比较器）**ABI 不一致**。
两条路：

1. 找到插件构建时的 nlohmann/spdlog 版本（`.dynsym` 里的 `json_abi_v3_11_2` 之类
   命名空间后缀能确认版本），用同一份头文件编 harness；
2. 或者直接跑现场自己的宿主 `nclink-service`（它按正确顺序加载插件），
   用 `driver_def` 把 `ipAddress` 指向假机床，再想办法触发一次读。

任一条通了，GSK / KEDE / Mitsubishi-HTTP / 相机这几家的**设备侧请求形状**就能
在本地补齐，不用等现场（`protocal/docs/29-现场模型与驱动定义.md` §6）。
