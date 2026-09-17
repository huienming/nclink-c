// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Nclink
{
    /// <summary>一条自定义路由收到的请求（全是 UTF-8 文本）。</summary>
    public sealed class NclHttpRequest
    {
        internal NclHttpRequest(string method, string path, string query, string body)
        {
            Method = method;
            Path = path;
            Query = query;
            Body = body;
        }

        /// <summary>HTTP 方法（GET / POST / ...，也可能是 "*" 之外的任何方法）。</summary>
        public string Method { get; private set; }

        /// <summary>路径（不含查询串）。</summary>
        public string Path { get; private set; }

        /// <summary>查询串（不含 '?'；没有就是 null）。</summary>
        public string Query { get; private set; }

        /// <summary>报文体（没有就是 null）。</summary>
        public string Body { get; private set; }

        public override string ToString()
        {
            return Method + " " + Path
                   + (string.IsNullOrEmpty(Query) ? string.Empty : "?" + Query);
        }
    }

    /// <summary>路由处理函数的返回值：状态码 + 内容类型 + 报文。</summary>
    public sealed class NclHttpReply
    {
        private NclHttpReply(int status, string contentType, string body)
        {
            Status = status;
            ContentType = contentType;
            Body = body;
        }

        /// <summary>HTTP 状态码。</summary>
        public int Status { get; private set; }

        /// <summary>Content-Type；null = 交给库按默认处理。</summary>
        public string ContentType { get; private set; }

        /// <summary>报文体；null = 只回状态码。</summary>
        public string Body { get; private set; }

        /// <summary>200 + text/plain（UTF-8）。</summary>
        public static NclHttpReply Text(string body)
        {
            return new NclHttpReply(200, "text/plain; charset=utf-8", body);
        }

        /// <summary>200 + application/json（UTF-8）。</summary>
        public static NclHttpReply Json(string body)
        {
            return new NclHttpReply(200, "application/json; charset=utf-8", body);
        }

        /// <summary>只有状态码，没有报文。</summary>
        public static NclHttpReply WithStatus(int status)
        {
            return new NclHttpReply(status, null, null);
        }

        /// <summary>完全自己决定（<paramref name="body"/> 为 null 时只改状态码）。</summary>
        public static NclHttpReply Of(int status, string contentType, string body)
        {
            return new NclHttpReply(status, contentType, body);
        }
    }

    /// <summary>
    /// 自定义路由的处理函数。返回 null 表示 404；抛异常 → 500 且异常文本进报文，
    /// 异常本身记在 <see cref="NclServer.LastCallbackError"/> 上（不穿回原生层）。
    /// </summary>
    public delegate NclHttpReply NclHttpRouteHandler(NclHttpRequest request);

    /// <summary>
    /// 设备端的 HTTP / REST 端点（OpenAPI 文档 + Swagger UI + 工具端点 [+ 配置端点]）。
    ///
    /// 由 <see cref="NclServer.StartHttp()"/> 创建，**关的时候要在
    /// <c>server.Dispose()</c> 之前**（它的路由回调还挂在服务器上；服务器 Dispose 时
    /// 也会替你关）。想自己加 REST 端点就用 <see cref="Route"/>。
    /// </summary>
    public sealed class NclHttpEndpoint : IDisposable
    {
        /* 自定义路由回调：out_* 里回填的字符串必须用 nclshim_strdup（垫片释放）。 */
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int RouteCallback(IntPtr user, IntPtr method, IntPtr path,
                                          IntPtr query, IntPtr body, out int status,
                                          out IntPtr contentType, out IntPtr bodyText);

        private sealed class RouteEntry
        {
            internal NclHttpEndpoint Endpoint;
            internal string Method;
            internal string Path;
            internal NclHttpRouteHandler Handler;
            internal RouteCallback Callback;    // 宿主活着期间不能被 GC
            internal GCHandle Anchor;
            internal IntPtr Host;
        }

        private readonly NclServer _server;
        private readonly int _port;
        private readonly List<RouteEntry> _routes = new List<RouteEntry>();
        private IntPtr _handle;

        internal NclHttpEndpoint(NclServer server, IntPtr handle, int port)
        {
            _server = server;
            _handle = handle;
            _port = port;
        }

        /// <summary>
        /// 实际绑定的端口（<c>StartHttp(0)</c> 时是系统给的随机端口）。
        /// </summary>
        public int Port { get { return _port; } }

        /// <summary>端点的基地址，如 http://localhost:9008。</summary>
        public string Url { get { return "http://localhost:" + _port; } }

        /// <summary>已处理的请求数（诊断用）。</summary>
        public int RequestCount
        {
            get { return _handle == IntPtr.Zero ? 0 : Native.HttpRequestCount(_handle); }
        }

        /// <summary>已挂的自定义路由条数。</summary>
        public int RouteCount { get { return _routes.Count; } }

        /// <summary>Access-Control-Allow-Origin: *（默认开）。</summary>
        public void SetCors(bool enabled = true)
        {
            Native.HttpSetCors(RequireOpen(), enabled ? 1 : 0);
        }

        /// <summary>
        /// 挂一条自己的路由。
        ///
        /// <paramref name="method"/> 支持 <c>"*"</c>；<paramref name="path"/> 以
        /// <c>/api/</c> 开头时是**前缀匹配**（否则要求完全相等）——这两条都是库的
        /// 匹配规则，见 ncl_http_server_route。
        /// </summary>
        public void Route(string method, string path, NclHttpRouteHandler handler)
        {
            if (string.IsNullOrEmpty(method))
            {
                throw new ArgumentNullException("method");
            }
            if (string.IsNullOrEmpty(path))
            {
                throw new ArgumentNullException("path");
            }
            if (handler == null)
            {
                throw new ArgumentNullException("handler");
            }
            IntPtr handle = RequireOpen();
            for (int i = 0; i < _routes.Count; i++)
            {
                if (_routes[i].Method == method && _routes[i].Path == path)
                {
                    throw new ArgumentException("路由已经挂过了: " + method + " " + path);
                }
            }

            RouteEntry route = new RouteEntry();
            route.Endpoint = this;
            route.Method = method;
            route.Path = path;
            route.Handler = handler;
            route.Callback = new RouteCallback(OnRouteNative);
            route.Anchor = GCHandle.Alloc(route);
            route.Host = NclCallbackHost.Allocate(route.Callback, route.Anchor);

            int rc = Native.HttpRoute(handle, Native.Utf8Z(method), Native.Utf8Z(path),
                                      route.Host);
            if (rc != 0)
            {
                NclCallbackHost.Release(ref route.Host, ref route.Anchor);
                route.Callback = null;
                throw new NclinkException(rc, "Route", method + " " + path);
            }
            _routes.Add(route);
        }

        /// <summary>停掉 HTTP 监听（可重复调用）。</summary>
        public void Dispose()
        {
            if (_handle == IntPtr.Zero)
            {
                return;
            }
            Native.HttpFree(_handle);
            _handle = IntPtr.Zero;
            for (int i = 0; i < _routes.Count; i++)
            {
                RouteEntry route = _routes[i];
                NclCallbackHost.Release(ref route.Host, ref route.Anchor);
                route.Callback = null;
            }
            _routes.Clear();
            _server.ForgetEndpoint(this);
        }

        public override string ToString()
        {
            return "NclHttpEndpoint(" + Url
                   + (_handle == IntPtr.Zero ? " closed" : string.Empty) + ")";
        }

        /* ---------------------------------------------------------- 回调桥 -- */

        private int OnRouteNative(IntPtr user, IntPtr method, IntPtr path, IntPtr query,
                                  IntPtr body, out int status, out IntPtr contentType,
                                  out IntPtr bodyText)
        {
            status = 200;
            contentType = IntPtr.Zero;
            bodyText = IntPtr.Zero;
            RouteEntry route = null;
            try
            {
                route = Resolve(user);
                if (route == null)
                {
                    throw new NclinkException(NclServer.ErrInvalidArg, "Route",
                                              "路由已经被回收了");
                }
                NclHttpReply reply = route.Handler(new NclHttpRequest(
                    Native.Utf8(method), Native.Utf8(path), Native.Utf8(query),
                    Native.Utf8(body)));
                if (reply == null)
                {
                    status = 404;                       // 没有 handler 接这条路径
                    return 0;
                }
                status = reply.Status >= 100 && reply.Status <= 599 ? reply.Status : 500;
                if (reply.ContentType != null)
                {
                    contentType = Native.Strdup(Native.Utf8Z(reply.ContentType));
                }
                if (reply.Body != null)
                {
                    bodyText = Native.Strdup(Native.Utf8Z(reply.Body));
                }
                return 0;
            }
            catch (Exception error)
            {
                if (route != null)
                {
                    route.Endpoint._server.RecordCallbackError(error);
                }
                status = 500;
                contentType = Native.Strdup(Native.Utf8Z("text/plain; charset=utf-8"));
                bodyText = Native.Strdup(Native.Utf8Z("handler failed: " + error.Message));
                return -1;
            }
        }

        private static RouteEntry Resolve(IntPtr user)
        {
            if (user == IntPtr.Zero)
            {
                return null;
            }
            return GCHandle.FromIntPtr(user).Target as RouteEntry;
        }

        private IntPtr RequireOpen()
        {
            if (_handle == IntPtr.Zero)
            {
                throw new NclinkException(-13, "NclHttpEndpoint", "HTTP 端点已关闭");
            }
            return _handle;
        }
    }
}
