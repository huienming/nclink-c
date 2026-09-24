// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

using System;

namespace Nclink
{
    /// <summary>报文种类（对应 ncl_msg_type）。</summary>
    public enum NclMessageType
    {
        Unknown = 0,
        Ping = 1,
        Pong = 2,
        ProbeVersion = 3,
        RegisterRequest = 4,
        RegisterResponse = 5,
        QueryRequest = 6,
        QueryResponse = 7,
        SetRequest = 8,
        SetResponse = 9,
        ProbeQueryRequest = 10,
        ProbeQueryResponse = 11,
        ProbeSetRequest = 12,
        ProbeSetResponse = 13,
        Sample = 14,
        Event = 15,
        MethodCallRequest = 16,
        MethodCallResponse = 17
    }

    /// <summary>
    /// 一条报文（按主题 + 报文体用库自己的解码器解出来）。自己拿到的 MQTT 报文
    /// （离线回放、日志里存的样本）也能这么解，不必再手写一遍 JSON。
    ///
    /// <code>
    /// using (NclMessage message = Nclink.Parse("Sample/V203243111F/s1", payload))
    /// {
    ///     if (message.Type == NclMessageType.Sample)
    ///     {
    ///         NclSample sample = message.AsSample();   // 托管快照，出了 using 也能用
    ///     }
    /// }
    /// </code>
    /// </summary>
    public sealed class NclMessage : IDisposable
    {
        private readonly string _topic;
        private IntPtr _handle;

        private NclMessage(string topic, IntPtr handle)
        {
            _topic = topic;
            _handle = handle;
        }

        /// <summary>按主题 + 报文体解析；报文非法时抛 <see cref="NclinkException"/>。</summary>
        public static NclMessage Parse(string topic, byte[] payload)
        {
            if (string.IsNullOrEmpty(topic))
            {
                throw new ArgumentNullException("topic");
            }
            if (payload == null)
            {
                throw new ArgumentNullException("payload");
            }
            byte[] buffer = payload.Length == 0 ? new byte[1] : payload;
            IntPtr handle = Native.MessageParse(Native.Utf8Z(topic), buffer,
                                                payload.Length);
            if (handle == IntPtr.Zero)
            {
                throw new NclinkException(-3, "ParseMessage", "报文解析失败: " + topic);
            }
            return new NclMessage(topic, handle);
        }

        /// <summary>主题。</summary>
        public string Topic { get { return _topic; } }

        /// <summary>报文种类。</summary>
        public NclMessageType Type
        {
            get
            {
                ThrowIfDisposed();
                return (NclMessageType)Native.MessageType(_handle);
            }
        }

        /// <summary>整条报文的 JSON 文本。</summary>
        public string Json
        {
            get
            {
                ThrowIfDisposed();
                return Native.TakeUtf8(Native.MessageWrite(_handle));
            }
        }

        /// <summary>是采样报文就拷成托管快照，否则返回 null。</summary>
        public NclSample AsSample()
        {
            return Type == NclMessageType.Sample ? NclSample.Capture(_handle, _topic)
                                                 : null;
        }

        /// <summary>是事件报文就拷成托管快照，否则返回 null。</summary>
        public NclEvent AsEvent()
        {
            return Type == NclMessageType.Event ? NclEvent.Capture(_handle, _topic)
                                                : null;
        }

        public override string ToString()
        {
            return _handle == IntPtr.Zero
                       ? "(disposed)"
                       : "Message " + _topic + " type=" + Type;
        }

        /// <summary>释放原生报文（幂等）。</summary>
        public void Dispose()
        {
            if (_handle != IntPtr.Zero)
            {
                Native.MessageFree(_handle);
                _handle = IntPtr.Zero;
            }
        }

        private void ThrowIfDisposed()
        {
            if (_handle == IntPtr.Zero)
            {
                throw new ObjectDisposedException("NclMessage");
            }
        }
    }
}
