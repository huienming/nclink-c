// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package nclink

import (
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
)

const (
	serverSN    = "V2GOTEST001"
	serverModel = `{"name":"Go 机床","id":"01","type":"NC_LINK_ROOT",` +
		`"devices":[{"id":"02","type":"MACHINE","name":"模拟机床",` +
		`"configs":[{"name":"采样通道","id":"ch1","type":"SAMPLE_CHANNEL",` +
		`"sampleInterval":200,"uploadInterval":200,` +
		`"ids":[{"id":"/MACHINE/STATUS"}]}],` +
		`"dataItems":[{"name":"状态","id":"030001","type":"STATUS",` +
		`"settable":false},` +
		`{"name":"加工计件","id":"030002","type":"PART_COUNT","settable":true}]}]}`
)

// newTestServer builds an offline device (no broker) with the test model.
func newTestServer(t *testing.T, publish func(topic string, payload []byte) error) *Server {
	t.Helper()
	server, err := NewServer(ServerOptions{SN: serverSN, Model: serverModel, Publish: publish})
	if err != nil {
		t.Fatalf("NewServer: %v", err)
	}
	t.Cleanup(server.Close)
	if got := server.SN(); got != serverSN {
		t.Fatalf("SN() = %q, want %q", got, serverSN)
	}
	return server
}

func queryRequest(t *testing.T, id, path string) *Message {
	t.Helper()
	payload := fmt.Sprintf(`{"@id":%q,"ids":[{"id":%q,"params":{"operation":"get_value"}}]}`,
		id, path)
	request, err := ParseMessage("Query/Request/"+serverSN, []byte(payload))
	if err != nil {
		t.Fatalf("ParseMessage(query): %v", err)
	}
	return request
}

func methodRequest(t *testing.T, id, method, params string) *Message {
	t.Helper()
	payload := fmt.Sprintf(`{"@id":%q,"method":%q,"params":%s}`, id, method, params)
	request, err := ParseMessage("Method/Call/Request/"+serverSN, []byte(payload))
	if err != nil {
		t.Fatalf("ParseMessage(method): %v", err)
	}
	return request
}

func TestServerToolAndQuery(t *testing.T) {
	server := newTestServer(t, nil)

	var mu sync.Mutex
	var seen []string
	setCount := 0
	err := server.RegisterTool("plc",
		[]ToolMethod{
			{Name: "getStatus"},
			{Name: "setCount", Schema: `{"type":"object","properties":{"value":` +
				`{"type":"number"}},"required":["value"]}`},
		},
		[]Binding{
			{Path: "/MACHINE/STATUS", Operation: OpGetValue, Method: "getStatus"},
			{Path: "/MACHINE/PART_COUNT", Operation: OpSetValue, Method: "setCount"},
		},
		func(method string, params any) (any, error) {
			mu.Lock()
			defer mu.Unlock()
			seen = append(seen, method)
			if method == "setCount" {
				if object, ok := params.(map[string]any); ok {
					if value, ok := object["value"].(float64); ok {
						setCount = int(value)
						return setCount, nil
					}
				}
				return nil, errors.New("缺少 value")
			}
			return 42, nil
		})
	if err != nil {
		t.Fatalf("RegisterTool: %v", err)
	}
	if got := server.OperationCount(); got != 2 {
		t.Fatalf("OperationCount() = %d, want 2", got)
	}
	// Every method name counts as a binding of its own, so two methods plus two
	// path bindings make four.
	if got := server.BindingCount(); got != 4 {
		t.Fatalf("BindingCount() = %d, want 4", got)
	}

	// ---- the capability surface: what was registered is what the model says ----
	methods := server.MethodsJSON()
	if !strings.Contains(methods, `"/plc/getStatus"`) ||
		!strings.Contains(methods, `"path":"/MACHINE/STATUS"`) {
		t.Fatalf("MethodsJSON() = %s", methods)
	}
	if !strings.Contains(methods, `"required":["value"]`) {
		t.Fatalf("MethodsJSON() carries no params schema: %s", methods)
	}

	// ---- query hits the binding and carries the handler's value ----
	request := queryRequest(t, "q1", "/MACHINE/STATUS")
	defer request.Close()
	response, err := server.InvokeQuery(request)
	if err != nil {
		t.Fatalf("InvokeQuery: %v", err)
	}
	defer response.Close()
	text := response.String()
	if !strings.Contains(text, `"code":"OK"`) || !strings.Contains(text, "42") {
		t.Fatalf("query response = %s", text)
	}
	if mu.Lock(); len(seen) != 1 || seen[0] != "getStatus" {
		t.Fatalf("handler calls = %v", seen)
	}
	mu.Unlock()

	// ---- an unbound path answers NG, and never reaches the handler ----
	missing := queryRequest(t, "q2", "/NOPE")
	defer missing.Close()
	ng, err := server.InvokeQuery(missing)
	if err != nil {
		t.Fatalf("InvokeQuery(NOPE): %v", err)
	}
	defer ng.Close()
	if !strings.Contains(ng.String(), `"code":"NG"`) {
		t.Fatalf("unbound path response = %s", ng.String())
	}

	// ---- set through the same tool ----
	params := `{"operation":"set_value","value":7}`
	// A Set request carries its items under "values" (a Query request uses
	// "ids"); the item payload is the same shape.
	writePayload, err := ParseMessage("Set/Request/"+serverSN,
		[]byte(fmt.Sprintf(`{"@id":"s1","values":[{"id":"/MACHINE/PART_COUNT","params":%s}]}`, params)))
	if err != nil {
		t.Fatalf("ParseMessage(set): %v", err)
	}
	defer writePayload.Close()
	setResponse, err := server.InvokeSet(writePayload)
	if err != nil {
		t.Fatalf("InvokeSet: %v", err)
	}
	defer setResponse.Close()
	if text := setResponse.String(); !strings.Contains(text, `"code":"OK"`) {
		t.Fatalf("set response = %s", text)
	}
	if setCount != 7 {
		t.Fatalf("setCount = %d, want 7", setCount)
	}
}

