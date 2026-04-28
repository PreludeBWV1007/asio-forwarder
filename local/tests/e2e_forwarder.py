#!/usr/bin/env python3
"""一体化黑盒回归：原 e2e_minimal + e2e_suite。用法：python3 e2e_forwarder.py HOST PORT"""

from __future__ import annotations

import socket
import sys
import time

import msgpack

from forwarder_wire import (
    MSG_CLIENT_CONTROL,
    MSG_CLIENT_DATA,
    MSG_CLIENT_HEARTBEAT,
    MSG_CLIENT_LOGIN,
    MSG_DELIVER,
    MSG_KICK,
    MSG_SERVER_REPLY,
    pack_header,
    recv_frame,
    recv_frame_msgpack,
    unpack_deliver_body,
)


def run_minimal(host: str, port: int) -> None:
    a = socket.create_connection((host, port), timeout=10)
    b = socket.create_connection((host, port), timeout=10)

    def send_login(sock: socket.socket, *, user: str, pw: str, role: str, recv_mode: str, seq: int) -> None:
        body = msgpack.packb(
            {"username": user, "password": pw, "peer_role": role, "recv_mode": recv_mode},
            use_bin_type=True,
        )
        sock.sendall(pack_header(len(body), MSG_CLIENT_LOGIN, seq=seq) + body)

    send_login(a, user="e2e_alice", pw="secret1", role="user", recv_mode="broadcast", seq=1)
    ha, ba = recv_frame_msgpack(a)
    assert ha["msg_type"] == MSG_SERVER_REPLY, ha
    assert isinstance(ba, dict) and ba.get("ok") is True and ba.get("op") == "LOGIN", ba
    conn_a = int(ba["conn_id"])

    send_login(b, user="e2e_bob", pw="secret2", role="user", recv_mode="broadcast", seq=1)
    hb, bb = recv_frame_msgpack(b)
    assert hb["msg_type"] == MSG_SERVER_REPLY, hb
    assert isinstance(bb, dict) and bb.get("ok") is True, bb
    conn_b = int(bb["conn_id"])

    hb_body = msgpack.packb({}, use_bin_type=True)
    a.sendall(pack_header(len(hb_body), MSG_CLIENT_HEARTBEAT, seq=2) + hb_body)
    h1, r1 = recv_frame_msgpack(a)
    assert h1["msg_type"] == MSG_SERVER_REPLY and r1.get("ok") is True, (h1, r1)

    payload = b"unicast-e2e"
    data_body = msgpack.packb({"dst_username": "e2e_bob", "payload": payload}, use_bin_type=True)
    a.sendall(pack_header(len(data_body), MSG_CLIENT_DATA, seq=3) + data_body)
    h2, r2 = recv_frame_msgpack(a)
    assert h2["msg_type"] == MSG_SERVER_REPLY and r2.get("ok") is True, (h2, r2)

    hd, raw = recv_frame(b)
    assert hd["msg_type"] == MSG_DELIVER, hd
    pl, sc, dc, su, du = unpack_deliver_body(raw)
    assert pl == payload, pl
    assert sc == conn_a and dc == conn_b, (sc, dc)
    assert su == "e2e_alice" and du == "e2e_bob", (su, du)

    print("e2e_minimal: OK")
    a.close()
    b.close()


def _conn(host: str, port: int) -> socket.socket:
    s = socket.create_connection((host, port), timeout=10)
    s.settimeout(5)
    return s


def _send_login(s: socket.socket, user: str, pw: str, role: str, recv_mode: str, seq: int = 1) -> dict:
    body = msgpack.packb(
        {"username": user, "password": pw, "peer_role": role, "recv_mode": recv_mode},
        use_bin_type=True,
    )
    s.sendall(pack_header(len(body), MSG_CLIENT_LOGIN, seq=seq) + body)
    h, body_obj = recv_frame_msgpack(s)
    assert h["msg_type"] == MSG_SERVER_REPLY, h
    assert isinstance(body_obj, dict) and body_obj.get("ok") is True and body_obj.get("op") == "LOGIN", body_obj
    return body_obj


def _hb(s: socket.socket, seq: int = 1) -> None:
    body = msgpack.packb({}, use_bin_type=True)
    s.sendall(pack_header(len(body), MSG_CLIENT_HEARTBEAT, seq=seq) + body)
    h, body_obj = recv_frame_msgpack(s)
    assert h["msg_type"] == MSG_SERVER_REPLY and body_obj.get("ok") is True, (h, body_obj)


def test_data_to_one_connection(host: str, port: int) -> None:
    a = _conn(host, port)
    b = _conn(host, port)
    ua = _send_login(a, "su_alice", "p1", "user", "broadcast", 1)
    ub = _send_login(b, "su_bob", "p2", "user", "broadcast", 1)
    ca = int(ua["conn_id"])
    cb = int(ub["conn_id"])
    payload = b"data-suite-1"
    body = msgpack.packb({"dst_username": "su_bob", "payload": payload}, use_bin_type=True)
    a.sendall(pack_header(len(body), MSG_CLIENT_DATA, seq=2) + body)
    h_ack, b_ack = recv_frame_msgpack(a)
    assert h_ack["msg_type"] == MSG_SERVER_REPLY and b_ack.get("ok") is True, (h_ack, b_ack)
    h_del, raw = recv_frame(b)
    assert h_del["msg_type"] == MSG_DELIVER, h_del
    pl, sc, dc, su, du = unpack_deliver_body(raw)
    assert pl == payload and sc == ca and dc == cb
    assert su == "su_alice" and du == "su_bob"
    a.close()
    b.close()


