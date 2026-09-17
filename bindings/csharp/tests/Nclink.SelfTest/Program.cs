// SPDX-License-Identifier: MIT
// Copyright (c) 2026 huienming

// C# 绑定的自检（不需要 broker）：
//
//   dotnet run --project bindings/csharp/tests/Nclink.SelfTest -c Release
//
// 对着真 broker 的端到端跑法见 bindings/csharp/README.md（设备端示例 + 客户端示例）。

using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Text;
using Nclink;

namespace Nclink.SelfTest
{
    internal static class Program
    {
        private const string Sn = "V2CSTEST001";
        private const string SampleTopic = "Sample/V203243111F/s1";
        private const string SamplePayload =
            "{\"@id\":\"m1\",\"paths\":[\"/STATUS\"],\"id\":\"s1\"," +
            "\"beginTime\":\"1700000000000\",\"data\":[{\"data\":[1,2]}]," +
            "\"interval\":1000,\"uploadInterval\":2000}";
        private const string EventTopic = "Event/V203243111F";
        private const string EventPayload =
            "{\"@id\":\"m1\",\"id\":\"e1\",\"time\":\"1700000000000\"," +
            "\"event\":{\"key\":\"PART_COUNT\",\"value\":7}}";

        /* 处理函数也可以直接回 NclJson（借用，自检进程活多久它就活多久）。 */
        private static readonly NclJson CountReply = NclJson.Parse("{\"n\":7}");

        private static int _checks;
        private static int _failures;

        private static int Main()
        {
            Nclink.LogInit();
            Nclink.SetConsoleLog(false);        // 自检输出干净：日志只落盘

            Version();
            Json();
            Model();
            Messages();
            Server();
            Http();
            Broker();

            Console.WriteLine();
            Console.WriteLine("{0} 项检查，{1} 失败", _checks, _failures);

            Nclink.LogShutdown();
            return _failures == 0 ? 0 : 1;
        }

        /* ------------------------------------------------------------ 用例 -- */

        private static void Version()
        {
            string version = Nclink.Version;
            Check("version 是 3.x（" + version + "）",
                  version != null && version.StartsWith("3."));
        }

        private static void Json()
        {
            using (NclJson value = NclJson.Parse("{\"a\":1}"))
            {
                Check("Json.Encode 往返", value.Encode() == "{\"a\":1}");
                Check("Json 类型", value.Type == NclJsonType.Object);
                Check("Json 成员个数", value.Count == 1);
            }

            NclJson disposable = NclJson.Parse("{\"a\":1}");
            disposable.Dispose();
            disposable.Dispose();                       // 幂等
            Check("Json.Dispose 幂等", true);
            try
            {
                disposable.Encode();
                Check("关闭后再用要抛异常", false);
            }
            catch (ObjectDisposedException)
            {
                Check("关闭后再用要抛异常", true);
            }

            try
            {
                NclJson.Parse("{oops");
                Check("非法 JSON 要抛异常", false);
            }
            catch (NclinkException error)
            {
                Check("非法 JSON 要抛异常（" + error.CodeName + "）",
                      error.Operation == "ParseJson");
            }

            using (NclJson chinese = NclJson.Parse("{\"s\":\"机床模型文件\"}"))
            {
                Check("中文 JSON 往返", chinese.Get("s").AsString() == "机床模型文件");
            }

            using (NclJson all = NclJson.Parse(
                       "{\"s\":\"x\",\"i\":42,\"d\":1.5,\"b\":true,\"n\":null,\"a\":[1,2]}"))
            {
                Check("AsString", all.Get("s").AsString() == "x");
                Check("AsLong", all.Get("i").AsLong() == 42);
                Check("AsDouble", all.Get("d").AsDouble() == 1.5);
                Check("AsBool", all.Get("b").AsBool());
                Check("IsNull", all.Get("n").IsNull);
                Check("数组长度", all.Get("a").Count == 2);
                Check("成员名按顺序", all.KeyAt(0) == "s" && all.KeyAt(5) == "a");
                Check("取不存在的成员得 null", all.Get("nope") == null);
                NclJson view = all.Get("a");            // 借用视图
                view.Dispose();                         // 空操作
                Check("借用视图 Dispose 后仍可用", view.Count == 2);
            }
        }