func TestServerMethodCall(t *testing.T) {
	server := newTestServer(t, nil)
	err := server.RegisterTool("plc",
		[]ToolMethod{
			{Name: "getStatus"},
			{Name: "boom"},
			{Name: "needValue", Schema: `{"type":"object","properties":{"value":` +
				`{"type":"number"}},"required":["value"]}`},
		},
		nil,
		func(method string, params any) (any, error) {
			switch method {
			case "boom":
				return nil, errors.New("工具内部出错")
			case "needValue":
				return 1, nil
			}
			return 42, nil
		})
	if err != nil {
		t.Fatalf("RegisterTool: %v", err)
	}

	call, err := server.InvokeMethodCall(methodRequest(t, "m1", "/plc/getStatus", "null"))
	if err != nil {
		t.Fatalf("InvokeMethodCall: %v", err)
	}
	defer call.Close()
	if text := call.String(); !strings.Contains(text, `"code":"OK"`) ||
		!strings.Contains(text, "42") {
		t.Fatalf("method call response = %s", text)
	}

	// ---- a handler error becomes code NG with the message ----
	failure, err := server.InvokeMethodCall(methodRequest(t, "m2", "/plc/boom", "null"))
	if err != nil {
		t.Fatalf("InvokeMethodCall(boom): %v", err)
	}
	defer failure.Close()
	text := failure.String()
	if !strings.Contains(text, `"code":"NG"`) || !strings.Contains(text, "工具内部出错") {
		t.Fatalf("failed method call response = %s", text)
	}

	// ---- check mode validates against the schema without running the tool ----
	check, err := server.CheckMethodCall(methodRequest(t, "m3", "/plc/needValue", `{"nope":1}`))
	if err != nil {
		t.Fatalf("CheckMethodCall: %v", err)
	}
	defer check.Close()
	if !strings.Contains(check.String(), `"code":"NG"`) {
		t.Fatalf("check response = %s", check.String())
	}

	if unknown, err := server.InvokeMethodCall(
		methodRequest(t, "m4", "/plc/nope", "null")); err != nil {
		t.Fatalf("InvokeMethodCall(unknown): %v", err)
	} else {
		defer unknown.Close()
		if !strings.Contains(unknown.String(), `"code":"NG"`) {
			t.Fatalf("unknown method response = %s", unknown.String())
		}
	}
}

