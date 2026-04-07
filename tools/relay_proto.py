#!/usr/bin/env python3
"""v2 帧头 + msgpack body 辅助（对等中继协议，供自测脚本使用）。"""
from __future__ import annotations

import struct
import msgpack

MAGIC = 0x44574641
VERSION = 2
HEADER_LEN = 40

MSG_DELIVER = 200
MSG_SERVER_REPLY = 201


def pack_header(
    body_len: int,
    msg_type: int = 0,
    flags: int = 0,
    seq: int = 0,
    src_user: int = 0,
    dst_user: int = 0,
) -> bytes:
    return struct.pack(
        "<IHHIIIIQQ",
        MAGIC,
        VERSION,
        HEADER_LEN,
        body_len & 0xFFFFFFFF,
        msg_type & 0xFFFFFFFF,
        flags & 0xFFFFFFFF,
        seq & 0xFFFFFFFF,
        src_user & 0xFFFFFFFFFFFFFFFF,
        dst_user & 0xFFFFFFFFFFFFFFFF,
    )


def pack_frame_msgpack(
    obj: dict,
    msg_type: int = 0,
    flags: int = 0,
    seq: int = 0,
    src_user: int = 0,
    dst_user: int = 0,
) -> bytes:
    body = msgpack.packb(obj, use_bin_type=True)
    return pack_header(len(body), msg_type, flags, seq, src_user, dst_user) + body


def unpack_header(hb: bytes) -> dict:
    magic, ver, hlen, blen, typ, fl, seq, src_u, dst_u = struct.unpack("<IHHIIIIQQ", hb)
    return {
        "magic": magic,
        "version": ver,
        "header_len": hlen,
        "body_len": blen,
        "msg_type": typ,
        "flags": fl,
        "seq": seq,
        "src_user_id": src_u,
        "dst_user_id": dst_u,
    }


def recv_all(sock, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c:
            raise EOFError("peer closed")
        buf += c
    return buf


def recv_frame(sock) -> tuple[dict, bytes]:
    hb = recv_all(sock, HEADER_LEN)
    h = unpack_header(hb)
    if h["magic"] != MAGIC or h["version"] != VERSION or h["header_len"] != HEADER_LEN:
        raise ValueError(f"bad header {h}")
    blen = h["body_len"]
    body = recv_all(sock, blen) if blen else b""
    return h, body


def recv_frame_msgpack(sock) -> tuple[dict, object | None]:
    h, body = recv_frame(sock)
    if not body:
        return h, None
    return h, msgpack.unpackb(body, raw=False)
