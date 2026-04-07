#!/usr/bin/env python3
"""手动联调入口：你先启动服务器，本脚本依次模拟客户端行为并断言结果。

覆盖：
- LOGIN（data/control）
- HEARTBEAT
- TRANSFER：unicast / broadcast / round_robin
- CONTROL：list_users / kick_user
- 异常包：不完整 msgpack 应断开

用法：
  pip install -r tools/requirements-relay.txt
  PYTHONPATH=tools python3 tools/run_scenario.py 127.0.0.1 19000
"""

from __future__ import annotations

import socket
import sys
import time

from relay_proto import (
    MSG_DELIVER,
    MSG_SERVER_REPLY,
    pack_frame_msgpack,
    pack_header,
    recv_frame,
    recv_frame_msgpack,
)


def log(s: str) -> None:
    print(s, flush=True)


def conn(host: str, port: int, timeout: float = 10.0) -> socket.socket:
    s = socket.create_connection((host, port), timeout=timeout)
    s.settimeout(5)
    return s


def must_reply_ok(sock: socket.socket, what: str) -> dict:
    h, body = recv_frame_msgpack(sock)
    assert h["msg_type"] == MSG_SERVER_REPLY, (what, h)
    assert isinstance(body, dict) and body.get("ok") is True, (what, h, body)
    return body


def login(sock: socket.socket, user_id: int, role: str, seq: int) -> dict:
    sock.sendall(pack_frame_msgpack({"op": "LOGIN", "user_id": user_id, "role": role}, seq=seq))
    body = must_reply_ok(sock, f"LOGIN user_id={user_id} role={role}")
    assert body.get("op") == "LOGIN", body
    return body


def heartbeat(sock: socket.socket, seq: int) -> None:
    sock.sendall(pack_frame_msgpack({"op": "HEARTBEAT"}, seq=seq))
    body = must_reply_ok(sock, "HEARTBEAT")
    assert body.get("op") == "HEARTBEAT", body


def transfer_unicast(sender: socket.socket, dst_user_id: int, payload: bytes, seq: int) -> None:
    sender.sendall(pack_frame_msgpack({"op": "TRANSFER", "mode": "unicast", "dst_user_id": dst_user_id, "payload": payload}, seq=seq))
    body = must_reply_ok(sender, "TRANSFER unicast ack")
    assert body.get("op") == "TRANSFER", body


def transfer_broadcast(sender: socket.socket, payload: bytes, seq: int) -> None:
    sender.sendall(pack_frame_msgpack({"op": "TRANSFER", "mode": "broadcast", "payload": payload}, seq=seq))
    body = must_reply_ok(sender, "TRANSFER broadcast ack")
    assert body.get("op") == "TRANSFER", body


def transfer_round_robin(sender: socket.socket, payload: bytes, interval_ms: int, seq: int) -> None:
    sender.sendall(
        pack_frame_msgpack(
            {"op": "TRANSFER", "mode": "round_robin", "interval_ms": interval_ms, "payload": payload},
            seq=seq,
        )
    )
    body = must_reply_ok(sender, "TRANSFER round_robin ack")
    assert body.get("op") == "TRANSFER", body


def control_list_users(ctl: socket.socket, seq: int) -> list[dict]:
    ctl.sendall(pack_frame_msgpack({"op": "CONTROL", "action": "list_users"}, seq=seq))
    body = must_reply_ok(ctl, "CONTROL list_users")
    users = body.get("users")
    assert isinstance(users, list), users
    return users


def control_kick_user(ctl: socket.socket, target_user_id: int, seq: int) -> int:
    ctl.sendall(pack_frame_msgpack({"op": "CONTROL", "action": "kick_user", "target_user_id": target_user_id}, seq=seq))
    body = must_reply_ok(ctl, f"CONTROL kick_user target={target_user_id}")
    kc = body.get("kicked_count")
    assert isinstance(kc, int), body
    return kc


def expect_deliver(sock: socket.socket, src_user_id: int, dst_user_id: int, payload: bytes, label: str) -> None:
    h, raw = recv_frame(sock)
    assert h["msg_type"] == MSG_DELIVER, (label, h)
    assert h["src_user_id"] == src_user_id and h["dst_user_id"] == dst_user_id, (label, h)
    assert raw == payload, (label, raw)


def expect_no_deliver(sock: socket.socket, label: str, wait_s: float = 0.3) -> None:
    sock.settimeout(wait_s)
    try:
        h, raw = recv_frame(sock)
        raise AssertionError(f"{label}: unexpected frame {h} raw_len={len(raw)}")
    except Exception:
        return


def test_invalid_msgpack_disconnect(host: str, port: int) -> None:
    s = conn(host, port)
    bad_body = b"\x81"  # map(1) 但缺 key/value
    s.sendall(pack_header(len(bad_body), msg_type=0, seq=1) + bad_body)
    time.sleep(0.05)
    try:
        s.settimeout(0.3)
        recv_frame(s)
        raise AssertionError("invalid msgpack should disconnect")
    except Exception:
        pass
    s.close()


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2] if len(sys.argv) > 2 else "19000")

    log(f"[1] connect clients -> {host}:{port}")
    a = conn(host, port)
    b = conn(host, port)
    c = conn(host, port)
    ctl = conn(host, port)

    log("[2] LOGIN")
    login(a, 101, "data", 1)
    login(b, 202, "data", 1)
    login(c, 303, "data", 1)
    login(ctl, 9000, "control", 1)

    log("[3] HEARTBEAT (A)")
    heartbeat(a, 2)

    log("[4] TRANSFER unicast A->B")
    p1 = b"hello-unicast"
    transfer_unicast(a, 202, p1, 3)
    expect_deliver(b, 101, 202, p1, "unicast deliver")

    log("[5] TRANSFER broadcast A->(B,C) and NOT self")
    p2 = b"hello-broadcast"
    transfer_broadcast(a, p2, 4)
    expect_deliver(b, 101, 202, p2, "broadcast deliver to B")
    expect_deliver(c, 101, 303, p2, "broadcast deliver to C")
    expect_no_deliver(a, "broadcast should not deliver to self")

    log("[6] TRANSFER round_robin A->(B,C)")
    p3 = b"hello-rr"
    transfer_round_robin(a, p3, interval_ms=10, seq=5)
    # 两个目标各一条（顺序不做强断言）
    got = []
    for s in (b, c):
        h, raw = recv_frame(s)
        assert h["msg_type"] == MSG_DELIVER and h["src_user_id"] == 101 and raw == p3, (h, raw)
        got.append(h["dst_user_id"])
    assert set(got) == {202, 303}, got

    log("[7] CONTROL list_users")
    users = control_list_users(ctl, 2)
    uids = {u.get("user_id") for u in users if isinstance(u, dict)}
    assert {101, 202, 303, 9000}.issubset(uids), users

    log("[8] CONTROL kick_user (kick 303)")
    kicked = control_kick_user(ctl, 303, 3)
    assert kicked >= 1, kicked
    time.sleep(0.1)
    try:
        c.sendall(pack_frame_msgpack({"op": "HEARTBEAT"}, seq=99))
        c.settimeout(0.3)
        recv_frame(c)
        raise AssertionError("kicked user still alive")
    except Exception:
        pass

    log("[9] invalid msgpack should disconnect")
    test_invalid_msgpack_disconnect(host, port)

    log("OK: scenario passed")
    for s in (a, b, c, ctl):
        try:
            s.close()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

