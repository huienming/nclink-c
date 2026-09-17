// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// C# 客户端示例（.NET Framework / .NET Core 都能跑）：
//
//   Nclink.Demo.Cli.exe tcp://127.0.0.1:1883 V2023A7B762 [秒数]
//
// 流程与 C / C++ 示例一致：probe 模型 → 打印各轴的功率与振动含义 → 读 / 写一个值
// → 订阅采样与事件 → 按行消费采样（前 8 行）→ 等若干秒 → 打印计数。

using System;
using System.Collections.Generic;
using System.Threading;
using Nclink;

namespace Nclink.Demo
{
    internal static class Program
    {
        private static int Main(string[] args)
        {
            string uri = args.Length > 0 ? args[0] : "tcp://127.0.0.1:1883";
            string sn = args.Length > 1 ? args[1] : "V2023A7B762";
            int seconds = args.Length > 2 ? int.Parse(args[2]) : 6;

            try
            {
                Nclink.LogInit();
                Nclink.Init(uri);
                Console.WriteLine("connected: {0} (nclink {1})", uri, Nclink.Version);

                using (NclDeviceClient device = Nclink.GetDevice(sn))
                {
                    using (NclModel model = device.Probe())
                    {
                        Console.WriteLine("probe: model root id={0} name={1}",
                                          model.Root.Id, model.Root.Name);
                        Console.WriteLine("各轴的功率与振动（路径 含义）:");
                        foreach (NclNode device_node in model.Root.Devices)
                        {
                            PrintAxisQuantities(device_node);
                        }

                        Console.WriteLine("GET /STATUS = {0}", device.GetLong("/STATUS"));
                        try
                        {
                            device.SetValue("/STATUS", "42");
                            Console.WriteLine("SET /STATUS = 42 ok");
                        }
                        catch (NclinkException error)
                        {
                            /* 对端模型里 /STATUS 不可写时会被拒绝（NG），不是错误 */
                            Console.WriteLine("SET /STATUS 被拒绝: {0}", error.CodeName);
                        }
                    }

                    device.SampleReceived += OnSample;
                    device.EventReceived += OnEvent;
                    device.SubscribeSamples();
                    device.SubscribeEvents();
                    Console.WriteLine("subscribed: Sample/{0}/# and Event/{0}", sn);

                    for (int i = 0; i < seconds; i++)
                    {
                        Thread.Sleep(1000);
                    }
                    Console.WriteLine("received {0} samples, {1} events",
                                      device.SampleCount, device.EventCount);
                    if (device.LastCallbackError != null)
                    {
                        Console.WriteLine("callback error: {0}",
                                          device.LastCallbackError.Message);
                    }
                    /* 退订交给 Dispose（它在 using 结束时跑；退订失败也不该炸，
                       比如对面 broker 不回 UNSUBACK 时）*/
                }
                Nclink.Shutdown();
                Nclink.LogShutdown();
                return 0;
            }
            catch (NclinkException ex)
            {
                Console.WriteLine("NC-Link error: {0}", ex.Message);
                return 1;
            }
        }

        /// <summary>把每根轴下的 POWER / ACCELERATION 数据项打出来（含义按"轴 + 物理量"）。</summary>
        private static void PrintAxisQuantities(NclNode node)
        {
            if (node.Kind == NclNodeKind.Component && node.TypeName == "AXIS")
            {
                string axis = string.IsNullOrEmpty(node.Name) ? "轴" : node.Name;
                foreach (NclNode item in node.DataItems)
                {
                    if (item.TypeName == "POWER")
                    {
                        Console.WriteLine("{0,-24} {1}功率", item.Path, axis);
                    }
                    else if (item.TypeName == "ACCELERATION")
                    {
                        Console.WriteLine("{0,-24} {1}加速度", item.Path, axis);
                    }
                }
            }
            foreach (NclNode child in node.Devices)
            {
                PrintAxisQuantities(child);
            }
            foreach (NclNode child in node.Components)
            {
                PrintAxisQuantities(child);
            }
        }

        private static void OnSample(object sender, NclSampleEventArgs args)
        {
            NclSample sample = args.Sample;

            Console.WriteLine("sample {0}: id={1} interval={2}ms upload={3}ms " +
                              "columns={4} rows={5}",
                              sample.Topic, sample.Id, sample.IntervalMs,
                              sample.UploadIntervalMs, sample.Columns.Count,
                              sample.Rows);

            foreach (NclSampleColumn column in sample.Columns)
            {
                int slots = column.Slots;
                Console.WriteLine("  {0}: {1} 个槽位 × 每槽约 {2} 点 = {3} 点{4}",
                                  column.Path, slots,
                                  slots > 0 ? column.Points / slots : 0,
                                  column.Points, column.IsNested ? "（批量）" : "");
            }

            // 按行消费：行数取数据最多的那一列；1 ms 的列在 0.25 ms 的 4 行里读到同一个点。
            int shown = Math.Min(sample.Rows, 8);
            for (int row = 0; row < shown; row++)
            {
                List<string> cells = new List<string>();
                for (int col = 0; col < sample.Columns.Count; col++)
                {
                    cells.Add(sample.Columns[col].Path + "=" + sample.GetString(row, col));
                }
                Console.WriteLine("  行[{0}] {1}", row, string.Join("  ", cells.ToArray()));
            }
            if (sample.Rows > shown)
            {
                Console.WriteLine("  ...（共 {0} 行，这里只打前 {1} 行）", sample.Rows,
                                  shown);
            }
        }

        private static void OnEvent(object sender, NclEventArgs args)
        {
            Console.WriteLine("event {0}: key={1} value={2}", args.Event.Topic,
                              args.Event.Key, args.Event.Value);
        }
    }
}
