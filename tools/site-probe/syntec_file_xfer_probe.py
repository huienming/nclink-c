#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 huienming
"""SYNTEC 文件服务（G 代码上下行）现场探针：端口 5572（见 10 册 §11.11）。

服务器侧（OCAPIServer.exe 的 IL）：
  包头 12 字节 {Length u32, CmdID u32, Reserved u32}（Length = 后面的字节数）
  In  { uFuncID u32, ... }，**dispatch 在 uFuncID 上**（= FileTransferCmd）
  路径是 UTF-16LE，长度字段是**字符数**（服务器按 nLength*2 字节拷贝）

  1  FileSendStart   { uFuncID, nFilePathLength, szFilePath }        -> { hr }
  2  FileSending     { uFuncID, nFileLength, pBufferIn }             -> { hr }
  3  FileRecvStart   { uFuncID, nFilePathLength, szFilePath }        -> { hr, nFileLength }
  4  FileRecving     { uFuncID, nFileOffset, nReqLength }            -> { pBufferOut }（数据跟在 [12..]）
  8  GetAllFileList  { uFuncID, nDirPathLength, szDirPath }          -> { nFileListLength, pBufferOut }
 11  FileExist       { uFuncID, nFilePathLength, szFilePath }        -> { bFileServiceSuccess }
 12  DirExist        { uFuncID, nDirPathLength, szDirPath }          -> { bFileServiceSuccess }
 13  FileNew         { uFuncID, nFilePathLength, szFilePath }
 14  FileDelete      { uFuncID, nFilePathLength, szFilePath }
 17  DirCreate       { uFuncID, nDirPathLength, szDirPath }
"""
import socket
import struct
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5572
DIR = 'C:/CNC'
NAME = 'PYTEST'


def path_frame(func_id, path, tail=b''):
    raw = path.encode('utf-16-le')
    body = struct.pack('<II', func_id, len(path)) + raw + tail
    return struct.pack('<IHHI', len(body), func_id, 0, 0) + body


class Link:
    """
    文件服务是**带状态**的：FileSendStart 记下来的路径放在这条连接上，
    后面的 FileSending 往它追加 —— 所以一次传输必须共用一条连接。
    """

    def __init__(self, port=PORT):
        self.sock = socket.create_connection(('127.0.0.1', port), 3)

    def close(self):
        try:
            self.sock.close()
        except Exception:  # noqa: BLE001
            pass

    def ask(self, frame, timeout=3.0):
        got = b''
        try:
            self.sock.sendall(frame)
            self.sock.settimeout(timeout)
            while len(got) < 12:
                d = self.sock.recv(12 - len(got))
                if not d:
                    break
                got += d
            if len(got) == 12:
                n = struct.unpack_from('<I', got, 0)[0]
                if 0 < n < 1 << 22:
                    while len(got) < 12 + n:
                        d = self.sock.recv(12 + n - len(got))
                        if not d:
                            break
                        got += d
        except Exception:  # noqa: BLE001
            pass
        return got


def body(got):
    return got[12:] if len(got) > 12 else b''


def hr(got):
    b = body(got)
    return struct.unpack_from('<i', b, 0)[0] if len(b) >= 4 else None


def main():
    target = DIR + '/' + NAME
    link = Link()

    print('=== 1) 传输前：DirExist / FileExist ===')
    print('  DirExist(%r)   -> %s' % (DIR, body(link.ask(path_frame(12, DIR)))[:4].hex()))
    print('  FileExist(%r) -> %s' % (target, body(link.ask(path_frame(11, target)))[:4].hex()))

    code = b'O1000\nG21 G40 G80\nG0 X0 Z0\nG1 X10. Z-5. F100\nM30\n'
    print()
    print('=== 2) 上传 %d 字节（同一条连接：FileSendStart -> FileSending）===' % len(code))
    g = link.ask(path_frame(1, target))
    print('  FileSendStart        -> len=%d hr=%s' % (len(g), hr(g)))
    in_body = struct.pack('<II', 2, len(code)) + code
    g = link.ask(struct.pack('<IHHI', len(in_body), 2, 0, 0) + in_body)
    print('  FileSending(%d 字节) -> len=%d hr=%s' % (len(code), len(g), hr(g)))
    link.close()

    print()
    print('=== 3) 读回：FileExist / GetAllFileList ===')
    link = Link()
    print('  FileExist(%r) -> %s' % (target, body(link.ask(path_frame(11, target)))[:4].hex()))
    g = link.ask(path_frame(8, DIR + '/'))
    b = body(g)
    if len(b) >= 4:
        n = struct.unpack_from('<I', b, 0)[0]
        blob = b[4:4 + n]
        print('  GetAllFileList(%r) nFileListLength=%d' % (DIR + '/', n))
        try:
            print('    -> %r' % blob.decode('utf-16-le', 'replace'))
        except Exception:  # noqa: BLE001
            print('    -> %s' % blob[:80].hex())
    else:
        print('  GetAllFileList 无应答/空: %s' % b.hex())

    print()
    print('=== 4) 下载（同一条连接：FileRecvStart -> FileRecving）===')
    g = link.ask(path_frame(3, target))
    b = body(g)
    if len(b) >= 8:
        total = struct.unpack_from('<I', b, 4)[0]
        print('  FileRecvStart -> hr=%d nFileLength=%d'
              % (struct.unpack_from('<i', b, 0)[0], total))
    else:
        print('  FileRecvStart -> len=%d body=%s' % (len(g), b.hex()))
    # FileRecving 的应答就是**裸数据**（没有 4 字节头）：长度 = nReqLength，
    # 内容 = 文件从 nFileOffset 起的 nReqLength 个字节。请求超过文件长度会扑空，
    # 所以按 FileRecvStart 报的大小分块取。
    total = None
    got_all = bytearray()
    chunk = 16
    while True:
        in_body = struct.pack('<III', 4, len(got_all), chunk)
        g = link.ask(struct.pack('<IHHI', len(in_body), 4, 0, 0) + in_body)
        piece = body(g)
        if not piece:
            break
        got_all += piece
        if total is not None and len(got_all) >= total:
            break
        if len(piece) < chunk:
            break
    print('  分块取回（每块 %d 字节）-> 一共 %d 字节: %r' % (chunk, len(got_all), bytes(got_all)[:80]))
    if bytes(got_all) == code:
        print('  ** 与上传的内容逐字节一致 **')
    link.close()

    print()
    print('=== 5) 收尾：删除 ===')
    link = Link()
    print('  FileDelete -> %s' % body(link.ask(path_frame(14, target)))[:4].hex())
    print('  FileExist  -> %s' % body(link.ask(path_frame(11, target)))[:4].hex())
    link.close()
    return 0

if __name__ == '__main__':
    sys.exit(main())
