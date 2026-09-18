// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;

namespace Nclink
{
    /// <summary>ncl_operation：路径绑定里的操作（"&lt;operation&gt;#&lt;path&gt;"）。</summary>
    public enum NclOperation
    {
        GetValue = 0,
        GetLength = 1,
        GetKeys = 2,
        GetAttributes = 3,
        SetValue = 4,
        Add = 5,
        Delete = 6,
        FuncCall = 7,
        FuncStatus = 8,
        FuncResult = 9,
        FuncCancel = 10
    }

    /// <summary>一条路径绑定：把模型里的路径绑到一个工具方法上。</summary>
    public sealed class NclToolBinding
    {
        /// <summary>模型里的完整路径（如 "/STATUS"）。</summary>
        public string Path { get; private set; }

        /// <summary>对这个路径做什么操作（GET_VALUE / SET_VALUE / ...）。</summary>
        public NclOperation Operation { get; private set; }

        /// <summary>工具方法名。</summary>
        public string Method { get; private set; }

        public NclToolBinding(string path, NclOperation operation, string method)
        {
            if (string.IsNullOrEmpty(path))
            {
                throw new ArgumentNullException("path");
            }
            if (string.IsNullOrEmpty(method))
            {
                throw new ArgumentNullException("method");
            }
            Path = path;
            Operation = operation;
            Method = method;
        }
    }

    /// <summary>
    /// 设备端工具方法的处理函数。
    ///
    /// <paramref name="method"/> 是被调用的方法名，<paramref name="parameters"/> 是请求
    /// 参数（**借用**视图：只在本次调用期间有效，别存下来）。返回值按下面的规则翻成
    /// JSON：
    ///
    /// <list type="bullet">
    ///   <item><see cref="NclJson"/>：原样作为结果；</item>
    ///   <item><see cref="string"/>：按 **JSON 文本**处理（要回字符串就返回
    ///         <c>NclJson.Parse("\"...\"")</c>）；</item>
    ///   <item>数字 / <see cref="bool"/>：序列化成 JSON 字面量；</item>
    ///   <item><c>null</c>：成功但没有值（库按 NG 应答，与 C API 一致）。</item>
    /// </list>
    ///
    /// 抛异常 → 该次调用按错误应答、异常文本进 reason，异常本身记在
    /// <see cref="NclServer.LastCallbackError"/> 上，不会穿回原生层。
    /// </summary>
    public delegate object NclToolHandler(string method, NclJson parameters);

    /// <summary>
    /// 自研传输：把每条出站报文（主题 + 已序列化的报文体）交给这个回调；给了它设备端
    /// 就不再往 MQTT 发布。回调里抛出的异常记在
    /// <see cref="NclServer.LastCallbackError"/> 上。
    /// </summary>
    public delegate void NclPublishSink(string topic, byte[] payload);

    /// <summary>
    /// 设备端（ncl_server）：这个进程就是一台机床。
    ///
    /// <code>
    /// using (NclServer device = new NclServer("V2CS0000001", null, "tcp://127.0.0.1:1883"))
    /// {
    ///     device.RegisterTool("plc", new string[] { "getStatus", "getCount" },
    ///         new NclToolBinding[] {
    ///             new NclToolBinding("/STATUS", NclOperation.GetValue, "getStatus"),
    ///             new NclToolBinding("/PART_COUNT", NclOperation.GetValue, "getCount") },
    ///         (method, parameters) =&gt; method == "getStatus" ? 1 : 42);
    ///     device.RegisterBuiltinTool();     // addSample / removeSample
    ///     device.Subscribe();               // 订阅 6 个请求主题
    ///     device.InitSamples();             // 启动模型里声明的采样通道
    ///     device.PushEvent("010307", "{\"key\":\"PART_COUNT\",\"value\":7}");
    ///     NclHttpEndpoint http = device.StartHttp(0, true);   // REST + Swagger UI
    ///     Console.WriteLine(http.Url);
    /// }
    /// </code>
    ///
    /// 工具回调跑在库自己的线程池上（不是 MQTT 读取线程），回调里抛出的异常会被记成
    /// 应答的 reason，不穿回原生层。不接 broker 也能用（<c>broker</c> 传 null）：用
    /// <see cref="Dispatch(string, byte[])"/> / <see cref="InvokeMethodCall(string, string)"/>
    /// 离线驱动，或者给一个 <see cref="NclPublishSink"/> 自己当传输。
    /// </summary>
    public sealed class NclServer : IDisposable
    {
        /* 工具方法回调：返回 ncl_err；out_result_json 为 NULL 表示"没有值"。 */
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int ToolCallback(IntPtr user, IntPtr tool, IntPtr method,
                                          IntPtr parameters, out IntPtr resultJson,
                                          out IntPtr reason);

