# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""对**真 broker** 的端到端自检（默认跳过）。

设了 `NCLINK_TEST_BROKER` 才会跑，例如：

    NCLINK_TEST_BROKER=tcp://127.0.0.1:18830 \
        python -m unittest discover -s bindings/python/tests -v

它把"设备端 + 客户端"放进**同一个进程**、中间过一遍真 broker：probe、路径绑定
取值/写值、methodCall（应答文本 -> Json）、采样上报、事件推送。离线自检
（test_nclink.py）覆盖不到的"过 MQTT 的那一段"就是靠它守住的。
"""

import json
import os
import shutil
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import nclink  # noqa: E402

BROKER = os.environ.get("NCLINK_TEST_BROKER", "").strip()
SN = "V2PYE2E0001"

MODEL = {
    "name": "Python E2E 机床",
    "id": "01",
    "type": "NC_LINK_ROOT",
    "devices": [{
        "id": "02",
        "type": "MACHINE",
        "name": "模拟机床",
        "configs": [{
            "name": "采样通道",
            "id": "e2e_channel",
            "type": "SAMPLE_CHANNEL",
            "sampleInterval": 200,
            "uploadInterval": 200,
            "ids": [{"id": "/STATUS"}, {"id": "/PART_COUNT"}],
        }],
        "dataItems": [
            {"name": "状态", "id": "030001", "type": "STATUS", "settable": False},
            {"name": "加工计件", "id": "030002", "type": "PART_COUNT", "settable": True},
        ],
    }],
}


def _wait_for(predicate, seconds=8.0):
    """等 predicate() 为真（采样/事件是异步到的）。"""
    deadline = time.time() + seconds
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.05)
    return False


@unittest.skipUnless(BROKER, "设置 NCLINK_TEST_BROKER=tcp://host:port 才会跑")
class BrokerE2ETest(unittest.TestCase):
    """设备端与客户端同进程，报文真的过一遍 broker。"""

    @staticmethod
    def _set_count(state, params):
        state["count"] = int((params or {}).get("value", 0))
        return state["count"]

    def test_device_and_client_round_trip(self):
        nclink.log_init()
        state = {"status": 1, "count": 0}

        device = nclink.Server(sn=SN, model=json.dumps(MODEL, ensure_ascii=False),
                               broker=BROKER)
        self.addCleanup(device.close)
        device.register_tool(
            "plc",
            methods={"getStatus": None, "getCount": None, "setCount": None},
            handlers={"getStatus": lambda params: state["status"],
                      "getCount": lambda params: state["count"],
                      "setCount": lambda params: self._set_count(state, params)},
            bindings=[("/STATUS", nclink.Operation.GET_VALUE, "getStatus"),
                      ("/PART_COUNT", nclink.Operation.GET_VALUE, "getCount"),
                      ("/PART_COUNT", nclink.Operation.SET_VALUE, "setCount")])
        device.subscribe()
        device.init_samples()

        nclink.init(BROKER)
        self.addCleanup(nclink.shutdown)

        with nclink.get_device(SN) as client:
            # probe：模型真的过了一遍 broker
            with client.probe() as model:
                self.assertEqual(model.root.id, "01")
                self.assertIsNotNone(model.find_by_id("030001"))
                paths = [item.path for item in model.root.devices[0].data_items]
                self.assertIn("/STATUS", paths)

            # 路径绑定 -> 工具方法（GET）
            with client.get_value("/STATUS") as value:
                self.assertEqual(value.to_python(), 1)

            # 路径绑定 -> 工具方法（SET），再读回来
            client.set_value("/PART_COUNT", 7)
            self.assertEqual(state["count"], 7)
            with client.get_value("/PART_COUNT") as value:
                self.assertEqual(value.to_python(), 7)

            # methodCall：应答是 **JSON 文本**，绑定必须解析成 Json（曾经在这里踩过）
            with client.method_call("/plc/getCount") as reply:
                body = reply.to_python()
                self.assertEqual(body["code"], "OK")
                self.assertEqual(body["data"], 7)
            with client.method_call("/plc/setCount", {"value": 21}) as reply:
                self.assertEqual(reply.to_python()["data"], 21)
                self.assertEqual(state["count"], 21)
            with client.method_call("/plc/getStatus", None, check=True) as reply:
                self.assertEqual(reply.to_python()["code"], "OK")

            # 采样与事件：设备端每 200 ms 推一次，等到就走
            samples = []
            events = []
            client.subscribe_samples(2, lambda topic, sample: samples.append(sample))
            client.subscribe_events(2, lambda topic, event: events.append(event))

            device.push_event("010307", {"key": "PART_COUNT",
                                         "value": state["count"]})
            self.assertTrue(_wait_for(lambda: samples and events),
                            "没等到采样/事件（samples=%d events=%d）"
                            % (len(samples), len(events)))
            client.unsubscribe_samples()
            client.unsubscribe_events()

            sample = samples[-1]
            self.assertEqual([column.path for column in sample.columns],
                             ["/STATUS", "/PART_COUNT"])
            self.assertEqual(sample.value_at(0, 0), 1)
            self.assertGreaterEqual(device.sample_upload_count, 1)
            self.assertEqual(events[-1].key, "PART_COUNT")
            self.assertEqual(events[-1].value, state["count"])

    def test_file_channel(self):
        """文件通道：MQTT 只传令牌，字节走 FTP（设备是 FTP 客户端，本机是服务端）。"""
        nclink.log_init()
        device = nclink.Server(sn=SN, model=json.dumps(MODEL, ensure_ascii=False),
                               broker=BROKER)
        self.addCleanup(device.close)
        device.register_file_tool()
        seen = {"size": -1}
        device.register_tool(
            "sink",
            methods={"take": None, "give": None},
            handlers={"take": lambda params: self._take(seen, params),
                      "give": lambda params: {"copy": {"@file": give["path"]}}})
        device.subscribe()

        nclink.init(BROKER)
        self.addCleanup(nclink.shutdown)
        try:
            nclink.start_file_server()          # 幂等（init 里已经起过）
        except nclink.NclinkError as error:
            self.skipTest("FTP 2323 起不来（%s），跳过文件通道" % error)

        give = {"path": None}
        work = tempfile.mkdtemp(prefix="nclink-e2e-")
        self.addCleanup(shutil.rmtree, work, True)
        local = os.path.join(work, "report.txt")
        content = "文件通道 e2e %s\n" % time.time()
        with open(local, "w", encoding="utf-8") as handle:
            handle.write(content)
        size = os.path.getsize(local)

        with nclink.get_device(SN) as client:
            client.upload_local_file(local, "/data/report.txt")
            on_device = os.path.join(nclink.root(), "uploadFile", "data",
                                     "report.txt")
            self.assertTrue(os.path.exists(on_device), "设备侧没收到文件")
            with open(on_device, encoding="utf-8") as handle:
                self.assertEqual(handle.read(), content)

            listed = client.list_files("/data")
            self.assertEqual([item.file_name for item in listed], ["report.txt"])
            self.assertEqual(listed[0].file_size, size)
            self.assertFalse(listed[0].is_dir)

            back = client.download_to("/data/report.txt",
                                      os.path.join(work, "back.txt"))
            with open(back, encoding="utf-8") as handle:
                self.assertEqual(handle.read(), content)

            client.make_directory("/docs")
            self.assertIn("docs", [item.file_name for item in client.list_files("/")])

            # 带文件参数的方法调用：参数里的文件传上去、返回值里的文件取回来
            with client.method_call_file(
                    "sink/take", params={"blob": ""}, keys=["blob"],
                    paths=[local]) as reply:
                self.assertEqual(reply.to_python()["code"], "OK")
            self.assertEqual(seen["size"], size)

            give["path"] = on_device
            with client.method_call_file("sink/give") as reply:
                data = reply.to_python()["data"]
                self.assertIn("copy", data["fileKeys"])
                with open(data["copy"], encoding="utf-8") as handle:
                    self.assertEqual(handle.read(), content)

            client.delete_file("/data/report.txt")
            self.assertEqual(client.list_files("/data"), [])

    @staticmethod
    def _take(seen, params):
        """设备侧的工具：把收到的文件参数读出来（路径在设备的 uploadFile 下）。"""
        token = (params or {}).get("blob") or ""
        path = os.path.join(nclink.root(), "uploadFile",
                            *token.lstrip("/").split("/"))
        seen["size"] = os.path.getsize(path) if os.path.exists(path) else -1
        return seen["size"]


if __name__ == "__main__":
    unittest.main(verbosity=2)
