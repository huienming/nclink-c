// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// 设备端示例：这个 Go 进程就是一台机床。
//
//	go run ./example/device [broker] [sn] [seconds] [http-port]
//
//	broker    tcp://… / ssl://…；"-" = 离线（不接 MQTT，出站报文打控制台）；
//	          省略 = 读 <root>/conf/mqtt.cfg（没有就先写一份默认的）
//	sn        省略或 "-" = <root>/bin/sn.txt（没有就生成 "V2" + 9 位十六进制）
//	seconds   0 或省略 = 一直运行到 Ctrl+C
//	http-port 省略 = 9008；0 = 系统分配的随机端口
//	安装根目录：环境变量 NCL_DEVICE_ROOT（省略 = 当前目录）
//
// **五个语言的设备端示例是同一台设备**：模型编译在绑定里（nclink.DeviceModel()，
// 与 C 示例共用 examples/device_model.c），29 个工具方法、20 条绑定，两个采样
// 通道与事件节拍都一样。每个轴一个功率 /AXIS@<轴>/POWER@1 与三个加速度
// /AXIS@<轴>/ACCELERATION@X|Y|Z —— 振动信号在三个方向上的分量。
package main

import (
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	nclink "github.com/huienming/nclink-c/bindings/go"
)

var (
	axes    = []string{"X", "Y", "Z", "C", "S"}
	dirs    = []string{"X", "Y", "Z"}
	scalars = []string{"getValue", "setValue", "getCount", "getWarning",
		"getProgram", "getToolNumber", "getFeedOverride", "getMachiningMode",
		"getSpeedS"}
	scalarPaths = []string{"/MACHINE/STATUS", "/MACHINE/STATUS", "/MACHINE/PART_COUNT",
		"/MACHINE/CONTROLLER/WARNING", "/MACHINE/CONTROLLER/PROGRAM", "/MACHINE/CONTROLLER/TOOL_NUMBER",
		"/MACHINE/FEED_OVERRIDE", "/MACHINE/MACHINING_MODE", "/MACHINE/AXIS@S/SPEED"}
)

const defaultBroker = "tcp://127.0.0.1:1883"

// machine 是被工具方法读写的"机床状态"（真机里换成你的 PLC / 采集卡）。
type machine struct {
	status        int
	partCount     int64
	program       int
	toolNumber    int
	feedOverride  int
	spindleSpeed  int
	mode          int
	powerTick     []int64
	vibrationTick [][3]int64
}

func newMachine() *machine {
	return &machine{
		status:        1,
		program:       1001,
		toolNumber:    1,
		feedOverride:  100,
		spindleSpeed:  3600,
		mode:          2,
		powerTick:     make([]int64, len(axes)),
		vibrationTick: make([][3]int64, len(axes)),
	}
}

func indexOf(values []string, value string) int {
	for i, item := range values {
		if item == value {
			return i
		}
	}
	return -1
}

// power：每轴一个基值 + 0~300 W 的缓升，25 格一个锯齿。
func (m *machine) power(axis string) float64 {
	slot := indexOf(axes, axis)
	tick := m.powerTick[slot]
	m.powerTick[slot]++
	return 800 + float64(slot)*250 + float64((tick+int64(slot)*7)%25)*12.5
}

// vibration：一次查询给 4 个 0.25 ms 子采样（1 ms 槽位里的 4 kHz 波形）。
func (m *machine) vibration(axis, dir string) []float64 {
	a := indexOf(axes, axis)
	d := indexOf(dirs, dir)
	slot := a*len(dirs) + d
	step := m.vibrationTick[a][d]
	m.vibrationTick[a][d] += 4
	block := make([]float64, 4)
	for k := range block {
		block[k] = float64(((step+int64(k)+int64(slot)*3)%16)-8) * 0.125
	}
	return block
}