        /* 自研传输：出站报文（主题 + 报文体）。 */
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int PublishCallback(IntPtr user, IntPtr topic, IntPtr payload,
                                            int length);

        private const int Err = -1;
        private const int ErrNoMemory = -2;
        private const int ErrIo = -5;
        private const int ErrNotFound = -6;
        private const int ErrClosed = -13;
        internal const int ErrInvalidArg = -9;

        private readonly string _sn;
        private IntPtr _handle;
        private readonly Dictionary<string, NclToolHandler> _handlers =
            new Dictionary<string, NclToolHandler>();
        private readonly List<NclHttpEndpoint> _endpoints = new List<NclHttpEndpoint>();

        private ToolCallback _toolCallback;         // 回调与宿主必须活到 ServerFree 之后
        private GCHandle _toolAnchor;
        private IntPtr _toolHost;
        private PublishCallback _publishCallback;
        private GCHandle _publishAnchor;
        private IntPtr _publishHost;
        private NclPublishSink _publishSink;

        /// <summary>设备端：内置模型、不接 broker（离线用）。</summary>
        public NclServer(string sn)
            : this(sn, null, null, null, null, null)
        {
        }

        /// <summary>
        /// 设备端。
        /// </summary>
        /// <param name="sn">设备序列号（必填；MQTT clientId 与主题都靠它）。</param>
        /// <param name="modelJson">模型文档 JSON；null = 库内置的那份。</param>
        /// <param name="broker">MQTT 地址（如 tcp://127.0.0.1:1883）；null = 不接 MQTT。</param>
        public NclServer(string sn, string modelJson, string broker)
            : this(sn, modelJson, broker, null, null, null)
        {
        }

        /// <summary>
        /// 设备端。
        /// </summary>
        /// <param name="sn">设备序列号（必填）。</param>
        /// <param name="modelJson">模型文档 JSON；null = 库内置的那份。</param>
        /// <param name="broker">MQTT 地址；null = 不接 MQTT（离线）。</param>
        /// <param name="username">用户名；null / 空 = 匿名。</param>
        /// <param name="password">密码；null / 空 = 匿名。</param>
        /// <param name="publish">自研传输；给了它就不再走 MQTT 发布。</param>
        public NclServer(string sn, string modelJson, string broker, string username,
                         string password, NclPublishSink publish)
            : this(sn, modelJson, broker, username, password, publish, null)
        {
        }

        /// <summary>
        /// 全量构造：<paramref name="tls"/> 只有 <paramref name="broker"/> 是
        /// <c>ssl://</c> / <c>tls://</c> 时用得上（要带 TLS 编译的库与垫片）。
        /// </summary>
        public NclServer(string sn, string modelJson, string broker, string username,
                         string password, NclPublishSink publish, NclTlsOptions tls)
        {
            if (string.IsNullOrEmpty(sn))
            {
                throw new ArgumentNullException("sn");
            }
            if (publish != null)
            {
                _publishSink = publish;
                _publishCallback = new PublishCallback(OnPublishNative);
                _publishAnchor = GCHandle.Alloc(this);
                _publishHost = NclCallbackHost.Allocate(_publishCallback, _publishAnchor);
            }

            IntPtr handle;
            if (tls == null)
            {
                handle = Native.ServerCreate(Native.Utf8Z(sn), Native.Utf8Z(modelJson),
                                             Native.Utf8Z(broker),
                                             Native.Utf8Z(username),
                                             Native.Utf8Z(password), _publishHost);
            }
            else
            {
                handle = Native.ServerCreateEx(
                    Native.Utf8Z(sn), Native.Utf8Z(modelJson), Native.Utf8Z(broker),
                    Native.Utf8Z(username), Native.Utf8Z(password),
                    Native.Utf8Z(tls.CaFile), Native.Utf8Z(tls.ClientCertificate),
                    Native.Utf8Z(tls.ClientKey), Native.Utf8Z(tls.ServerName),
                    tls.VerifyPeer ? 1 : 0, _publishHost);
            }
            if (handle == IntPtr.Zero)
            {
                NclCallbackHost.Release(ref _publishHost, ref _publishAnchor);
                _publishCallback = null;
                _publishSink = null;
                throw new NclinkException(ErrNotFound, "NclServer",
                    "创建设备端失败（broker 连不上？）: " + sn);
            }
            _sn = sn;
            _handle = handle;
        }

