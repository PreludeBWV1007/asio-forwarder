#!/usr/bin/env python3
"""local/tests/e2e_minimal.py — 最短黑盒（需 PYTHONPATH=local/tools）。"""
from __future__ import annotations

import socket
import sys

import msgpack

from forwarder_wire import (
    MSG_CLIENT_DATA,
    MSG_CLIENT_HEARTBEAT,
    MSG_CLIENT_LOGIN,
    MSG_DELIVER,
    MSG_SERVER_REPLY,
    pack_header,
    recv_frame,
    recv_frame_msgpack,
    unpack_deliver_body,
)


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2] if len(sys.argv) > 2 else "0")

    a = socket.create_connection((host, port), timeout=10)
    b = socket.create_connection((host, port), timeout=10)

    def send_login(sock, *, user: str, pw: str, role: str, register: bool, seq: int) -> None:
        body = msgpack.packb(
            {"username": user, "password": pw, "peer_role": role, "register": register},
            use_bin_type=True,
        )
        sock.sendall(pack_header(len(body), MSG_CLIENT_LOGIN, seq=seq) + body)

    send_login(a, user="e2e_alice", pw="secret1", role="user", register=True, seq=1)
    ha, ba = recv_frame_msgpack(a)
    assert ha["msg_type"] == MSG_SERVER_REPLY, ha
    assert isinstance(ba, dict) and ba.get("ok") is True and ba.get("op") == "LOGIN", ba
    conn_a = int(ba["conn_id"])

    send_login(b, user="e2e_bob", pw="secret2", role="user", register=True, seq=1)
    hb, bb = recv_frame_msgpack(b)
    assert hb["msg_type"] == MSG_SERVER_REPLY, hb
    assert isinstance(bb, dict) and bb.get("ok") is True, bb
    conn_b = int(bb["conn_id"])

    hb_body = msgpack.packb({}, use_bin_type=True)
    a.sendall(pack_header(len(hb_body), MSG_CLIENT_HEARTBEAT, seq=2) + hb_body)
    h1, r1 = recv_frame_msgpack(a)
    assert h1["msg_type"] == MSG_SERVER_REPLY and r1.get("ok") is True, (h1, r1)

    payload = b"unicast-e2e"
    data_body = msgpack.packb(
        {"mode": "unicast", "dst_username": "e2e_bob", "dst_conn_id": conn_b, "payload": payload},
        use_bin_type=True,
    )
    a.sendall(pack_header(len(data_body), MSG_CLIENT_DATA, seq=3) + data_body)
    h2, r2 = recv_frame_msgpack(a)
    assert h2["msg_type"] == MSG_SERVER_REPLY and r2.get("ok") is True, (h2, r2)

    hd, raw = recv_frame(b)
    assert hd["msg_type"] == MSG_DELIVER, hd
    assert hd["src_user_id"] == 0 and hd["dst_user_id"] == 0, hd
    pl, sc, dc, su, du = unpack_deliver_body(raw)
    assert pl == payload, pl
    assert sc == conn_a and dc == conn_b, (sc, dc)
    assert su == "e2e_alice" and du == "e2e_bob", (su, du)

    print("e2e_minimal: OK")
    a.close()
    b.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
