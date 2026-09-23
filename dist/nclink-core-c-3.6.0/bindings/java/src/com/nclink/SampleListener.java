// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/**
 * 采样回调。
 *
 * <p>在客户端自己的**读取线程**上调用，收到的 {@link Sample} 已经是快照；
 * 回调里别做耗时操作，抛出的异常会被记在
 * {@link DeviceClient#lastCallbackError()} 上，不会穿回原生层。
 */
public interface SampleListener {
    void onSample(String topic, Sample sample);
}