        /* ------------------------------------------------------------ 基本 -- */

        /// <summary>设备序列号。</summary>
        public string Sn { get { return _sn; } }

        /// <summary>设备模型（**借用**视图：设备端活着它就有效，别 Dispose）。</summary>
        public NclModel Model
        {
            get
            {
                IntPtr node = Native.ServerModel(RequireOpen());
                return node == IntPtr.Zero ? null : NclModel.Borrowed(node, this);
            }
        }

        /// <summary>模型文档 JSON 文本。</summary>
        public string ModelJson()
        {
            return Native.TakeUtf8(Native.ServerModelJson(RequireOpen()));
        }

        /// <summary>OpenAPI 3.0 文档（每个 &lt;工具&gt;/&lt;方法&gt; 一个 POST 路径）。</summary>
        public string OpenapiJson(string baseUrl = "")
        {
            return Native.TakeUtf8(
                Native.ServerOpenapiJson(RequireOpen(), Native.Utf8Z(baseUrl ?? "")));
        }

        /// <summary>已注册的 "&lt;operation&gt;#&lt;path&gt;" 绑定数（每个方法名本身也算一条）。</summary>
        public int BindingCount
        {
            get { return Native.ServerBindingCount(RequireOpen()); }
        }

        /// <summary>可调用的 (工具, 方法) 对数。</summary>
        public int OperationCount
        {
            get { return Native.ServerOperationCount(RequireOpen()); }
        }

        /// <summary>已注册的采样通道数。</summary>
        public int SampleCount
        {
            get { return Native.ServerSampleCount(RequireOpen()); }
        }

        /// <summary>采样上报次数（诊断用）。</summary>
        public int SampleUploadCount
        {
            get { return Native.ServerSampleUploadCount(RequireOpen()); }
        }

        /// <summary>已发布事件数。</summary>
        public int EventCount
        {
            get { return Native.ServerEventCount(RequireOpen()); }
        }

        /// <summary>工具 / 发布回调里抛出的异常（没抛过就是 null）。</summary>
        public Exception LastCallbackError { get; private set; }

        /// <summary>回调桥记异常（HTTP 路由回调也要往这儿记）。</summary>
        internal void RecordCallbackError(Exception error)
        {
            LastCallbackError = error;
        }

        /* ------------------------------------------------------------ 工具 -- */

        /// <summary>
        /// <c>{工具名: 处理函数}</c>：注册时可以只给方法表，处理函数先放这里，或者干脆
        /// 只在这里放。
        /// </summary>
        public IDictionary<string, NclToolHandler> Handlers
        {
            get { return _handlers; }
        }

        /// <summary>注册工具（方法都没有 schema）。</summary>
        public void RegisterTool(string tool, string[] methods, NclToolBinding[] bindings,
                                 NclToolHandler handler)
        {
            if (methods == null)
            {
                throw new ArgumentNullException("methods");
            }
            Dictionary<string, string> map = new Dictionary<string, string>();
            foreach (string method in methods)
            {
                map[method] = null;
            }
            RegisterTool(tool, map, bindings, handler);
        }

