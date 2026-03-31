#!/usr/bin/env python3
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

