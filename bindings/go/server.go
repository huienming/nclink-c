// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package nclink

/*
#include <stdlib.h>
#include <string.h>

#include "nclink/ncl_common.h"
#include "nclink/ncl_file.h"
#include "nclink/ncl_http.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_model.h"
#include "nclink/ncl_mqtt.h"
#include "nclink/ncl_rest.h"
#include "nclink/ncl_server.h"

// Defined in nclink_thunks.c: cgo cannot hand a Go function pointer to C, and
// the C API identifies a tool method by its function pointer, so the thunks
// (and the callback casts) live on the C side.
extern ncl_err nclinkGoToolThunk(int index, void *instance, ncl_json *params,
                                 ncl_json **result, char **reason);
extern void nclinkGoRouteThunk(int index, ncl_http_request *request,
                               ncl_http_response *response, void *user);
extern ncl_err nclinkGoPublishThunk(void *user, char *topic, char *payload,
                                    size_t length);
extern void nclinkGoMessageThunk(void *user, ncl_mqtt_publish *publish);

extern ncl_tool_fn nclink_tool_thunk_at(int index);
extern ncl_http_handler nclink_route_thunk_at(int index);
extern void nclink_mqtt_options_set_on_message(ncl_mqtt_client_options *options,
                                               void *fn, void *user);
extern void nclink_server_options_set_publish(ncl_server_options *options,
                                              void *fn, void *user);
extern ncl_mqtt_client_options *nclink_mqtt_options_new(void);
extern ncl_server_options *nclink_server_options_new(void);
extern void nclink_options_free(void *options);
*/
import "C"

import (
	"encoding/json"
	"runtime"
	"runtime/cgo"
	"unsafe"
)

// -------------------------------------------------------------- server -----

// Operation is the operation part of a "<operation>#<path>" binding.
type Operation int

// Operations accepted by RegisterTool; the values match ncl_operation.
const (
	OpGetValue      Operation = 0
	OpGetLength     Operation = 1
	OpGetKeys       Operation = 2
	OpGetAttributes Operation = 3
	OpSetValue      Operation = 4
	OpAdd           Operation = 5
	OpDelete        Operation = 6
	OpFuncCall      Operation = 7
	OpFuncStatus    Operation = 8
	OpFuncResult    Operation = 9
	OpFuncCancel    Operation = 10
)

// TLSConfig carries the ssl:// / tls:// settings of a device connection. It
// only has an effect when the C library was built with TLS support.
type TLSConfig struct {
	CAFile     string // PEM bundle; "" = the platform trust store
	ClientCert string // optional PEM client certificate (mutual TLS)
	ClientKey  string // optional PEM key of ClientCert
	ServerName string // SNI / verified name; "" = the host from the URL
	// SkipVerify disables chain and host name verification. The zero value
	// verifies, which is what a device should do.
	SkipVerify bool
}

// ServerOptions describes the device this process acts as.
type ServerOptions struct {
	SN       string // serial number (required)
	Model    string // model document; "" = the library's built in model
	Broker   string // tcp://host:port or ssl://host:port; "" = offline
	Username string // optional broker credentials
	Password string
	TLS      *TLSConfig
	// Publish replaces MQTT as the outbound transport: every response and event
	// is handed to this callback instead of being published. Useful when the
	// host owns its own transport, and for tests without a broker.
	Publish func(topic string, payload []byte) error
}

// ToolMethod is one method of a tool.
type ToolMethod struct {
	Name   string
	Schema string // optional JSON Schema (draft-07 subset) of the parameters
}

// Binding binds one operation of one model path to a method of the tool.
type Binding struct {
	Path      string
	Operation Operation
	Method    string
	Tool      string // "" = the tool being registered
}

// ToolHandler computes the result of one method call. params is the decoded
// JSON value of the request parameters (nil when the request carried none);
// return any JSON-marshalable value, or nil for "no value". A non-nil error is
// reported to the caller as code NG with the error text as the message.
//
// The handler runs on the server's worker thread, never on the caller's.
type ToolHandler func(method string, params any) (any, error)

// maxToolMethods is how many methods one process may register: every method
// needs its own C trampoline (see nclink_thunks.c).
const maxToolMethods = 32

// maxHTTPRoutes is the same limit for HTTP routes.
const maxHTTPRoutes = 32