        /// <summary>
        /// 注册工具。
        /// </summary>
        /// <param name="tool">工具名（形如 "plc"）。</param>
        /// <param name="methods">
        /// <c>{方法名: 参数 schema 的 JSON 文本}</c>；schema 为 null 表示没有 schema——
        /// 这种方法的 check 只接受空参数，是库的规则。
        /// </param>
        /// <param name="bindings">路径绑定；可以为 null。</param>
        /// <param name="handler">处理函数；传 null 时用 <see cref="Handlers"/> 里先放好的。</param>
        public void RegisterTool(string tool, IDictionary<string, string> methods,
                                 NclToolBinding[] bindings, NclToolHandler handler)
        {
            if (string.IsNullOrEmpty(tool) || methods == null || methods.Count == 0)
            {
                throw new ArgumentException("工具名与方法表都不能为空");
            }
            bool added = false;
            if (handler != null && !_handlers.ContainsKey(tool))
            {
                _handlers[tool] = handler;
                added = true;
            }
            NclToolHandler resolved;
            if (!_handlers.TryGetValue(tool, out resolved) || resolved == null)
            {
                throw new InvalidOperationException(
                    "工具 " + tool + " 没有处理函数（用 handler 参数或 Handlers）");
            }

            StringBuilder methodJson = new StringBuilder("[");
            bool first = true;
            foreach (KeyValuePair<string, string> entry in methods)
            {
                if (!first)
                {
                    methodJson.Append(',');
                }
                first = false;
                methodJson.Append("{\"name\":").Append(Quote(entry.Key));
                if (entry.Value != null)
                {
                    methodJson.Append(",\"schema\":").Append(entry.Value);
                }
                methodJson.Append('}');
            }
            methodJson.Append(']');

            StringBuilder bindingJson = new StringBuilder("[");
            if (bindings != null)
            {
                for (int i = 0; i < bindings.Length; i++)
                {
                    if (i > 0)
                    {
                        bindingJson.Append(',');
                    }
                    bindingJson.Append("{\"path\":").Append(Quote(bindings[i].Path))
                               .Append(",\"operation\":")
                               .Append(((int)bindings[i].Operation).ToString(
                                   CultureInfo.InvariantCulture))
                               .Append(",\"method\":").Append(Quote(bindings[i].Method))
                               .Append('}');
                }
            }
            bindingJson.Append(']');

            int rc = Native.ServerRegisterTool(RequireOpen(), Native.Utf8Z(tool),
                                               Native.Utf8Z(methodJson.ToString()),
                                               Native.Utf8Z(bindingJson.ToString()),
                                               EnsureToolHost());
            if (rc != 0)
            {
                if (added)
                {
                    _handlers.Remove(tool);
                }
                throw new NclinkException(rc, "RegisterTool", tool);
            }
        }

        /// <summary>注册内置的 "nclinkServer" 工具（addSample / removeSample）。</summary>
        public void RegisterBuiltinTool()
        {
            NclinkException.Check(Native.ServerRegisterBuiltinTool(RequireOpen()),
                                  "RegisterBuiltinTool");
        }

        /// <summary>注册文件工具（/CONTROLLER/FILE，配 <see cref="StartFtp"/> 用）。</summary>
        public void RegisterFileTool()
        {
            NclinkException.Check(Native.ServerRegisterFileTool(RequireOpen()),
                                  "RegisterFileTool");
        }

        /// <summary>
        /// 覆盖文件通道对端的 FTP 端点。
        ///
        /// 默认按 <c>conf/mqtt.cfg</c> 推：broker 的主机名 + 端口 2323 + admin/123456
        /// ——只有对端跑在 broker 那台机器上才成立。对端在别处（或者端口/账号不同）时
        /// 在**第一次传文件之前**调它。<paramref name="port"/> 传 0、
        /// <paramref name="username"/> / <paramref name="password"/> 留空就用默认。
        /// </summary>
        public void SetFilePeer(string host, int port = 2323, string username = null,
                                string password = null)
        {
            if (string.IsNullOrEmpty(host))
            {
                throw new ArgumentNullException("host");
            }
            NclinkException.Check(
                Native.ServerSetFilePeer(RequireOpen(), Native.Utf8Z(host),
                                         unchecked((uint)port),
                                         Native.Utf8Z(username),
                                         Native.Utf8Z(password)),
                "SetFilePeer");
        }

        /// <summary>启动 FTP 端点（端口与账号来自 bin/ftp.txt）。</summary>
        public void StartFtp()
        {
            NclinkException.Check(Native.ServerStartFtp(RequireOpen()), "StartFtp");
        }

