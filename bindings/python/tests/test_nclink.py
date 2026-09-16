# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""Python 绑定的自检（不需要 broker）。

跑法（仓库根）：

    python -m unittest discover -s bindings/python/tests -v

对着真 broker 的端到端跑法见 bindings/python/README.md（设备端示例 +
examples/client_demo.py）。
"""

import os
import sys
import unittest

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

    def test_init_rejects_an_unreachable_broker(self):
        with self.assertRaises(nclink.NclinkError) as caught:
            nclink.init("tcp://127.0.0.1:1")     # 1 号端口不会有 broker
        self.assertEqual(caught.exception.op, "init")
        self.assertFalse(nclink.is_open())

    def test_empty_broker_uri(self):
        with self.assertRaises(ValueError):
            nclink.init("")


if __name__ == "__main__":
    unittest.main(verbosity=2)
