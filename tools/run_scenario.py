#!/usr/bin/env python3
"""联调脚本：注册/登录、DATA 单播/广播/轮询、CONTROL、KICK、异常包。"""

from __future__ import annotations

import socket
import sys
import time

import msgpack

from relay_proto import (
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


def login(sock: socket.socket, username: str, password: str, peer_role: str, register: bool, seq: int) -> dict:
    body = msgpack.packb(
        {"username": username, "password": password, "peer_role": peer_role, "register": register},
        use_bin_type=True,
    )
    sock.sendall(pack_header(len(body), MSG_CLIENT_LOGIN, seq=seq) + body)
    body = must_reply_ok(sock, f"LOGIN {username}")
    assert body.get("op") == "LOGIN", body
    return body


def heartbeat(sock: socket.socket, seq: int) -> None:
    body = msgpack.packb({}, use_bin_type=True)
    sock.sendall(pack_header(len(body), MSG_CLIENT_HEARTBEAT, seq=seq) + body)
    body = must_reply_ok(sock, "HEARTBEAT")
    assert body.get("op") == "HEARTBEAT", body


def data_unicast(sender: socket.socket, dst_username: str, dst_conn: int, payload: bytes, seq: int) -> None:
    body = msgpack.packb(
        {"mode": "unicast", "dst_username": dst_username, "dst_conn_id": dst_conn, "payload": payload},
        use_bin_type=True,
    )
    sender.sendall(pack_header(len(body), MSG_CLIENT_DATA, seq=seq) + body)
    must_reply_ok(sender, "DATA unicast ack")


def data_broadcast(sender: socket.socket, dst_username: str, payload: bytes, seq: int) -> None:
    body = msgpack.packb({"mode": "broadcast", "dst_username": dst_username, "payload": payload}, use_bin_type=True)
    sender.sendall(pack_header(len(body), MSG_CLIENT_DATA, seq=seq) + body)
    must_reply_ok(sender, "DATA broadcast ack")


def data_round_robin(sender: socket.socket, dst_username: str, payload: bytes, interval_ms: int, seq: int) -> None:
    body = msgpack.packb(
        {"mode": "round_robin", "dst_username": dst_username, "interval_ms": interval_ms, "payload": payload},
        use_bin_type=True,
    )
    sender.sendall(pack_header(len(body), MSG_CLIENT_DATA, seq=seq) + body)
    must_reply_ok(sender, "DATA round_robin ack")


def control_list_users(ctl: socket.socket, seq: int) -> list[dict]:
    body = msgpack.packb({"action": "list_users"}, use_bin_type=True)
    ctl.sendall(pack_header(len(body), MSG_CLIENT_CONTROL, seq=seq) + body)
    body = must_reply_ok(ctl, "CONTROL list_users")
    users = body.get("users")
    assert isinstance(users, list), users
    return users


def control_kick_user(ctl: socket.socket, target_user_id: int, seq: int) -> int:
    body = msgpack.packb({"action": "kick_user", "target_user_id": target_user_id}, use_bin_type=True)
    ctl.sendall(pack_header(len(body), MSG_CLIENT_CONTROL, seq=seq) + body)
    body = must_reply_ok(ctl, f"CONTROL kick_user target={target_user_id}")
    kc = body.get("kicked_count")
    assert isinstance(kc, int), body
    return kc


def expect_deliver(sock: socket.socket, src_username: str, dst_username: str, payload: bytes, label: str) -> None:
    h, raw = recv_frame(sock)
    assert h["msg_type"] == MSG_DELIVER, (label, h)
    assert h["src_user_id"] == 0 and h["dst_user_id"] == 0, (label, h)
    pl, _, _, su, du = unpack_deliver_body(raw)
    assert pl == payload, (label, raw)
    assert su == src_username and du == dst_username, (label, su, du)


def expect_no_deliver(sock: socket.socket, label: str, wait_s: float = 0.3) -> None:
    sock.settimeout(wait_s)
    try:
        h, raw = recv_frame(sock)
        raise AssertionError(f"{label}: unexpected frame {h} raw_len={len(raw)}")
    except Exception:
        return


def test_invalid_msgpack_disconnect(host: str, port: int) -> None:
    s = conn(host, port)
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


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2] if len(sys.argv) > 2 else "19000")

    log(f"[1] connect clients -> {host}:{port}")
    a = conn(host, port)
    b = conn(host, port)
    c = conn(host, port)
    ctl = conn(host, port)

    log("[2] register / login")
    la = login(a, "sc_alice", "pa", "user", True, 1)
    lb = login(b, "sc_bob", "pb", "user", True, 1)
    lc = login(c, "sc_carol", "pc", "user", True, 1)
    lctl = login(ctl, "sc_admin", "padm", "admin", True, 1)
    conn_b = int(lb["conn_id"])
    conn_c = int(lc["conn_id"])
    int(lctl["user_id"])

    log("[3] HEARTBEAT (A)")
    heartbeat(a, 2)

    log("[4] DATA unicast A->B")
    p1 = b"hello-unicast"
    data_unicast(a, "sc_bob", conn_b, p1, 3)
    expect_deliver(b, "sc_alice", "sc_bob", p1, "unicast deliver")

    log("[5] DATA broadcast A->B 的全部连接（此处仅 B 单连接）")
    p2 = b"hello-broadcast"
    data_broadcast(a, "sc_bob", p2, 4)
    expect_deliver(b, "sc_alice", "sc_bob", p2, "broadcast deliver to B")
    expect_no_deliver(a, "broadcast should not deliver to sender")

    log("[6] DATA round_robin A->C（单连接）")
    p3 = b"hello-rr"
    data_round_robin(a, "sc_carol", p3, interval_ms=10, seq=5)
    expect_deliver(c, "sc_alice", "sc_carol", p3, "rr deliver")

    log("[7] CONTROL list_users")
    users = control_list_users(ctl, 6)
    uids = {int(u["user_id"]) for u in users if isinstance(u, dict) and "user_id" in u}
    assert len(uids) >= 3, users

    log("[8] CONTROL kick_user (kick C)")
    kicked = control_kick_user(ctl, int(lc["user_id"]), 7)
    assert kicked >= 1, kicked
    hk, kb = recv_frame(c)
    assert hk["msg_type"] == MSG_KICK
    kobj = msgpack.unpackb(kb, raw=False)
    assert isinstance(kobj, dict) and kobj.get("op") == "KICK" and "reason" in kobj
    time.sleep(0.1)
    try:
        body = msgpack.packb({}, use_bin_type=True)
        c.sendall(pack_header(len(body), MSG_CLIENT_HEARTBEAT, seq=99) + body)
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