        /// <summary>订阅本 SN 的 6 个请求主题（接 MQTT 时用）。</summary>
        public void Subscribe()
        {
            NclinkException.Check(Native.ServerSubscribe(RequireOpen()), "Subscribe");
        }

        /* ------------------------------------------------------------ 离线 -- */

        /// <summary>
        /// 离线驱动一条请求，返回应答报文（不经过 MQTT，也不发布应答）；调用方负责
        /// Dispose 返回的报文。
        /// </summary>
        /// <param name="topic">请求主题（如 Method/Call/Request/&lt;sn&gt;）。</param>
        /// <param name="payload">原始报文体（UTF-8 字节）。</param>
        public NclJson Dispatch(string topic, byte[] payload)
        {
            if (topic == null)
            {
                throw new ArgumentNullException("topic");
            }
            byte[] data = payload ?? new byte[0];
            byte[] buffer = data.Length == 0 ? new byte[1] : data;
            IntPtr response;
            NclinkException.Check(
                Native.ServerDispatch(RequireOpen(), Native.Utf8Z(topic), buffer,
                                      data.Length, out response),
                "Dispatch");
            return NclJson.TakeText(response);
        }

        /// <summary>离线驱动一条请求，报文体按 UTF-8 文本给。</summary>
        public NclJson Dispatch(string topic, string payload)
        {
            return Dispatch(topic,
                            System.Text.Encoding.UTF8.GetBytes(payload ?? string.Empty));
        }

        /// <summary>离线调用一个工具方法（不经过 MQTT），返回应答报文。</summary>
        public NclJson InvokeMethodCall(string method, string paramsJson = null)
        {
            return CallMethod(method, paramsJson, false);
        }

        /// <summary>离线调用一个工具方法，参数用 JSON 值给。</summary>
        public NclJson InvokeMethodCall(string method, NclJson parameters)
        {
            return CallMethod(method, parameters == null ? null : parameters.Encode(), false);
        }

        /// <summary>只按 schema 校验参数、不执行工具（不经过 MQTT），返回应答报文。</summary>
        public NclJson CheckMethodCall(string method, string paramsJson = null)
        {
            return CallMethod(method, paramsJson, true);
        }

        /// <summary>
        /// 离线发起异步方法调用：应答 code=OK + handler，方法在库的线程池里跑；
        /// 随后用 <see cref="InvokeMethodStatus"/> / <see cref="InvokeMethodResult"/> 按句柄查。
        /// </summary>
        public NclJson InvokeMethodCallAsync(string method, string paramsJson = null)
        {
            IntPtr response;
            NclinkException.Check(
                Native.ServerInvokeMethodCallAsync(RequireOpen(),
                                                   Native.Utf8Z(method),
                                                   paramsJson != null
                                                       ? Native.Utf8Z(paramsJson)
                                                       : null,
                                                   out response),
                "InvokeMethodCallAsync");
            return NclJson.TakeText(response);
        }

        /// <summary>按句柄查异步调用的状态（离线驱动；真机上由 Method/Status/Request 触发）。</summary>
        public NclJson InvokeMethodStatus(string objectId, string handler)
        {
            IntPtr response;
            NclinkException.Check(
                Native.ServerInvokeMethodStatus(RequireOpen(),
                                                Native.Utf8Z(objectId),
                                                Native.Utf8Z(handler),
                                                out response),
                "InvokeMethodStatus");
            return NclJson.TakeText(response);
        }

        /// <summary>按句柄查异步调用的结果（离线驱动；真机上由 Method/Result/Request 触发）。</summary>
        public NclJson InvokeMethodResult(string objectId, string handler)
        {
            IntPtr response;
            NclinkException.Check(
                Native.ServerInvokeMethodResult(RequireOpen(),
                                                Native.Utf8Z(objectId),
                                                Native.Utf8Z(handler),
                                                out response),
                "InvokeMethodResult");
            return NclJson.TakeText(response);
        }

        /// <summary>给正在跑的异步调用上报进度（可选）。</summary>
        public void ReportMethodProgress(string handler, long process,
                                         string status = null)
        {
            NclinkException.Check(
                Native.ServerReportMethodProgress(RequireOpen(),
                                                  Native.Utf8Z(handler), process,
                                                  Native.Utf8Z(status)),
                "ReportMethodProgress");
        }