        private static void Model()
        {
            using (NclModel model = NclModel.Parse(null))    // 内置默认模型
            {
                Check("内置模型根节点 id=01", model.Root.Id == "01");
                Check("内置模型根节点类型", model.Root.Kind == NclNodeKind.Root);
                Check("内置模型能序列化回去", model.ToJson().Contains("NC_LINK_ROOT"));
                // find 是深度优先、且**不含节点自身**，所以从树里挑一个子节点的 id 来查
                NclNode child = FirstWithId(model.Root);
                Check("按 id 查节点", child != null && model.FindById(child.Id) != null);
                Check("查不存在的 id 得 null", model.FindById("zzz") == null);
            }
            try
            {
                NclModel.Parse("{oops");
                Check("非法模型要抛异常", false);
            }
            catch (NclinkException)
            {
                Check("非法模型要抛异常", true);
            }
        }

        private static void Messages()
        {
            using (NclMessage message = Nclink.Parse(SampleTopic, SamplePayload))
            {
                Check("采样报文类型", message.Type == NclMessageType.Sample);
                NclSample sample = message.AsSample();
                Check("采样快照 id", sample != null && sample.Id == "s1");
                Check("采样快照 interval", sample.IntervalMs == 1000);
                Check("采样快照 uploadInterval", sample.UploadIntervalMs == 2000);
                Check("采样快照列数", sample.Columns.Count == 1);
                Check("采样快照列路径", sample.Columns[0].Path == "/STATUS");
                Check("采样快照点数", sample.Columns[0].Points == 2);
                Check("采样快照按行取值", sample.GetLong(1, 0) == 2);
                Check("采样报文也能拿 JSON", message.Json.Contains("\"paths\""));
                Check("非采样报文 AsEvent 得 null", message.AsEvent() == null);
            }

            using (NclMessage message = Nclink.Parse(EventTopic, EventPayload))
            {
                Check("事件报文类型", message.Type == NclMessageType.Event);
                NclEvent item = message.AsEvent();
                Check("事件快照 id", item != null && item.Id == "e1");
                Check("事件快照 key", item.Key == "PART_COUNT");
                Check("事件快照 value", Convert.ToInt64(item.Value) == 7);
                Check("非事件报文 AsSample 得 null", message.AsSample() == null);
            }

            try
            {
                Nclink.Parse(SampleTopic, "{oops");
                Check("坏报文要抛异常", false);
            }
            catch (NclinkException)
            {
                Check("坏报文要抛异常", true);
            }
        }

