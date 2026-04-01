#!/usr/bin/env python3
"""
本文件职责（上游发包自测工具）：
- 连接转发器 upstream 端口，按协议打包并发送 1 帧：Header(24, little-endian) + Body
- 默认 body 为 UTF-8 文本（--text），用于快速验证“上游 -> 下游广播”链路

常用示例：
  python3 tools/upstream_send.py --host 127.0.0.1 --port 19001 --type 100 --seq 1 --text "你好"
"""
import argparse
import socket
import struct


MAGIC = 0x44574641
VERSION = 1
HEADER_LEN = 24


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument("--host", default="127.0.0.1")
  ap.add_argument("--port", type=int, default=19001)
  ap.add_argument("--type", type=int, default=100)
  ap.add_argument("--flags", type=int, default=0)
  ap.add_argument("--seq", type=int, default=1)
  ap.add_argument("--text", default="hello-binary")
  args = ap.parse_args()

  body = args.text.encode("utf-8")
  hdr = struct.pack(
      "<IHHIIII",
      MAGIC,
      VERSION,
      HEADER_LEN,
      len(body),
      args.type & 0xFFFFFFFF,
      args.flags & 0xFFFFFFFF,
      args.seq & 0xFFFFFFFF,
  )

  s = socket.create_connection((args.host, args.port), timeout=5)
  s.sendall(hdr + body)
  s.close()
  print(f"sent frame to {args.host}:{args.port} body_len={len(body)} type={args.type} seq={args.seq}")


if __name__ == "__main__":
  main()

