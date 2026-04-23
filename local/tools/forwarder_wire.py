#!/usr/bin/env python3
"""v2 帧头 + msgpack body（对等中继）：`local/tools/` 联调与 `local/tests/` 黑盒共用，与 C++ `pack_wire` / `deliver/client/include/fwd/protocol.hpp` 对齐。"""
from __future__ import annotations

import struct
import msgpack

MAGIC = 0x44574641
VERSION = 2
HEADER_LEN = 40

MSG_CLIENT_LOGIN = 1
MSG_CLIENT_HEARTBEAT = 2
MSG_CLIENT_CONTROL = 3
MSG_CLIENT_DATA = 4

MSG_DELIVER = 200
MSG_SERVER_REPLY = 201
MSG_KICK = 202


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


def unpack_deliver_body(body: bytes) -> tuple[bytes, int, int, str, str]:
    """DELIVER(200) body 为 msgpack：{payload, src_conn_id, dst_conn_id, src_username, dst_username}。"""
    o = msgpack.unpackb(body, raw=False)
    if not isinstance(o, dict):
        raise ValueError("DELIVER body must be msgpack map")
    pl = o.get("payload")
    if isinstance(pl, memoryview):
        pl = pl.tobytes()
    if not isinstance(pl, (bytes, bytearray)):
        raise ValueError("DELIVER missing payload bytes")
    return (
        bytes(pl),
        int(o.get("src_conn_id", 0)),
        int(o.get("dst_conn_id", 0)),
        str(o.get("src_username", "")),
        str(o.get("dst_username", "")),
    )
