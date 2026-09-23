// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

/**
 * 设备端工具方法的处理函数。
 *
 * <p>`method` 是被调用的方法名，`params` 是请求参数（借用视图，只在本次调用期间
 * 有效）。返回要应答的值：
 *
 * <ul>
 *   <li>{@link Json}：原样作为结果；
 *   <li>{@code String}：按 **JSON 文本**处理（要回字符串就返回 {@code Map}/{@code List}
 *       或 {@code Json.parse("\"...\"")}）；
 *   <li>{@code Map}/{@code List}/{@code Number}/{@code Boolean}：序列化成 JSON；
 *   <li>{@code null}：成功但没有值（库按 NG 应答，和 C API 一致）。
 * </ul>
 *
 * <p>抛异常 → 该次调用按错误应答，异常文本进 reason；异常不会穿回原生层。
 */
public interface ToolHandler {
    Object handle(String method, Json params) throws Exception;
}