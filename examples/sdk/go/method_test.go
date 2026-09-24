// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package nclink

import (
	"testing"
	"time"
)

/*
异步方法调用（设备端离线驱动）：立刻拿句柄 → 查状态 → 轮询结果 →
句柄释放；失败的方法给出 NG + error；同步调用不带 handler。
*/
func TestAsyncMethodCall(t *testing.T) {
	server := newTestServer(t, nil)
	if err := server.RegisterTool("slow", []ToolMethod{{Name: "work"}, {Name: "boom"}},
		nil, func(method string, params any) (any, error) {
			if method == "boom" {
				return nil, &Error{Code: -1, Name: "IOException", Op: "boom"}
			}
			time.Sleep(150 * time.Millisecond)
			return map[string]any{"done": true}, nil
		}); err != nil {
		t.Fatalf("RegisterTool: %v", err)
	}

	ack, err := server.InvokeMethodCallAsync("slow/work", "")
	if err != nil {
		t.Fatalf("InvokeMethodCallAsync: %v", err)
	}
	text := ack.String()
	ack.Close()
	if !contains(text, `"code":"OK"`) || !contains(text, `"handler":"`) {
		t.Fatalf("异步应答应带 code=OK 与 handler: %s", text)
	}
	handler := fieldOf(text, "handler")
	if handler == "" {
		t.Fatalf("没有拿到 handler: %s", text)
	}

	status, err := server.InvokeMethodStatus(server.SN(), handler)
	if err != nil {
		t.Fatalf("InvokeMethodStatus: %v", err)
	}
	statusText := status.String()
	status.Close()
	if !contains(statusText, `"code":"OK"`) || !contains(statusText, `"status":"`) {
		t.Fatalf("状态应答不对: %s", statusText)
	}

	deadline := time.Now().Add(5 * time.Second)
	resultText := ""
	for time.Now().Before(deadline) {
		result, err := server.InvokeMethodResult(server.SN(), handler)
		if err != nil {
			t.Fatalf("InvokeMethodResult: %v", err)
		}
		resultText = result.String()
		result.Close()
		if !contains(resultText, `"code":"PENDING"`) {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if !contains(resultText, `"code":"OK"`) || !contains(resultText, `"result":"finished"`) {
		t.Fatalf("结果应答不对: %s", resultText)
	}
	if !contains(resultText, `"done":true`) {
		t.Fatalf("结果里应带方法返回值: %s", resultText)
	}

	// 句柄取走后释放：再查同句柄是 NG
	gone, err := server.InvokeMethodResult(server.SN(), handler)
	if err != nil {
		t.Fatalf("InvokeMethodResult(2): %v", err)
	}
	goneText := gone.String()
	gone.Close()
	if !contains(goneText, `"code":"NG"`) {
		t.Fatalf("取走后的句柄应查不到: %s", goneText)
	}

	// 失败的方法：NG + error
	ack, err = server.InvokeMethodCallAsync("slow/boom", "")
	if err != nil {
		t.Fatalf("InvokeMethodCallAsync(boom): %v", err)
	}
	handler = fieldOf(ack.String(), "handler")
	ack.Close()
	deadline = time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		result, err := server.InvokeMethodResult(server.SN(), handler)
		if err != nil {
			t.Fatalf("InvokeMethodResult(boom): %v", err)
		}
		resultText = result.String()
		result.Close()
		if !contains(resultText, `"code":"PENDING"`) {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if !contains(resultText, `"code":"NG"`) || !contains(resultText, `"result":"error"`) {
		t.Fatalf("失败方法的应答不对: %s", resultText)
	}

	// 同步调用：不带 handler
	if err := server.RegisterTool("sync", []ToolMethod{{Name: "work"}}, nil,
		func(string, any) (any, error) { return 7, nil }); err != nil {
		t.Fatalf("RegisterTool(sync): %v", err)
	}
	request, err := ParseMessage("Method/Call/Request/"+server.SN(),
		[]byte(`{"@id":"sync-1","method":"/sync/work"}`))
	if err != nil {
		t.Fatalf("ParseMessage: %v", err)
	}
	sync, err := server.InvokeMethodCall(request)
	if err != nil {
		t.Fatalf("InvokeMethodCall: %v", err)
	}
	syncText := sync.String()
	sync.Close()
	if contains(syncText, `"handler"`) {
		t.Fatalf("同步调用不该有 handler: %s", syncText)
	}
}

/* 下面是本文件用的两个极简 JSON 文本检查（不引入第三方库）。 */
func contains(text, needle string) bool {
	return len(text) >= len(needle) && indexOf(text, needle) >= 0
}

func indexOf(text, needle string) int {
	for i := 0; i+len(needle) <= len(text); i++ {
		if text[i:i+len(needle)] == needle {
			return i
		}
	}
	return -1
}

/** fieldOf 取 "key":"value" 里的 value（只用于测试断言）。 */
func fieldOf(text, key string) string {
	needle := `"` + key + `":"`
	at := indexOf(text, needle)
	if at < 0 {
		return ""
	}
	rest := text[at+len(needle):]
	end := indexOf(rest, `"`)
	if end < 0 {
		return ""
	}
	return rest[:end]
}
