// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package nclink

/*
#include <stdlib.h>
#include "nclink/ncl_client.h"
#include "nclink/ncl_json.h"
#include "nclink/ncl_message.h"
*/
import "C"

import "unsafe"

/*
方法调用（Method/Call），以及异步方法调用的两对查询（Method/Status、Method/Result）。

异步：MethodCallAsync 立刻拿到应答（code=OK + handler），方法在设备端线程池里跑；
随后用 MethodStatus/MethodResult 按那个 handler 查进度与结果（没跑完是 PENDING，
结果取走后句柄释放）。设备端工具方法就是普通函数，异步调度由库负责。
*/

// methodCall issues one method call and returns the response message.
func (c *Client) methodCall(method, paramsJSON string, validate bool, async bool,
	timeoutMS uint) (*Json, error) {
	if c == nil || c.c == nil {
		return nil, &Error{Code: -13, Name: "ClosedException", Op: "MethodCall"}
	}
	cmethod := C.CString(method)
	defer C.free(unsafe.Pointer(cmethod))

	request := C.ncl_message_new(C.NCL_MSG_METHOD_CALL_REQUEST)
	if request == nil {
		return nil, &Error{Code: -2, Name: "OutOfMemoryException", Op: "MethodCall"}
	}
	if err := check(C.ncl_message_set_method(request, cmethod), "MethodCall"); err != nil {
		C.ncl_message_free(request)
		return nil, err
	}
	if paramsJSON != "" {
		cparams := C.CString(paramsJSON)
		document := C.ncl_json_parse_cstr(cparams, nil)
		C.free(unsafe.Pointer(cparams))
		if document == nil {
			C.ncl_message_free(request)
			return nil, &Error{Code: -3, Name: "JsonParseException", Op: "MethodCall"}
		}
		C.ncl_message_set_params(request, document) /* 转移所有权 */
	}
	if validate {
		C.ncl_message_set_check(request, C.bool(true))
	}
	var response *C.ncl_message
	var rc C.ncl_err
	if async {
		rc = C.ncl_client_method_call_async(c.c, request, C.uint(timeoutMS), &response)
	} else {
		rc = C.ncl_client_method_call(c.c, request, C.uint(timeoutMS), &response)
	}
	if err := check(rc, "MethodCall"); err != nil {
		C.ncl_message_free(response)
		return nil, err
	}
	return jsonOfMessage(response, "MethodCall")
}

// MethodCall runs a tool method; paramsJSON may be "" for no parameters.
func (c *Client) MethodCall(method, paramsJSON string, timeoutMS uint) (*Json, error) {
	return c.methodCall(method, paramsJSON, false, false, timeoutMS)
}

// MethodCallCheck validates the parameters without running the method.
func (c *Client) MethodCallCheck(method, paramsJSON string, timeoutMS uint) (*Json, error) {
	return c.methodCall(method, paramsJSON, true, false, timeoutMS)
}

// MethodCallAsync starts an asynchronous call: the answer carries code=OK and
// a handler (read it with ack.String("handler")); the method itself runs on the
// device. Ask MethodStatus/MethodResult with that handler for progress/result.
func (c *Client) MethodCallAsync(method, paramsJSON string, timeoutMS uint) (*Json, error) {
	return c.methodCall(method, paramsJSON, false, true, timeoutMS)
}

// methodQuery issues one Method/Status or Method/Result request.
func (c *Client) methodQuery(objectID, handler string, timeoutMS uint,
	wantResult bool) (*Json, error) {
	if c == nil || c.c == nil {
		return nil, &Error{Code: -13, Name: "ClosedException", Op: "MethodQuery"}
	}
	cid := C.CString(objectID)
	chandler := C.CString(handler)
	defer C.free(unsafe.Pointer(cid))
	defer C.free(unsafe.Pointer(chandler))

	var response *C.ncl_message
	var rc C.ncl_err
	if wantResult {
		rc = C.ncl_client_method_result(c.c, cid, chandler, C.uint(timeoutMS), &response)
	} else {
		rc = C.ncl_client_method_status(c.c, cid, chandler, C.uint(timeoutMS), &response)
	}
	if err := check(rc, "MethodQuery"); err != nil {
		C.ncl_message_free(response)
		return nil, err
	}
	return jsonOfMessage(response, "MethodQuery")
}

// MethodStatus asks a running asynchronous call for its status
// (process / status / code).
func (c *Client) MethodStatus(objectID, handler string, timeoutMS uint) (*Json, error) {
	return c.methodQuery(objectID, handler, timeoutMS, false)
}

// MethodResult asks for the outcome: code=PENDING while it still runs, then
// code + return + result (finished / error); the handle is released by the
// first successful read.
func (c *Client) MethodResult(objectID, handler string, timeoutMS uint) (*Json, error) {
	return c.methodQuery(objectID, handler, timeoutMS, true)
}

// jsonOfMessage serialises a response message and hands it back as a Json
// (the message itself is released here).
func jsonOfMessage(message *C.ncl_message, op string) (*Json, error) {
	if message == nil {
		return nil, &Error{Code: -1, Name: "IOException", Op: op}
	}
	text := C.ncl_message_write_string(message)
	C.ncl_message_free(message)
	if text == nil {
		return nil, &Error{Code: -2, Name: "OutOfMemoryException", Op: op}
	}
	document := C.ncl_json_parse_cstr(text, nil)
	C.free(unsafe.Pointer(text))
	if document == nil {
		return nil, &Error{Code: -3, Name: "JsonParseException", Op: op}
	}
	return &Json{j: document}, nil
}