        private static void Server()
        {
            try
            {
                new NclServer("");
                Check("空 SN 要抛异常", false);
            }
            catch (ArgumentNullException)
            {
                Check("空 SN 要抛异常", true);
            }

            using (NclServer device = Device())
            {
                Check("Server.Sn", device.Sn == Sn);
                Check("Server 默认模型（内置）", device.Model.Root.Id == "01");
                Check("Server 模型 JSON", device.ModelJson().Contains("NC_LINK_ROOT"));
                Check("工具计数", device.OperationCount == 4);
                Check("路径绑定计数", device.BindingCount >= 2);
                Check("初始没有采样通道", device.SampleCount == 0);

                // 离线驱动一条 Query 请求（路径 绑定 -> 工具方法）
                using (NclJson reply = device.Dispatch(
                           "Query/Request/" + Sn,
                           "{\"@id\":\"q1\",\"ids\":[{\"id\":\"/STATUS\"," +
                           "\"params\":{\"operation\":\"get_value\"}}]}"))
                {
                    NclJson value = reply.Get("values")[0];
                    Check("路径绑定应答 code=OK", value.Get("code").AsString() == "OK");
                    Check("路径绑定应答的值", value.Get("values")[0].AsLong() == 1);
                }

                // 没绑定的路径按 NG 应答
                using (NclJson reply = device.Dispatch(
                           "Query/Request/" + Sn,
                           "{\"@id\":\"q2\",\"ids\":[{\"id\":\"/NOPE\"," +
                           "\"params\":{\"operation\":\"get_value\"}}]}"))
                {
                    Check("没绑定的路径应答 NG",
                          reply.Get("values")[0].Get("code").AsString() != "OK");
                }

                using (NclJson reply = device.InvokeMethodCall("getCount"))
                {
                    Check("InvokeMethodCall code=OK",
                          reply.Get("code").AsString() == "OK");
                    Check("InvokeMethodCall data",
                          reply.Get("data").Get("n").AsLong() == 7);
                }

                // 没有 schema 的方法只接受空参数（库的规则）；check 只校验、不执行
                using (NclJson reply = device.CheckMethodCall("getCount", "{\"a\":1}"))
                {
                    Check("无 schema 的方法收参数要 NG",
                          reply.Get("code").AsString() == "NG"
                          && !string.IsNullOrEmpty(reply.Get("reason").AsString()));
                }
                using (NclJson reply = device.CheckMethodCall("getCount"))
                {
                    Check("check 空参数 OK", reply.Get("code").AsString() == "OK");
                }

                Check("openapi 文档含方法路径",
                      device.OpenapiJson("http://localhost:9008/api")
                            .Contains("/plc/getCount"));

                // 采样通道是模型里的一个 CONFIG 节点
                device.AddSample("{\"name\":\"测试通道\",\"id\":\"ch1\"," +
                                 "\"type\":\"SAMPLE_CHANNEL\",\"sampleInterval\":1000," +
                                 "\"uploadInterval\":1000,\"ids\":[{\"id\":\"/STATUS\"}]}");
                Check("加采样通道", device.SampleCount == 1);
                device.RemoveSample("ch1");
                Check("删采样通道", device.SampleCount == 0);
                device.StopAllSamples();
                Check("停所有采样通道", true);
            }

            // 处理函数抛异常 -> 该次调用 NG，异常文本进 reason，异常记在设备端上
            using (NclServer device = Device())
            {
                using (NclJson reply = device.InvokeMethodCall("boom"))
                {
                    Check("处理函数抛异常 -> NG",
                          reply.Get("code").AsString() == "NG");
                    Check("异常文本进 reason",
                          reply.Get("reason").AsString().Contains("坏掉了"));
                }
                Check("异常记在 LastCallbackError",
                      device.LastCallbackError is InvalidOperationException);
            }

            // 处理函数返回 null -> 成功但没有值，库按 NG 应答
            using (NclServer device = Device())
            {
                using (NclJson reply = device.InvokeMethodCall("nothing"))
                {
                    // methodCall 路径：code=OK 但没有 data
                    Check("返回 null 的调用不产生 data",
                          reply.Get("code").AsString() == "OK"
                          && reply.Get("data") == null);
                }
                // 路径绑定路径：这条取值按 NG 应答（和 C API 一致）
                using (NclJson reply = device.Dispatch(
                           "Query/Request/" + Sn,
                           "{\"@id\":\"q3\",\"ids\":[{\"id\":\"/WARNING\"," +
                           "\"params\":{\"operation\":\"get_value\"}}]}"))
                {
                    Check("返回 null 的路径取值按 NG 应答",
                          reply.Get("values")[0].Get("code").AsString() == "NG");
                }
            }

            // 缺处理函数要拒绝注册
            using (NclServer device = new NclServer(Sn))
            {
                try
                {
                    device.RegisterTool("plc", new string[] { "getValue" }, null, null);
                    Check("缺处理函数要抛异常", false);
                }
                catch (InvalidOperationException)
                {
                    Check("缺处理函数要抛异常", true);
                }
            }

            // 自研传输：事件与采样应答都交给托管侧
            List<KeyValuePair<string, string>> published =
                new List<KeyValuePair<string, string>>();
            using (NclServer device = new NclServer(
                       Sn, null, null, null, null,
                       (topic, payload) => published.Add(
                           new KeyValuePair<string, string>(
                               topic, Encoding.UTF8.GetString(payload)))))
            {
                device.PushEvent("010307", "{\"key\":\"PART_COUNT\",\"value\":7}");
                Check("事件计数", device.EventCount == 1);
                Check("事件进了自研传输", published.Count == 1);
                if (published.Count == 1)
                {
                    Check("事件主题", published[0].Key == "Event/" + Sn);
                    using (NclJson eventJson = NclJson.Parse(published[0].Value))
                    {
                        Check("事件报文体 id", eventJson.Get("id").AsString() == "010307");
                        Check("事件报文体 value",
                              eventJson.Get("event").Get("value").AsLong() == 7);
                    }
                }
            }

            // 关闭：可重复调用，关闭后再用要抛异常
            NclServer closing = Device();
            closing.Dispose();
            closing.Dispose();
            Check("Server.Dispose 幂等", true);
            try
            {
                closing.ModelJson();
                Check("关闭后再用要抛异常", false);
            }
            catch (NclinkException error)
            {
                Check("关闭后再用要抛异常（" + error.CodeName + "）",
                      error.Code == -13);
            }
        }

