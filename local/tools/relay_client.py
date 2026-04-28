#!/usr/bin/env python3
"""
生产级 Python 客户端接口（v3 头 + Header.msg_type = login/heartbeat/control/data）。

- DELIVER(200) body 为 msgpack：{payload, src_conn_id, dst_conn_id}
- SERVER_REPLY(201)、KICK(202)
"""

from __future__ import annotations

import queue
import socket
import threading
import time
from dataclasses import dataclass
from typing import Any, Callable, Optional

import msgpack

from forwarder_wire import (
    MSG_CLIENT_CONTROL,
    MSG_CLIENT_DATA,
    MSG_CLIENT_HEARTBEAT,
    MSG_CLIENT_LOGIN,
    MSG_DELIVER,
    MSG_KICK,
    MSG_SERVER_REPLY,
    pack_frame_msgpack,
    recv_frame,
    unpack_deliver_body,
)


@dataclass(frozen=True)
class Deliver:
    seq: int
    payload: bytes
    src_conn_id: int
    dst_conn_id: int
    src_username: str
    dst_username: str


@dataclass(frozen=True)
class ServerReply:
    seq: int
    body: dict[str, Any]


@dataclass(frozen=True)
class KickNotice:
    seq: int
    reason: str


class RelayClient:
    """一个 TCP 连接 = 一个 RelayClient；同一 username 可多连接（服务器上限 8）。"""

    def __init__(
        self,
        host: str,
        port: int,
        *,
        hb_interval_s: Optional[float] = 5.0,
        recv_timeout_s: float = 1.0,
    ) -> None:
        self.host = host
        self.port = port
        self.hb_interval_s = hb_interval_s
        self.recv_timeout_s = recv_timeout_s

        self._sock: Optional[socket.socket] = None
        self._stop = threading.Event()
        self._rx_th: Optional[threading.Thread] = None
        self._hb_th: Optional[threading.Thread] = None

        self._seq = 1
        self._seq_lock = threading.Lock()

        self.user_id: int = 0
        self.username: str = ""
        self.peer_role: str = "user"
        self.conn_id: int = 0
        self.logged_in: bool = False

        self.inbox_deliver: "queue.Queue[Deliver]" = queue.Queue()
        self.inbox_reply: "queue.Queue[ServerReply]" = queue.Queue()
        self.inbox_kick: "queue.Queue[KickNotice]" = queue.Queue()

        self.on_deliver: Optional[Callable[[Deliver], None]] = None
        self.on_reply: Optional[Callable[[ServerReply], None]] = None
        self.on_kick: Optional[Callable[[KickNotice], None]] = None
        self.on_closed: Optional[Callable[[Exception], None]] = None

    def _next_seq(self) -> int:
        with self._seq_lock:
            self._seq = (self._seq + 1) & 0x7FFFFFFF
            if self._seq == 0:
                self._seq = 1
            return self._seq

    def connect(self, timeout_s: float = 10.0) -> None:
        if self._sock is not None:
            raise RuntimeError("already connected")
        s = socket.create_connection((self.host, self.port), timeout=timeout_s)
        s.settimeout(self.recv_timeout_s)
        self._sock = s
        self._stop.clear()
        self._rx_th = threading.Thread(target=self._rx_loop, name="relay-rx", daemon=True)
        self._rx_th.start()

    def close(self) -> None:
        self._stop.set()
        self.logged_in = False
        s = self._sock
        self._sock = None
        if s is not None:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass
            try:
                s.close()
            except Exception:
                pass

    def login(
        self,
        username: str,
        password: str,
        *,
        peer_role: str = "user",
        recv_mode: str = "broadcast",
    ) -> int:
        if self._sock is None:
            raise RuntimeError("not connected")
        if peer_role not in ("user", "admin"):
            peer_role = "user"
        if recv_mode not in ("broadcast", "round_robin", "roundrobin"):
            recv_mode = "broadcast"
        seq = self._next_seq()
        obj = {
            "username": username,
            "password": password,
            "peer_role": peer_role,
            "recv_mode": recv_mode,
        }
        self._send_msgpack(obj, seq=seq, msg_type=MSG_CLIENT_LOGIN)
        if self._hb_th is None:
            self._hb_th = threading.Thread(target=self._hb_loop, name="relay-hb", daemon=True)
            self._hb_th.start()
        return seq

    def heartbeat(self) -> int:
        seq = self._next_seq()
        self._send_msgpack({}, seq=seq, msg_type=MSG_CLIENT_HEARTBEAT)
        return seq

    def send_data(self, dst_username: str, payload: bytes) -> int:
        seq = self._next_seq()
        self._send_msgpack(
            {"dst_username": str(dst_username), "payload": payload},
            seq=seq,
            msg_type=MSG_CLIENT_DATA,
        )
        return seq

    def send_data_obj(self, dst_username: str, obj: Any) -> int:
        payload = msgpack.packb(obj, use_bin_type=True)
        return self.send_data(dst_username, payload)

    def control_list_users(self) -> int:
        seq = self._next_seq()
        self._send_msgpack({"action": "list_users"}, seq=seq, msg_type=MSG_CLIENT_CONTROL)
        return seq

    def control_kick_user(self, target_user_id: int) -> int:
        seq = self._next_seq()
        self._send_msgpack(
            {"action": "kick_user", "target_user_id": int(target_user_id)},
            seq=seq,
            msg_type=MSG_CLIENT_CONTROL,
        )
        return seq

    def wait_reply(self, seq: int, timeout_s: float = 3.0) -> ServerReply:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                r = self.inbox_reply.get(timeout=0.1)
            except queue.Empty:
                continue
            if r.seq == seq:
                return r
            self.inbox_reply.put(r)
        raise TimeoutError(f"wait_reply timeout seq={seq}")

    def _send_msgpack(self, obj: dict[str, Any], *, seq: int, msg_type: int) -> None:
        if self._sock is None:
            raise RuntimeError("not connected")
        frame = pack_frame_msgpack(obj, msg_type=msg_type, seq=seq)
        self._sock.sendall(frame)

    def _rx_loop(self) -> None:
        assert self._sock is not None
        s = self._sock
        try:
            while not self._stop.is_set():
                try:
                    h, body_raw = recv_frame(s)
                except socket.timeout:
                    continue
                mt = int(h.get("msg_type", 0))
                if mt == MSG_DELIVER:
                    pl, sc, dc, su, du = unpack_deliver_body(body_raw or b"")
                    d = Deliver(
                        seq=int(h.get("seq", 0)),
                        payload=pl,
                        src_conn_id=sc,
                        dst_conn_id=dc,
                        src_username=su,
                        dst_username=du,
                    )
                    if self.on_deliver:
                        self.on_deliver(d)
                    else:
                        self.inbox_deliver.put(d)
                elif mt == MSG_SERVER_REPLY:
                    obj = msgpack.unpackb(body_raw, raw=False) if body_raw else {}
                    if isinstance(obj, dict) and obj.get("ok") is True and obj.get("op") == "LOGIN":
                        self.logged_in = True
                        self.user_id = int(obj.get("user_id", 0))
                        self.username = str(obj.get("username", ""))
                        self.peer_role = str(obj.get("peer_role", "user"))
                        self.conn_id = int(obj.get("conn_id", 0))
                    r = ServerReply(seq=int(h.get("seq", 0)), body=obj if isinstance(obj, dict) else {"raw": obj})
                    if self.on_reply:
                        self.on_reply(r)
                    else:
                        self.inbox_reply.put(r)
                elif mt == MSG_KICK:
                    obj = msgpack.unpackb(body_raw, raw=False) if body_raw else {}
                    reason = str(obj.get("reason", "")) if isinstance(obj, dict) else ""
                    kn = KickNotice(seq=int(h.get("seq", 0)), reason=reason)
                    if self.on_kick:
                        self.on_kick(kn)
                    else:
                        self.inbox_kick.put(kn)
                else:
                    continue
        except Exception as e:
            if self.on_closed:
                self.on_closed(e)
        finally:
            try:
                self.close()
            except Exception:
                pass

    def _hb_loop(self) -> None:
        while not self._stop.is_set():
            if not self.logged_in:
                time.sleep(0.2)
                continue
            if self.hb_interval_s is None or self.hb_interval_s <= 0:
                time.sleep(0.5)
                continue
            time.sleep(self.hb_interval_s)
            if self._stop.is_set() or not self.logged_in:
                continue
            try:
                self.heartbeat()
            except Exception:
                continue
