// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// C# 设备端示例（.NET Framework / .NET Core 都能跑）：这个进程就是一台机床。
//
//   Nclink.Demo.Device.exe [broker] [设备SN] [秒数] [HTTP端口]
//   # 默认：tcp://127.0.0.1:1883、V2CS0000001、一直运行到 Ctrl+C、HTTP 9008
//   # 秒数写 0 = 一直跑；HTTP 端口写 0 = 用系统分配的随机端口
//   # broker 写 "-" = 不接 MQTT（离线：只用 REST 端点/自己驱动）
//
// 它注册工具方法、绑定模型里的路径、启动采样通道、每秒推一条事件，还起一个
// REST 端点（OpenAPI + Swagger UI + 工具端点 + 配置端点）；然后用仓库里**任何一个
// 客户端**都能读它，例如 C 的示例：
//
//   build\examples\ncl_client_demo.exe tcp://127.0.0.1:1883 V2CS0000001 8
//
// 模型（含采样通道）直接写在下面的 MODEL 里：换模型就是换这段 JSON。

using System;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;
using Nclink;

namespace Nclink.Demo
{
    internal static class Program
    {
        /// <summary>模拟机床的模型：一个采样通道（1 s 采、1 s 传）+ 三个数据项。</summary>
        private const string Model = @"{
  ""name"": ""C# 模拟机床"",
  ""id"": ""01"",
  ""type"": ""NC_LINK_ROOT"",
  ""devices"": [{
    ""id"": ""02"",
    ""type"": ""MACHINE"",
    ""name"": ""模拟机床"",
    ""version"": ""2.0"",
    ""configs"": [{
      ""name"": ""采样通道"",
      ""id"": ""cs_channel"",
      ""type"": ""SAMPLE_CHANNEL"",
      ""sampleInterval"": 1000,
      ""uploadInterval"": 1000,
      ""ids"": [{""id"": ""/STATUS""}, {""id"": ""/PART_COUNT""},
               {""id"": ""/CONTROLLER/WARNNING""}]
    }],
    ""dataItems"": [
      {""name"": ""状态"", ""id"": ""030001"", ""type"": ""STATUS"",
       ""settable"": false, ""description"": ""1=运行""},
      {""name"": ""加工计件"", ""id"": ""030002"", ""type"": ""PART_COUNT"",
       ""settable"": true, ""description"": ""加工计件,单位:件""},
      {""name"": ""报警"", ""id"": ""030003"", ""type"": ""WARNNING"",
       ""source"": ""CONTROLLER""}
    ]
  }]
}";

        private static volatile bool _stopping;

