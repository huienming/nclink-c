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
            Files();
            Broker();
            BrokerTls();

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
                Check("采样快照列路径", sample.Columns[0].Path == "/MACHINE/STATUS");
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
        /// 文件通道端到端：上传 / 列目录 / 下载 / 建目录 / 删；再走一遍"带文件参数
        /// 的方法调用"（参数里的文件传上去、返回值里的文件取回来）。
        /// </summary>
        private static void FileChannel(NclServer device, NclDeviceClient client)
        {
            string dir = Path.Combine(Path.GetTempPath(),
                                      "nclink-e2e-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(dir);
            string local = Path.Combine(dir, "report.txt");
            string content = "文件通道 e2e " + Guid.NewGuid().ToString("N") + "\n";
            File.WriteAllText(local, content, new UTF8Encoding(false));
            long contentBytes = new FileInfo(local).Length;
            string deviceRoot = Path.Combine(Nclink.RootDirectory, "uploadFile");
            string onDevice = Path.Combine(deviceRoot, "data", "report.txt");
            long seen = -1;
            string[] givePath = new string[1];

            try
            {
                // 设备端挂文件工具；"sink" 用来试带文件参数的方法调用
                device.RegisterFileTool();
                device.RegisterTool("sink", new string[] { "take", "give" }, null,
                    delegate(string method, NclJson parameters)
                    {
                        if (method == "take")
                        {
                            string token = parameters.Get("blob").AsString();
                            string path = Path.Combine(deviceRoot,
                                token.TrimStart('/').Replace('/',
                                    Path.DirectorySeparatorChar));
                            seen = File.Exists(path) ? new FileInfo(path).Length : -1;
                            return seen;
                        }
                        return "{\"copy\":{\"@file\":\""
                               + givePath[0].Replace("\\", "\\\\") + "\"}}";
                    });

                client.UploadLocalFile(local, "/data/report.txt");
                Check("文件通道：上传后通道是开着的（便利方法按需握过手）",
                      client.FileChannelIsOpen);
                Check("文件通道：上传后设备侧有文件", File.Exists(onDevice));
                Check("文件通道：设备侧字节一致（" + contentBytes + " 字节）",
                      File.Exists(onDevice) && new FileInfo(onDevice).Length == contentBytes
                      && File.ReadAllText(onDevice) == content);

                IList<NclFileInfo> listed = client.ListFiles("/data");
                bool found = false;
                foreach (NclFileInfo entry in listed)
                {
                    if (entry.FileName == "report.txt")
                    {
                        found = !entry.IsDirectory
                                && entry.FileSize == contentBytes;
                    }
                }
                Check("文件通道：列目录看到它（" + listed.Count + " 项）", found);

                string back = Path.Combine(dir, "back.txt");
                client.DownloadTo("/data/report.txt", back);
                Check("文件通道：下载回来的字节一致",
                      File.Exists(back) && File.ReadAllText(back) == content);

                client.MakeDirectory("/docs");
                bool hasDocs = false;
                foreach (NclFileInfo entry in client.ListFiles("/"))
                {
                    hasDocs = hasDocs || (entry.FileName == "docs" && entry.IsDirectory);
                }
                Check("文件通道：建目录", hasDocs);

                // 带文件参数的方法调用：参数里的文件传上去
                using (NclJson reply = client.MethodCallFile(
                           "sink/take", "{\"blob\":\"\"}", new string[] { "blob" },
                           new string[] { local }))
                {
                    Check("文件通道：方法参数带文件（设备收到 "
                          + seen + " 字节）", seen == contentBytes);
                    Check("文件通道：这次的应答正常",
                          reply != null && reply.Get("code").AsString() == "OK");
                }

                // 返回值里的文件取回来（工具返回 {"@file": ...} 标记）
                givePath[0] = onDevice;
                using (NclJson reply = client.MethodCallFile("sink/give", "{}", null, null))
                {
                    NclJson copy = reply == null ? null : reply.Get("data").Get("copy");
                    string got = copy == null ? null : copy.AsString();
                    Check("文件通道：返回值里的文件已下载到本地", got != null
                          && File.Exists(got) && File.ReadAllText(got) == content);
                    NclJson keys = reply == null ? null : reply.Get("data").Get("fileKeys");
                    Check("文件通道：应答里有 fileKeys",
                          keys != null && keys[0].AsString() == "copy");
                }

                // 托管侧自己写的文件（路径不在文件通道的暂存目录里）也要能取回来
                string own = Path.Combine(dir, "own.txt");
                File.WriteAllText(own, "own file " + content, new UTF8Encoding(false));
                givePath[0] = own;
                using (NclJson reply = client.MethodCallFile("sink/give", "{}", null, null))
                {
                    string got = reply == null
                                     ? null
                                     : reply.Get("data").Get("copy").AsString();
                    Check("文件通道：返回值里的文件（本地任意路径）也下得回来",
                          got != null && File.Exists(got)
                          && File.ReadAllText(got) == File.ReadAllText(own));
                }

                client.DeleteRemoteFile("/data/report.txt");
                bool stillThere = false;
                foreach (NclFileInfo entry in client.ListFiles("/data"))
                {
                    stillThere = stillThere || entry.FileName == "report.txt";
                }
                Check("文件通道：删掉之后列不到了", !stillThere);

                client.CloseFileChannel();
                Check("文件通道：close 之后通道关掉了", !client.FileChannelIsOpen);
                client.CloseFileChannel();       // 幂等
                Check("文件通道：重复 close 不抛异常", true);
            }
            catch (NclinkException error)
            {
                Check("文件通道用例（" + error.Operation + ": " + error.Message + "）",
                      false);
            }
            finally
            {
                Directory.Delete(dir, true);
            }
        }

        /// <summary>文件小工具（离线：本地文件，不需要 broker 与设备）。</summary>
        private static void Files()
        {
            // 文件端点的参数化：换端口、换根目录都要真的在听
            int port = 24123;
            Nclink.StopFileServer();
            try
            {
                Nclink.StartFileServer(port, Nclink.RootDirectory);
                using (System.Net.Sockets.TcpClient probe = new System.Net.Sockets.TcpClient())
                {
                    probe.Connect("127.0.0.1", port);
                    probe.ReceiveTimeout = 3000;
                    byte[] buffer = new byte[64];
                    int got = probe.GetStream().Read(buffer, 0, buffer.Length);
                    string greeting = Encoding.ASCII.GetString(buffer, 0, got).Trim();
                    Check("文件端点：自定义端口在听（" + port + "：" + greeting + "）",
                          greeting.StartsWith("220"));
                }
            }
            catch (Exception error)
            {
                Check("文件端点：自定义端口用例（" + error.Message + "）", false);
            }
            finally
            {
                Nclink.StopFileServer();
                try
                {
                    Nclink.StartFileServer();       // 恢复默认（2323）
                }
                catch (NclinkException)
                {
                    /* 2323 被占就不管了：真 broker 那一段会自己判断 */
                }
            }

            string dir = Path.Combine(Path.GetTempPath(),
                                      "nclink-selftest-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(dir);
            string path = Path.Combine(dir, "hello.txt");
            string content = "hello nclink 文件通道\n";
            File.WriteAllText(path, content, new UTF8Encoding(false));
            try
            {
                Check("文件工具：文本类要压缩",
                      Nclink.NeedCompression("a.txt")
                      && Nclink.NeedCompression("model.json"));
                Check("文件工具：二进制类不压缩", !Nclink.NeedCompression("a.bin"));
                Check("文件工具：分片数（256 KB 一片）",
                      Nclink.TotalChunks(0) == 0 && Nclink.TotalChunks(1) == 1
                      && Nclink.TotalChunks(256 * 1024) == 1
                      && Nclink.TotalChunks(256 * 1024 + 1) == 2);

                byte[] bytes = Encoding.UTF8.GetBytes(content);
                string expected;
                using (System.Security.Cryptography.SHA256 sha =
                           System.Security.Cryptography.SHA256.Create())
                {
                    StringBuilder hex = new StringBuilder(64);
                    foreach (byte b in sha.ComputeHash(bytes))
                    {
                        hex.Append(b.ToString("x2"));
                    }
                    expected = hex.ToString();
                }
                Check("文件工具：SHA-256 与 .NET 算的一致",
                      Nclink.FileChecksum(path) == expected);

                NclFileInfo info = Nclink.FileAttribute(path, dir);
                Check("文件工具：属性（名字/大小/片数/不是目录）",
                      info != null && info.FileName == "hello.txt"
                      && info.FileSize == bytes.Length && info.TotalChunks == 1
                      && !info.IsDirectory && info.Compressed);
                NclFileInfo folder = Nclink.FileAttribute(dir);
                Check("文件工具：目录属性（fileType=1）",
                      folder != null && folder.IsDirectory && folder.FileSize == 0);
                Check("文件工具：不存在的路径返回 null",
                      Nclink.FileAttribute(Path.Combine(dir, "nope.bin")) == null);
            }
            finally
            {
                Directory.Delete(dir, true);
            }
        }

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
                // 文件通道要本机当 FTP 服务端（端口 2323）：被别的程序占着就跳过这一段，
                // 别把整个自检拖垮（StartFileServer 起不来会抛）。
                bool fileServer = true;
                try
                {
                    Nclink.StartFileServer();
                }
                catch (NclinkException error)
                {
                    fileServer = false;
                    Console.WriteLine("跳过文件通道（FTP 2323 起不来: "
                                      + error.CodeName + "）");
                }
                device.RegisterTool(
                    "plc",
                    new string[] { "getStatus", "getCount", "setCount" },
                    new NclToolBinding[]
                    {
                        new NclToolBinding("/MACHINE/STATUS", NclOperation.GetValue,
                                           "getStatus"),
                        new NclToolBinding("/MACHINE/PART_COUNT", NclOperation.GetValue,
                                           "getCount"),
                        new NclToolBinding("/MACHINE/PART_COUNT", NclOperation.SetValue,
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

                // 顺手过一遍带 TLS 选项的 init：给了选项也要能连普通的 tcp://
                Nclink.Init(broker, null, null, new NclTlsOptions());
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

                        using (NclJson value = client.GetValue("/MACHINE/STATUS"))
                        {
                            Check("真 broker：路径绑定取值", value.AsLong() == 1);
                        }

                        client.SetValue("/MACHINE/PART_COUNT", "7");
                        Check("真 broker：路径绑定写值到达处理函数", state[1] == 7);
                        using (NclJson value = client.GetValue("/MACHINE/PART_COUNT"))
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
                        Check("带 TLS 选项的 init 已连上（open_ex 走通；"
                              + "TlsAvailable=" + Nclink.TlsAvailable + "）",
                              Nclink.IsOpen);

                        /* ---- 文件通道：MQTT 只传令牌，字节走 FTP ---- */
                        if (fileServer)
                        {
                            device.SetFilePeer("127.0.0.1", 2323);   // 显式指定对端
                            FileChannel(device, client);
                        }
                    }
                }
                finally
                {
                    Nclink.Shutdown();
                }
            }
        }

        /// <summary>
        /// 对**真 TLS broker** 的端到端：设备端与客户端都走 <c>ssl://</c>。
        ///
        /// 设 <c>NCLINK_TEST_TLS_BROKER=ssl://host:port</c> 与
        /// <c>NCLINK_TEST_TLS_CA=&lt;pem&gt;</c> 才跑；垫片要带 TLS 编
        /// （<c>build-shim.ps1 -Tls</c>）。与 Java / Python 的同类用例同形。
        /// </summary>
        private static void BrokerTls()
        {
            string broker = Environment.GetEnvironmentVariable("NCLINK_TEST_TLS_BROKER");
            string ca = Environment.GetEnvironmentVariable("NCLINK_TEST_TLS_CA");
            if (string.IsNullOrEmpty(broker) || string.IsNullOrEmpty(ca))
            {
                Console.WriteLine("跳过（设置 NCLINK_TEST_TLS_BROKER=ssl://host:port 与 "
                                  + "NCLINK_TEST_TLS_CA=<pem> 才跑 TLS 端到端）");
                return;
            }
            if (!Nclink.TlsAvailable)
            {
                Console.WriteLine("跳过 TLS 用例（这个 nclink_shim 没带 TLS："
                                  + "build-shim.ps1 -Tls）");
                return;
            }

            const string sn = "V2CSTLSE2E1";
            string model =
                "{\"name\":\"C# TLS E2E 机床\",\"id\":\"01\",\"type\":\"NC_LINK_ROOT\"," +
                "\"devices\":[{\"id\":\"02\",\"type\":\"MACHINE\",\"name\":\"模拟机床\"," +
                "\"dataItems\":[{\"name\":\"状态\",\"id\":\"030001\"," +
                "\"type\":\"STATUS\"}]}]}";
            NclTlsOptions tls = new NclTlsOptions
            {
                CaFile = ca,
                ServerName = "127.0.0.1"
            };

            using (NclServer device = new NclServer(sn, model, broker, null, null, null, tls))
            {
                device.RegisterTool(
                    "plc",
                    new string[] { "getStatus" },
                    null,
                    delegate(string method, NclJson parameters) { return 42; });
                device.Subscribe();

                Nclink.Init(broker, null, null, tls);
                try
                {
                    using (NclDeviceClient client = Nclink.GetDevice(sn))
                    using (NclJson reply = client.MethodCall("/plc/getStatus"))
                    {
                        Check("真 TLS broker：设备端与客户端都过 ssl://（code="
                              + reply.Get("code").AsString() + "）",
                              reply.Get("code").AsString() == "OK"
                              && reply.Get("data").AsLong() == 42);
                    }
                }
                finally
                {
                    Nclink.Shutdown();
                }
            }

            // 不给 CA：自签证书链必须被校验挡下来（同一台 broker、同一个地址）。
            bool rejected = false;
            try
            {
                Nclink.Init(broker, null, null,
                            new NclTlsOptions { ServerName = "127.0.0.1" });
            }
            catch (NclinkException)
            {
                rejected = true;
            }
            finally
            {
                try
                {
                    Nclink.Shutdown();
                }
                catch (NclinkException)
                {
                    // 没连上时本来就没有东西要收
                }
            }
            Check("真 TLS broker：不给 CA 时握手被拒", rejected);
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
                    new NclToolBinding("/MACHINE/STATUS", NclOperation.GetValue, "getStatus"),
                    new NclToolBinding("/MACHINE/PART_COUNT", NclOperation.GetValue, "getCount"),
                    new NclToolBinding("/MACHINE/WARNING", NclOperation.GetValue, "nothing")
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
