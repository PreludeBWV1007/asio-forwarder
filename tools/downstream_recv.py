#!/usr/bin/env python3
"""
本文件职责（下游收包自测工具）：
- 连接转发器 downstream 端口，循环接收并打印帧信息（按 Header(40,v2)+Body 解析）
- body 仅做前 64 字节预览（bytes），用于快速确认是否收到广播数据

常用示例：
  python3 tools/downstream_recv.py --host 127.0.0.1 --port 19002
"""
import argparse
import socket
import struct
import sys


MAGIC = 0x44574641
VERSION = 2
HEADER_LEN = 40


def recv_all(sock: socket.socket, n: int) -> bytes:
  buf = b""
  while len(buf) < n:
    chunk = sock.recv(n - len(buf))
    if not chunk:
      raise RuntimeError("peer closed")
    buf += chunk
  return buf


def main():
  ap = argparse.ArgumentParser()
  ap.add_argument("--host", default="127.0.0.1")
  ap.add_argument("--port", type=int, default=19002)
  args = ap.parse_args()

  sys.stdout.reconfigure(line_buffering=True)
  s = socket.create_connection((args.host, args.port), timeout=5)
  s.settimeout(None)
  print(f"connected to downstream {args.host}:{args.port}, waiting frames...", flush=True)

  while True:
    hb = recv_all(s, HEADER_LEN)
    magic, ver, hlen, blen, typ, flags, seq, src_u, dst_u = struct.unpack("<IHHIIIIQQ", hb)
    if magic != MAGIC or ver != VERSION or hlen != HEADER_LEN:
      raise RuntimeError(f"invalid header: magic={hex(magic)} ver={ver} hlen={hlen}")
    body = recv_all(s, blen) if blen else b""
    preview = body[:64]
    print(
        f"frame: body_len={blen} type={typ} flags={flags} seq={seq} "
        f"src_user_id={src_u} dst_user_id={dst_u} "
        f"body_preview={preview!r}{'...' if blen > 64 else ''}",
        flush=True,
    )


if __name__ == "__main__":
  main()