func (m *machine) handle(method string, params any) (any, error) {
	if method == "slow" {
		/* 异步方法调用的示例：跑 0.3 s 再返回值（请求带 async 时库会立刻回
		 * handler，客户端随后用 Method/Status、Method/Result 查）。 */
		time.Sleep(300 * time.Millisecond)
		return map[string]any{"done": true, "parts": m.partCount}, nil
	}
	if method == "setValue" {
		if object, ok := params.(map[string]any); ok {
			if value, ok := object["value"].(float64); ok {
				m.status = int(value)
				fmt.Println("STATUS 被设置为", m.status)
				return true, nil
			}
		}
		return nil, fmt.Errorf("value 必须是整数")
	}
	switch method {
	case "getValue":
		return m.status, nil
	case "getCount":
		return m.partCount, nil
	case "getWarning":
		return 0, nil
	case "getProgram":
		return m.program, nil
	case "getToolNumber":
		return m.toolNumber, nil
	case "getFeedOverride":
		return m.feedOverride, nil
	case "getMachiningMode":
		return m.mode, nil
	case "getSpeedS":
		return m.spindleSpeed, nil
	}
	if strings.HasPrefix(method, "getPower") {
		return m.power(method[len("getPower"):]), nil
	}
	if strings.HasPrefix(method, "getAcceleration") {
		rest := method[len("getAcceleration"):]
		return m.vibration(rest[:1], rest[1:]), nil
	}
	return nil, fmt.Errorf("没有方法 %s", method)
}

func (m *machine) tick(i int64) {
	m.partCount++
	m.feedOverride = 60 + int(i%7)*10
	m.spindleSpeed = 3000 + int(i%5)*300
	if i%20 == 0 {
		m.program++
		m.toolNumber = 1 + m.program%8
		if m.program%2 == 0 {
			m.mode = 1
		} else {
			m.mode = 2
		}
	}
}

func bootstrap(root string) {
	for _, sub := range []string{"conf", "bin", "log"} {
		_ = os.MkdirAll(filepath.Join(root, sub), 0o755)
	}
	cfg := filepath.Join(root, "conf", "mqtt.cfg")
	if _, err := os.Stat(cfg); os.IsNotExist(err) {
		_ = os.WriteFile(cfg, []byte("url="+defaultBroker+
			"\r\nusername=\r\npassword=\r\n"), 0o644)
		fmt.Println("首次启动：写入 MQTT 配置（" + cfg + "）")
	}
}

func readMqttCfg(root string) string {
	text, err := os.ReadFile(filepath.Join(root, "conf", "mqtt.cfg"))
	if err == nil {
		for _, line := range strings.Split(string(text), "\n") {
			line = strings.TrimSpace(line)
			if strings.HasPrefix(line, "url=") {
				if url := strings.TrimSpace(line[len("url="):]); url != "" {
					return url
				}
			}
		}
	}
	return defaultBroker
}

// loadModel：现场 <root>/conf/model/nclink.json 优先，没有就写一份下来。
func loadModel(root string) string {
	installed := filepath.Join(root, "conf", "model", "nclink.json")
	if text, err := os.ReadFile(installed); err == nil {
		return string(text)
	}
	text := nclink.DeviceModel()
	_ = os.MkdirAll(filepath.Dir(installed), 0o755)
	_ = os.WriteFile(installed, []byte(text), 0o644)
	fmt.Println("首次启动：写入设备模型（" + installed + "，编译在绑定里的那份）")
	return text
}

