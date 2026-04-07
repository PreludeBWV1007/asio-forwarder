#!/usr/bin/env python3
"""
从上游发送一帧：Header(40, v2, LE) + Protobuf Body。
依赖：先执行 ../scripts/gen_proto_python.sh，再 pip install -r requirements-proto.txt
"""
import argparse
import os
import socket
import struct
import sys
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
GEN = os.path.join(ROOT, "tools", "generated")
if GEN not in sys.path:
    sys.path.insert(0, GEN)

try:
    import market_pb2  # type: ignore
except ImportError:
    print(
        "missing generated stubs: run ./scripts/gen_proto_python.sh "
        "and pip install -r tools/requirements-proto.txt",
        file=sys.stderr,
    )
    sys.exit(1)

MAGIC = 0x44574641
VERSION = 2
HEADER_LEN = 40

# 与 market_pb2 中 WireMsgType 一致（避免手写魔数漂移）
STOCK_QUOTE = market_pb2.STOCK_QUOTE
HEARTBEAT = market_pb2.HEARTBEAT
MARKET_PAYLOAD = market_pb2.MARKET_PAYLOAD


def pack_frame(
    msg_type: int,
    flags: int,
    seq: int,
    body: bytes,
    src_user_id: int = 0,
    dst_user_id: int = 0,
) -> bytes:
    hdr = struct.pack(
        "<IHHIIIIQQ",
        MAGIC,
        VERSION,
        HEADER_LEN,
        len(body),
        msg_type & 0xFFFFFFFF,
        flags & 0xFFFFFFFF,
        seq & 0xFFFFFFFF,
        src_user_id & 0xFFFFFFFFFFFFFFFF,
        dst_user_id & 0xFFFFFFFFFFFFFFFF,
    )
    return hdr + body


def main() -> None:
    ap = argparse.ArgumentParser(description="Send one protobuf frame to forwarder upstream.")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19001)
    ap.add_argument("--kind", choices=("stock", "heartbeat", "envelope"), default="stock")
    ap.add_argument("--type", type=int, default=None, help="override Header.msg_type")
    ap.add_argument("--flags", type=int, default=0)
    ap.add_argument("--seq", type=int, default=1)
    ap.add_argument("--src-user", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--dst-user", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--symbol", default="600000.SH")
    ap.add_argument("--price", type=float, default=10.5)
    ap.add_argument("--volume", type=int, default=1_000_000)
    ap.add_argument("--exchange", default="SSE")
    ap.add_argument("--node-id", default="py_upstream")
    ap.add_argument("--inner", choices=("quote", "heartbeat"), default="quote")
    args = ap.parse_args()

    body = b""
    msg_type = args.type

    if args.kind == "stock":
        q = market_pb2.StockQuote()
        q.symbol = args.symbol
        q.last_price = args.price
        q.volume = args.volume
        q.exchange = args.exchange
        body = q.SerializeToString()
        if msg_type is None:
            msg_type = STOCK_QUOTE
    elif args.kind == "heartbeat":
        hb = market_pb2.Heartbeat()
        hb.ts_ms = int(time.time() * 1000)
        hb.node_id = args.node_id
        body = hb.SerializeToString()
        if msg_type is None:
            msg_type = HEARTBEAT
    else:
        mp = market_pb2.MarketPayload()
        if args.inner == "quote":
            mp.stock_quote.symbol = args.symbol
            mp.stock_quote.last_price = args.price
            mp.stock_quote.volume = args.volume
            mp.stock_quote.exchange = args.exchange
        else:
            mp.heartbeat.node_id = args.node_id
        body = mp.SerializeToString()
        if msg_type is None:
            msg_type = MARKET_PAYLOAD

    wire = pack_frame(msg_type, args.flags, args.seq, body, args.src_user, args.dst_user)
    with socket.create_connection((args.host, args.port), timeout=10) as s:
        s.sendall(wire)
    print(f"sent kind={args.kind} msg_type={msg_type} body_len={len(body)} -> {args.host}:{args.port}")


if __name__ == "__main__":
    main()