type toolMethod struct {
	name    string
	handler ToolHandler
}

// Server is the device side: this process is one NC-Link device.
type Server struct {
	server  *C.ncl_server
	mqtt    *C.ncl_mqtt_client
	box     *handleBox
	methods []toolMethod
	sink    func(topic string, payload []byte) error
	http    *HTTPEndpoint
}

// NewServer creates the device, loading the model and connecting the broker
// (unless Broker is empty, in which case the device is offline).
func NewServer(options ServerOptions) (*Server, error) {
	if options.SN == "" {
		return nil, &Error{Code: -9, Name: "IllegalArgumentException", Op: "NewServer"}
	}
	s := &Server{sink: options.Publish}
	s.box = &handleBox{handle: cgo.NewHandle(s)}

	var mqtt *C.ncl_mqtt_client
	if options.Broker != "" {
		// Allocated in C memory: it carries the callback user data (see
		// nclink_thunks.c).
		mqttOptions := C.nclink_mqtt_options_new()
		if mqttOptions == nil {
			s.release()
			return nil, &Error{Code: -2, Name: "NoMemoryException", Op: "NewServer"}
		}
		defer C.nclink_options_free(unsafe.Pointer(mqttOptions))

		broker := C.CString(options.Broker)
		clientID := C.CString(options.SN)
		defer C.free(unsafe.Pointer(broker))
		defer C.free(unsafe.Pointer(clientID))
		mqttOptions.url = broker
		mqttOptions.client_id = clientID
		mqttOptions.keep_alive_seconds = 60
		mqttOptions.automatic_reconnect = C.bool(true)

		// Empty credentials mean anonymous: passing NULL keeps them out of the
		// CONNECT packet.
		if options.Username != "" {
			user := C.CString(options.Username)
			defer C.free(unsafe.Pointer(user))
			mqttOptions.username = user
		}
		if options.Password != "" {
			pass := C.CString(options.Password)
			defer C.free(unsafe.Pointer(pass))
			mqttOptions.password = pass
		}
		if tls := options.TLS; tls != nil {
			if tls.CAFile != "" {
				value := C.CString(tls.CAFile)
				defer C.free(unsafe.Pointer(value))
				mqttOptions.tls_ca_file = value
			}
			if tls.ClientCert != "" {
				value := C.CString(tls.ClientCert)
				defer C.free(unsafe.Pointer(value))
				mqttOptions.tls_client_cert = value
			}
			if tls.ClientKey != "" {
				value := C.CString(tls.ClientKey)
				defer C.free(unsafe.Pointer(value))
				mqttOptions.tls_client_key = value
			}
			if tls.ServerName != "" {
				value := C.CString(tls.ServerName)
				defer C.free(unsafe.Pointer(value))
				mqttOptions.tls_server_name = value
			}
			mqttOptions.tls_verify_peer = C.bool(!tls.SkipVerify)
		}

		C.nclink_mqtt_options_set_on_message(mqttOptions,
			unsafe.Pointer((*[0]byte)(C.nclinkGoMessageThunk)),
			unsafe.Pointer(&s.box.handle))
		mqtt = C.ncl_mqtt_client_create(mqttOptions)
		if mqtt == nil {
			s.release()
			return nil, &Error{Code: -2, Name: "NoMemoryException", Op: "NewServer"}
		}
		if rc := C.ncl_mqtt_client_connect(mqtt); rc != C.NCL_OK {
			message := C.GoString(C.ncl_mqtt_client_last_error(mqtt))
			C.ncl_mqtt_client_destroy(mqtt)
			s.release()
			return nil, &Error{Code: int(rc), Name: "ConnectException",
				Op: "NewServer", Message: message}
		}
		s.mqtt = mqtt
	}

	serverOptions := C.nclink_server_options_new()
	if serverOptions == nil {
		if s.mqtt != nil {
			C.ncl_mqtt_client_disconnect(s.mqtt)
			C.ncl_mqtt_client_destroy(s.mqtt)
			s.mqtt = nil
		}
		s.release()
		return nil, &Error{Code: -2, Name: "NoMemoryException", Op: "NewServer"}
	}
	defer C.nclink_options_free(unsafe.Pointer(serverOptions))
	serverOptions.mqtt = mqtt
	sn := C.CString(options.SN)
	defer C.free(unsafe.Pointer(sn))
	serverOptions.sn = sn
	if options.Model != "" {
		model := C.CString(options.Model)
		defer C.free(unsafe.Pointer(model))
		serverOptions.model_json = model
	}
	if s.sink != nil {
		C.nclink_server_options_set_publish(serverOptions,
			unsafe.Pointer((*[0]byte)(C.nclinkGoPublishThunk)),
			unsafe.Pointer(&s.box.handle))
	}

	s.server = C.ncl_server_create(serverOptions)
	if s.server == nil {
		if s.mqtt != nil {
			C.ncl_mqtt_client_disconnect(s.mqtt)
			C.ncl_mqtt_client_destroy(s.mqtt)
			s.mqtt = nil
		}
		s.release()
		return nil, &Error{Code: -111, Name: "InvalidModelException", Op: "NewServer"}
	}
	runtime.SetFinalizer(s, (*Server).Close)
	return s, nil
}

