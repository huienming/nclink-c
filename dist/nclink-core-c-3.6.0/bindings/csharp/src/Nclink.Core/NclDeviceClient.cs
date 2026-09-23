// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

using System;
using System.Runtime.InteropServices;

namespace Nclink
{
    /// <summary>收到采样上报。</summary>
    public sealed class NclSampleEventArgs : EventArgs
    {
        internal NclSampleEventArgs(NclSample sample) { Sample = sample; }

        /// <summary>已经拷成托管对象的报文快照。</summary>
        public NclSample Sample { get; private set; }
    }

    /// <summary>收到事件推送。</summary>
    public sealed class NclEventArgs : EventArgs
    {
        internal NclEventArgs(NclEvent item) { Event = item; }

        public NclEvent Event { get; private set; }
    }

    /// <summary>
    /// 一台设备的客户端（由进程级的 <see cref="Nclink"/> 持有，别自己释放）。
    ///
    /// 生命周期：先用 <see cref="Nclink.Init(string, string, string)"/> 建进程级连接，再
    /// <see cref="Nclink.GetDevice"/> 拿某个 SN 的客户端；<see cref="Nclink.Shutdown"/>
    /// 之后这个对象就失效了。
    /// </summary>
    public sealed partial class NclDeviceClient : IDisposable
    {
        /* 采样/事件回调：原生侧要求"两格数组 [函数指针, 用户数据]"。 */
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate void MessageCallback(IntPtr user, IntPtr topic, IntPtr msg);

        private readonly string _sn;
        private readonly IntPtr _client;

        private MessageCallback _sampleCallback;
        private MessageCallback _eventCallback;
        private GCHandle _sampleAnchor;
        private GCHandle _eventAnchor;
        private IntPtr _sampleHost;
        private IntPtr _eventHost;
        private bool _disposed;

        internal NclDeviceClient(string sn, IntPtr client)
        {
            _sn = sn;
            _client = client;
        }

        /// <summary>设备序列号。</summary>
        public string Sn { get { return _sn; } }

        /// <summary>收到采样上报（<see cref="SubscribeSamples"/> 之后才有）。</summary>
        public event EventHandler<NclSampleEventArgs> SampleReceived;

        /// <summary>收到事件推送（<see cref="SubscribeEvents"/> 之后才有）。</summary>
        public event EventHandler<NclEventArgs> EventReceived;

        /// <summary>回调里抛出的异常（不会往外冒，避免跨原生边界）。</summary>
        public Exception LastCallbackError { get; private set; }

        /* ------------------------------------------------------------ 请求 -- */

        /// <summary>probe：把设备模型拉回来（调用方负责 Dispose）。</summary>
        public NclModel Probe(uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            IntPtr model;
            NclinkException.Check(
                Native.ClientProbe(_client, timeoutMs, out model), "Probe");
            return new NclModel(model);
        }

        /// <summary>
        /// 把一份模型装进客户端（<c>null</c> = 清掉当前模型）：路径 ↔ 节点 id 互查、
        /// 采样报文按模型补齐缺的 paths 都靠它。客户端拿的是**自己的拷贝**，
        /// 传进来的 <paramref name="model"/> 还是调用方的（照常 Dispose）。
        /// </summary>
        public void LoadModel(NclModel model)
        {
            ThrowIfDisposed();
            NclinkException.Check(
                Native.ClientSetRootNode(_client,
                                         model == null ? IntPtr.Zero : model.Handle),
                "LoadModel");
        }

        /// <summary>清掉客户端当前装载的模型。</summary>
        public void ClearModel()
        {
            LoadModel(null);
        }

        /// <summary>getValue(path)。</summary>
        public NclJson GetValue(string path, uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            IntPtr value;
            NclinkException.Check(
                Native.ClientGetValue(_client, Native.Utf8Z(path), timeoutMs, out value),
                "GetValue");
            return NclJson.Owned(value);
        }

        /// <summary>getValue(path) 的便利版本：直接当整数取。</summary>
        public long GetLong(string path, uint timeoutMs = 5000, long defaultValue = 0)
        {
            using (NclJson value = GetValue(path, timeoutMs))
            {
                return value == null ? defaultValue : value.AsLong(defaultValue);
            }
        }