def test_broadcast_to_user_connections(host: str, port: int) -> None:
    a = _conn(host, port)
    b1 = _conn(host, port)
    b2 = _conn(host, port)
    _send_login(a, "sb_alice", "p1", "user", "broadcast", 1)
    _send_login(b1, "sb_bob", "p2", "user", "broadcast", 1)
    _send_login(b2, "sb_bob", "p2", "user", "broadcast", 1)
    payload = b"broadcast-suite"
    body = msgpack.packb({"dst_username": "sb_bob", "payload": payload}, use_bin_type=True)
    a.sendall(pack_header(len(body), MSG_CLIENT_DATA, seq=2) + body)
    h_ack, b_ack = recv_frame_msgpack(a)
    assert h_ack["msg_type"] == MSG_SERVER_REPLY and b_ack.get("ok") is True, (h_ack, b_ack)

    for s in (b1, b2):
        h, raw = recv_frame(s)
        assert h["msg_type"] == MSG_DELIVER
        pl, _, _, su, du = unpack_deliver_body(raw)
        assert pl == payload
        assert su == "sb_alice" and du == "sb_bob"

    a.settimeout(0.3)
    try:
        recv_frame(a)
        raise AssertionError("sender should not receive own broadcast")
    except Exception:
        pass

    a.close()
    b1.close()
    b2.close()


def test_round_robin_messages_on_target_user(host: str, port: int) -> None:
    a = _conn(host, port)
    b1 = _conn(host, port)
    b2 = _conn(host, port)
    _send_login(a, "rr2_alice", "p1", "user", "broadcast", 1)
    _send_login(b1, "rr2_bob", "p2", "user", "round_robin", 1)
    _send_login(b2, "rr2_bob", "p2", "user", "round_robin", 1)
    p1 = b"msg-1"
    p2 = b"msg-2"
    body1 = msgpack.packb({"dst_username": "rr2_bob", "payload": p1}, use_bin_type=True)
    a.sendall(pack_header(len(body1), MSG_CLIENT_DATA, seq=2) + body1)
    h_ack, b_ack = recv_frame_msgpack(a)
    assert b_ack.get("ok") is True
    body2 = msgpack.packb({"dst_username": "rr2_bob", "payload": p2}, use_bin_type=True)
    a.sendall(pack_header(len(body2), MSG_CLIENT_DATA, seq=3) + body2)
    h_ack2, b_ack2 = recv_frame_msgpack(a)
    assert b_ack2.get("ok") is True

    got = {}
    for s, label in ((b1, "b1"), (b2, "b2")):
        h, raw = recv_frame(s)
        pl, _, _, su, du = unpack_deliver_body(raw)
        got[label] = (pl, su, du)
    assert got["b1"][0] != got["b2"][0]
    assert {got["b1"][0], got["b2"][0]} == {p1, p2}
    assert got["b1"][1] == "rr2_alice" and got["b1"][2] == "rr2_bob"

    a.close()
    b1.close()
    b2.close()


def test_control_list_and_kick(host: str, port: int) -> None:
    ctl = _conn(host, port)
    u = _conn(host, port)
    _send_login(ctl, "adm_kick2", "pwadm", "admin", "broadcast", 1)
    body_u = _send_login(u, "usr_kick2", "pwu", "user", "broadcast", 1)
    uid_u = int(body_u["user_id"])

    cbody = msgpack.packb({"action": "list_users"}, use_bin_type=True)
    ctl.sendall(pack_header(len(cbody), MSG_CLIENT_CONTROL, seq=2) + cbody)
    h, body = recv_frame_msgpack(ctl)
    assert h["msg_type"] == MSG_SERVER_REPLY and body.get("ok") is True
    users = body.get("users")
    assert isinstance(users, list) and any(
        isinstance(x, dict) and int(x.get("user_id", 0)) == uid_u for x in users
    )

    kbody = msgpack.packb({"action": "kick_user", "target_user_id": uid_u}, use_bin_type=True)
    ctl.sendall(pack_header(len(kbody), MSG_CLIENT_CONTROL, seq=3) + kbody)
    h2, body2 = recv_frame_msgpack(ctl)
    assert body2.get("ok") is True and body2.get("kicked_count", 0) >= 1

    hk, kickb = recv_frame(u)
    assert hk["msg_type"] == MSG_KICK
    kick_obj = msgpack.unpackb(kickb, raw=False)
    assert isinstance(kick_obj, dict) and kick_obj.get("op") == "KICK" and "reason" in kick_obj

    time.sleep(0.15)
    try:
        u.sendall(pack_header(1, MSG_CLIENT_HEARTBEAT, seq=9) + b"\x80")
        u.settimeout(0.3)
        recv_frame(u)
    except Exception:
        pass

    ctl.close()
    u.close()


def test_invalid_msgpack_disconnect(host: str, port: int) -> None:
    s = _conn(host, port)
    bad_body = b"\x81"
    s.sendall(pack_header(len(bad_body), MSG_CLIENT_LOGIN, seq=1) + bad_body)
    time.sleep(0.05)
    try:
        s.settimeout(0.3)
        recv_frame(s)
        raise AssertionError("invalid msgpack should disconnect")
    except Exception:
        pass
    s.close()


def run_suite(host: str, port: int) -> None:
    _ = _conn(host, port)
    _.close()

    test_data_to_one_connection(host, port)
    test_broadcast_to_user_connections(host, port)
    test_round_robin_messages_on_target_user(host, port)
    test_control_list_and_kick(host, port)
    test_invalid_msgpack_disconnect(host, port)

    print("e2e_suite: OK")


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2] if len(sys.argv) > 2 else "0")

    run_minimal(host, port)
    run_suite(host, port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
