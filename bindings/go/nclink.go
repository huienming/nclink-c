// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// Package nclink provides Go bindings for the NC-Link C core, built on cgo.
//
// The C library stays the engine; this package adds Go ownership rules:
// every type has Close(), and a finalizer frees the C object if the caller
// forgets. Errors are reported as Go errors carrying the ncl_err code.
//
// Linking: by default the package links the prebuilt static library that ships
// with the release package (lib/<goos>-<goarch>). Build with -tags nclink_embed
// to compile the C sources directly instead (see bindings/go/README.md).

package nclink

/*
#cgo CFLAGS: -I${SRCDIR}/../../include

// -tags nclink_tls swaps in the TLS build of the core library (staged by
// tools/stage-go-libs.sh as libnclink_core_tls.a) and links OpenSSL with it.
#cgo !nclink_tls,linux LDFLAGS: -L${SRCDIR}/lib/linux-amd64 -lnclink_core -lpthread
#cgo nclink_tls,linux LDFLAGS: -L${SRCDIR}/lib/linux-amd64 -lnclink_core_tls -lssl -lcrypto -lpthread
#cgo !nclink_tls,windows LDFLAGS: -L${SRCDIR}/lib/windows-amd64 -lnclink_core -lws2_32 -liphlpapi -lwinmm
#cgo nclink_tls,windows LDFLAGS: -L${SRCDIR}/lib/windows-amd64 -lnclink_core_tls -lssl -lcrypto -lgdi32 -lcrypt32 -lws2_32 -liphlpapi -lwinmm

#include <stdlib.h>
#include <string.h>
#include "nclink/ncl_client.h"
#include "nclink/ncl_common.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_message.h"
#include "nclink/ncl_model.h"
#include "nclink/ncl_server.h"
#include "nclink/ncl_socket.h"

// Declared for cgo so its address can be handed to the C client. The types must
// match what cgo generates for the exported Go function (no const).
extern void nclinkGoSampleThunk(ncl_client *client, char *topic,
                                ncl_message *message, void *user);

// Tiny shim: a Go function value can only be converted to a C function pointer
// through a void* here, so the cast happens in C.
static void ncl_set_sample_handler_shim(ncl_client *client, void *fn, void *user) {
    ncl_client_set_sample_handler(client, (ncl_client_sample_fn)fn, user);
}
*/
import "C"

import (
	"errors"
	"runtime"
	"runtime/cgo"
	"unsafe"
)

// Error carries the ncl_err value of a failed call.
type Error struct {
	Code int
	Name string
	Op   string
	// Message is extra detail when the layer below reported one (the broker
	// error text, a rejected tool argument, ...). Empty otherwise.
	Message string
}

// handleBox isolates a cgo.Handle so that its address can be handed to C.
//
// cgo only allows passing a pointer to Go memory that holds no Go pointers, so
// the handle cannot live inside a struct that has slices or funcs (the Client
// and Server types do): it gets an allocation of its own, kept alive by the
// owner. C stores the address and reads the handle back in the callback.
type handleBox struct {
	handle cgo.Handle
}

func (e *Error) Error() string {
	if e.Message != "" {
		if e.Op == "" {
			return e.Message
		}
		return e.Op + ": " + e.Message
	}
	if e.Op == "" {
		return e.Name
	}
	return e.Op + ": " + e.Name
}

func check(rc C.ncl_err, op string) error {
	if rc == C.NCL_OK {
		return nil
	}
	return &Error{Code: int(rc), Name: C.GoString(C.ncl_err_name(rc)), Op: op}
}

// Version is the C library version string.
func Version() string { return C.NCL_VERSION } // a string literal macro

// TLSAvailable reports whether the linked library was built with TLS support
// (-tags nclink_tls plus the TLS build of the core library). When it is false,
// an "ssl://" URI fails with NOT_SUPPORTED instead of "connection failed".
func TLSAvailable() bool { return C.ncl_socket_tls_available() == C.bool(true) }

// ---------------------------------------------------------------- JSON ------

// Json owns an ncl_json value.
type Json struct{ j *C.ncl_json }

