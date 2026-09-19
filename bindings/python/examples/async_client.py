# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""异步方法调用客户端示例（跨语言时它可以直接调别的语言的设备端）。

    python async_client.py tcp://127.0.0.1:1883 V2GODEV0001 /plc/slow

设备端只要注册了同名方法即可（`bindings/go/example/device` 就带一个 `slow`）：
请求带 async → 设备立刻回 code=OK + handler（方法在设备端线程池里跑）→ 这里按句柄
查状态、轮询结果，直到 result=finished|error。
"""
from __future__ import annotations

import sys
import time

import nclink


def main() -> int:
    broker = sys.argv[1] if len(sys.argv) > 1 else "tcp://127.0.0.1:1883"
    sn = sys.argv[2] if len(sys.argv) > 2 else "V2GODEV0001"
    method = sys.argv[3] if len(sys.argv) > 3 else "/plc/slow"

    nclink.init(broker)
    try:
        with nclink.get_device(sn) as client:
            with client.method_call_async(method) as ack:
                body = ack.to_python()
            print("异步受理:", body.get("code"), "handler=", body.get("handler"))
            handler = body.get("handler")
            if not handler:
                return 1

            with client.method_status(sn, handler) as status:
                print("状态:", status.to_python().get("status"))

            deadline = time.time() + 10
            while time.time() < deadline:
                with client.method_result(sn, handler) as result:
                    body = result.to_python()
                if body.get("code") != "PENDING":
                    break
                time.sleep(0.05)
            print("结果:", body.get("code"), body.get("result"), body.get("return"))
            return 0 if body.get("result") == "finished" else 1
    finally:
        nclink.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
