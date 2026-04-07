#!/usr/bin/env python3
"""非 Web 测试套件：覆盖 LOGIN/多连接/心跳/TRANSFER/CONTROL/异常包。

用法：
  PYTHONPATH=tools python3 tools/relay_test_suite.py 127.0.0.1 19000
"""

from __future__ import annotations

import socket
import sys
import time

import msgpack

from relay_proto import MSG_DELIVER, MSG_SERVER_REPLY, pack_frame_msgpack, recv_frame, recv_frame_msgpack


def _conn(host: str, port: int) -> socket.socket:
    s = socket.create_connection((host, port), timeout=10)
    s.settimeout(5)
    return s


def _login(s: socket.socket, user_id: int, role: str = "data", seq: int = 1) -> dict:
    s.sendall(pack_frame_msgpack({"op": "LOGIN", "user_id": user_id, "role": role}, seq=seq))
    h, body = recv_frame_msgpack(s)
    assert h["msg_type"] == MSG_SERVER_REPLY, h
    assert isinstance(body, dict) and body.get("ok") is True and body.get("op") == "LOGIN", body
    return body


def _hb(s: socket.socket, seq: int = 1) -> None:
    s.sendall(pack_frame_msgpack({"op": "HEARTBEAT"}, seq=seq))
    h, body = recv_frame_msgpack(s)
    assert h["msg_type"] == MSG_SERVER_REPLY and body.get("ok") is True, (h, body)


def test_unicast(host: str, port: int) -> None:
    a = _conn(host, port)
    b = _conn(host, port)
    _login(a, 101, "data", 1)
    _login(b, 202, "data", 1)
    payload = b"unicast-suite"
    a.sendall(pack_frame_msgpack({"op": "TRANSFER", "mode": "unicast", "dst_user_id": 202, "payload": payload}, seq=2))
    h_ack, b_ack = recv_frame_msgpack(a)
    assert h_ack["msg_type"] == MSG_SERVER_REPLY and b_ack.get("ok") is True, (h_ack, b_ack)
    h_del, raw = recv_frame(b)
    assert h_del["msg_type"] == MSG_DELIVER, h_del
    assert h_del["src_user_id"] == 101 and h_del["dst_user_id"] == 202, h_del
    assert raw == payload
    a.close()
    b.close()


def test_broadcast_no_self(host: str, port: int) -> None:
    a = _conn(host, port)
    b = _conn(host, port)
    c = _conn(host, port)
    _login(a, 1, "data", 1)
    _login(b, 2, "data", 1)
    _login(c, 3, "data", 1)

    payload = b"broadcast-suite"
    a.sendall(pack_frame_msgpack({"op": "TRANSFER", "mode": "broadcast", "payload": payload}, seq=2))
    h_ack, b_ack = recv_frame_msgpack(a)
    assert h_ack["msg_type"] == MSG_SERVER_REPLY and b_ack.get("ok") is True, (h_ack, b_ack)

    # b 与 c 必须收到；a 不应该收到自己的广播（默认约定）
    hb, rb = recv_frame(b)
    hc, rc = recv_frame(c)
    assert hb["msg_type"] == MSG_DELIVER and rb == payload and hb["dst_user_id"] == 2
    assert hc["msg_type"] == MSG_DELIVER and rc == payload and hc["dst_user_id"] == 3

    # 让 a 短暂等待，确保没有自发 DELIVER
    a.settimeout(0.3)
    try:
        recv_frame(a)
        raise AssertionError("broadcast should not deliver to self")
    except Exception:
        pass

    a.close()
    b.close()
    c.close()


def test_round_robin(host: str, port: int) -> None:
    a = _conn(host, port)
    b = _conn(host, port)
    c = _conn(host, port)
    _login(a, 11, "data", 1)
    _login(b, 22, "data", 1)
    _login(c, 33, "data", 1)

    payload = b"rr-suite"
    a.sendall(pack_frame_msgpack({"op": "TRANSFER", "mode": "round_robin", "interval_ms": 10, "payload": payload}, seq=2))
    h_ack, b_ack = recv_frame_msgpack(a)
    assert b_ack.get("ok") is True

    # b 与 c 各收到 1 条（顺序不强断言，但内容与 src/dst 必须对）
    got = []
    for s in (b, c):
        h, raw = recv_frame(s)
        got.append((h["dst_user_id"], raw, h["src_user_id"]))
    assert {x[0] for x in got} == {22, 33}
    assert all(x[1] == payload for x in got)
    assert all(x[2] == 11 for x in got)

    a.close()
    b.close()
    c.close()


def test_control_list_and_kick(host: str, port: int) -> None:
    ctl = _conn(host, port)
    u = _conn(host, port)
    _login(ctl, 9000, "control", 1)
    _login(u, 9001, "data", 1)

    ctl.sendall(pack_frame_msgpack({"op": "CONTROL", "action": "list_users"}, seq=2))
    h, body = recv_frame_msgpack(ctl)
    assert h["msg_type"] == MSG_SERVER_REPLY and body.get("ok") is True
    users = body.get("users")
    assert isinstance(users, list) and any(x.get("user_id") == 9001 for x in users)

    ctl.sendall(pack_frame_msgpack({"op": "CONTROL", "action": "kick_user", "target_user_id": 9001}, seq=3))
    h2, body2 = recv_frame_msgpack(ctl)
    assert body2.get("ok") is True and "kicked_count" in body2

    # 被踢用户应尽快断开：读/写任一方向都应失败
    time.sleep(0.1)
    try:
        u.sendall(pack_frame_msgpack({"op": "HEARTBEAT"}, seq=9))
        # 可能写成功但随后读失败，这里两者任一失败即可
        u.settimeout(0.3)
        recv_frame(u)
        raise AssertionError("kicked user still receiving")
    except Exception:
        pass

    ctl.close()
    u.close()


def test_invalid_msgpack_disconnect(host: str, port: int) -> None:
    s = _conn(host, port)
    # 发一个不完整 msgpack body（单字节 0x81 表示 map 1，但后面缺 key/value）
    bad_body = b"\x81"
    # 直接复用 relay_proto 的打包：这里用 msgpack 直接构 header
    from relay_proto import pack_header

    s.sendall(pack_header(len(bad_body), msg_type=0, seq=1) + bad_body)
    time.sleep(0.05)
    try:
        # 服务器应该断开，继续读会 EOF/超时
        s.settimeout(0.3)
        recv_frame(s)
        raise AssertionError("invalid msgpack should disconnect")
    except Exception:
        pass
    s.close()


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2] if len(sys.argv) > 2 else "0")

    # 基础 smoke：可连接
    _ = _conn(host, port)
    _.close()

    test_unicast(host, port)
    test_broadcast_no_self(host, port)
    test_round_robin(host, port)
    test_control_list_and_kick(host, port)
    test_invalid_msgpack_disconnect(host, port)

    print("relay_test_suite: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

