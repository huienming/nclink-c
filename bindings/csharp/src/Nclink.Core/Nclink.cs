// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

using System;

namespace Nclink
{
    /// <summary>日志级别（对应 ncl_log_level）。</summary>
    public enum NclinkLogLevel
    {
        Debug = 0,
        Info = 1,
        Warn = 2,
        Error = 3,
        Fatal = 4
    }

    /// <summary>
    /// 绑定入口：进程级初始化 / 关闭、日志、安装根目录，以及按 SN 取设备客户端。
    ///
    /// <code>
    /// Nclink.Init("tcp://127.0.0.1:1883");
    /// using (NclDeviceClient device = Nclink.GetDevice("V2023A7B762"))
    /// {
    ///     using (NclModel model = device.Probe()) { ... }
    ///     device.SubscribeSamples();
    ///     device.SampleReceived += (s, e) => Console.WriteLine(e.Sample);
    /// }
    /// Nclink.Shutdown();
    /// </code>
    /// </summary>
    public static class Nclink
    {
        /// <summary>C 库版本号。</summary>
        public static string Version { get { return Native.Utf8(Native.Version()); } }

        /// <summary>进程级连接是否已经建好。</summary>
        public static bool IsOpen { get { return Native.IsOpen() != 0; } }

        /// <summary>
        /// 建进程级连接（内部建 MQTT 连接并起客户端管理器）。一个进程调一次，配
        /// <see cref="Shutdown"/>。
        /// </summary>
        /// <param name="brokerUri">例如 tcp://127.0.0.1:1883 或 ssl://host:8883。</param>
        /// <param name="username">可空 = 匿名。</param>
        /// <param name="password">可空。</param>
        public static void Init(string brokerUri, string username = null,
                                string password = null)
        {
            if (brokerUri == null)
            {
                throw new ArgumentNullException("brokerUri");
            }
            NclinkException.Check(
                Native.Open(Native.Utf8Z(brokerUri), Native.Utf8Z(username),
                            Native.Utf8Z(password)),
                "Init");
        }

        /// <summary>取（必要时创建）某个 SN 的设备客户端。</summary>
        public static NclDeviceClient GetDevice(string sn)
        {
            if (string.IsNullOrEmpty(sn))
            {
                throw new ArgumentNullException("sn");
            }
            IntPtr client = Native.ClientGet(Native.Utf8Z(sn));
            if (client == IntPtr.Zero)
            {
                throw new NclinkException(-6, "GetDevice",
                                          "取设备客户端失败: " + sn + "（先调 Init？）");
            }
            return new NclDeviceClient(sn, client);
        }

        /// <summary>
        /// 把一条 MQTT 报文按库的规则解码（采样 / 事件也是同一条路，只是之后可以
        /// <see cref="NclMessage.AsSample"/> / <see cref="NclMessage.AsEvent"/> 拿快照）。
        /// 调用方负责 Dispose 返回的报文。
        /// </summary>
        public static NclMessage Parse(string topic, byte[] payload)
        {
            return NclMessage.Parse(topic, payload);
        }

        /// <summary>把一条报文按 UTF-8 文本解码，省得自己编码。</summary>
        public static NclMessage Parse(string topic, string payload)
        {
            return NclMessage.Parse(topic,
                                    System.Text.Encoding.UTF8.GetBytes(payload ?? string.Empty));
        }

        /// <summary>断开连接、释放所有客户端。</summary>
        public static void Shutdown()
        {
            if (IsOpen)
            {
                Native.Close();
            }
        }

        /// <summary>安装根目录（conf/、bin/、log/ 的父目录）；设置时复制进原生侧。</summary>
        public static string RootDirectory
        {
            get { return Native.Utf8(Native.EnvRoot()); }
            set { Native.EnvSetRoot(Native.Utf8Z(value)); }
        }

        /// <summary>初始化日志；<paramref name="dir"/> 为空时用 &lt;root&gt;/log。</summary>
        public static bool LogInit(string dir = null)
        {
            return Native.LogInit(Native.Utf8Z(dir)) != 0;
        }

        /// <summary>关掉日志（写盘线程回收）。</summary>
        public static void LogShutdown()
        {
            Native.LogShutdown();
        }

        /// <summary>日志级别。</summary>
        public static NclinkLogLevel LogLevel
        {
            set { Native.LogSetLevel((int)value); }
        }

        /// <summary>是否同时往控制台打（默认开）。</summary>
        public static void SetConsoleLog(bool enabled)
        {
            Native.LogSetConsole(enabled ? 1 : 0);
        }
    }
}
