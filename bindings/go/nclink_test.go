// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package nclink

import (
	"errors"
	"strings"
	"testing"
)

func TestVersion(t *testing.T) {
	if got := Version(); !strings.HasPrefix(got, "3.") {
		t.Fatalf("Version() = %q, want a 3.x version", got)
	}
}

func TestJSONRoundTrip(t *testing.T) {
	j, err := ParseJSON(`{"a":1}`)
	if err != nil {
		t.Fatalf("ParseJSON: %v", err)
	}
	defer j.Close()
	if got := j.String(); got != `{"a":1}` {
		t.Fatalf("String() = %q", got)
	}
	j.Close() // Close is idempotent
	if got := j.String(); got != "" {
		t.Fatalf("String() after Close = %q, want empty", got)
	}
}

func TestJSONParseError(t *testing.T) {
	_, err := ParseJSON("{oops")
	if err == nil {
		t.Fatal("ParseJSON accepted invalid JSON")
	}
	var nerr *Error
	if !errors.As(err, &nerr) {
		t.Fatalf("error type = %T, want *Error", err)
	}
	if nerr.Name == "" {
		t.Fatal("Error.Name is empty")
	}
}

func TestMessageAndModel(t *testing.T) {
	if _, err := ParseMessage("Query/Request/V1", []byte("not a message")); err == nil {
		t.Fatal("ParseMessage accepted a broken payload")
	}

	m, err := ParseModel("")
	if err != nil {
		t.Fatalf("ParseModel: %v", err)
	}
	defer m.Close()
	if got := m.String(); !strings.Contains(got, "nclink") {
		t.Fatalf("model dump = %q, want the built in default model", got)
	}
}

func TestNilReceiverSafe(t *testing.T) {
	var j *Json
	if got := j.String(); got != "" {
		t.Fatalf("nil Json.String() = %q", got)
	}
	j.Close() // must not panic
}

func TestOpenFailsOnUnreachableBroker(t *testing.T) {
	err := Open("tcp://127.0.0.1:1", "", "") // port 1 is reserved
	if err == nil {
		t.Fatal("Open accepted an unreachable broker")
	}
	var nerr *Error
	if !errors.As(err, &nerr) {
		t.Fatalf("error type = %T, want *Error", err)
	}
	t.Logf("Open returned %v (code %d)", nerr, nerr.Code)
}

func TestGetBeforeOpenFails(t *testing.T) {
	if _, err := Get("V000000000"); err == nil {
		t.Fatal("Get succeeded without Open")
	}
}

// The two builds report themselves honestly: without TLS support an "ssl://"
// URI must come back as NOT_SUPPORTED rather than a vague connect failure.
func TestTLSReporting(t *testing.T) {
	t.Logf("TLSAvailable() = %v", TLSAvailable())
	if TLSAvailable() {
		t.Skip("this build has TLS: the NOT_SUPPORTED assertion only holds without it")
	}
	err := Open("ssl://127.0.0.1:1", "", "")
	if err == nil {
		t.Fatal("Open accepted an ssl:// URI in a build without TLS")
	}
	var nerr *Error
	if !errors.As(err, &nerr) {
		t.Fatalf("error type = %T, want *Error", err)
	}
	if nerr.Code != -8 { // NCL_ERR_NOT_SUPPORTED
		t.Fatalf("error = %v (code %d), want code -8", nerr, nerr.Code)
	}
}