        /// <summary>getValueRange(path, start, end)（读取一段历史值）。</summary>
        public NclJson GetValueRange(string path, int start, int end,
                                     uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            IntPtr value;
            NclinkException.Check(
                Native.ClientGetValueRange(_client, Native.Utf8Z(path), start, end,
                                           timeoutMs, out value),
                "GetValueRange");
            return NclJson.Owned(value);
        }

        /// <summary>getLength(path)（该路径累计了多少个值）。</summary>
        public long GetLength(string path, uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            long length;
            NclinkException.Check(
                Native.ClientGetLength(_client, Native.Utf8Z(path), timeoutMs,
                                       out length),
                "GetLength");
            return length;
        }

        /// <summary>setValue(path, value)；值用 JSON 文本给。</summary>
        public void SetValue(string path, string valueJson, uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            NclinkException.Check(
                Native.ClientSetValue(_client, Native.Utf8Z(path),
                                      Native.Utf8Z(valueJson), timeoutMs),
                "SetValue");
        }

        /// <summary>setValue(path, value)：值用 <see cref="NclJson"/> 给。</summary>
        public void SetValue(string path, NclJson value, uint timeoutMs = 5000)
        {
            if (value == null)
            {
                throw new ArgumentNullException("value");
            }
            SetValue(path, value.Encode(), timeoutMs);
        }

        /// <summary>setValue(path, value, index)：写列表里的第 index 项。</summary>
        public void SetValueIndex(string path, string valueJson, int index,
                                  uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            NclinkException.Check(
                Native.ClientSetValueIndex(_client, Native.Utf8Z(path),
                                           Native.Utf8Z(valueJson), index, timeoutMs),
                "SetValueIndex");
        }

        /// <summary>
        /// 方法调用。method 形如 "/plc/setValue"（也接受 "plc/setValue"），
        /// paramsJson 可为 null；check=true 时设备只校验参数不执行。
        /// 返回应答报文的 JSON（自有，调用方 Dispose）。
        /// </summary>
        public NclJson MethodCall(string method, string paramsJson = null,
                                  bool check = false, uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            IntPtr response;
            NclinkException.Check(
                Native.ClientMethodCall(_client, Native.Utf8Z(method),
                                        paramsJson != null ? Native.Utf8Z(paramsJson)
                                                           : null,
                                        check ? 1 : 0, timeoutMs, out response),
                "MethodCall");
            /* 方法调用返还的是**应答报文的 JSON 文本**（malloc），不是 JSON 句柄 */
            return NclJson.TakeText(response);
        }

        /// <summary>
        /// 异步方法调用：立刻回一个应答（code=OK + handler），方法在设备端线程池里跑。
        /// 用 <see cref="MethodStatus"/> / <see cref="MethodResult"/> 拿那个 handler 查。
        /// </summary>
        public NclJson MethodCallAsync(string method, string paramsJson = null,
                                       uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            IntPtr response;
            NclinkException.Check(
                Native.ClientMethodCallAsync(_client, Native.Utf8Z(method),
                                             paramsJson != null
                                                 ? Native.Utf8Z(paramsJson)
                                                 : null,
                                             timeoutMs, out response),
                "MethodCallAsync");
            return NclJson.TakeText(response);
        }

        /// <summary>按句柄查异步调用的状态（process / status / code）。</summary>
        public NclJson MethodStatus(string objectId, string handler,
                                    uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            IntPtr response;
            NclinkException.Check(
                Native.ClientMethodStatus(_client, Native.Utf8Z(objectId),
                                          Native.Utf8Z(handler), timeoutMs,
                                          out response),
                "MethodStatus");
            return NclJson.TakeText(response);
        }

        /// <summary>
        /// 按句柄查异步调用的结果：未完成 code=PENDING（无 result），完成后
        /// code + return + result（finished / error），并释放该句柄。
        /// </summary>
        public NclJson MethodResult(string objectId, string handler,
                                    uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            IntPtr response;
            NclinkException.Check(
                Native.ClientMethodResult(_client, Native.Utf8Z(objectId),
                                          Native.Utf8Z(handler), timeoutMs,
                                          out response),
                "MethodResult");
            return NclJson.TakeText(response);
        }

