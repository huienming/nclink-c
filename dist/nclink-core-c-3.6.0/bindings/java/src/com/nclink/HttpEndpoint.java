// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

package com.nclink;

import java.util.ArrayList;
import java.util.List;

/**
 * 设备端的 HTTP / REST 端点（OpenAPI 文档 + Swagger UI + 工具端点 [+ 配置端点]）。
 *
 * <p>由 {@link Server#startHttp} 创建，**关的时候要在 {@code server.close()} 之前**
 * （路由回调还挂在服务器上）。想自己加 REST 端点就 {@link #route}。
 */
public final class HttpEndpoint implements AutoCloseable {
    /** 一条自定义路由的处理函数。 */
    public interface RouteHandler {
        Reply handle(String method, String path, String query, String body);
    }

    /** 处理函数的返回值：状态码 + 内容类型 + 报文（后两个可以只有类型没报文）。 */
    public static final class Reply {
        final int status;
        final String contentType;
        final String body;

        private Reply(int status, String contentType, String body) {
            this.status = status;
            this.contentType = contentType;
            this.body = body;
        }

        public static Reply text(String body) {
            return new Reply(200, "text/plain; charset=utf-8", body);
        }

        public static Reply json(String body) {
            return new Reply(200, "application/json; charset=utf-8", body);
        }

        public static Reply of(int status, String contentType, String body) {
            return new Reply(status, contentType, body);
        }

        public static Reply status(int status) {
            return new Reply(status, null, null);
        }
    }

    private final Server server;
    private long handle;
    private final int port;
    private final List<Long> routeHosts = new ArrayList<Long>();

    HttpEndpoint(Server server, long handle, int port) {
        this.server = server;
        this.handle = handle;
        this.port = port;
    }

    /** 实际绑定的端口（{@code startHttp(0)} 时是系统给的随机端口）。 */
    public int port() {
        return port;
    }

    public String url() {
        return "http://localhost:" + port;
    }

    /** 已处理的请求数（诊断用）。 */
    public int requestCount() {
        return handle == 0 ? 0 : Native.httpRequestCount(handle);
    }

    /** Access-Control-Allow-Origin: *（默认开）。 */
    public void setCors(boolean enabled) {
        Native.httpSetCors(requireOpen(), enabled);
    }

    /**
     * 挂一条自己的路由。{@code method} 支持 {@code "*"}；{@code path} 以
     * {@code /api/} 开头时是前缀匹配。
     */
    public void route(String method, String path, RouteHandler handler) {
        if (handler == null) {
            throw new IllegalArgumentException("handler 不能为空");
        }
        /* 先登记处理函数（JNI 回调里按 "方法 路径" 查），再挂原生路由。 */
        server.routeHandlers().put(method + " " + path, handler);
        long[] host = new long[1];
        int rc = Native.httpRoute(requireOpen(), method, path, this, host);
        if (rc != 0) {
            server.routeHandlers().remove(method + " " + path);
            throw new NclinkException(rc, "route", path);
        }
        routeHosts.add(Long.valueOf(host[0]));
    }

    /** 停掉 HTTP 监听（可重复调用）。 */
    @Override
    public void close() {
        if (handle == 0) {
            return;
        }
        Native.httpFree(handle);
        handle = 0;
        for (Long host : routeHosts) {
            Native.hostFree(host.longValue());
        }
        routeHosts.clear();
    }

    private long requireOpen() {
        if (handle == 0) {
            throw new NclinkException(-13, "HttpEndpoint", "HTTP 端点已关闭");
        }
        return handle;
    }

    // ---------------------------------------------------------- 回调桥 -- //

    /* 由 JNI 从 HTTP 工作线程回调（JNI 负责把线程挂到 JVM 上）。 */
    String[] onRouteNative(String method, String path, String query, String body) {
        return server.dispatchRoute(method, path, query, body);
    }

    @Override
    public String toString() {
        return "HttpEndpoint(" + url() + (handle == 0 ? " closed" : "") + ")";
    }
}