// release drops the callback handle; the C objects must be gone by then.
func (s *Server) release() {
	if s.box != nil {
		s.box.handle.Delete()
		s.box = nil
	}
	runtime.SetFinalizer(s, nil)
}

// Close stops sampling, the FTP endpoint and the file tool, drops the broker
// connection and releases every callback. It is idempotent.
func (s *Server) Close() {
	if s == nil {
		return
	}
	if s.http != nil {
		s.http.Close() // the endpoint must go before the server it answers for
		s.http = nil
	}
	if s.server != nil {
		C.ncl_server_free(s.server)
		s.server = nil
	}
	if s.mqtt != nil {
		C.ncl_mqtt_client_disconnect(s.mqtt)
		C.ncl_mqtt_client_destroy(s.mqtt)
		s.mqtt = nil
	}
	s.methods = nil
	s.release()
}

func (s *Server) requireOpen(op string) error {
	if s == nil || s.server == nil {
		return &Error{Code: -13, Name: "ClosedException", Op: op}
	}
	return nil
}

// SN is the serial number this device answers for.
func (s *Server) SN() string {
	if s == nil || s.server == nil {
		return ""
	}
	return C.GoString(C.ncl_server_sn(s.server))
}

// ModelJSON is the device model document.
func (s *Server) ModelJSON() string {
	if s == nil || s.server == nil {
		return ""
	}
	node := C.ncl_server_model(s.server)
	if node == nil {
		return ""
	}
	text := C.ncl_node_write_string(node)
	if text == nil {
		return ""
	}
	defer C.free(unsafe.Pointer(text))
	return C.GoString(text)
}

// OpenAPI is the OpenAPI 3.0 document describing the registered operations.
func (s *Server) OpenAPI(baseURL string) string {
	if s == nil || s.server == nil {
		return ""
	}
	var base *C.char
	if baseURL != "" {
		base = C.CString(baseURL)
		defer C.free(unsafe.Pointer(base))
	}
	text := C.ncl_server_openapi_schema_json(s.server, base)
	if text == nil {
		return ""
	}
	defer C.free(unsafe.Pointer(text))
	return C.GoString(text)
}

// BindingCount is the number of "<operation>#<path>" bindings.
func (s *Server) BindingCount() int {
	if s == nil || s.server == nil {
		return 0
	}
	return int(C.ncl_server_binding_count(s.server))
}

// OperationCount is the number of callable (tool, method) pairs.
func (s *Server) OperationCount() int {
	if s == nil || s.server == nil {
		return 0
	}
	return int(C.ncl_server_operation_count(s.server))
}