        /// <summary>ping。</summary>
        public void Ping(uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            NclinkException.Check(Native.ClientPing(_client, timeoutMs), "Ping");
        }

        /// <summary>路径 → 节点 id（找不到返回 null）。</summary>
        public string GetId(string path)
        {
            ThrowIfDisposed();
            return Native.TakeUtf8(Native.ClientGetId(_client, Native.Utf8Z(path)));
        }

        /// <summary>节点 id → 路径（找不到返回 null）。</summary>
        public string GetPath(string id)
        {
            ThrowIfDisposed();
            return Native.TakeUtf8(Native.ClientGetPath(_client, Native.Utf8Z(id)));
        }

        /* ------------------------------------------------------------ 能力面 -- */

        /// <summary>
        /// 设备方法清单：模型 METHODS 项的 value（一个 NclJson 数组，用完 Dispose）。
        /// 每条含 tool / method / address（MethodCall 里写这个）/ params（入参
        /// JSON Schema）/ result（返回 JSON Schema）/ bindings（服务哪些路径）；
        /// 后三项没有就不出现。设备没报能力面时是空数组。
        /// </summary>
        public NclJson Methods()
        {
            ThrowIfDisposed();
            NclinkException.Check(Native.ClientMethodsJson(_client, out IntPtr json),
                                  "Methods");
            return NclJson.Parse(Native.TakeUtf8(json) ?? "[]");
        }

        /// <summary>
        /// 按地址取一个方法的元数据（"/plc/setValue"，前导斜杠可省）；没有就是 null。
        /// </summary>
        public NclJson FindMethod(string address)
        {
            ThrowIfDisposed();
            NclinkException.Check(
                Native.ClientFindMethodJson(_client, Native.Utf8Z(address), out IntPtr json),
                "FindMethod");
            string text = Native.TakeUtf8(json);
            return text == null ? null : NclJson.Parse(text);
        }

        /* ------------------------------------------------------ 采样与事件 -- */

        /// <summary>订阅采样（Sample/&lt;sn&gt;/#）；回调走 <see cref="SampleReceived"/>。</summary>
        public void SubscribeSamples(int qos = 0)
        {
            ThrowIfDisposed();
            if (_sampleHost == IntPtr.Zero)
            {
                _sampleCallback = OnSampleNative;
                _sampleAnchor = GCHandle.Alloc(this, GCHandleType.Normal);
                _sampleHost = AllocHost(_sampleCallback, _sampleAnchor);
            }
            NclinkException.Check(
                Native.ClientSubscribeSamples(_client, qos, _sampleHost),
                "SubscribeSamples");
        }

        /// <summary>退订采样。</summary>
        public void UnsubscribeSamples()
        {
            if (_disposed)
            {
                return;
            }
            NclinkException.Check(Native.ClientUnsubscribeSamples(_client),
                                  "UnsubscribeSamples");
            ReleaseHost(ref _sampleHost, ref _sampleAnchor, ref _sampleCallback);
        }

        /// <summary>订阅事件（Event/&lt;sn&gt;）；回调走 <see cref="EventReceived"/>。</summary>
        public void SubscribeEvents(int qos = 2)
        {
            ThrowIfDisposed();
            if (_eventHost == IntPtr.Zero)
            {
                _eventCallback = OnEventNative;
                _eventAnchor = GCHandle.Alloc(this, GCHandleType.Normal);
                _eventHost = AllocHost(_eventCallback, _eventAnchor);
            }
            NclinkException.Check(
                Native.ClientSubscribeEvents(_client, qos, _eventHost),
                "SubscribeEvents");
        }

        /// <summary>退订事件。</summary>
        public void UnsubscribeEvents()
        {
            if (_disposed)
            {
                return;
            }
            NclinkException.Check(Native.ClientUnsubscribeEvents(_client),
                                  "UnsubscribeEvents");
            ReleaseHost(ref _eventHost, ref _eventAnchor, ref _eventCallback);
        }

