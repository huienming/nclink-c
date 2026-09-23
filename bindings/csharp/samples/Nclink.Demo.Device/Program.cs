// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// 设备端示例：这个 .NET 进程就是一台机床。
//
//   Nclink.Demo.Device.exe [broker] [sn] [seconds] [http-port]
//
//     broker    tcp://… / ssl://…；"-" = 离线（不接 MQTT，出站报文打控制台）；
//               省略 = 读 <root>/conf/mqtt.cfg（没有就先写一份默认的）
//     sn        省略或 "-" = <root>/bin/sn.txt（没有就生成 "V2" + 9 位十六进制）
//     seconds   0 或省略 = 一直运行到 Ctrl+C
//     http-port 省略 = 9008；0 = 系统分配的随机端口
//     安装根目录：环境变量 NCL_DEVICE_ROOT（省略 = 当前目录）
//
// **五个语言的设备端示例是同一台设备**：模型编译在库里（Nclink.DeviceModel，
// 与 C 示例共用 examples/device_model.c），29 个工具方法、20 条绑定，两个采样
// 通道与事件节拍都一样。每个轴一个功率 /AXIS@<轴>/POWER@1 与三个加速度
// /AXIS@<轴>/ACCELERATION@X|Y|Z —— 振动信号在三个方向上的分量。

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;
using Nclink;

namespace Nclink.Demo
{
    internal static class Program
    {
        private static readonly string[] Axes = { "X", "Y", "Z", "C", "S" };
        private static readonly string[] Dirs = { "X", "Y", "Z" };
        private static readonly string[] Scalars =
        {
            "getValue", "setValue", "getCount", "getWarning", "getProgram",
            "getToolNumber", "getFeedOverride", "getMachiningMode", "getSpeedS"
        };
        private static readonly string[] ScalarPaths =
        {
            "/MACHINE/STATUS", "/MACHINE/STATUS", "/MACHINE/PART_COUNT", "/MACHINE/CONTROLLER/WARNING",
            "/MACHINE/CONTROLLER/PROGRAM", "/MACHINE/CONTROLLER/TOOL_NUMBER", "/MACHINE/FEED_OVERRIDE",
            "/MACHINE/MACHINING_MODE", "/MACHINE/AXIS@S/SPEED"
        };
        private const string DefaultBroker = "tcp://127.0.0.1:1883";

        /// <summary>被工具方法读写的"机床状态"（真机里换成你的 PLC / 采集卡）。</summary>
        private sealed class Machine
        {
            private readonly long[] _powerTick = new long[Axes.Length];
            private readonly long[,] _vibrationTick =
                new long[Axes.Length, Dirs.Length];
            private int _status = 1;

            public long PartCount { get; private set; }
            public int Program { get; private set; }
            public int ToolNumber { get; private set; }
            public int FeedOverride { get; private set; }
            public int SpindleSpeed { get; private set; }
            public int Mode { get; private set; }

            public Machine()
            {
                Program = 1001;
                ToolNumber = 1;
                FeedOverride = 100;
                SpindleSpeed = 3600;
                Mode = 2;
            }

            public int Status { get { return Volatile.Read(ref _status); } }

            /// <summary>功率（W）：每轴一个基值 + 0~300 W 的缓升，25 格一个锯齿。</summary>
            private double Power(string axis)
            {
                int slot = Array.IndexOf(Axes, axis);
                lock (_powerTick)
                {
                    long tick = _powerTick[slot]++;
                    return 800.0 + slot * 250.0 + ((tick + slot * 7) % 25) * 12.5;
                }
            }

            /// <summary>
            /// 振动：一次查询给 4 个 0.25 ms 子采样（1 ms 槽位里的 4 kHz 波形）。
            /// 返回 **JSON 文本**（绑定把 string 当 JSON 处理）：一批 4 个点，
            /// 库会摊平成一列 —— 与其它语言的示例同一形状。
            /// </summary>
            private string Vibration(string axis, string direction)
            {
                int a = Array.IndexOf(Axes, axis);
                int d = Array.IndexOf(Dirs, direction);
                int slot = a * Dirs.Length + d;
                double[] block = new double[4];
                lock (_vibrationTick)
                {
                    long step = _vibrationTick[a, d];
                    _vibrationTick[a, d] += 4;
                    for (int k = 0; k < block.Length; k++)
                    {
                        block[k] = (((step + k + slot * 3) % 16) - 8) * 0.125;
                    }
                }
                StringBuilder text = new StringBuilder("[");
                for (int k = 0; k < block.Length; k++)
                {
                    if (k > 0)
                    {
                        text.Append(',');
                    }
                    text.Append(block[k].ToString(CultureInfo.InvariantCulture));
                }
                return text.Append(']').ToString();
            }

