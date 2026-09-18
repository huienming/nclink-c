#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming

"""Append the API index / error code / topic appendices to MANUAL.md.

The appendices are generated from the public headers so they can never drift
from the code. Run it from the repository root:

    python tools/gen_api_index.py
"""
import io
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INCLUDE = os.path.join(ROOT, "include", "nclink")
MANUAL = os.path.join(ROOT, "MANUAL.md")

DECL = re.compile(
    r"^(ncl_|const char|const ncl_json|char|bool|void|size_t|int|long long|"
    r"int64_t|double|ncl_err|uint\w+|float|unsigned|static inline)"
)


def strip_block(lines):
    """Return (signature, description) for the declaration starting at 0."""
    text = " ".join(l.strip() for l in lines)
    text = re.sub(r"\s+", " ", text)
    text = text.split("{")[0].strip()
    if ";" in text:
        text = text[: text.index(";") + 1]
    return text


def parse_header(path):
    """Yield (signature, description) for every function declaration."""
    raw = io.open(path, encoding="utf-8").read().splitlines()
    entries = []
    pending_doc = []
    doc_block = []
    i = 0
    while i < len(raw):
        line = raw[i]
        stripped = line.strip()
        if stripped.startswith("/**"):
            doc_block = [stripped]
            if "*/" in stripped:
                pending_doc = doc_block
                doc_block = []
            i += 1
            continue
        if doc_block:
            doc_block.append(stripped)
            if "*/" in stripped:
                pending_doc = doc_block
                doc_block = []
            i += 1
            continue
        if stripped.startswith("/*") or stripped.startswith("*") or stripped.startswith("//"):
            i += 1
            continue
        if DECL.match(stripped) and "(" in stripped:
            block = []
            while i < len(raw):
                block.append(raw[i])
                if ";" in raw[i]:
                    break
                i += 1
            signature = strip_block(block)
            if "(" in signature and "typedef" not in signature:
                description = ""
                for doc in pending_doc:
                    body = doc.strip().lstrip("/").lstrip("*").strip()
                    if body and not body.startswith("*/"):
                        description = summarise(body)
                        break
                entries.append((signature, description))
            pending_doc = []
        elif stripped and not stripped.startswith("}"):
            pending_doc = []
        i += 1
    return entries


def summarise(text):
    """First sentence of a doc comment, without the closing marker."""
    text = text.replace("*/", "").strip()
    cut = text.find(". ")
    if cut != -1:
        text = text[: cut + 1]
    return text


def api_index():
    out = [
        "## 附录 A · API 索引",
        "",
        "按头文件分组，由 `tools/gen_api_index.py` 从 `include/nclink/*.h` 自动生成"
        "（重新生成：`python tools/gen_api_index.py`）。",
        "",
    ]
    for name in sorted(os.listdir(INCLUDE)):
        if not name.endswith(".h"):
            continue
        entries = parse_header(os.path.join(INCLUDE, name))
        if not entries:
            continue
        out.append("### `nclink/%s`" % name)
        out.append("")
        for signature, description in entries:
            if description:
                out.append("- `%s` — %s" % (signature, description))
            else:
                out.append("- `%s`" % signature)
        out.append("")
    return out


def defines(path, prefix):
    """Yield (name, value, trailing comment) for #define lines."""
    out = []
    text = io.open(path, encoding="utf-8").read().splitlines()
    for line in text:
        stripped = line.strip()
        if stripped.startswith("#  define " + prefix) or stripped.startswith("#define " + prefix):
            body = stripped.split("define", 1)[1].strip()
            m = re.match(r"(\w+)\s+(.*)", body)
            if not m:
                continue
            value = m.group(2)
            comment = ""
            cm = re.search(r"/\*+\s*<?\s*(.*?)\s*\*/", value)
            if cm:
                comment = cm.group(1)
                value = value[: cm.start()]
            out.append((m.group(1), value.strip(), comment))
    return out