        private static void Http()
        {
            using (NclServer device = Device())
            using (NclHttpEndpoint http = device.StartHttp(0, true))
            {
                Check("HTTP 端点端口（0 = 随机）", http.Port > 0);
                Check("HTTP 端点 URL", http.Url == "http://localhost:" + http.Port);

                int status;
                string body = Http(http, "GET", "/api/schema", null, out status);
                Check("GET /api/schema 是 200", status == 200);
                using (NclJson document = NclJson.Parse(body))
                {
                    Check("OpenAPI 版本", document.Get("openapi").AsString() == "3.0.0");
                }
                Check("OpenAPI 里有工具方法", body.Contains("/plc/getCount"));

                body = Http(http, "POST", "/api/plc/getCount", "{}", out status);
                Check("POST /api/<工具>/<方法> 是 200", status == 200);
                using (NclJson reply = NclJson.Parse(body))
                {
                    Check("REST 调用等价于 methodCall（走 Result 信封）",
                          reply.Get("status").AsBool()
                          && reply.Get("data").Get("n").AsLong() == 7);
                }

                // 配置端点：没装模型文件时是 NG，但端点必须在
                body = Http(http, "GET", "/api/cfg/getModel", null, out status);
                Check("GET /api/cfg/getModel 是 200", status == 200);
                using (NclJson reply = NclJson.Parse(body))
                {
                    Check("配置端点有 status 字段", reply.Get("status") != null);
                }

                // Swagger UI
                body = Http(http, "GET", "/swagger-ui", null, out status);
                Check("GET /swagger-ui 是 200", status == 200);

                Check("请求计数 > 0", http.RequestCount >= 4);
            }

            using (NclServer device = Device())
            using (NclHttpEndpoint http = device.StartHttp(0, false))
            {
                http.Route("GET", "/hello", request => NclHttpReply.Json(
                    "{\"path\":\"" + request.Path + "\",\"query\":\""
                    + (request.Query ?? string.Empty) + "\"}"));
                http.Route("POST", "/echo", request => NclHttpReply.Of(
                    201, "text/plain; charset=utf-8", "echo:" + request.Body));
                http.Route("GET", "/none", request => null);
                http.Route("GET", "/boom", request =>
                {
                    throw new InvalidOperationException("炸了");
                });
                http.Route("GET", "/give-up", request => NclHttpReply.WithStatus(204));
                Check("自定义路由计数", http.RouteCount == 5);
                try
                {
                    http.Route("GET", "/hello", request => null);
                    Check("同一条路由挂两次要抛异常", false);
                }
                catch (ArgumentException)
                {
                    Check("同一条路由挂两次要抛异常", true);
                }

                int status;
                string body = Http(http, "GET", "/hello?x=1", null, out status);
                Check("自定义 GET 路由是 200", status == 200);
                Check("自定义路由能拿到 query",
                      body.Contains("\"query\":\"x=1\"") && body.Contains("/hello"));

                body = Http(http, "POST", "/echo", "hi", out status);
                Check("自定义路由能给状态码与内容类型",
                      status == 201 && body == "echo:hi");

                body = Http(http, "GET", "/none", null, out status);
                Check("处理函数返回 null -> 404", status == 404);

                body = Http(http, "GET", "/give-up", null, out status);
                Check("只回状态码的路由", status == 204);

                body = Http(http, "GET", "/boom", null, out status);
                Check("处理函数抛异常 -> 500", status == 500);
                Check("异常文本进报文", body != null && body.Contains("炸了"));
                Check("异常记在 LastCallbackError",
                      device.LastCallbackError is InvalidOperationException);

                body = Http(http, "GET", "/no-such-path", null, out status);
                Check("没挂过的路径是 404", status == 404);

                http.SetCors(true);
                http.SetCors(false);
                Check("SetCors 可切换", true);

                int port = http.Port;
                http.Dispose();
                http.Dispose();
                Check("HTTP 端点 Dispose 幂等", true);
                try
                {
                    Http(http, "GET", "/hello", null, out status);
                    Check("关了之后端口不再监听", false);
                }
                catch (Exception)
                {
                    Check("关了之后端口不再监听（port=" + port + "）", true);
                }
                try
                {
                    http.Route("GET", "/after-close", request => null);
                    Check("关了之后挂路由要抛异常", false);
                }
                catch (NclinkException)
                {
                    Check("关了之后挂路由要抛异常", true);
                }
            }

            // server.Dispose() 也会收掉没关的 HTTP 端点
            using (NclServer device = Device())
            {
                NclHttpEndpoint http = device.StartHttp(0, false);
                int port = http.Port;
                device.Dispose();
                Check("server.Dispose 收掉 HTTP 端点", http.RequestCount == 0);
                http.Dispose();                          // 再关一次也不该炸
                Check("端点已被服务器收掉，再关也不炸（port=" + port + "）", true);
            }
        }

