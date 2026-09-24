// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// Go client example: connect, probe the model, read a value and print the
// device's sample reports.
//
//	cd examples/client/go && go run . tcp://127.0.0.1:1883 V2023A7B762
//
// The SDK lives next door (examples/sdk/go); this directory is its own small
// module that points at it with a replace directive, so the two sides stay
// separate without breaking `go run`.
package main

import (
	"fmt"
	"os"
	"time"

	nclink "github.com/huienming/nclink-c/examples/sdk/go"
)

func main() {
	uri := "tcp://127.0.0.1:1883"
	sn := "V203243111F"
	if len(os.Args) > 1 {
		uri = os.Args[1]
	}
	if len(os.Args) > 2 {
		sn = os.Args[2]
	}

	if err := nclink.Open(uri, "", ""); err != nil {
		fmt.Println("open:", err)
		os.Exit(1)
	}
	defer nclink.Shutdown()
	fmt.Printf("connected: %s (nclink %s)\n", uri, nclink.Version())

	client, err := nclink.Get(sn)
	if err != nil {
		fmt.Println("get:", err)
		os.Exit(1)
	}

	model, err := client.Probe(5000)
	if err != nil {
		fmt.Println("probe:", err)
		os.Exit(1)
	}
	fmt.Printf("probe: model is %d bytes\n", len(model.String()))
	model.Close()

	value, err := client.Value("/MACHINE/STATUS", 5000)
	if err != nil {
		fmt.Println("value:", err)
		os.Exit(1)
	}
	fmt.Println("GET /STATUS =", value.String())
	value.Close()

	// Sample reports: the callback runs on the C reader thread, and msg only
	// lives for the duration of the call.
	err = client.SubscribeSamples(2, func(topic string, msg *nclink.Message) {
		fmt.Printf("sample %s\n", topic)
	})
	if err != nil {
		fmt.Println("subscribe:", err)
		os.Exit(1)
	}

	time.Sleep(6 * time.Second)
	fmt.Printf("received %d sample reports\n", client.SampleCount())
}