func TestServerModelAndOpenAPI(t *testing.T) {
	server := newTestServer(t, nil)
	if got := server.ModelJSON(); !strings.Contains(got, `"id":"030001"`) {
		t.Fatalf("ModelJSON() = %s", got)
	}
	if err := server.RegisterTool("plc", []ToolMethod{{Name: "getStatus"}}, nil,
		func(string, any) (any, error) { return 1, nil }); err != nil {
		t.Fatalf("RegisterTool: %v", err)
	}
	schema := server.OpenAPI("http://127.0.0.1:9008/api")
	if !strings.Contains(schema, `"openapi"`) ||
		!strings.Contains(schema, "/plc/getStatus") {
		t.Fatalf("OpenAPI() = %s", schema)
	}
}

func TestServerEventsAndSampling(t *testing.T) {
	var mu sync.Mutex
	topics := map[string]string{}
	sink := func(topic string, payload []byte) error {
		mu.Lock()
		defer mu.Unlock()
		topics[topic] = string(payload)
		return nil
	}
	server := newTestServer(t, sink)

	if err := server.PushEvent("030001", map[string]any{
		"key": "PART_COUNT", "value": 21}); err != nil {
		t.Fatalf("PushEvent: %v", err)
	}
	if got := server.EventCount(); got != 1 {
		t.Fatalf("EventCount() = %d, want 1", got)
	}
	mu.Lock()
	event, ok := topics["Event/"+serverSN]
	mu.Unlock()
	if !ok || !strings.Contains(event, "PART_COUNT") {
		t.Fatalf("event payload = %q (topics %v)", event, topics)
	}

	// ---- the model declares one SAMPLE_CHANNEL; it uploads through the sink ----
	if err := server.InitSamples(); err != nil {
		t.Fatalf("InitSamples: %v", err)
	}
	if got := server.SampleCount(); got != 1 {
		t.Fatalf("SampleCount() = %d, want 1", got)
	}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) && server.SampleUploadCount() == 0 {
		time.Sleep(50 * time.Millisecond)
	}
	if got := server.SampleUploadCount(); got == 0 {
		t.Fatal("no sample report was published")
	}
	mu.Lock()
	_, ok = topics["Sample/"+serverSN+"/ch1"]
	mu.Unlock()
	if !ok {
		t.Fatalf("sample topic missing from %v", topics)
	}

	// ---- addSample / removeSample at run time ----
	stop := "60000"
	config := fmt.Sprintf(`{"id":"ch2","type":"SAMPLE_CHANNEL","sampleInterval":1000,`+
		`"uploadInterval":%s,"ids":[{"id":"/MACHINE/STATUS"}]}`, stop)
	if err := server.AddSample(config); err != nil {
		t.Fatalf("AddSample: %v", err)
	}
	if got := server.SampleCount(); got != 2 {
		t.Fatalf("SampleCount() after AddSample = %d, want 2", got)
	}
	if err := server.RemoveSample("ch2"); err != nil {
		t.Fatalf("RemoveSample: %v", err)
	}
	if got := server.SampleCount(); got != 1 {
		t.Fatalf("SampleCount() after RemoveSample = %d, want 1", got)
	}
	server.StopAllSamples()
	if got := server.SampleCount(); got != 0 {
		t.Fatalf("SampleCount() after StopAllSamples = %d, want 0", got)
	}
}