def error_table():
    out = ["## 附录 B · 错误码全表", ""]
    out.append("来自 `nclink/ncl_common.h`。第三列是 `ncl_err_name()` 返回的稳定名称，")
    out.append("便于跨语言比日志。")
    out.append("")
    out.append("| 名称 | 值 | `ncl_err_name()` |")
    out.append("|------|----|------------------|")
    mapping = {
        "NCL_ERR": "（通用失败）",
        "NCL_ERR_NOMEM": "内存不足",
        "NCL_ERR_PARSE": "JSON/报文解析失败",
        "NCL_ERR_TIMEOUT": "请求超时",
        "NCL_ERR_IO": "IoException / 文件失败",
        "NCL_ERR_NOT_FOUND": "查找失败",
        "NCL_ERR_EXISTS": "重复条目",
        "NCL_ERR_NOT_SUPPORTED": "功能未编译 / 未实现",
        "NCL_ERR_INVALID_ARG": "IllegalArgumentException / 调用方传参错误",
        "NCL_ERR_STATE": "对象不可用",
        "NCL_ERR_RANGE": "越界",
        "NCL_ERR_CONNECT": "MqttException / 连接失败",
        "NCL_ERR_CLOSED": "对象已关闭",
        "NCL_ERR_NO_CHANNEL": "NoFileChannelException / 设备还没有文件通道",
    }
    # The names ncl_err_name() returns for the protocol domain errors
    # (src/core/common.c); one per NC-Link validity rule.
    for name in ("CODE", "DATA_NAME", "DATA_TYPE", "DEVICE_ID", "ENCODING", "ID",
                 "INDEX_RANGE", "ITEM", "KEY", "MESSAGE", "MESSAGE_ID", "MODEL",
                 "NODE", "NUMBER", "REQUEST", "TYPE", "VALUE", "VERSION"):
        pretty = name.replace("_", " ").title().replace(" ", "")
        if name == "DATA_TYPE":
            pretty = "DataTyp"
        mapping["NCL_ERR_INVALID_" + name] = "Invalid%sException" % pretty
    for name, value, _comment in defines(os.path.join(INCLUDE, "ncl_common.h"),
                                         "NCL_ERR"):
        out.append("| `%s` | %s | %s |" % (name, value, mapping.get(name, "-")))
    out.append("")
    return out


def topic_table():
    out = ["## 附录 C · 主题前缀一览", ""]
    out.append("来自 `nclink/ncl_topic.h`；除 `Register` 外都带设备 SN。")
    out.append("")
    out.append("| 常量 | 值 |")
    out.append("|------|----|")
    for name, value, _comment in defines(os.path.join(INCLUDE, "ncl_topic.h"),
                                         "NCL_TOPIC_"):
        out.append("| `%s` | %s |" % (name, value))
    out.append("")
    return out


def layout_table():
    out = ["## 附录 D · 安装根目录布局", ""]
    out.append("```")
    out.append("<root>/")
    out.append('  bin/sn.txt              设备序列号（首次启动生成 "V2" + 9 位十六进制，见 3.4）')
    out.append("  bin/ftp.txt             FTP 端口与账号（设备端 FTP 端点用）")
    out.append("  conf/mqtt.cfg           url/username/password（示例首次启动写本机 1883、匿名）")
    out.append("  conf/model/nclink.json  数据模型（示例首次启动写默认机床模型）")
    out.append("  conf/driver/*.json      驱动配置")
    out.append("  conf/ipConf.json        网络配置")
    out.append("  conf/server.json        服务器列表")
    out.append("  log/out.txt             日志（10 MB 轮转）")
    out.append("  uploadFile/             设备侧文件镜像（相对路径的基准）")
    out.append("  temp/                   文件通道的临时交换目录")
    out.append("  <sn>/                   客户端侧文件镜像（相对路径的基准）")
    out.append("```")
    out.append("")
    return out


def main():
    text = io.open(MANUAL, encoding="utf-8").read()
    marker = "## 附录 A · API 索引"
    if marker in text:
        text = text[: text.index(marker)].rstrip() + "\n\n"
    lines = []
    lines += api_index()
    lines += error_table()
    lines += topic_table()
    lines += layout_table()
    with io.open(MANUAL, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(text + "\n".join(lines) + "\n")
    print("appended %d lines" % len(lines))


if __name__ == "__main__":
    main()