        /// <summary>只按 schema 校验参数、不执行工具（不经过 MQTT），返回应答报文。</summary>
        public NclJson CheckMethodCall(string method, NclJson parameters)
        {
            return CallMethod(method, parameters == null ? null : parameters.Encode(), true);
        }

        private NclJson CallMethod(string method, string paramsJson, bool check)
        {
            if (string.IsNullOrEmpty(method))
            {
                throw new ArgumentNullException("method");
            }
            IntPtr response;
            int rc = check
                         ? Native.ServerCheckMethodCall(RequireOpen(),
                                                        Native.Utf8Z(method),
                                                        Native.Utf8Z(paramsJson),
                                                        out response)
                         : Native.ServerInvokeMethodCall(RequireOpen(),
                                                         Native.Utf8Z(method),
                                                         Native.Utf8Z(paramsJson),
                                                         out response);
            NclinkException.Check(rc, check ? "CheckMethodCall" : "InvokeMethodCall");
            return NclJson.TakeText(response);
        }


        /* ------------------------------------------------------------ 采样 -- */

        /// <summary>启动模型里声明的采样通道。</summary>
        public void InitSamples()
        {
            NclinkException.Check(Native.ServerInitSamples(RequireOpen()), "InitSamples");
        }

        /// <summary>运行时加一个采样通道（configJson 是一个 SAMPLE_CHANNEL 配置节点）。</summary>
        public void AddSample(string configJson)
        {
            if (configJson == null)
            {
                throw new ArgumentNullException("configJson");
            }
            NclinkException.Check(
                Native.ServerAddSample(RequireOpen(), Native.Utf8Z(configJson)),
                "AddSample");
        }

        /// <summary>运行时加一个采样通道，配置用 JSON 值给。</summary>
        public void AddSample(NclJson config)
        {
            if (config == null)
            {
                throw new ArgumentNullException("config");
            }
            AddSample(config.Encode());
        }

        /// <summary>运行时删掉一个采样通道。</summary>
        public void RemoveSample(string channelId)
        {
            NclinkException.Check(
                Native.ServerRemoveSample(RequireOpen(), Native.Utf8Z(channelId)),
                "RemoveSample");
        }

        /// <summary>停掉所有采样通道。</summary>
        public void StopAllSamples()
        {
            Native.ServerStopAllSamples(RequireOpen());
        }

        /* ------------------------------------------------------------ 事件 -- */

        /// <summary>推一条事件到 Event/&lt;sn&gt;；eventJson 形如 {"key":...,"value":...}。</summary>
        public void PushEvent(string eventId, string eventJson)
        {
            NclinkException.Check(
                Native.ServerPushEvent(RequireOpen(), Native.Utf8Z(eventId),
                                       Native.Utf8Z(eventJson)),
                "PushEvent");
        }

        /// <summary>推一条带时间和报文 id 的事件。</summary>
        public void PushEvent(string eventId, string eventJson, long timeMs,
                              string messageId)
        {
            NclinkException.Check(
                Native.ServerPushEventEx(RequireOpen(), Native.Utf8Z(eventId),
                                         Native.Utf8Z(eventJson), timeMs,
                                         Native.Utf8Z(messageId)),
                "PushEvent");
        }

        /* ------------------------------------------------------------ HTTP -- */