func TestServerBuiltinAndFileTools(t *testing.T) {
	server := newTestServer(t, nil)
	// Experiment switch: with NCL_GO_TOOL_ON_SAMPLED_PATH=1 the tool binding sits
	// on the path the sample channel samples, which makes the sampler thread call
	// the Go callback.
	path := "/MACHINE/PART_COUNT"
	if os.Getenv("NCL_GO_TOOL_ON_SAMPLED_PATH") == "1" {
		path = "/MACHINE/STATUS"
	}
	if err := server.RegisterTool("plc", []ToolMethod{{Name: "getStatus"}},
		[]Binding{{Path: path, Operation: OpGetValue, Method: "getStatus"}},
		func(string, any) (any, error) { return 42, nil }); err != nil {
		t.Fatalf("RegisterTool: %v", err)
	}
	if err := server.RegisterBuiltinTool(); err != nil {
		t.Fatalf("RegisterBuiltinTool: %v", err)
	}
	if err := server.RegisterFileTool(); err != nil {
		t.Fatalf("RegisterFileTool: %v", err)
	}
	// addSample is reachable through the built in tool.
	call, err := server.InvokeMethodCall(methodRequest(t, "b1", "/nclinkServer/addSample",
		`{"request":{"id":"ch3","type":"SAMPLE_CHANNEL","sampleInterval":1000,`+
			`"uploadInterval":60000,"ids":[{"id":"/MACHINE/STATUS"}]}}`))
	if err != nil {
		t.Fatalf("InvokeMethodCall(addSample): %v", err)
	}
	defer call.Close()
	if text := call.String(); !strings.Contains(text, `"code":"OK"`) {
		t.Fatalf("addSample response = %s", text)
	}
	if got := server.SampleCount(); got != 1 {
		t.Fatalf("SampleCount() = %d, want 1", got)
	}
	// The file tool answers its own schema.
	fileCall, err := server.InvokeMethodCall(methodRequest(t, "f1",
		"/MACHINE/CONTROLLER/FILE/getAttribute", `{"name":"nope.txt"}`))
	if err != nil {
		t.Fatalf("InvokeMethodCall(file): %v", err)
	}
	defer fileCall.Close()
	if got := fileCall.String(); !strings.Contains(got, `"code"`) {
		t.Fatalf("file tool response = %s", got)
	}
}

func TestServerHTTP(t *testing.T) {
	server := newTestServer(t, nil)
	if err := server.RegisterTool("plc", []ToolMethod{{Name: "getStatus"}}, nil,
		func(string, any) (any, error) { return 42, nil }); err != nil {
		t.Fatalf("RegisterTool: %v", err)
	}
	endpoint, err := server.StartHTTP(0, true)
	if err != nil {
		t.Fatalf("StartHTTP: %v", err)
	}
	defer endpoint.Close()
	base := fmt.Sprintf("http://127.0.0.1:%d", endpoint.Port())
	if endpoint.Port() == 0 {
		t.Fatal("StartHTTP returned port 0 for an ephemeral bind")
	}

	get := func(path string) (int, string) {
		t.Helper()
		response, err := http.Get(base + path)
		if err != nil {
			t.Fatalf("GET %s: %v", path, err)
		}
		defer response.Body.Close()
		body, err := io.ReadAll(response.Body)
		if err != nil {
			t.Fatalf("read %s: %v", path, err)
		}
		return response.StatusCode, string(body)
	}

	if status, body := get("/api/schema"); status != 200 ||
		!strings.Contains(body, "openapi") {
		t.Fatalf("GET /api/schema = %d %s", status, body)
	}
	// The configuration API answers from the installed root (bin/sn.txt), which
	// is not necessarily the server's own serial number.
	if status, body := get("/api/cfg/getSn"); status != 200 ||
		!strings.Contains(body, `"status":true`) {
		t.Fatalf("GET /api/cfg/getSn = %d %s", status, body)
	}
	if status, body := get("/api/nope"); status != 404 {
		t.Fatalf("GET /api/nope = %d %s", status, body)
	}

	// POST /api/<tool>/<method> is the methodCall entry point.
	response, err := http.Post(base+"/api/plc/getStatus", "application/json",
		strings.NewReader(`{}`))
	if err != nil {
		t.Fatalf("POST /api/plc/getStatus: %v", err)
	}
	body, _ := io.ReadAll(response.Body)
	response.Body.Close()
	if response.StatusCode != 200 || !strings.Contains(string(body), `"status":true`) {
		t.Fatalf("POST /api/plc/getStatus = %d %s", response.StatusCode, body)
	}

	// ---- a route of our own ----
	if err := endpoint.Route("GET", "/api/hello", func(request *HTTPRequest) *HTTPReply {
		return &HTTPReply{Status: 200,
			Body: fmt.Sprintf(`{"method":%q,"path":%q,"query":%q}`,
				request.Method, request.Path, request.Query)}
	}); err != nil {
		t.Fatalf("Route: %v", err)
	}
	if status, body := get("/api/hello?x=1"); status != 200 ||
		!strings.Contains(body, `"query":"x=1"`) {
		t.Fatalf("GET /api/hello = %d %s", status, body)
	}
	if status, _ := get("/api/other"); status != 404 {
		t.Fatalf("unrouted GET /api/other = %d, want 404", status)
	}
	if endpoint.RequestCount() == 0 {
		t.Fatal("RequestCount() = 0 after several requests")
	}
	endpoint.SetCORS(false)
	endpoint.Close()
	endpoint.Close() // idempotent
}

