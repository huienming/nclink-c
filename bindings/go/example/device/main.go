// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// Go device example: this process is one NC-Link device. A C, C#, Java or
// Python client can probe it, read /STATUS, watch the sample reports and talk
// to its REST endpoint.
//
//	go run ./example/device                          # offline: outbound messages are printed
//	go run ./example/device tcp://127.0.0.1:1883     # publish through a broker
package main

import (
	"fmt"
	"os"
	"os/signal"
	"syscall"

	nclink "github.com/huienming/nclink-c/bindings/go"
)

const (
	sn = "V2GODEV0001"

	model = `{"name":"Go 设备示例","id":"01","type":"NC_LINK_ROOT",` +
		`"devices":[{"id":"02","type":"MACHINE","name":"模拟机床",` +
		`"configs":[{"name":"采样通道","id":"ch1","type":"SAMPLE_CHANNEL",` +
		`"sampleInterval":1000,"uploadInterval":1000,` +
		`"ids":[{"id":"/STATUS"}]}],` +
		`"dataItems":[{"name":"状态","id":"030001","type":"STATUS",` +
		`"settable":false},` +
		`{"name":"加工计件","id":"030002","type":"PART_COUNT",` +
		`"settable":true}]}]}`
)

func main() {
	broker := ""
	if len(os.Args) > 1 {
		broker = os.Args[1]
	}

	options := nclink.ServerOptions{SN: sn, Model: model, Broker: broker}
	if broker == "" {
		// No broker: hand every outbound message to this callback instead, so
		// the device can be exercised without any infrastructure.
		options.Publish = func(topic string, payload []byte) error {
			fmt.Printf("out  %s  %s\n", topic, payload)
			return nil
		}
	}

	device, err := nclink.NewServer(options)
	if err != nil {
		fmt.Println("new server:", err)
		os.Exit(1)
	}
	defer device.Close()

	// The tool behind /STATUS and /PART_COUNT. A handler may return any
	// JSON-marshalable value, or an error (reported as code NG with the text).
	count := int64(0)
	status := int64(1)
	err = device.RegisterTool("plc",
		[]nclink.ToolMethod{
			{Name: "getStatus"},
			{Name: "getCount"},
			{Name: "setCount", Schema: `{"type":"object","properties":` +
				`{"value":{"type":"number"}},"required":["value"]}`},
		},
		[]nclink.Binding{
			{Path: "/STATUS", Operation: nclink.OpGetValue, Method: "getStatus"},
			{Path: "/PART_COUNT", Operation: nclink.OpGetValue, Method: "getCount"},
			{Path: "/PART_COUNT", Operation: nclink.OpSetValue, Method: "setCount"},
		},
		func(method string, params any) (any, error) {
			switch method {
			case "setCount":
				object, ok := params.(map[string]any)
				if !ok {
					return nil, fmt.Errorf("缺少 value 参数")
				}
				value, ok := object["value"].(float64)
				if !ok {
					return nil, fmt.Errorf("value 必须是数字")
				}
				count = int64(value)
				return count, nil
			case "getCount":
				return count, nil
			}
			return status, nil
		})
	if err != nil {
		fmt.Println("register tool:", err)
		os.Exit(1)
	}

	if err := device.RegisterBuiltinTool(); err != nil { // /nclinkServer/addSample
		fmt.Println("builtin tool:", err)
		os.Exit(1)
	}
	if err := device.RegisterFileTool(); err != nil { // /CONTROLLER/FILE
		fmt.Println("file tool:", err)
		os.Exit(1)
	}
	if broker != "" {
		if err := device.Subscribe(); err != nil {
			fmt.Println("subscribe:", err)
			os.Exit(1)
		}
	}
	if err := device.InitSamples(); err != nil {
		fmt.Println("init samples:", err)
		os.Exit(1)
	}

	endpoint, err := device.StartHTTP(9008, true)
	if err != nil {
		fmt.Println("http:", err)
		os.Exit(1)
	}
	defer endpoint.Close()
	if err := endpoint.Route("GET", "/api/hello",
		func(request *nclink.HTTPRequest) *nclink.HTTPReply {
			return &nclink.HTTPReply{Status: 200,
				Body: fmt.Sprintf(`{"hello":%q}`, request.Query)}
		}); err != nil {
		fmt.Println("route:", err)
		os.Exit(1)
	}

	fmt.Printf("device %s is up (nclink %s), operations=%d\n",
		device.SN(), nclink.Version(), device.OperationCount())
	fmt.Printf("  broker   : %s\n", orDash(broker))
	fmt.Printf("  swagger  : http://127.0.0.1:%d/swagger-ui\n", endpoint.Port())
	fmt.Printf("  samples  : %d channel(s)\n", device.SampleCount())
	fmt.Println("Ctrl+C to stop")

	signals := make(chan os.Signal, 1)
	signal.Notify(signals, os.Interrupt, syscall.SIGTERM)
	<-signals
	fmt.Printf("\nstopping: %d sample report(s), %d event(s)\n",
		device.SampleUploadCount(), device.EventCount())
}

func orDash(text string) string {
	if text == "" {
		return "-  (offline: outbound messages go to stdout)"
	}
	return text
}