// ParseJSON parses a JSON document; it returns an error when the text is not
// valid JSON.
func ParseJSON(text string) (*Json, error) {
	cs := C.CString(text)
	defer C.free(unsafe.Pointer(cs))
	j := C.ncl_json_parse_cstr(cs, nil)
	if j == nil {
		return nil, &Error{Code: -3, Name: "ParseError", Op: "ParseJSON"}
	}
	out := &Json{j: j}
	runtime.SetFinalizer(out, (*Json).Close)
	return out, nil
}

// String returns the compact wire form.
func (j *Json) String() string {
	if j == nil || j.j == nil {
		return ""
	}
	text := C.ncl_json_write_string(j.j)
	if text == nil {
		return ""
	}
	defer C.free(unsafe.Pointer(text))
	return C.GoString(text)
}

// Close releases the value; it is safe to call twice.
func (j *Json) Close() {
	if j != nil && j.j != nil {
		C.ncl_json_free(j.j)
		j.j = nil
		runtime.SetFinalizer(j, nil)
	}
}

// ------------------------------------------------------------- message ------

// Message owns an ncl_message.
type Message struct{ m *C.ncl_message }

// ParseMessage parses a payload that arrived on topic.
func ParseMessage(topic string, payload []byte) (*Message, error) {
	ct := C.CString(topic)
	defer C.free(unsafe.Pointer(ct))
	var data unsafe.Pointer
	if len(payload) > 0 {
		data = C.malloc(C.size_t(len(payload)))
		defer C.free(data)
		C.memcpy(data, unsafe.Pointer(&payload[0]), C.size_t(len(payload)))
	}
	m := C.ncl_message_parse(ct, (*C.char)(data), C.size_t(len(payload)))
	if m == nil {
		return nil, &Error{Code: -3, Name: "ParseError", Op: "ParseMessage"}
	}
	out := &Message{m: m}
	runtime.SetFinalizer(out, (*Message).Close)
	return out, nil
}

// String returns the compact JSON form of the message.
func (m *Message) String() string {
	if m == nil || m.m == nil {
		return ""
	}
	text := C.ncl_message_write_string(m.m)
	if text == nil {
		return ""
	}
	defer C.free(unsafe.Pointer(text))
	return C.GoString(text)
}

// Close releases the message; it is safe to call twice.
func (m *Message) Close() {
	if m != nil && m.m != nil {
		C.ncl_message_free(m.m)
		m.m = nil
		runtime.SetFinalizer(m, nil)
	}
}

// --------------------------------------------------------------- model ------

// Model owns a device model tree.
type Model struct{ n *C.ncl_node }

// ParseModel parses a model document; an empty string yields the built in
// default model.
func ParseModel(text string) (*Model, error) {
	var cs *C.char
	if text != "" {
		cs = C.CString(text)
		defer C.free(unsafe.Pointer(cs))
	}
	root := C.ncl_root_node_parse(cs)
	if root == nil {
		return nil, &Error{Code: -111, Name: "InvalidModelException", Op: "ParseModel"}
	}
	out := &Model{n: root}
	runtime.SetFinalizer(out, (*Model).Close)
	return out, nil
}

// String serialises the tree.
func (m *Model) String() string {
	if m == nil || m.n == nil {
		return ""
	}
	text := C.ncl_node_write_string(m.n)
	if text == nil {
		return ""
	}
	defer C.free(unsafe.Pointer(text))
	return C.GoString(text)
}

// Close releases the tree; it is safe to call twice.
func (m *Model) Close() {
	if m != nil && m.n != nil {
		C.ncl_node_free(m.n)
		m.n = nil
		runtime.SetFinalizer(m, nil)
	}
}

// -------------------------------------------------------------- client ------

// Client is one device client, owned by the process wide holder.
type Client struct {
	c      *C.ncl_client
	box    *handleBox
	onData func(topic string, msg *Message)
}

