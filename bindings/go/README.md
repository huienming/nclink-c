# NC-Link Go 绑定（cgo）

`go get github.com/huienming/nclink-c/bindings/go`（或直接把本目录拷进工程）。

## 链接

包内默认链接 `lib/<goos>-<goarch>/` 下的静态库：

```
bindings/go/lib/windows-amd64/libnclink_core.a   # 用 mingw 编的（cgo 的 Windows 工具链是 mingw）
bindings/go/lib/linux-amd64/libnclink_core.a
```

`tools/stage-go-libs.sh` 会把各平台的库拷到这里（库本身不入库）。

- **Windows**：cgo 需要 mingw-w64（不认 MSVC），且必须链 **mingw 编的库**；
  MSVC 的 `nclink_core.lib` 不能用于 Go。
  编库：`CC=<mingw>/gcc AR=<mingw>/ar sh build-linux.sh build-mingw`
  或直接：`for f in $(find src -name '*.c'); do gcc -c -O2 -Iinclude -Isrc $f; done` + `ar rcs`。
- **Linux**：`sh build-linux.sh build-linux` 出的 `libnclink_core.a` 直接用。
- **TLS**：加 `-tags nclink_tls`（Linux 需 `-lssl -lcrypto`，Windows 需静态 OpenSSL）。

## 用法

```go
if err := nclink.Open("tcp://broker:1883", "", ""); err != nil { log.Fatal(err) }
defer nclink.Shutdown()

c, err := nclink.Get("V2023A7B762")
model, err := c.Probe(5000)              // 拉模型
v, err := c.Value("/STATUS", 5000)       // 读值
defer v.Close()
err = c.Set("/STATUS", mustJSON("42"), 5000)

err = c.SubscribeSamples(2, func(topic string, msg *nclink.Message) {
    // msg 只在回调期间有效（C 侧规则一致）
})
```

所有 C 内存都由 `Close()` 释放（`runtime.SetFinalizer` 兜底）。
