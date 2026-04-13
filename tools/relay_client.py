#!/usr/bin/env python3
"""
生产级 Python 客户端接口（最小可用版）。

设计目标：
- 屏蔽 v2 帧头与 msgpack 细节
- 提供：connect/login/close、自动心跳、send_*、接收循环（回调或队列）
- DELIVER(200) 为纯 payload bytes；SERVER_REPLY(201) 为 msgpack dict

注意：
- 当前服务器只提供“受理 ACK(201)”，不提供端到端已读/已处理 ACK。
"""

from __future__ import annotations

import queue
import socket
import threading
import time
from dataclasses import dataclass
from typing import Any, Callable, Optional

import msgpack

from relay_proto import MSG_DELIVER, MSG_SERVER_REPLY, pack_frame_msgpack, recv_frame


@dataclass(frozen=True)
class Deliver:
    src_user_id: int
    dst_user_id: int
    seq: int
    payload: bytes


@dataclass(frozen=True)
class ServerReply:
    seq: int
    body: dict[str, Any]


class RelayClient:
    """
    一个 TCP 连接 = 一个 RelayClient 实例。
    一个 user_id 可以多连接（由服务器策略管理）。
    """

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
        self.role: str = "data"
        self.logged_in: bool = False

        self.inbox_deliver: "queue.Queue[Deliver]" = queue.Queue()
        self.inbox_reply: "queue.Queue[ServerReply]" = queue.Queue()

        self.on_deliver: Optional[Callable[[Deliver], None]] = None
        self.on_reply: Optional[Callable[[ServerReply], None]] = None
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

    def login(self, user_id: int, role: str = "data") -> int:
        if self._sock is None:
            raise RuntimeError("not connected")
        if role not in ("data", "control"):
            role = "data"
        self.user_id = int(user_id)
        self.role = role
        seq = self._next_seq()
        self._send_msgpack({"op": "LOGIN", "user_id": self.user_id, "role": self.role}, seq=seq)
        # 自动心跳线程在登录后启动（避免未登录就发 HEARTBEAT）
        if self._hb_th is None:
            self._hb_th = threading.Thread(target=self._hb_loop, name="relay-hb", daemon=True)
            self._hb_th.start()
        return seq

    def heartbeat(self) -> int:
        seq = self._next_seq()
        self._send_msgpack({"op": "HEARTBEAT"}, seq=seq)
        return seq

    def send_unicast(self, dst_user_id: int, payload: bytes) -> int:
        seq = self._next_seq()
        self._send_msgpack(
            {"op": "TRANSFER", "mode": "unicast", "dst_user_id": int(dst_user_id), "payload": payload},
            seq=seq,
        )
        return seq

    def send_unicast_obj(self, dst_user_id: int, obj: Any) -> int:
        """
        方式 A：obj 用 msgpack 序列化为 bytes，作为 TRANSFER.payload(bin) 发送。
        obj 需为 msgpack 可序列化类型（dict/list/str/int/bytes...）。
        """
        payload = msgpack.packb(obj, use_bin_type=True)
        return self.send_unicast(dst_user_id, payload)

    def send_broadcast(self, payload: bytes) -> int:
        seq = self._next_seq()
        self._send_msgpack({"op": "TRANSFER", "mode": "broadcast", "payload": payload}, seq=seq)
        return seq

    def send_broadcast_obj(self, obj: Any) -> int:
        payload = msgpack.packb(obj, use_bin_type=True)
        return self.send_broadcast(payload)

    def send_round_robin(self, payload: bytes, interval_ms: int = 0) -> int:
        seq = self._next_seq()
        self._send_msgpack(
            {"op": "TRANSFER", "mode": "round_robin", "interval_ms": int(interval_ms), "payload": payload},
            seq=seq,
        )
        return seq

    def send_round_robin_obj(self, obj: Any, interval_ms: int = 0) -> int:
        payload = msgpack.packb(obj, use_bin_type=True)
        return self.send_round_robin(payload, interval_ms=interval_ms)

    def control_list_users(self) -> int:
        seq = self._next_seq()
        self._send_msgpack({"op": "CONTROL", "action": "list_users"}, seq=seq)
        return seq

    def control_kick_user(self, target_user_id: int) -> int:
        seq = self._next_seq()
        self._send_msgpack({"op": "CONTROL", "action": "kick_user", "target_user_id": int(target_user_id)}, seq=seq)
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
            # 非目标 seq 的 reply 放回队列尾部（简单实现）
            self.inbox_reply.put(r)
        raise TimeoutError(f"wait_reply timeout seq={seq}")

    def _send_msgpack(self, obj: dict[str, Any], *, seq: int) -> None:
        if self._sock is None:
            raise RuntimeError("not connected")
        frame = pack_frame_msgpack(obj, seq=seq)
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
                    d = Deliver(
                        src_user_id=int(h.get("src_user_id", 0)),
                        dst_user_id=int(h.get("dst_user_id", 0)),
                        seq=int(h.get("seq", 0)),
                        payload=body_raw or b"",
                    )
                    if self.on_deliver:
                        self.on_deliver(d)
                    else:
                        self.inbox_deliver.put(d)
                elif mt == MSG_SERVER_REPLY:
                    obj = msgpack.unpackb(body_raw, raw=False) if body_raw else {}
                    if isinstance(obj, dict) and obj.get("ok") is True and obj.get("op") == "LOGIN":
                        self.logged_in = True
                    r = ServerReply(seq=int(h.get("seq", 0)), body=obj if isinstance(obj, dict) else {"raw": obj})
                    if self.on_reply:
                        self.on_reply(r)
                    else:
                        self.inbox_reply.put(r)
                else:
                    # 忽略未知类型（可扩展）
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
                # 连接断了会在 rx_loop 收到异常并关闭
                continue

