// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/**
 * 自研传输：设备端把每条出站报文（主题 + 已序列化的报文体）交给它，而不是走
 * MQTT。给了它以后 `broker` 仍然可以用来收请求（也可以完全不接 broker，用
 * {@link Server#dispatch} 离线驱动）。
 */
public interface PublishSink {
    void publish(String topic, byte[] payload);
}