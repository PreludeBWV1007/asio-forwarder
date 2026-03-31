#!/usr/bin/env python3
import argparse
import socket
import struct
import sys


MAGIC = 0x44574641
VERSION = 1
HEADER_LEN = 24


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
    magic, ver, hlen, blen, typ, flags, seq = struct.unpack("<IHHIIII", hb)
    if magic != MAGIC or ver != VERSION or hlen != HEADER_LEN:
      raise RuntimeError(f"invalid header: magic={hex(magic)} ver={ver} hlen={hlen}")
    body = recv_all(s, blen) if blen else b""
    preview = body[:64]
    print(
        f"frame: body_len={blen} type={typ} flags={flags} seq={seq} "
        f"body_preview={preview!r}{'...' if blen > 64 else ''}",
        flush=True,
    )


if __name__ == "__main__":
  main()