        private static int Main(string[] args)
        {
            string brokerArg = args.Length > 0 ? args[0] : "tcp://127.0.0.1:1883";
            string broker = brokerArg == "-" || brokerArg.Length == 0 ? null : brokerArg;
            string sn = args.Length > 1 ? args[1] : "V2CS0000001";
            int seconds = args.Length > 2
                              ? int.Parse(args[2], CultureInfo.InvariantCulture)
                              : 0;
            int httpPort = args.Length > 3
                               ? int.Parse(args[3], CultureInfo.InvariantCulture)
                               : 9008;

            Console.CancelKeyPress += delegate(object sender, ConsoleCancelEventArgs e)
            {
                e.Cancel = true;                // 走正常收尾（停采样、停 HTTP、断 MQTT）
                _stopping = true;
            };

            // 被工具方法读写的"机床状态"（真实设备里换成你的 PLC / 采集卡）
            long status = 1;
            long partCount = 0;
            long warning = 0;

            Nclink.LogInit();
            try
            {
                // 离线（broker 为 null）时给一个"自研传输"：出站报文打到控制台。
                // 不接 MQTT 又没有自研传输的话，报文没有去处 —— 推送会被拒。
                NclPublishSink sink = broker == null ? (NclPublishSink)OnOfflinePublish
                                                    : null;
                using (NclServer device = new NclServer(sn, Model, broker, null, null,
                                                        sink))
                {
                    device.RegisterTool(
                        "plc",
                        new string[] { "getStatus", "getCount", "setCount",
                                       "getWarning" },
                        new NclToolBinding[]
                        {
                            new NclToolBinding("/STATUS", NclOperation.GetValue,
                                               "getStatus"),
                            new NclToolBinding("/PART_COUNT", NclOperation.GetValue,
                                               "getCount"),
                            new NclToolBinding("/PART_COUNT", NclOperation.SetValue,
                                               "setCount"),
                            new NclToolBinding("/CONTROLLER/WARNNING",
                                               NclOperation.GetValue, "getWarning")
                        },
                        delegate(string method, NclJson parameters)
                        {
                            switch (method)
                            {
                            case "getStatus":
                                return status;
                            case "getCount":
                                return partCount;
                            case "getWarning":
                                return warning;
                            case "setCount":
                                // 参数是 JSON：setValue 的形状是 {"value": ...}
                                partCount = parameters.Get("value").AsLong();
                                return partCount;
                            default:
                                return null;    // 没写的方法 = 没有值
                            }
                        });
                    device.RegisterBuiltinTool();   // addSample / removeSample
                    if (broker != null)
                    {
                        device.Subscribe();         // 订阅 6 个请求主题（要接 MQTT）
                    }
                    device.InitSamples();           // 启动模型里声明的采样通道

                    // 注意别写成 using (...) { ... } 把端点关在块里：这里要让端点
                    // 一直活到进程退出（device.Dispose() 也会替你收）。
                    NclHttpEndpoint http = device.StartHttp(httpPort, true);
                    http.Route("GET", "/api/hello", delegate(NclHttpRequest request)
                    {
                        return NclHttpReply.Json(
                            "{\"sn\":\"" + device.Sn
                            + "\",\"partCount\":" + partCount
                               .ToString(CultureInfo.InvariantCulture) + "}");
                    });
                    Console.WriteLine("HTTP: {0}/api/schema（Swagger UI: {0}/swagger-ui）",
                                      http.Url);

                    Console.WriteLine(
                        "设备端已就绪：SN={0} broker={1}，工具 {2} 个操作，采样通道 {3} 个",
                        sn, broker ?? "(不接 MQTT)", device.OperationCount,
                        device.SampleCount);
                    Console.WriteLine("（用仓库里的任意客户端读它，例如 " +
                                      "build\\examples\\ncl_client_demo.exe）");
                    if (broker == null)
                    {
                        Console.WriteLine("离线模式：没有 MQTT，客户端读不到；" +
                                          "REST 端点照常用。");
                    }

                    DateTime deadline = seconds > 0
                                            ? DateTime.UtcNow.AddSeconds(seconds)
                                            : DateTime.MaxValue;
                    while (!_stopping && DateTime.UtcNow < deadline)
                    {
                        Thread.Sleep(1000);
                        partCount++;                    // 模拟产量累加
                        status = 1;
                        warning = 0;
                        device.PushEvent("010307",
                            "{\"key\":\"PART_COUNT\",\"value\":" + partCount
                                .ToString(CultureInfo.InvariantCulture)
                            + ",\"oldValue\":" + (partCount - 1)
                                .ToString(CultureInfo.InvariantCulture) + "}");
                        if (partCount % 5 == 0)
                        {
                            Console.WriteLine("已上报采样 {0} 次，事件 {1} 条（计件 {2}）",
                                              device.SampleUploadCount,
                                              device.EventCount, partCount);
                        }
                    }

                    Console.WriteLine("设备端已退出：采样上报 {0} 次，事件 {1} 条",
                                      device.SampleUploadCount, device.EventCount);
                }
                return 0;
            }
            catch (NclinkException error)
            {
                Console.WriteLine("NC-Link error: {0}", error.Message);
                return 1;
            }
            catch (IOException error)
            {
                Console.WriteLine("IO error: {0}", error.Message);
                return 1;
            }
            finally
            {
                Nclink.LogShutdown();
            }
        }

        /// <summary>离线模式的传输：每条出站报文（事件 / 采样）打到控制台。</summary>
        private static void OnOfflinePublish(string topic, byte[] payload)
        {
            string text = Encoding.UTF8.GetString(payload);
            Console.WriteLine("publish {0} {1}", topic,
                              text.Length > 120 ? text.Substring(0, 120) + " ..."
                                                : text);
        }
    }
}