// RegisterTool registers one tool, its methods and the path bindings that make
// them reachable. Every method of every tool gets its own C trampoline, so a
// process may register at most 32 methods in total.
func (s *Server) RegisterTool(tool string, methods []ToolMethod,
	bindings []Binding, handler ToolHandler) error {
	if err := s.requireOpen("RegisterTool"); err != nil {
		return err
	}
	if tool == "" || len(methods) == 0 || handler == nil {
		return &Error{Code: -9, Name: "IllegalArgumentException", Op: "RegisterTool"}
	}
	if len(s.methods)+len(methods) > maxToolMethods {
		return &Error{Code: -9, Name: "IllegalArgumentException", Op: "RegisterTool"}
	}
	first := len(s.methods)

	cTool := C.CString(tool)
	defer C.free(unsafe.Pointer(cTool))

	cMethods := make([]C.ncl_tool_method, len(methods))
	names := make([]*C.char, len(methods))
	schemas := make([]*C.char, len(methods))
	for i, method := range methods {
		names[i] = C.CString(method.Name)
		defer C.free(unsafe.Pointer(names[i]))
		cMethods[i].name = names[i]
		cMethods[i].fn = C.nclink_tool_thunk_at(C.int(first + i))
		if cMethods[i].fn == nil {
			return &Error{Code: -9, Name: "IllegalArgumentException", Op: "RegisterTool"}
		}
		if method.Schema != "" {
			schemas[i] = C.CString(method.Schema)
			defer C.free(unsafe.Pointer(schemas[i]))
			cMethods[i].params_schema = schemas[i]
		}
	}

	cBindings := make([]C.ncl_tool_binding, len(bindings))
	paths := make([]*C.char, len(bindings))
	methodNames := make([]*C.char, len(bindings))
	toolNames := make([]*C.char, len(bindings))
	for i, binding := range bindings {
		paths[i] = C.CString(binding.Path)
		defer C.free(unsafe.Pointer(paths[i]))
		methodNames[i] = C.CString(binding.Method)
		defer C.free(unsafe.Pointer(methodNames[i]))
		cBindings[i].path = paths[i]
		cBindings[i].operation = C.ncl_operation(binding.Operation)
		cBindings[i].method = methodNames[i]
		if binding.Tool != "" {
			toolNames[i] = C.CString(binding.Tool)
			defer C.free(unsafe.Pointer(toolNames[i]))
			cBindings[i].tool = toolNames[i]
		}
	}

	var cMethodsPtr *C.ncl_tool_method
	if len(cMethods) > 0 {
		cMethodsPtr = &cMethods[0]
	}
	var cBindingsPtr *C.ncl_tool_binding
	if len(cBindings) > 0 {
		cBindingsPtr = &cBindings[0]
	}
	rc := C.ncl_server_register_tool(s.server, cTool,
		unsafe.Pointer(&s.box.handle), cMethodsPtr,
		C.size_t(len(cMethods)), cBindingsPtr, C.size_t(len(cBindings)))
	if rc != C.NCL_OK {
		return check(rc, "RegisterTool")
	}
	for _, method := range methods {
		s.methods = append(s.methods, toolMethod{name: method.Name, handler: handler})
	}
	return nil
}

// RegisterBuiltinTool installs the "nclinkServer" tool (addSample /
// removeSample), which lets a client manage the sampling channels.
func (s *Server) RegisterBuiltinTool() error {
	if err := s.requireOpen("RegisterBuiltinTool"); err != nil {
		return err
	}
	return check(C.ncl_server_register_builtin_tool(s.server), "RegisterBuiltinTool")
}

// Subscribe listens on the request topics of this serial number.
func (s *Server) Subscribe() error {
	if err := s.requireOpen("Subscribe"); err != nil {
		return err
	}
	return check(C.ncl_server_subscribe(s.server), "Subscribe")
}

func (s *Server) methodAt(index int) (toolMethod, bool) {
	if index < 0 || index >= len(s.methods) {
		return toolMethod{}, false
	}
	return s.methods[index], true
}

/* ------------------------------------------------------------ dispatch -- */

// InvokeQuery answers a Query request without a broker.
func (s *Server) InvokeQuery(request *Message) (*Message, error) {
	return s.invoke("InvokeQuery", request, func(m *C.ncl_message) *C.ncl_message {
		return C.ncl_server_invoke_query(s.server, m)
	})
}

// InvokeSet answers a Set request without a broker.
func (s *Server) InvokeSet(request *Message) (*Message, error) {
	return s.invoke("InvokeSet", request, func(m *C.ncl_message) *C.ncl_message {
		return C.ncl_server_invoke_set(s.server, m)
	})
}

// InvokeMethodCall answers a Method/Call request without a broker.
func (s *Server) InvokeMethodCall(request *Message) (*Message, error) {
	return s.invoke("InvokeMethodCall", request, func(m *C.ncl_message) *C.ncl_message {
		return C.ncl_server_invoke_method_call(s.server, m)
	})
}