func TestServerCloseIsIdempotent(t *testing.T) {
	server, err := NewServer(ServerOptions{SN: serverSN, Model: serverModel})
	if err != nil {
		t.Fatalf("NewServer: %v", err)
	}
	server.Close()
	server.Close()
	if got := server.SN(); got != "" {
		t.Fatalf("SN() after Close = %q, want empty", got)
	}
	if err := server.RegisterTool("plc", []ToolMethod{{Name: "getStatus"}}, nil,
		func(string, any) (any, error) { return 1, nil }); err == nil {
		t.Fatal("RegisterTool succeeded on a closed server")
	} else {
		var nerr *Error
		if !errors.As(err, &nerr) || nerr.Name != "ClosedException" {
			t.Fatalf("closed error = %v", err)
		}
	}
}

func TestNewServerRejectsEmptySN(t *testing.T) {
	if _, err := NewServer(ServerOptions{}); err == nil {
		t.Fatal("NewServer accepted an empty serial number")
	}
}

// TestServerFreeWithRunningSamples exercises repeated create/tool/sample/free
// cycles with a 1 ms channel: closing a device that still has a channel running
// has to stop (and join) the tasks before anything is released.
//
// It is a smoke test, not a reproduction: the teardown bug it guards against
// (bindings freed before the tasks were stopped) only crashes when the freed
// memory is reused inside a window of microseconds, so whether a round trips it
// is luck. It showed up here as an intermittent SIGSEGV in ncl_server_lookup on
// a sampler thread; see the changelog entry for the fix.
func TestServerFreeWithRunningSamples(t *testing.T) {
	const busyModel = `{"name":"busy","id":"01","type":"NC_LINK_ROOT",` +
		`"devices":[{"id":"02","type":"MACHINE","name":"模拟机床",` +
		`"configs":[{"name":"采样通道","id":"ch1","type":"SAMPLE_CHANNEL",` +
		`"sampleInterval":1,"uploadInterval":60000,` +
		`"ids":[{"id":"/MACHINE/STATUS"},{"id":"/MACHINE/STATUS"},{"id":"/MACHINE/STATUS"},` +
		`{"id":"/MACHINE/STATUS"},{"id":"/MACHINE/STATUS"},{"id":"/MACHINE/STATUS"},` +
		`{"id":"/MACHINE/STATUS"},{"id":"/MACHINE/STATUS"}]}],` +
		`"dataItems":[{"name":"状态","id":"030001","type":"STATUS"}]}]}`

	for round := 0; round < 12; round++ {
		server, err := NewServer(ServerOptions{SN: serverSN, Model: busyModel})
		if err != nil {
			t.Fatalf("round %d: NewServer: %v", round, err)
		}
		// A long binding list makes every lookup walk it: that is where the
		// task used to read freed memory.
		methods := make([]ToolMethod, 30)
		for i := range methods {
			methods[i] = ToolMethod{Name: fmt.Sprintf("m%d", i)}
		}
		err = server.RegisterTool("plc", methods,
			[]Binding{{Path: "/MACHINE/STATUS", Operation: OpGetValue, Method: "m0"}},
			func(string, any) (any, error) { return 1, nil })
		if err != nil {
			t.Fatalf("round %d: RegisterTool: %v", round, err)
		}
		if err := server.InitSamples(); err != nil {
			t.Fatalf("round %d: InitSamples: %v", round, err)
		}
		time.Sleep(5 * time.Millisecond) // let the 1 ms task get into a lookup
		server.Close()                   // must join the task before freeing
	}
}

func TestServerTooManyMethods(t *testing.T) {
	server := newTestServer(t, nil)
	methods := make([]ToolMethod, maxToolMethods+1)
	for i := range methods {
		methods[i] = ToolMethod{Name: fmt.Sprintf("m%d", i)}
	}
	err := server.RegisterTool("plc", methods, nil,
		func(string, any) (any, error) { return 1, nil })
	if err == nil {
		t.Fatalf("RegisterTool accepted %d methods (limit %d)", len(methods),
			maxToolMethods)
	}
}
