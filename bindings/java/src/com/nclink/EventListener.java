// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/** 事件回调（规则同 {@link SampleListener}）。 */
public interface EventListener {
    void onEvent(String topic, Event event);
}
