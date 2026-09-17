# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""Python 绑定的自检（不需要 broker）。

跑法（仓库根）：

    python -m unittest discover -s bindings/python/tests -v

对着真 broker 的端到端跑法见 bindings/python/README.md（设备端示例 +
examples/client_demo.py）。
"""

import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import nclink  # noqa: E402
from nclink._message import MessageType  # noqa: E402


def _repo_root():
    here = os.path.dirname(os.path.abspath(__file__))      # bindings/python/tests
    return os.path.dirname(os.path.dirname(os.path.dirname(here)))


def _walk(node):
    """深度优先遍历整棵树（子节点顺序：数据项、设备、组件、配置）。"""
    yield node
    for kind in (0, 1, 2, 3):
        for child in node.children(kind):
            for descendant in _walk(child):
                yield descendant


def _fixture_text():
    path = os.path.join(_repo_root(), "tests", "data", "model_nclink.json")
    if not os.path.isfile(path):
        raise unittest.SkipTest("tests/data/model_nclink.json is missing")
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


SAMPLE_TOPIC = "Sample/V203243111F/s1"
SAMPLE_PAYLOAD = ('{"@id":"m1","paths":["/STATUS"],"id":"s1",'
                  '"beginTime":"1700000000000","data":[{"data":[1,2]}],'
                  '"interval":1000,"uploadInterval":2000}')
EVENT_TOPIC = "Event/V203243111F"
EVENT_PAYLOAD = '{"@id":"m1","id":"e1","time":"1700000000000","event":{"key":"PART_COUNT","value":7}}'


class VersionTest(unittest.TestCase):
    def test_version(self):
        self.assertTrue(nclink.version().startswith("3."), nclink.version())


class JsonTest(unittest.TestCase):
    def test_round_trip(self):
        with nclink.Json.parse('{"a":1}') as value:
            self.assertEqual(value.encode(), '{"a":1}')
            self.assertEqual(value.to_python(), {"a": 1})

    def test_close_is_idempotent(self):
        value = nclink.Json.parse("[1,2,3]")
        self.assertEqual(value.to_python(), [1, 2, 3])
        value.close()
        value.close()
        self.assertTrue(value.closed)
        with self.assertRaises(nclink.NclinkError):
            value.to_python()

    def test_parse_error(self):
        with self.assertRaises(nclink.NclinkError) as caught:
            nclink.Json.parse("{oops")
        self.assertTrue(caught.exception.name)
        self.assertEqual(caught.exception.op, "Json.parse")

    def test_types_and_accessors(self):
        with nclink.Json.parse('{"s":"x","i":42,"d":1.5,"b":true,"n":null,"a":[1,2]}') as value:
            self.assertEqual(value.type, nclink.JsonType.OBJECT)
            self.assertTrue(value.is_object)
            self.assertEqual(value.count, 6)
            self.assertEqual(value["s"].as_string(), "x")
            self.assertEqual(value["i"].as_long(), 42)
            self.assertAlmostEqual(value["d"].as_double(), 1.5)
            self.assertTrue(value["b"].as_bool())
            self.assertTrue(value["n"].is_null)
            self.assertEqual(value["a"].count, 2)
            self.assertEqual([item.to_python() for item in value["a"].values()], [1, 2])
            self.assertEqual(value.keys(), ["s", "i", "d", "b", "n", "a"])
            self.assertIsNone(value.get("nope"))
            # 借用视图：宿主关掉之前都有效，不用（也不该）自己关
            view = value["a"]
            view.close()
            self.assertEqual(view.to_python(), [1, 2])

    def test_big_integer_keeps_precision(self):
        with nclink.Json.parse("17000000000001234567") as value:
            self.assertEqual(value.to_python(), 17000000000001234567)

    def test_encode_of_python_objects(self):
        # 这是 set_value / method_call 的参数编排规则
        from nclink.client import _as_json_text

        self.assertEqual(_as_json_text(42), "42")
        self.assertEqual(_as_json_text({"a": [1, 2]}), '{"a":[1,2]}')
        self.assertEqual(_as_json_text("[1,2]"), "[1,2]")          # 文本原样
        with nclink.Json.parse('{"a":1}') as value:
            self.assertEqual(_as_json_text(value), '{"a":1}')


class ModelTest(unittest.TestCase):
    def test_builtin_default_model(self):
        with nclink.Model.parse() as model:
            root = model.root
            self.assertEqual(root.type, nclink.NodeType.ROOT)
            self.assertEqual(root.type_name, "NC_LINK_ROOT")
            self.assertIn("NC_LINK_ROOT", model.to_json())

    def test_parse_fixture(self):
        with nclink.Model.parse(_fixture_text()) as model:
            self.assertIn("NC_LINK_ROOT", model.to_json())
            root = model.root
            self.assertEqual(root.path, "/NC_LINK_ROOT")
            self.assertEqual(root.id, "01")
            # 设备 -> 数据项：遍历拿到的 id，反查要能找回同一个路径
            items = [node for node in _walk(root)
                     if node.type == nclink.NodeType.DATA_ITEM]
            self.assertTrue(items)
            for item in items:
                found = model.find_by_id(item.id)
                self.assertIsNotNone(found, item.id)
                self.assertEqual(found.path, item.path)
            self.assertIsNone(model.find_by_id("does-not-exist"))

    def test_bad_model(self):
        with self.assertRaises(nclink.NclinkError):
            nclink.Model.parse("{not a model}")

    def test_sample_channel_fields(self):
        with nclink.Model.parse(_fixture_text()) as model:
            channels = [node for node in _walk(model.root) if node.is_sample_channel]
            self.assertTrue(channels, "模型里应有采样通道")
            for channel in channels:
                self.assertGreater(channel.sample_interval_ms, 0)
                self.assertEqual(channel.type, nclink.NodeType.CONFIG)
                self.assertTrue(channel.sample_item_paths())


class MessageTest(unittest.TestCase):
    def test_sample(self):
        sample = nclink.parse(SAMPLE_TOPIC, SAMPLE_PAYLOAD)
        self.assertIsInstance(sample, nclink.Sample)
        self.assertEqual(sample.id, "s1")
        self.assertEqual(sample.begin_time, "1700000000000")
        self.assertEqual(sample.interval_ms, 1000)
        self.assertEqual(sample.upload_interval_ms, 2000)
        self.assertEqual(sample.rows, 2)
        self.assertEqual(len(sample.columns), 1)
        self.assertEqual(sample.columns[0].path, "/STATUS")
        self.assertEqual(sample.columns[0].points, 2)
        self.assertEqual([sample.value_at(r, 0) for r in range(sample.rows)], [1, 2])
        self.assertEqual(sample.get_long(0, 0), 1)
        self.assertAlmostEqual(sample.get_double(1, 0), 2.0)
        self.assertEqual(sample.get_string(1, 0), "2")
        self.assertEqual(sample.header(), "/STATUS")
        self.assertIn("paths", sample.raw_json)

    def test_event(self):
        event = nclink.parse(EVENT_TOPIC, EVENT_PAYLOAD)
        self.assertIsInstance(event, nclink.Event)
        self.assertEqual(event.id, "e1")
        self.assertEqual(event.time, "1700000000000")
        self.assertEqual(event.key, "PART_COUNT")
        self.assertEqual(event.value, 7)

    def test_other_message(self):
        message = nclink.parse("Ping/V203243111F", '{"@id":"m1","time":"1700000000000"}')
        self.assertIsInstance(message, nclink.Message)
        self.assertEqual(message.type, MessageType.PING)
        self.assertIn("@id", message.raw_json)

    def test_broken_payload(self):
        with self.assertRaises(nclink.NclinkError):
            nclink.parse(SAMPLE_TOPIC, "not a message")

    def test_payload_may_be_bytes(self):
        sample = nclink.parse(SAMPLE_TOPIC, SAMPLE_PAYLOAD.encode("utf-8"))
        self.assertEqual(sample.id, "s1")


class ConnectionTest(unittest.TestCase):
    """没连 broker 时不该崩，只该报错（连着也不该有副作用）。"""

    def test_get_device_without_connection(self):
        with self.assertRaises(nclink.NclinkError) as caught:
            nclink.get_device("V203243111F")
        self.assertEqual(caught.exception.op, "get_device")

    def test_init_rejects_a_broken_broker_uri(self):
        # 端口 0 是非法的：库立刻报错，不碰网络。
        # （别用"没人监听的端口"来测：某些环境里本机端口会被代理黑洞掉，connect
        #   既不失败也不超时，测试会挂很久。）
        with self.assertRaises(nclink.NclinkError) as caught:
            nclink.init("tcp://127.0.0.1:0")
        self.assertEqual(caught.exception.op, "init")
        self.assertFalse(nclink.is_open())

    def test_empty_broker_uri(self):
        with self.assertRaises(ValueError):
            nclink.init("")


class ServerTest(unittest.TestCase):
    """设备端（不需要 broker）：注册工具、离线驱动请求、采样通道、事件推送。"""

    def _device(self, publish=None):
        device = nclink.Server(sn="V2TEST00001", publish=publish)
        self.addCleanup(device.close)
        device.register_tool(
            "plc",
            methods={"getValue": None, "getCount": None},
            handlers={"getValue": lambda params: 42,
                      "getCount": lambda params: {"n": 7}},
            bindings=[("/STATUS", nclink.Operation.GET_VALUE, "getValue")])
        return device

    def test_create_and_model(self):
        with nclink.Server(sn="V2TEST00001") as device:
            self.assertEqual(device.sn, "V2TEST00001")
            # 不给模型时用库内置的那份，路径与采样才有依据
            self.assertEqual(device.model.root.id, "01")
            self.assertEqual(device.model.root.type_name, "NC_LINK_ROOT")
            self.assertIn("NC_LINK_ROOT", device.model_json())

    def test_empty_sn(self):
        with self.assertRaises(ValueError):
            nclink.Server(sn="")

    def test_register_tool_and_dispatch(self):
        device = self._device()
        self.assertEqual(device.operation_count, 2)
        self.assertGreaterEqual(device.binding_count, 1)

        payload = '{"@id":"q1","ids":[{"id":"/STATUS","params":{"operation":"get_value"}}]}'
        reply = device.dispatch("Query/Request/V2TEST00001", payload)
        self.assertEqual(reply.type, nclink.MessageType.QUERY_RESPONSE)
        body = reply.to_python()
        self.assertEqual(body["@id"], "q1")
        self.assertEqual(body["values"][0]["code"], "OK")
        self.assertEqual(body["values"][0]["values"], [42])

    def test_unbound_path_is_ng(self):
        device = self._device()
        payload = '{"@id":"q1","ids":[{"id":"/NOPE","params":{"operation":"get_value"}}]}'
        body = device.dispatch("Query/Request/V2TEST00001", payload).to_python()
        self.assertNotEqual(body["values"][0]["code"], "OK")

    def test_method_call_and_check(self):
        device = self._device()
        body = device.invoke_method_call("getCount").to_python()
        self.assertEqual(body["code"], "OK")
        self.assertEqual(body["data"], {"n": 7})

        # 没有 schema 的方法只接受空参数（库的规则），check 只校验不执行
        body = device.check_method_call("getCount", {"a": 1}).to_python()
        self.assertEqual(body["code"], "NG")
        self.assertTrue(body["reason"])
        body = device.check_method_call("getValue", None).to_python()
        self.assertEqual(body["code"], "OK")

    def test_schema_checked_method(self):
        device = nclink.Server(sn="V2TEST00001")
        self.addCleanup(device.close)
        device.register_tool(
            "plc",
            methods={"setValue": {"type": "object", "properties": {"value": {"type": "integer"}},
                                  "required": ["value"]}},
            handlers={"setValue": lambda params: params["value"]})
        body = device.check_method_call("setValue", {"value": 42}).to_python()
        self.assertEqual(body["code"], "OK")
        body = device.check_method_call("setValue", {"value": "nope"}).to_python()
        self.assertEqual(body["code"], "NG")
        body = device.invoke_method_call("setValue", {"value": 42}).to_python()
        self.assertEqual(body["data"], 42)

    def test_handler_that_raises_is_ng(self):
        device = nclink.Server(sn="V2TEST00001")
        self.addCleanup(device.close)

        def boom(params):
            raise ValueError("坏掉了")

        device.register_tool("plc", methods={"getValue": None},
                             handlers={"getValue": boom},
                             bindings=[("/STATUS", nclink.Operation.GET_VALUE, "getValue")])
        payload = '{"@id":"q1","ids":[{"id":"/STATUS","params":{"operation":"get_value"}}]}'
        body = device.dispatch("Query/Request/V2TEST00001", payload).to_python()
        self.assertEqual(body["values"][0]["code"], "NG")
        self.assertIn("坏掉了", body["values"][0]["reason"])
        self.assertIsInstance(device.last_callback_error, ValueError)

    def test_handler_returning_none_has_no_value(self):
        device = nclink.Server(sn="V2TEST00001")
        self.addCleanup(device.close)
        device.register_tool("plc", methods={"getValue": None},
                             handlers={"getValue": lambda params: None},
                             bindings=[("/STATUS", nclink.Operation.GET_VALUE, "getValue")])
        payload = '{"@id":"q1","ids":[{"id":"/STATUS","params":{"operation":"get_value"}}]}'
        body = device.dispatch("Query/Request/V2TEST00001", payload).to_python()
        self.assertEqual(body["values"][0]["code"], "NG")

    def test_missing_handler_is_rejected(self):
        device = nclink.Server(sn="V2TEST00001")
        self.addCleanup(device.close)
        with self.assertRaises(ValueError):
            device.register_tool("plc", methods={"getValue": None})

    def test_push_event_goes_to_the_transport(self):
        published = []
        device = self._device(publish=lambda topic, payload: published.append((topic, payload)))
        device.push_event("010307", {"key": "PART_COUNT", "value": 7})
        self.assertEqual(device.event_count, 1)
        topic, payload = published[0]
        self.assertEqual(topic, "Event/V2TEST00001")
        event = nclink.Json.parse(payload).to_python()
        self.assertEqual(event["id"], "010307")
        self.assertEqual(event["event"], {"key": "PART_COUNT", "value": 7})

    def test_add_and_remove_sample_channel(self):
        device = self._device()
        self.assertEqual(device.sample_count, 0)
        # 采样通道是模型里的一个 CONFIG 节点：ids 是各项路径，周期单位毫秒
        device.add_sample({"name": "测试通道", "id": "ch1", "type": "SAMPLE_CHANNEL",
                           "sampleInterval": 1000, "uploadInterval": 1000,
                           "ids": [{"id": "/STATUS"}]})
        self.assertEqual(device.sample_count, 1)
        device.remove_sample("ch1")
        self.assertEqual(device.sample_count, 0)
        device.stop_all_samples()

    def test_openapi_document(self):
        device = self._device()
        document = device.openapi_json("http://localhost:9008/api")
        self.assertIn("/plc/getCount", document)
        self.assertIn("openapi", document)

    def test_close_is_idempotent(self):
        device = self._device()
        device.close()
        device.close()
        with self.assertRaises(nclink.NclinkError):
            device.model


class HttpTest(unittest.TestCase):
    """HTTP / REST 端点（离线也能测：本机回环，不需要 broker）。"""

    def _http(self, endpoint, method, path, body=None, content_type="application/json"):
        request = urllib.request.Request(endpoint.url + path,
                                         data=None if body is None else body.encode("utf-8"),
                                         method=method)
        if body is not None:
            request.add_header("Content-Type", content_type)
        try:
            with urllib.request.urlopen(request, timeout=5) as reply:
                return reply.status, reply.read().decode("utf-8")
        except urllib.error.HTTPError as error:
            return error.code, error.read().decode("utf-8")

    def test_openapi_tool_and_config_endpoints(self):
        device = nclink.Server(sn="V2TEST00001")
        self.addCleanup(device.close)
        device.register_tool("plc", methods={"getCount": None},
                             handlers={"getCount": lambda params: 42})
        endpoint = device.start_http(0, with_config=True)
        self.addCleanup(endpoint.close)
        self.assertGreater(endpoint.port, 0)

        # GET /api/schema：OpenAPI 3.0 文档
        status, body = self._http(endpoint, "GET", "/api/schema")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["openapi"], "3.0.0")
        self.assertIn("/plc/getCount", body)

        # POST /api/<工具>/<方法>：等价于 methodCall（应答走 Result 信封）
        status, body = self._http(endpoint, "POST", "/api/plc/getCount", "{}")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), {"status": True, "data": 42})

        # 配置端点：GET /api/cfg/getModel（没装模型文件时是 NG，但端点必须在）
        status, body = self._http(endpoint, "GET", "/api/cfg/getModel")
        self.assertEqual(status, 200)
        self.assertIn("status", json.loads(body))

    def test_custom_routes(self):
        device = nclink.Server(sn="V2TEST00001")
        self.addCleanup(device.close)
        endpoint = device.start_http(0, with_config=False)
        self.addCleanup(endpoint.close)

        endpoint.route("GET", "/hello",
                       lambda method, path, query, body: {"path": path, "query": query})
        endpoint.route("POST", "/echo",
                       lambda method, path, query, body: (201, "text/plain; charset=utf-8",
                                                          "echo:" + body))
        endpoint.route("GET", "/none", lambda method, path, query, body: None)

        status, body = self._http(endpoint, "GET", "/hello?x=1")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), {"path": "/hello", "query": "x=1"})

        status, body = self._http(endpoint, "POST", "/echo", "hi", "text/plain")
        self.assertEqual((status, body), (201, "echo:hi"))

        status, body = self._http(endpoint, "GET", "/none")
        self.assertEqual((status, body), (200, ""))

        self.assertGreaterEqual(endpoint.request_count, 3)

    def test_route_handler_error_is_500(self):
        device = nclink.Server(sn="V2TEST00001")
        self.addCleanup(device.close)
        endpoint = device.start_http(0, with_config=False)
        self.addCleanup(endpoint.close)

        def boom(method, path, query, body):
            raise ValueError("炸了")

        endpoint.route("GET", "/boom", boom)
        status, body = self._http(endpoint, "GET", "/boom")
        self.assertEqual(status, 500)
        self.assertIn("炸了", body)
        self.assertIsInstance(device.last_callback_error, ValueError)

    def test_close_is_idempotent(self):
        device = nclink.Server(sn="V2TEST00001")
        endpoint = device.start_http(0, with_config=False)
        endpoint.close()
        endpoint.close()
        with self.assertRaises(nclink.NclinkError):
            endpoint.route("GET", "/x", lambda *args: None)
        device.close()          # 再关服务器也不该炸


class FileToolTest(unittest.TestCase):
    """文件小工具（离线：本地文件，不需要 broker 与设备）。"""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="nclink-file-")
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.content = "文件通道 hello\n"
        self.path = os.path.join(self.dir, "hello.txt")
        with open(self.path, "w", encoding="utf-8") as handle:
            handle.write(self.content)

    def test_compression_and_chunks(self):
        self.assertTrue(nclink.file_need_compression("a.txt"))
        self.assertTrue(nclink.file_need_compression("model.json"))
        self.assertFalse(nclink.file_need_compression("a.bin"))
        self.assertEqual(nclink.file_total_chunks(0), 0)
        self.assertEqual(nclink.file_total_chunks(1), 1)
        self.assertEqual(nclink.file_total_chunks(256 * 1024), 1)
        self.assertEqual(nclink.file_total_chunks(256 * 1024 + 1), 2)

    def test_checksum_matches_hashlib(self):
        # 按**文件字节**算（Windows 上文本模式会把 \n 写成 \r\n）
        with open(self.path, "rb") as handle:
            expected = hashlib.sha256(handle.read()).hexdigest()
        self.assertEqual(nclink.file_checksum(self.path), expected)
        self.assertIsNone(nclink.file_checksum(os.path.join(self.dir, "nope")))

    def test_attribute_of_file_and_directory(self):
        info = nclink.file_attribute(self.path, self.dir)
        self.assertEqual(info.file_name, "hello.txt")
        self.assertEqual(info.file_size, os.path.getsize(self.path))
        self.assertEqual(info.total_chunks, 1)
        self.assertFalse(info.is_dir)
        self.assertTrue(info.compressed)
        folder = nclink.file_attribute(self.dir)
        self.assertTrue(folder.is_dir)
        self.assertEqual(folder.file_size, 0)
        self.assertIsNone(nclink.file_attribute(os.path.join(self.dir, "nope")))

    def test_file_info_parsing(self):
        info = nclink.FileInfo.from_json({"fileName": "x.bin", "fileType": 1,
                                          "fileSize": 10, "totalChunks": 1,
                                          "compressed": False,
                                          "checksum": "ab", "parantDir": "/a",
                                          "modifyTime": 123})
        self.assertTrue(info.is_dir)
        self.assertEqual(info.parent_dir, "/a")
        self.assertIn("目录", str(info))
        self.assertEqual(nclink.FileInfo.parse_list("[]"), [])
        self.assertEqual(len(nclink.FileInfo.parse_list(
            '[{"fileName":"a"},{"fileName":"b"}]')), 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