        /// <summary>
        /// 起一个 HTTP 端点（用完 Dispose；<see cref="Dispose"/> 也会收）。
        ///
        /// 端点内容全在库里：REST（<c>GET /api/schema</c> 是 OpenAPI 3.0 文档、
        /// <c>GET /swagger-ui</c>、<c>POST /api/&lt;工具&gt;/&lt;方法&gt;</c> 等价于
        /// methodCall）；<paramref name="withConfig"/> 再挂配置端点
        /// （<c>/api/cfg/getSn|init|getModel|setModel|getDriver|setDriver</c>、
        /// <c>GET /api/getMqttUrl</c> + <c>POST /api/setMqttUrl</c>、
        /// <c>/api/method/getServerList|setServer</c>）。
        ///
        /// 想自己加 REST 端点用 <see cref="NclHttpEndpoint.Route"/>。
        /// </summary>
        /// <param name="port">0 表示用系统分配的随机端口（从
        /// <see cref="NclHttpEndpoint.Port"/> 读）。</param>
        /// <param name="withConfig">是否再挂配置端点。</param>
        public NclHttpEndpoint StartHttp(int port, bool withConfig)
        {
            IntPtr handle = Native.HttpStart(unchecked((uint)port), RequireOpen(),
                                             withConfig ? 1 : 0);
            if (handle == IntPtr.Zero)
            {
                throw new NclinkException(ErrIo, "StartHttp",
                    "HTTP 端口 " + port + " 起不来（被占用？）");
            }
            NclHttpEndpoint endpoint = new NclHttpEndpoint(this, handle,
                                                           Native.HttpPort(handle));
            _endpoints.Add(endpoint);
            return endpoint;
        }

        /// <summary>起 HTTP 端点（带配置端点）。</summary>
        public NclHttpEndpoint StartHttp(int port)
        {
            return StartHttp(port, true);
        }

        /// <summary>起 HTTP 端点（9008 + 配置端点；被占用时用
        /// <see cref="StartHttp(int, bool)"/> 换个端口或给 0）。</summary>
        public NclHttpEndpoint StartHttp()
        {
            return StartHttp(9008, true);
        }

        /// <summary>端点自己 Dispose 时把它从名单里摘掉。</summary>
        internal void ForgetEndpoint(NclHttpEndpoint endpoint)
        {
            _endpoints.Remove(endpoint);
        }

        /* ------------------------------------------------------------ 收尾 -- */

        /// <summary>停采样、停 FTP、断开 MQTT、放掉所有回调（可重复调用）。</summary>
        public void Dispose()
        {
            if (_handle == IntPtr.Zero)
            {
                return;
            }
            for (int i = 0; i < _endpoints.Count; i++)
            {
                _endpoints[i].Dispose();     // 先收 HTTP：路由回调还挂在服务器上
            }
            _endpoints.Clear();
            Native.ServerFree(_handle);      // 先停服务：之后不会再回调进来
            _handle = IntPtr.Zero;
            NclCallbackHost.Release(ref _toolHost, ref _toolAnchor);
            _toolCallback = null;
            NclCallbackHost.Release(ref _publishHost, ref _publishAnchor);
            _publishCallback = null;
            _publishSink = null;
            _handlers.Clear();
        }

        public override string ToString()
        {
            return "NclServer(" + (_handle != IntPtr.Zero ? _sn : "closed") + ")";
        }

        /* ---------------------------------------------------------- 回调桥 -- */

        private IntPtr EnsureToolHost()
        {
            if (_toolHost != IntPtr.Zero)
            {
                return _toolHost;
            }
            _toolCallback = new ToolCallback(OnToolNative);
            _toolAnchor = GCHandle.Alloc(this);
            _toolHost = NclCallbackHost.Allocate(_toolCallback, _toolAnchor);
            return _toolHost;
        }

        private int OnToolNative(IntPtr user, IntPtr tool, IntPtr method,
                                 IntPtr parameters, out IntPtr resultJson,
                                 out IntPtr reason)
        {
            resultJson = IntPtr.Zero;
            reason = IntPtr.Zero;
            NclServer self = null;
            try
            {
                self = Resolve(user);
                string toolName = Native.Utf8(tool);
                string methodName = Native.Utf8(method);
                NclToolHandler handler = null;
                if (self == null || toolName == null
                    || !self._handlers.TryGetValue(toolName, out handler)
                    || handler == null)
                {
                    reason = Native.Strdup(Native.Utf8Z(
                        "没有注册工具 " + (toolName ?? "?")));
                    return ErrNotFound;
                }

                object result;
                try
                {
                    result = handler(methodName, NclJson.Borrowed(parameters, null));
                }
                catch (NclinkException error)
                {
                    self.LastCallbackError = error;
                    reason = Native.Strdup(Native.Utf8Z(error.Message));
                    return error.Code;
                }
                catch (Exception error)
                {
                    self.LastCallbackError = error;
                    reason = Native.Strdup(Native.Utf8Z("处理失败: " + error.Message));
                    return Err;
                }

                string text = NclHandlerResult.ToJsonText(result);
                if (text == null)
                {
                    return 0;               // 成功但没有值：库按 NG 应答
                }
                resultJson = Native.Strdup(Native.Utf8Z(text));
                if (resultJson == IntPtr.Zero)
                {
                    reason = Native.Strdup(Native.Utf8Z("内存不足"));
                    return ErrNoMemory;
                }
                return 0;
            }
            catch (Exception error)
            {
                if (self != null)
                {
                    self.LastCallbackError = error;
                }
                reason = Native.Strdup(Native.Utf8Z("处理失败: " + error.Message));
                return Err;
            }
        }