func main() {
	brokerArg, snArg := "", ""
	seconds, httpPort := 0, 9008
	if len(os.Args) > 1 {
		brokerArg = os.Args[1]
	}
	if len(os.Args) > 2 {
		snArg = os.Args[2]
	}
	if len(os.Args) > 3 {
		seconds, _ = strconv.Atoi(os.Args[3])
	}
	if len(os.Args) > 4 {
		httpPort, _ = strconv.Atoi(os.Args[4])
	}
	root := os.Getenv("NCL_DEVICE_ROOT")
	if root == "" {
		root = "."
	}
	offline := brokerArg == "-"

	bootstrap(root)
	nclink.SetRoot(root)
	nclink.LogInit("")

	sn := snArg
	if sn == "" || sn == "-" {
		var err error
		if sn, err = nclink.ReadSN(); err != nil {
			fmt.Println("读 SN 失败:", err)
			os.Exit(1)
		}
	}
	broker := brokerArg
	if !offline && broker == "" {
		broker = readMqttCfg(root)
	}
	model := loadModel(root)

	// 方法表与绑定表：9 个标量 + 5 个轴功率 + 15 个方向加速度。
	var methods []nclink.ToolMethod
	var bindings []nclink.Binding
	for i, name := range scalars {
		operation := nclink.OpGetValue
		if name == "setValue" {
			operation = nclink.OpSetValue
		}
		methods = append(methods, nclink.ToolMethod{Name: name})
		bindings = append(bindings, nclink.Binding{
			Path: scalarPaths[i], Operation: operation, Method: name})
	}
	for _, axis := range axes {
		name := "getPower" + axis
		methods = append(methods, nclink.ToolMethod{Name: name})
		bindings = append(bindings, nclink.Binding{
			Path: "/MACHINE/AXIS@" + axis + "/MACHINE/POWER@1", Operation: nclink.OpGetValue,
			Method: name})
	}
	for _, axis := range axes {
		for _, dir := range dirs {
			name := "getAcceleration" + axis + dir
			methods = append(methods, nclink.ToolMethod{Name: name})
			bindings = append(bindings, nclink.Binding{
				Path:      "/MACHINE/AXIS@" + axis + "/ACCELERATION@" + dir,
				Operation: nclink.OpGetValue, Method: name})
		}
	}

	work := newMachine()
	/* 异步方法调用演示：慢方法（方法调用专用，不绑数据点路径）。 */
	methods = append(methods, nclink.ToolMethod{Name: "slow"})
	options := nclink.ServerOptions{SN: sn, Model: model}
	if offline {
		options.Publish = func(topic string, payload []byte) error {
			fmt.Printf("out  %s  %s\n", topic, payload)
			return nil
		}
	} else {
		options.Broker = broker
	}
	device, err := nclink.NewServer(options)
	if err != nil {
		fmt.Println("创建设备端失败:", err)
		os.Exit(1)
	}
	defer device.Close()

	if err := device.RegisterTool("plc", methods, bindings, work.handle); err != nil {
		fmt.Println("注册工具失败:", err)
		os.Exit(1)
	}
	_ = device.RegisterBuiltinTool()
	_ = device.RegisterFileTool()
	if !offline {
		if err := device.Subscribe(); err != nil {
			fmt.Println("订阅失败:", err)
			os.Exit(1)
		}
	}
	if err := device.InitSamples(); err != nil {
		fmt.Println("启动采样失败:", err)
		os.Exit(1)
	}
	endpoint, err := device.StartHTTP(uint(httpPort), true)
	if err != nil {
		fmt.Println("HTTP 端点失败:", err)
		os.Exit(1)
	}
	_ = endpoint.Route("GET", "/api/hello", func(*nclink.HTTPRequest) *nclink.HTTPReply {
		return &nclink.HTTPReply{Status: 200, Body: fmt.Sprintf(
			`{"sn":%q,"status":%d,"parts":%d}`, sn, work.status, work.partCount)}
	})

	fmt.Println("设备 SN:", sn)
	if offline {
		fmt.Println("MQTT: 离线模式（出站报文打到控制台）")
	} else {
		fmt.Println("MQTT:", broker)
	}
	fmt.Printf("HTTP: http://localhost:%d/swagger-ui（自定义路由 /api/hello）\n",
		endpoint.Port())
	fmt.Printf("工具 %d 个操作，采样通道 %d 个\n",
		device.OperationCount(), device.SampleCount())
	if seconds > 0 {
		fmt.Printf("运行 %d 秒（Ctrl+C 可随时退出）\n", seconds)
	} else {
		fmt.Println("运行直到 Ctrl+C")
	}

	signals := make(chan os.Signal, 1)
	signal.Notify(signals, os.Interrupt, syscall.SIGTERM)
	deadline := time.Time{}
	if seconds > 0 {
		deadline = time.Now().Add(time.Duration(seconds) * time.Second)
	}
	var i int64
running:
	for {
		select {
		case <-signals:
			fmt.Println("收到退出信号（Ctrl+C），开始停止…")
			break running
		default:
		}
		if !deadline.IsZero() && time.Now().After(deadline) {
			break
		}
		time.Sleep(100 * time.Millisecond)
		i++
		work.tick(i)
		if i%10 == 0 {
			_ = device.PushEvent("010307", map[string]any{
				"key": "PART_COUNT", "value": work.partCount,
				"oldValue": work.partCount - 1})
			fmt.Printf("事件 PART_COUNT=%d；采样上报 %d 次，状态 %d，刀号 %d\n",
				work.partCount, device.SampleUploadCount(), work.status,
				work.toolNumber)
		}
	}
	fmt.Printf("设备端退出统计：采样上报 %d 次，事件 %d 条\n",
		device.SampleUploadCount(), device.EventCount())
	nclink.LogShutdown()
}