// CheckMethodCall validates the parameters of a method call without running it.
func (s *Server) CheckMethodCall(request *Message) (*Message, error) {
	return s.invoke("CheckMethodCall", request, func(m *C.ncl_message) *C.ncl_message {
		return C.ncl_server_check_method_call(s.server, m)
	})
}

// Dispatch routes a parsed request to the matching invoke function.
func (s *Server) Dispatch(topic string, request *Message) (*Message, error) {
	if err := s.requireOpen("Dispatch"); err != nil {
		return nil, err
	}
	if request == nil || request.m == nil {
		return nil, &Error{Code: -9, Name: "IllegalArgumentException", Op: "Dispatch"}
	}
	ct := C.CString(topic)
	defer C.free(unsafe.Pointer(ct))
	response := C.ncl_server_dispatch(s.server, ct, request.m)
	if response == nil {
		return nil, &Error{Code: -1, Name: "Exception", Op: "Dispatch"}
	}
	out := &Message{m: response}
	runtime.SetFinalizer(out, (*Message).Close)
	return out, nil
}

func (s *Server) invoke(op string, request *Message,
	call func(*C.ncl_message) *C.ncl_message) (*Message, error) {
	if err := s.requireOpen(op); err != nil {
		return nil, err
	}
	if request == nil || request.m == nil {
		return nil, &Error{Code: -9, Name: "IllegalArgumentException", Op: op}
	}
	response := call(request.m)
	if response == nil {
		return nil, &Error{Code: -1, Name: "Exception", Op: op}
	}
	out := &Message{m: response}
	runtime.SetFinalizer(out, (*Message).Close)
	return out, nil
}

/* ------------------------------------------------------------ sampling -- */

// InitSamples starts a task for every SAMPLE_CHANNEL of the model.
func (s *Server) InitSamples() error {
	if err := s.requireOpen("InitSamples"); err != nil {
		return err
	}
	return check(C.ncl_server_init_samples(s.server), "InitSamples")
}

// AddSample registers one sample channel from its JSON configuration.
func (s *Server) AddSample(configJSON string) error {
	if err := s.requireOpen("AddSample"); err != nil {
		return err
	}
	text := C.CString(configJSON)
	defer C.free(unsafe.Pointer(text))
	document := C.ncl_json_parse_cstr(text, nil)
	if document == nil {
		return &Error{Code: -3, Name: "ParseException", Op: "AddSample"}
	}
	defer C.ncl_json_free(document)
	config := C.ncl_node_from_json(document, C.NCL_NODE_CONFIG)
	if config == nil {
		return &Error{Code: -111, Name: "InvalidModelException", Op: "AddSample"}
	}
	defer C.ncl_node_free(config)
	return check(C.ncl_server_add_sample(s.server, config), "AddSample")
}

// RemoveSample stops and removes one sample channel.
func (s *Server) RemoveSample(id string) error {
	if err := s.requireOpen("RemoveSample"); err != nil {
		return err
	}
	cid := C.CString(id)
	defer C.free(unsafe.Pointer(cid))
	return check(C.ncl_server_remove_sample(s.server, cid), "RemoveSample")
}

// StopAllSamples stops every sample channel.
func (s *Server) StopAllSamples() {
	if s == nil || s.server == nil {
		return
	}
	C.ncl_server_stop_all_samples(s.server)
}

// SampleCount is the number of live sample channels.
func (s *Server) SampleCount() int {
	if s == nil || s.server == nil {
		return 0
	}
	return int(C.ncl_server_sample_count(s.server))
}

// SampleUploadCount is the number of sample reports published so far.
func (s *Server) SampleUploadCount() int {
	if s == nil || s.server == nil {
		return 0
	}
	return int(C.ncl_server_sample_upload_count(s.server))
}

/* -------------------------------------------------------------- events -- */

// PushEvent publishes an Event message on "Event/<sn>". event is any
// JSON-marshalable value, conventionally {"key": ..., "value": ...}.
func (s *Server) PushEvent(eventID string, event any) error {
	encoded, err := json.Marshal(event)
	if err != nil {
		return &Error{Code: -9, Name: "IllegalArgumentException", Op: "PushEvent",
			Message: err.Error()}
	}
	return s.PushEventJSON(eventID, string(encoded))
}