        /* ------------------------------------------------------------ 夹具 -- */

        /// <summary>
        /// 对**真 broker** 的端到端检查：设了 <c>NCLINK_TEST_BROKER</c> 才跑。
        ///
        /// 设备端与客户端放在同一个进程里，报文真的过一遍 broker。离线用例
        /// （Server/Http 那两节）覆盖不到的"过 MQTT 的那一段"就靠它守住——
        /// 客户端方法调用应答是 JSON **文本**、要解析成 NclJson，就是这里发现的。
        /// </summary>
        private static void Broker()
        {
            string broker = Environment.GetEnvironmentVariable("NCLINK_TEST_BROKER");
            if (string.IsNullOrEmpty(broker))
            {
                Console.WriteLine("跳过（设置 NCLINK_TEST_BROKER=tcp://host:port 才跑真 broker 用例）");
                return;
            }

            const string sn = "V2CSE2E0001";
            string model =
                "{\"name\":\"C# E2E 机床\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\"," +
                "\"devices\":[{\"id\":\"02\",\"type\":\"MACHINE\",\"name\":\"模拟机床\"," +
                "\"configs\":[{\"name\":\"采样通道\",\"id\":\"e2e_channel\"," +
                "\"type\":\"SAMPLE_CHANNEL\",\"sampleInterval\":200," +
                "\"uploadInterval\":200,\"ids\":[{\"id\":\"/STATUS\"}," +
                "{\"id\":\"/PART_COUNT\"}]}]," +
                "\"dataItems\":[{\"name\":\"状态\",\"id\":\"030001\"," +
                "\"type\":\"STATUS\",\"settable\":false}," +
                "{\"name\":\"加工计件\",\"id\":\"030002\",\"type\":\"PART_COUNT\"," +
                "\"settable\":true}]}]}";

            long[] state = new long[2];              // [0]=status [1]=count
            state[0] = 1;
            using (NclServer device = new NclServer(sn, model, broker))
            {
                device.RegisterTool(
                    "plc",
                    new string[] { "getStatus", "getCount", "setCount" },
                    new NclToolBinding[]
                    {
                        new NclToolBinding("/STATUS", NclOperation.GetValue,
                                           "getStatus"),
                        new NclToolBinding("/PART_COUNT", NclOperation.GetValue,
                                           "getCount"),
                        new NclToolBinding("/PART_COUNT", NclOperation.SetValue,
                                           "setCount")
                    },
                    delegate(string method, NclJson parameters)
                    {
                        if (method == "setCount")
                        {
                            state[1] = parameters.Get("value").AsLong();
                            return state[1];
                        }
                        return method == "getCount" ? state[1] : state[0];
                    });
                device.Subscribe();
                device.InitSamples();

                Nclink.Init(broker);
                try
                {
                    using (NclDeviceClient client = Nclink.GetDevice(sn))
                    {
                        using (NclModel probed = client.Probe())
                        {
                            Check("真 broker：probe 到模型", probed.Root.Id == "01");
                            Check("真 broker：模型能按 id 查节点",
                                  probed.FindById("030001") != null);
                        }

                        using (NclJson value = client.GetValue("/STATUS"))
                        {
                            Check("真 broker：路径绑定取值", value.AsLong() == 1);
                        }

                        client.SetValue("/PART_COUNT", "7");
                        Check("真 broker：路径绑定写值到达处理函数", state[1] == 7);
                        using (NclJson value = client.GetValue("/PART_COUNT"))
                        {
                            Check("真 broker：写进去的值读得回来", value.AsLong() == 7);
                        }

                        // 方法调用的应答是 JSON 文本：绑定必须解析成 NclJson
                        using (NclJson reply = client.MethodCall("/plc/getCount"))
                        {
                            Check("真 broker：methodCall 应答解析成 JSON",
                                  reply.Get("code").AsString() == "OK"
                                  && reply.Get("data").AsLong() == 7);
                        }
                        using (NclJson reply = client.MethodCall(
                                   "/plc/setCount", "{\"value\":21}"))
                        {
                            Check("真 broker：methodCall 带参数",
                                  reply.Get("data").AsLong() == 21 && state[1] == 21);
                        }
                        using (NclJson reply = client.MethodCall("/plc/getStatus", null,
                                                                 check: true))
                        {
                            Check("真 broker：methodCall check",
                                  reply.Get("code").AsString() == "OK");
                        }

                        int samples = 0;
                        int events = 0;
                        string samplePaths = null;
                        string eventKey = null;
                        long eventValue = -1;
                        client.SampleReceived += delegate(object sender,
                                                          NclSampleEventArgs args)
                        {
                            samples++;
                            List<string> paths = new List<string>();
                            foreach (NclSampleColumn column in args.Sample.Columns)
                            {
                                paths.Add(column.Path);
                            }
                            samplePaths = string.Join(",", paths.ToArray());
                        };
                        client.EventReceived += delegate(object sender, NclEventArgs args)
                        {
                            events++;
                            eventKey = args.Event.Key;
                            eventValue = Convert.ToInt64(args.Event.Value);
                        };
                        client.SubscribeSamples(2);
                        client.SubscribeEvents(2);
                        device.PushEvent("010307", "{\"key\":\"PART_COUNT\",\"value\":21}");

                        for (int i = 0; i < 80 && (samples == 0 || events == 0); i++)
                        {
                            System.Threading.Thread.Sleep(100);
                        }
                        Check("真 broker：收到采样上报（" + samples + " 条）", samples > 0);
                        Check("真 broker：采样列路径",
                              samplePaths == "/STATUS,/PART_COUNT");
                        Check("真 broker：收到事件推送（" + events + " 条）", events > 0);
                        Check("真 broker：事件内容",
                              eventKey == "PART_COUNT" && eventValue == 21);
                        Check("真 broker：设备端上报计数 > 0",
                              device.SampleUploadCount > 0);
                        Check("真 broker：回调没出错", device.LastCallbackError == null
                                                       && client.LastCallbackError == null);
                    }
                }
                finally
                {
                    Nclink.Shutdown();
                }
            }
        }