        private int OnPublishNative(IntPtr user, IntPtr topic, IntPtr payload, int length)
        {
            NclServer self = null;
            try
            {
                self = Resolve(user);
                NclPublishSink sink = self == null ? null : self._publishSink;
                if (sink == null)
                {
                    return 0;
                }
                byte[] data = new byte[length > 0 ? length : 0];
                if (data.Length > 0)
                {
                    Marshal.Copy(payload, data, 0, data.Length);
                }
                sink(Native.Utf8(topic), data);
                return 0;
            }
            catch (Exception error)
            {
                try
                {
                    if (self != null)
                    {
                        self.LastCallbackError = error;
                    }
                }
                catch (Exception)
                {
                    /* 回调里再抛就真的没人接了 */
                }
                return Err;
            }
        }

        private static NclServer Resolve(IntPtr user)
        {
            if (user == IntPtr.Zero)
            {
                return null;
            }
            return GCHandle.FromIntPtr(user).Target as NclServer;
        }

        private IntPtr RequireOpen()
        {
            if (_handle == IntPtr.Zero)
            {
                throw new NclinkException(ErrClosed, "NclServer", "设备端已关闭");
            }
            return _handle;
        }

        /// <summary>JSON 字符串字面量（工具名 / 路径 / 方法名都要转义后再塞进 JSON）。</summary>
        internal static string Quote(string text)
        {
            StringBuilder builder = new StringBuilder(text.Length + 2);
            builder.Append('"');
            foreach (char ch in text)
            {
                switch (ch)
                {
                    case '"': builder.Append("\\\""); break;
                    case '\\': builder.Append("\\\\"); break;
                    case '\n': builder.Append("\\n"); break;
                    case '\r': builder.Append("\\r"); break;
                    case '\t': builder.Append("\\t"); break;
                    case '\b': builder.Append("\\b"); break;
                    case '\f': builder.Append("\\f"); break;
                    default:
                        if (ch < 0x20)
                        {
                            builder.Append("\\u").Append(
                                ((int)ch).ToString("x4", CultureInfo.InvariantCulture));
                        }
                        else
                        {
                            builder.Append(ch);
                        }
                        break;
                }
            }
            builder.Append('"');
            return builder.ToString();
        }

    }

    /// <summary>处理函数的返回值 → JSON 文本（null = 成功但没有值）。</summary>
    internal static class NclHandlerResult
    {
        internal static string ToJsonText(object value)
        {
            if (value == null)
            {
                return null;
            }
            NclJson json = value as NclJson;
            if (json != null)
            {
                return json.Encode();
            }
            string text = value as string;
            if (text != null)
            {
                return text;                    // 字符串按 JSON 文本处理
            }
            if (value is bool)
            {
                return (bool)value ? "true" : "false";
            }
            if (value is float)
            {
                return ((float)value).ToString("R", CultureInfo.InvariantCulture);
            }
            if (value is double)
            {
                return ((double)value).ToString("R", CultureInfo.InvariantCulture);
            }
            if (value is sbyte || value is byte || value is short || value is ushort
                || value is int || value is uint || value is long || value is ulong
                || value is decimal)
            {
                return Convert.ToString(value, CultureInfo.InvariantCulture);
            }
            throw new NclinkException(NclServer.ErrInvalidArg, "tool",
                "处理函数只认 NclJson / JSON 文本 / 数字 / 布尔，收到 "
                + value.GetType().FullName);
        }
    }
}