// PushEventJSON is PushEvent for callers that already hold JSON text.
func (s *Server) PushEventJSON(eventID, eventJSON string) error {
	if err := s.requireOpen("PushEvent"); err != nil {
		return err
	}
	cid := C.CString(eventID)
	defer C.free(unsafe.Pointer(cid))
	text := C.CString(eventJSON)
	defer C.free(unsafe.Pointer(text))
	event := C.ncl_json_parse_cstr(text, nil)
	if event == nil {
		return &Error{Code: -3, Name: "ParseException", Op: "PushEvent"}
	}
	defer C.ncl_json_free(event)
	return check(C.ncl_server_push_event(s.server, cid, event), "PushEvent")
}

// EventCount is the number of events published so far.
func (s *Server) EventCount() int {
	if s == nil || s.server == nil {
		return 0
	}
	return int(C.ncl_server_event_count(s.server))
}

/* --------------------------------------------------------- file channel -- */

// RegisterFileTool installs the "/CONTROLLER/FILE" tool: peers can push files
// to this device and pull files back from it.
func (s *Server) RegisterFileTool() error {
	if err := s.requireOpen("RegisterFileTool"); err != nil {
		return err
	}
	return check(C.ncl_server_register_file_tool(s.server), "RegisterFileTool")
}

// SetFilePeer pins the peer's FTP endpoint without the channel handshake: the
// peer must run its own FTP server and lay files out as "/<sn>/...". A peer
// that opens a channel (file/openFileChannel) replaces it. There is no
// implicit default any more (3.4.0 removed the conf/mqtt.cfg guess): pass the
// host, and an empty user or password keeps the endpoint's defaults.
func (s *Server) SetFilePeer(host string, port uint, username, password string) error {
	if err := s.requireOpen("SetFilePeer"); err != nil {
		return err
	}
	if host == "" {
		return &Error{Code: -9, Name: "IllegalArgumentException", Op: "SetFilePeer"}
	}
	chost := C.CString(host)
	defer C.free(unsafe.Pointer(chost))
	var cuser, cpass *C.char
	if username != "" {
		cuser = C.CString(username)
		defer C.free(unsafe.Pointer(cuser))
	}
	if password != "" {
		cpass = C.CString(password)
		defer C.free(unsafe.Pointer(cpass))
	}
	return check(C.ncl_server_set_file_peer(s.server, chost, C.uint(port), cuser, cpass),
		"SetFilePeer")
}

// StartFTP starts this device's own FTP endpoint (bin/ftp.txt holds the port
// and credentials). Peers that host their files elsewhere do not need it.
func (s *Server) StartFTP() error {
	if err := s.requireOpen("StartFTP"); err != nil {
		return err
	}
	return check(C.ncl_server_start_ftp(s.server), "StartFTP")
}

// StopFTP stops the device's FTP endpoint.
func (s *Server) StopFTP() {
	if s == nil || s.server == nil {
		return
	}
	C.ncl_server_stop_ftp(s.server)
}

/* ---------------------------------------------------------------- HTTP -- */

// HTTPRequest is the borrowed view of one inbound request. It is only valid
// for the duration of the handler call.
type HTTPRequest struct {
	Method string
	Path   string // without the query string
	Query  string // raw query string, without "?"
	Body   string
}

// HTTPReply is what a route handler returns. A nil *HTTPReply means 404.
type HTTPReply struct {
	Status      int
	ContentType string // "" = application/json; charset=utf-8
	Body        string
}

// HTTPHandler serves one route. It runs on the HTTP accept thread.
type HTTPHandler func(request *HTTPRequest) *HTTPReply

type httpRoute struct {
	handler HTTPHandler
}

// HTTPEndpoint is the REST layer of a device: the library's own endpoints
// (OpenAPI document, Swagger UI, the tool entry point, optionally the
// configuration API) plus the routes registered here.
type HTTPEndpoint struct {
	server *C.ncl_http_server
	owner  *Server
	handle cgo.Handle
	routes []httpRoute
	closed bool
}