        /// <summary>深度优先找一个带 id 的子节点（find 不含节点自身）。</summary>
        private static NclNode FirstWithId(NclNode node)
        {
            foreach (NclNode child in Children(node))
            {
                if (!string.IsNullOrEmpty(child.Id))
                {
                    return child;
                }
                NclNode deeper = FirstWithId(child);
                if (deeper != null)
                {
                    return deeper;
                }
            }
            return null;
        }

        private static IEnumerable<NclNode> Children(NclNode node)
        {
            foreach (NclNode child in node.Devices) yield return child;
            foreach (NclNode child in node.Components) yield return child;
            foreach (NclNode child in node.Configs) yield return child;
            foreach (NclNode child in node.DataItems) yield return child;
        }

        private static NclServer Device()
        {
            NclServer device = new NclServer(Sn);
            device.RegisterTool(
                "plc",
                new Dictionary<string, string>
                {
                    { "getStatus", null },
                    { "getCount", null },
                    { "boom", null },
                    { "nothing", null }
                },
                new NclToolBinding[]
                {
                    new NclToolBinding("/STATUS", NclOperation.GetValue, "getStatus"),
                    new NclToolBinding("/PART_COUNT", NclOperation.GetValue, "getCount"),
                    new NclToolBinding("/WARNING", NclOperation.GetValue, "nothing")
                },
                delegate(string method, NclJson parameters)
                {
                    if (method == "getStatus")
                    {
                        return 1L;
                    }
                    if (method == "boom")
                    {
                        throw new InvalidOperationException("坏掉了");
                    }
                    if (method == "nothing")
                    {
                        return null;                    // 成功但没有值
                    }
                    return CountReply;                  // 也支持回 NclJson
                });
            return device;
        }