            public object Handle(string method, NclJson parameters)
            {
                if (method == "setValue")
                {
                    Volatile.Write(ref _status,
                                   Convert.ToInt32(parameters.Get("value").AsLong()));
                    Console.WriteLine("STATUS 被设置为 " + _status);
                    return true;
                }
                switch (method)
                {
                    case "getValue": return Status;
                    case "getCount": return PartCount;
                    case "getWarning": return 0;
                    case "getProgram": return Program;
                    case "getToolNumber": return ToolNumber;
                    case "getFeedOverride": return FeedOverride;
                    case "getMachiningMode": return Mode;
                    case "getSpeedS": return SpindleSpeed;
                }
                if (method.StartsWith("getPower", StringComparison.Ordinal))
                {
                    return Power(method.Substring("getPower".Length));
                }
                if (method.StartsWith("getAcceleration", StringComparison.Ordinal))
                {
                    string axis = method.Substring("getAcceleration".Length, 1);
                    string dir = method.Substring("getAcceleration".Length + 1);
                    return Vibration(axis, dir);
                }
                throw new InvalidOperationException("没有方法 " + method);
            }

            public void Tick(long i)
            {
                PartCount++;
                FeedOverride = 60 + (int)(i % 7) * 10;
                SpindleSpeed = 3000 + (int)(i % 5) * 300;
                if (i % 20 == 0)
                {
                    Program++;
                    ToolNumber = 1 + Program % 8;
                    Mode = Program % 2 == 0 ? 1 : 2;
                }
            }
        }

        private static string ReadMqttCfg(string root)
        {
            string path = Path.Combine(root, "conf", "mqtt.cfg");
            if (File.Exists(path))
            {
                foreach (string line in File.ReadAllLines(path))
                {
                    string trimmed = line.Trim();
                    if (trimmed.StartsWith("url=", StringComparison.Ordinal))
                    {
                        string url = trimmed.Substring(4).Trim();
                        return url.Length == 0 ? DefaultBroker : url;
                    }
                }
            }
            return DefaultBroker;
        }

        private static void Bootstrap(string root)
        {
            foreach (string sub in new[] { "conf", "bin", "log" })
            {
                Directory.CreateDirectory(Path.Combine(root, sub));
            }
            string cfg = Path.Combine(root, "conf", "mqtt.cfg");
            if (!File.Exists(cfg))
            {
                File.WriteAllText(cfg,
                                  "url=" + DefaultBroker
                                  + "\r\nusername=\r\npassword=\r\n");
                Console.WriteLine("首次启动：写入 MQTT 配置（" + cfg + "）");
            }
        }

        /// <summary>&lt;root&gt;/bin/sn.txt；没有就按库的规则生成。</summary>
        private static string ReadSn(string root)
        {
            string path = Path.Combine(root, "bin", "sn.txt");
            if (File.Exists(path))
            {
                string text = File.ReadAllText(path).Trim();
                if (text.Length > 0)
                {
                    return text;
                }
            }
            Random random = new Random();
            const string digits = "0123456789ABCDEF";
            string sn;
            do
            {
                StringBuilder text = new StringBuilder("V2");
                for (int i = 0; i < 9; i++)
                {
                    text.Append(digits[random.Next(digits.Length)]);
                }
                sn = text.ToString();
            }
            while (sn.Replace("0", "").Replace("1", "").Replace("2", "")
                     .Replace("3", "").Replace("4", "").Replace("5", "")
                     .Replace("6", "").Replace("7", "").Replace("8", "")
                     .Replace("9", "").Length == 0);
            File.WriteAllText(path, sn);
            Console.WriteLine("首次启动：生成 SN（" + path + "）");
            return sn;
        }

        /// <summary>模型编译在库里：现场那份优先，没有就写一份下来。</summary>
        private static string LoadModel(string root)
        {
            string installed = Path.Combine(root, "conf", "model", "nclink.json");
            if (File.Exists(installed))
            {
                return File.ReadAllText(installed);
            }
            string text = Nclink.DeviceModel;
            Directory.CreateDirectory(Path.GetDirectoryName(installed));
            File.WriteAllText(installed, text);
            Console.WriteLine("首次启动：写入设备模型（" + installed
                              + "，编译在库里的那份）");
            return text;
        }