// StartHTTP binds the REST endpoint to port (0 = ephemeral) and starts it.
// withConfig additionally exposes the configuration endpoints (SN, model,
// driver, mqtt.cfg, server list).
func (s *Server) StartHTTP(port uint, withConfig bool) (*HTTPEndpoint, error) {
	if err := s.requireOpen("StartHTTP"); err != nil {
		return nil, err
	}
	if s.http != nil {
		return s.http, nil // already running: idempotent
	}
	http := C.ncl_http_server_create(C.uint(port))
	if http == nil {
		return nil, &Error{Code: -2, Name: "NoMemoryException", Op: "StartHTTP"}
	}
	endpoint := &HTTPEndpoint{server: http, owner: s}
	endpoint.handle = cgo.NewHandle(endpoint)
	if rc := C.ncl_http_server_start(http); rc != C.NCL_OK {
		C.ncl_http_server_free(http)
		endpoint.handle.Delete()
		return nil, check(rc, "StartHTTP")
	}
	if rc := C.ncl_rest_attach(http, s.server); rc != C.NCL_OK {
		endpoint.Close()
		return nil, check(rc, "StartHTTP")
	}
	if withConfig {
		if rc := C.ncl_rest_attach_config(http); rc != C.NCL_OK {
			endpoint.Close()
			return nil, check(rc, "StartHTTP")
		}
	}
	s.http = endpoint
	return endpoint, nil
}

// Route registers a handler. path ending in "/*" registers a prefix route;
// method may be "" or "*" to accept any method.
func (e *HTTPEndpoint) Route(method, path string, handler HTTPHandler) error {
	if e == nil || e.server == nil || e.closed {
		return &Error{Code: -13, Name: "ClosedException", Op: "Route"}
	}
	if path == "" || handler == nil {
		return &Error{Code: -9, Name: "IllegalArgumentException", Op: "Route"}
	}
	if len(e.routes) >= maxHTTPRoutes {
		return &Error{Code: -9, Name: "IllegalArgumentException", Op: "Route"}
	}
	index := len(e.routes)
	thunk := C.nclink_route_thunk_at(C.int(index))
	if thunk == nil {
		return &Error{Code: -9, Name: "IllegalArgumentException", Op: "Route"}
	}
	var cmethod *C.char
	if method != "" {
		cmethod = C.CString(method)
		defer C.free(unsafe.Pointer(cmethod))
	}
	cpath := C.CString(path)
	defer C.free(unsafe.Pointer(cpath))
	rc := C.ncl_http_server_route(e.server, cmethod, cpath, thunk,
		unsafe.Pointer(&e.handle))
	if rc != C.NCL_OK {
		return check(rc, "Route")
	}
	e.routes = append(e.routes, httpRoute{handler: handler})
	return nil
}

// Port is the port the endpoint actually listens on.
func (e *HTTPEndpoint) Port() uint {
	if e == nil || e.server == nil {
		return 0
	}
	return uint(C.ncl_http_server_port(e.server))
}

// RequestCount is the number of requests handled so far.
func (e *HTTPEndpoint) RequestCount() int {
	if e == nil || e.server == nil {
		return 0
	}
	return int(C.ncl_http_server_request_count(e.server))
}

// SetCORS toggles the Access-Control-Allow-Origin header.
func (e *HTTPEndpoint) SetCORS(enabled bool) {
	if e == nil || e.server == nil {
		return
	}
	C.ncl_http_server_set_cors(e.server, C.bool(enabled))
}

// Close stops the listener. It is idempotent and also runs when the device is
// closed.
func (e *HTTPEndpoint) Close() {
	if e == nil || e.closed {
		return
	}
	e.closed = true
	if e.server != nil {
		C.ncl_http_server_stop(e.server)
		C.ncl_http_server_free(e.server)
		e.server = nil
	}
	if e.owner != nil && e.owner.http == e {
		e.owner.http = nil
	}
	if e.handle != 0 {
		e.handle.Delete()
		e.handle = 0
	}
}

func (e *HTTPEndpoint) routeAt(index int) HTTPHandler {
	if e == nil || index < 0 || index >= len(e.routes) {
		return nil
	}
	return e.routes[index].handler
}

/* ------------------------------------------------------- C -> Go bridges -- */

