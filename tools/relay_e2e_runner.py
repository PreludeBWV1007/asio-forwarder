#!/usr/bin/env python3
"""非 Web 端到端：两客户端 LOGIN + HEARTBEAT + TRANSFER 单播 + 校验 DELIVER payload。"""
from __future__ import annotations

import socket
import sys

# 同目录导入
from relay_proto import MSG_DELIVER, MSG_SERVER_REPLY, pack_frame_msgpack, recv_frame, recv_frame_msgpack


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2] if len(sys.argv) > 2 else "0")

    a = socket.create_connection((host, port), timeout=10)
    b = socket.create_connection((host, port), timeout=10)

    a.sendall(pack_frame_msgpack({"op": "LOGIN", "user_id": 101, "role": "data"}, seq=1))
    ha, ba = recv_frame_msgpack(a)
    assert ha["msg_type"] == MSG_SERVER_REPLY, ha
    assert ba["ok"] is True and ba.get("op") == "LOGIN", ba

    b.sendall(pack_frame_msgpack({"op": "LOGIN", "user_id": 202, "role": "data"}, seq=1))
    hb, bb = recv_frame_msgpack(b)
    assert hb["msg_type"] == MSG_SERVER_REPLY, hb
    assert bb["ok"] is True, bb

    a.sendall(pack_frame_msgpack({"op": "HEARTBEAT"}, seq=2))
    h1, r1 = recv_frame_msgpack(a)
    assert h1["msg_type"] == MSG_SERVER_REPLY and r1["ok"] is True, (h1, r1)

    payload = b"unicast-e2e"
    a.sendall(
        pack_frame_msgpack(
            {"op": "TRANSFER", "mode": "unicast", "dst_user_id": 202, "payload": payload},
            seq=3,
        )
    )
    h2, r2 = recv_frame_msgpack(a)
    assert h2["msg_type"] == MSG_SERVER_REPLY and r2["ok"] is True, (h2, r2)

    hd, raw = recv_frame(b)
    assert hd["msg_type"] == MSG_DELIVER, hd
    assert hd["src_user_id"] == 101 and hd["dst_user_id"] == 202, hd
    assert raw == payload, raw

    print("relay_e2e_runner: OK")
    a.close()
    b.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
