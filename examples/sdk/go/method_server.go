// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package nclink

/*
#include <stdlib.h>
#include "nclink/ncl_message.h"
#include "nclink/ncl_server.h"
#include "nclink/ncl_topic.h"
*/
import "C"

import "unsafe"

/*
设备端的异步方法调用：离线驱动一次 async 调用，以及按句柄查状态/结果。
真机上这三步分别由 Method/Call|Status|Result 请求触发，工具方法仍是普通函数。
*/

// InvokeMethodCallAsync starts a method on the library's thread pool and
// returns the immediate answer (code=OK plus a handler).
func (s *Server) InvokeMethodCallAsync(method, paramsJSON string) (*Json, error) {
	if err := s.requireOpen("InvokeMethodCallAsync"); err != nil {
		return nil, err
	}
	cmethod := C.CString(method)
	defer C.free(unsafe.Pointer(cmethod))

	request := C.ncl_message_new(C.NCL_MSG_METHOD_CALL_REQUEST)
	if request == nil {
		return nil, &Error{Code: -2, Name: "OutOfMemoryException", Op: "InvokeMethodCallAsync"}
	}
	C.ncl_message_set_method(request, cmethod)
	C.ncl_message_set_async(request, C.bool(true))
	if paramsJSON != "" {
		cparams := C.CString(paramsJSON)
		document := C.ncl_json_parse_cstr(cparams, nil)
		C.free(unsafe.Pointer(cparams))
		if document == nil {
			C.ncl_message_free(request)
			return nil, &Error{Code: -3, Name: "JsonParseException", Op: "InvokeMethodCallAsync"}
		}
		C.ncl_message_set_params(request, document)
	}
	response := C.ncl_server_invoke_method_call(s.server, request)
	C.ncl_message_free(request)
	return jsonOfMessage(response, "InvokeMethodCallAsync")
}

// serverMethodQuery issues one Method/Status or Method/Result request offline.
func (s *Server) serverMethodQuery(objectID, handler string, wantResult bool) (*Json, error) {
	op := "InvokeMethodStatus"
	if wantResult {
		op = "InvokeMethodResult"
	}
	if err := s.requireOpen(op); err != nil {
		return nil, err
	}
	cid := C.CString(objectID)
	chandler := C.CString(handler)
	defer C.free(unsafe.Pointer(cid))
	defer C.free(unsafe.Pointer(chandler))

	var request *C.ncl_message
	var topic *C.char
	if wantResult {
		request = C.ncl_message_new(C.NCL_MSG_METHOD_RESULT_REQUEST)
		topic = C.ncl_topic_method_result_request(C.ncl_server_sn(s.server), nil)
	} else {
		request = C.ncl_message_new(C.NCL_MSG_METHOD_STATUS_REQUEST)
		topic = C.ncl_topic_method_status_request(C.ncl_server_sn(s.server), nil)
	}
	if request == nil || topic == nil {
		C.ncl_message_free(request)
		C.free(unsafe.Pointer(topic))
		return nil, &Error{Code: -2, Name: "OutOfMemoryException", Op: op}
	}
	C.ncl_message_set_request_id(request, cid)
	C.ncl_message_set_handler(request, chandler)
	response := C.ncl_server_dispatch(s.server, topic, request)
	C.free(unsafe.Pointer(topic))
	C.ncl_message_free(request)
	return jsonOfMessage(response, op)
}

// InvokeMethodStatus asks a handle for its status (process / status / code).
func (s *Server) InvokeMethodStatus(objectID, handler string) (*Json, error) {
	return s.serverMethodQuery(objectID, handler, false)
}

// InvokeMethodResult asks a handle for its outcome: PENDING while it still
// runs, then code + return + result (finished / error) and the handle is gone.
func (s *Server) InvokeMethodResult(objectID, handler string) (*Json, error) {
	return s.serverMethodQuery(objectID, handler, true)
}

// ReportMethodProgress publishes progress for a running asynchronous call.
func (s *Server) ReportMethodProgress(handler string, process int64, status string) error {
	if err := s.requireOpen("ReportMethodProgress"); err != nil {
		return err
	}
	chandler := C.CString(handler)
	var cstatus *C.char
	if status != "" {
		cstatus = C.CString(status)
		defer C.free(unsafe.Pointer(cstatus))
	}
	defer C.free(unsafe.Pointer(chandler))
	return check(C.ncl_server_report_method_progress(s.server, chandler,
		C.longlong(process), cstatus), "ReportMethodProgress")
}