        private static int Main(string[] args)
        {
            string brokerArg = args.Length > 0 ? args[0] : "";
            string snArg = args.Length > 1 ? args[1] : "";
            int seconds = args.Length > 2 ? int.Parse(args[2], CultureInfo.InvariantCulture)
                                          : 0;
            int httpPort = args.Length > 3 ? int.Parse(args[3], CultureInfo.InvariantCulture)
                                           : 9008;
            string root = Environment.GetEnvironmentVariable("NCL_DEVICE_ROOT");
            if (string.IsNullOrEmpty(root))
            {
                root = ".";
            }
            bool offline = brokerArg == "-";

            Bootstrap(root);
            Nclink.RootDirectory = root;
            Nclink.LogInit();
            string sn = snArg.Length == 0 || snArg == "-" ? ReadSn(root) : snArg;
            string broker = offline ? null
                                    : (brokerArg.Length == 0 ? ReadMqttCfg(root)
                                                             : brokerArg);
            string model = LoadModel(root);

            // 方法表与绑定表：9 个标量 + 5 个轴功率 + 15 个方向加速度。
            List<string> methods = new List<string>();
            List<NclToolBinding> bindings = new List<NclToolBinding>();
            for (int i = 0; i < Scalars.Length; i++)
            {
                methods.Add(Scalars[i]);
                bindings.Add(new NclToolBinding(
                    ScalarPaths[i],
                    Scalars[i] == "setValue" ? NclOperation.SetValue
                                             : NclOperation.GetValue,
                    Scalars[i]));
            }
            foreach (string axis in Axes)
            {
                methods.Add("getPower" + axis);
                bindings.Add(new NclToolBinding("/MACHINE/AXIS@" + axis + "/MACHINE/POWER@1",
                                                NclOperation.GetValue,
                                                "getPower" + axis));
            }
            // 振动只留主轴：模型里 X/Y/Z/C 四轴已经没有 ACCELERATION 数据项了。
            foreach (string axis in new[] { "S" })
            {
                foreach (string dir in Dirs)
                {
                    methods.Add("getAcceleration" + axis + dir);
                    bindings.Add(new NclToolBinding(
                        "/MACHINE/AXIS@" + axis + "/ACCELERATION@" + dir,
                        NclOperation.GetValue, "getAcceleration" + axis + dir));
                }
            }

            Machine machine = new Machine();
            bool stopped = false;
            Console.CancelKeyPress += delegate(object sender, ConsoleCancelEventArgs e)
            {
                e.Cancel = true;
                stopped = true;
            };
            NclPublishSink sink = null;
            if (offline)
            {
                sink = delegate(string topic, byte[] payload)
                {
                    Console.WriteLine("out  " + topic + "  "
                                      + Encoding.UTF8.GetString(payload));
                };
            }

            using (NclServer device = new NclServer(sn, model, broker, null, null, sink))
            {
                device.RegisterTool("plc", methods.ToArray(), bindings.ToArray(),
                                    machine.Handle);
                device.RegisterBuiltinTool();
                device.RegisterFileTool();
                if (!offline)
                {
                    device.Subscribe();
                }
                device.InitSamples();
                NclHttpEndpoint http = device.StartHttp(httpPort, true);
                http.Route("GET", "/api/hello", delegate(NclHttpRequest request)
                {
                    return NclHttpReply.Json("{\"sn\":\"" + sn + "\",\"status\":"
                                             + machine.Status + ",\"parts\":"
                                             + machine.PartCount + "}");
                });

                Console.WriteLine("设备 SN: " + sn);
                Console.WriteLine("MQTT: " + (offline
                    ? "离线模式（出站报文打到控制台）" : broker));
                Console.WriteLine("HTTP: " + http.Url
                                  + "/swagger-ui（自定义路由 /api/hello）");
                Console.WriteLine("工具 " + device.OperationCount + " 个操作，采样通道 "
                                  + device.SampleCount + " 个");
                Console.WriteLine("运行 " + (seconds > 0
                    ? seconds + " 秒（Ctrl+C 可随时退出）" : "直到 Ctrl+C"));

                long deadline = seconds > 0
                    ? DateTime.Now.Ticks / TimeSpan.TicksPerMillisecond + seconds * 1000L
                    : 0;
                long i = 0;
                while (!stopped
                       && (deadline == 0
                           || DateTime.Now.Ticks / TimeSpan.TicksPerMillisecond < deadline))
                {
                    Thread.Sleep(100);
                    i++;
                    machine.Tick(i);
                    if (i % 10 == 0)
                    {
                        device.PushEvent("010307", "{\"key\":\"PART_COUNT\",\"value\":"
                            + machine.PartCount + ",\"oldValue\":"
                            + (machine.PartCount - 1) + "}");
                        Console.WriteLine("事件 PART_COUNT=" + machine.PartCount
                            + "；采样上报 " + device.SampleUploadCount + " 次，状态 "
                            + machine.Status + "，刀号 " + machine.ToolNumber);
                    }
                }
                Console.WriteLine("设备端退出统计：采样上报 " + device.SampleUploadCount
                                  + " 次，事件 " + device.EventCount + " 条");
            }
            Nclink.LogShutdown();
            return 0;
        }
    }
}