        /* ------------------------------------------------------------ 工具 -- */

        private static string Http(NclHttpEndpoint endpoint, string method, string path,
                                   string body, out int status)
        {
            HttpWebRequest request = (HttpWebRequest)WebRequest.Create(
                endpoint.Url + path);
            request.Method = method;
            request.Timeout = 5000;
            if (body != null)
            {
                byte[] data = Encoding.UTF8.GetBytes(body);
                request.ContentType = "application/json";
                request.ContentLength = data.Length;
                using (Stream stream = request.GetRequestStream())
                {
                    stream.Write(data, 0, data.Length);
                }
            }
            try
            {
                using (HttpWebResponse response = (HttpWebResponse)request.GetResponse())
                {
                    status = (int)response.StatusCode;
                    return Read(response);
                }
            }
            catch (WebException error)
            {
                HttpWebResponse response = error.Response as HttpWebResponse;
                if (response == null)
                {
                    throw;
                }
                using (response)
                {
                    status = (int)response.StatusCode;
                    return Read(response);
                }
            }
        }

        private static string Read(HttpWebResponse response)
        {
            using (Stream stream = response.GetResponseStream())
            {
                if (stream == null)
                {
                    return string.Empty;
                }
                using (StreamReader reader = new StreamReader(stream, Encoding.UTF8))
                {
                    return reader.ReadToEnd();
                }
            }
        }

        private static void Check(string name, bool ok)
        {
            _checks++;
            if (ok)
            {
                Console.WriteLine("ok   " + name);
            }
            else
            {
                _failures++;
                Console.WriteLine("FAIL " + name);
            }
        }
    }
}