        /// <summary>已收到的采样报文条数。</summary>
        public int SampleCount { get { return Native.ClientSampleCount(_client); } }

        /// <summary>已收到的事件条数。</summary>
        public int EventCount { get { return Native.ClientEventCount(_client); } }

        /// <summary>运行时加一个采样通道：configJson 是 SAMPLE_CHANNEL 配置节点。</summary>
        public void AddSample(string configJson, uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            NclinkException.Check(
                Native.ClientAddSample(_client, Native.Utf8Z(configJson), timeoutMs),
                "AddSample");
        }

        /// <summary>运行时删掉一个采样通道。</summary>
        public void RemoveSample(string channelId, uint timeoutMs = 5000)
        {
            ThrowIfDisposed();
            NclinkException.Check(
                Native.ClientRemoveSample(_client, Native.Utf8Z(channelId), timeoutMs),
                "RemoveSample");
        }

        /* -------------------------------------------------------- 回调桥接 -- */

        private static IntPtr AllocHost(MessageCallback callback, GCHandle anchor)
        {
            IntPtr host = Marshal.AllocHGlobal(2 * IntPtr.Size);
            Marshal.WriteIntPtr(host, 0, Marshal.GetFunctionPointerForDelegate(callback));
            Marshal.WriteIntPtr(host, IntPtr.Size, GCHandle.ToIntPtr(anchor));
            return host;
        }

        private static void ReleaseHost(ref IntPtr host, ref GCHandle anchor,
                                        ref MessageCallback callback)
        {
            if (host != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(host);
                host = IntPtr.Zero;
            }
            if (anchor.IsAllocated)
            {
                anchor.Free();
            }
            callback = null;
        }

        private void OnSampleNative(IntPtr user, IntPtr topic, IntPtr msg)
        {
            try
            {
                NclDeviceClient self = Resolve(user);
                string topicText = Native.Utf8(topic);
                NclSample sample = NclSample.Capture(msg, topicText);
                EventHandler<NclSampleEventArgs> handler = self != null
                                                               ? self.SampleReceived
                                                               : null;
                if (handler != null)
                {
                    handler(self, new NclSampleEventArgs(sample));
                }
            }
            catch (Exception ex)
            {
                RecordCallbackError(user, ex);
            }
        }

        private void OnEventNative(IntPtr user, IntPtr topic, IntPtr msg)
        {
            try
            {
                NclDeviceClient self = Resolve(user);
                string topicText = Native.Utf8(topic);
                NclEvent item = NclEvent.Capture(msg, topicText);
                EventHandler<NclEventArgs> handler = self != null
                                                         ? self.EventReceived
                                                         : null;
                if (handler != null)
                {
                    handler(self, new NclEventArgs(item));
                }
            }
            catch (Exception ex)
            {
                RecordCallbackError(user, ex);
            }
        }

        private static NclDeviceClient Resolve(IntPtr user)
        {
            GCHandle handle = GCHandle.FromIntPtr(user);
            return handle.Target as NclDeviceClient;
        }

        private static void RecordCallbackError(IntPtr user, Exception error)
        {
            try
            {
                NclDeviceClient self = Resolve(user);
                if (self != null)
                {
                    self.LastCallbackError = error;
                }
            }
            catch (Exception)
            {
                /* 回调里再抛就真的没人接了 */
            }
        }

        /// <summary>退订两类回调并释放桥接内存（<see cref="Nclink.Shutdown"/> 也会做）。</summary>
        public void Dispose()
        {
            if (_disposed)
            {
                return;
            }
            try
            {
                UnsubscribeSamples();
                UnsubscribeEvents();
            }
            catch (Exception)
            {
                /* 连接已经断了就算了 */
            }
            ReleaseHost(ref _sampleHost, ref _sampleAnchor, ref _sampleCallback);
            ReleaseHost(ref _eventHost, ref _eventAnchor, ref _eventCallback);
            _disposed = true;
        }

        private void ThrowIfDisposed()
        {
            if (_disposed)
            {
                throw new ObjectDisposedException("NclDeviceClient");
            }
        }
    }
}