// Open connects the process wide client; call it once per process.
func Open(uri, username, password string) error {
	cu, cp := C.CString(uri), C.CString(username)
	defer C.free(unsafe.Pointer(cu))
	defer C.free(unsafe.Pointer(cp))
	var cuser, cpass *C.char
	if username != "" {
		cuser = cu
	}
	if password != "" {
		cpw := C.CString(password)
		defer C.free(unsafe.Pointer(cpw))
		cpass = cpw
	}
	return check(C.ncl_client_holder_init(cu, cuser, cpass), "Open")
}

// Shutdown drops the connection and every client.
func Shutdown() { C.ncl_client_holder_shutdown() }

// Get returns the client of a device serial number, creating it on demand.
func Get(sn string) (*Client, error) {
	cs := C.CString(sn)
	defer C.free(unsafe.Pointer(cs))
	client := C.ncl_client_holder_get(cs)
	if client == nil {
		return nil, &Error{Code: -6, Name: "NotFoundException", Op: "Get"}
	}
	return &Client{c: client}, nil
}

// Probe fetches the device model.
func (c *Client) Probe(timeoutMS uint) (*Model, error) {
	var response *C.ncl_message
	if err := check(C.ncl_client_probe(c.c, C.uint(timeoutMS), &response), "Probe"); err != nil {
		return nil, err
	}
	msg := &Message{m: response}
	node := C.ncl_message_take_model(msg.m)
	msg.Close()
	if node == nil {
		return nil, errors.New("probe response carries no model")
	}
	out := &Model{n: node}
	runtime.SetFinalizer(out, (*Model).Close)
	return out, nil
}

// Value reads a single value; the caller closes the result.
func (c *Client) Value(path string, timeoutMS uint) (*Json, error) {
	cp := C.CString(path)
	defer C.free(unsafe.Pointer(cp))
	var out *C.ncl_json
	if err := check(C.ncl_client_get_value(c.c, cp, C.uint(timeoutMS), &out), "Value"); err != nil {
		return nil, err
	}
	j := &Json{j: out}
	runtime.SetFinalizer(j, (*Json).Close)
	return j, nil
}

// Length reads the collected length of a path.
func (c *Client) Length(path string, timeoutMS uint) (int64, error) {
	cp := C.CString(path)
	defer C.free(unsafe.Pointer(cp))
	var out C.longlong
	if err := check(C.ncl_client_get_length(c.c, cp, C.uint(timeoutMS), &out), "Length"); err != nil {
		return 0, err
	}
	return int64(out), nil
}

// Set writes a value; it fails when the device answers NG.
func (c *Client) Set(path string, value *Json, timeoutMS uint) error {
	cp := C.CString(path)
	defer C.free(unsafe.Pointer(cp))
	if value == nil || value.j == nil {
		return &Error{Code: -9, Name: "IllegalArgumentException", Op: "Set"}
	}
	return check(C.ncl_client_set_value(c.c, cp, C.ncl_json_clone(value.j),
		C.uint(timeoutMS)), "Set")
}

// SubscribeSamples delivers every Sample report to handler. The message it
// receives only lives for the duration of the call; copy what you keep.
func (c *Client) SubscribeSamples(qos int, handler func(topic string, msg *Message)) error {
	c.onData = handler
	c.box = &handleBox{handle: cgo.NewHandle(c)}
	if err := check(C.ncl_client_subscribe_samples(c.c, C.int(qos)), "SubscribeSamples"); err != nil {
		return err
	}
	C.ncl_set_sample_handler_shim(c.c,
		unsafe.Pointer((*[0]byte)(C.nclinkGoSampleThunk)),
		unsafe.Pointer(&c.box.handle))
	return nil
}

// SampleCount is the number of sample reports delivered so far.
func (c *Client) SampleCount() int { return int(C.ncl_client_sample_count(c.c)) }

//export nclinkGoSampleThunk
func nclinkGoSampleThunk(client *C.ncl_client, topic *C.char, message *C.ncl_message, user unsafe.Pointer) {
	_ = client
	pointer := (*cgo.Handle)(user)
	if pointer == nil {
		return
	}
	c, ok := pointer.Value().(*Client)
	if !ok || c.onData == nil {
		return
	}
	c.onData(C.GoString(topic), &Message{m: message})
}