//export nclinkGoToolThunk
func nclinkGoToolThunk(index C.int, instance unsafe.Pointer, params *C.ncl_json,
	result **C.ncl_json, reason **C.char) C.ncl_err {
	if result != nil {
		*result = nil
	}
	if reason != nil {
		*reason = nil
	}
	if instance == nil {
		return C.NCL_ERR_INVALID_ARG
	}
	s, ok := (*cgo.Handle)(instance).Value().(*Server)
	if !ok || s == nil {
		return C.NCL_ERR_INVALID_ARG
	}
	method, ok := s.methodAt(int(index))
	if !ok {
		return C.NCL_ERR_INVALID_ARG
	}

	var args any
	if params != nil {
		text := C.ncl_json_write_string(params)
		if text != nil {
			value := C.GoString(text)
			C.free(unsafe.Pointer(text))
			if err := json.Unmarshal([]byte(value), &args); err != nil {
				return C.NCL_ERR_PARSE
			}
		}
	}

	value, err := method.handler(method.name, args)
	if err != nil {
		if reason != nil {
			*reason = C.CString(err.Error())
		}
		return C.NCL_ERR
	}
	if value == nil {
		return C.NCL_OK // no value: the protocol reports NG on its own
	}
	encoded, err := json.Marshal(value)
	if err != nil {
		if reason != nil {
			*reason = C.CString(err.Error())
		}
		return C.NCL_ERR
	}
	if len(encoded) == 0 {
		return C.NCL_OK
	}
	ctext := C.CString(string(encoded))
	defer C.free(unsafe.Pointer(ctext))
	document := C.ncl_json_parse_cstr(ctext, nil)
	if document == nil {
		return C.NCL_ERR_PARSE
	}
	if result != nil {
		*result = document // the library takes ownership
	} else {
		C.ncl_json_free(document)
	}
	return C.NCL_OK
}

//export nclinkGoPublishThunk
func nclinkGoPublishThunk(user unsafe.Pointer, topic *C.char, payload *C.char,
	length C.size_t) C.ncl_err {
	if user == nil {
		return C.NCL_ERR
	}
	s, ok := (*cgo.Handle)(user).Value().(*Server)
	if !ok || s == nil || s.sink == nil {
		return C.NCL_ERR
	}
	body := C.GoBytes(unsafe.Pointer(payload), C.int(length))
	if err := s.sink(C.GoString(topic), body); err != nil {
		return C.NCL_ERR
	}
	return C.NCL_OK
}

//export nclinkGoMessageThunk
func nclinkGoMessageThunk(user unsafe.Pointer, publish *C.ncl_mqtt_publish) {
	if user == nil || publish == nil || publish.topic == nil {
		return
	}
	s, ok := (*cgo.Handle)(user).Value().(*Server)
	if !ok || s == nil || s.server == nil {
		return
	}
	// ncl_server_on_message takes ownership of the parsed request and answers
	// asynchronously, so parsing here keeps the reader thread short.
	request := C.ncl_message_parse(publish.topic,
		(*C.char)(unsafe.Pointer(publish.payload)), publish.payload_len)
	if request == nil {
		return
	}
	C.ncl_server_on_message(s.server, publish.topic, request)
}

//export nclinkGoRouteThunk
func nclinkGoRouteThunk(index C.int, request *C.ncl_http_request,
	response *C.ncl_http_response, user unsafe.Pointer) {
	if user == nil || request == nil || response == nil {
		return
	}
	endpoint, ok := (*cgo.Handle)(user).Value().(*HTTPEndpoint)
	if !ok || endpoint == nil {
		return
	}
	if endpoint.routeAt(int(index)) == nil {
		return // the pre-set 200 with an empty body stays
	}
	reply := endpoint.routeAt(int(index))(&HTTPRequest{
		Method: C.GoString(C.ncl_http_method(request)),
		Path:   C.GoString(C.ncl_http_path(request)),
		Query:  C.GoString(C.ncl_http_query_string(request)),
		Body:   C.GoStringN(C.ncl_http_body(request), C.int(C.ncl_http_body_len(request))),
	})
	if reply == nil {
		text := C.CString("not found")
		defer C.free(unsafe.Pointer(text))
		C.ncl_http_reply_text(response, C.NCL_HTTP_NOT_FOUND, text)
		return
	}
	contentType := reply.ContentType
	if contentType == "" {
		contentType = "application/json; charset=utf-8"
	}
	cType := C.CString(contentType)
	defer C.free(unsafe.Pointer(cType))
	cBody := C.CString(reply.Body)
	defer C.free(unsafe.Pointer(cBody))
	C.ncl_http_reply(response, C.int(reply.Status), cType, cBody,
		C.size_t(len(reply.Body)))
}
