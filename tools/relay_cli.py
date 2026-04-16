#!/usr/bin/env python3
"""
交互式 Relay 测试控制台（对等中继协议）。

目标：
- 在一个终端里创建/关闭多个客户端连接
- 对每条连接执行 login(1)/heartbeat(2)/control(3)/data(4) 帧
- 实时打印收到的 DELIVER/SERVER_REPLY，并汇总统计（吞吐、错误、延迟）
- 可选：一键拉起本地 build/asio_forwarder（--spawn-server）

用法示例：
  PYTHONPATH=tools python3 tools/relay_cli.py --host 127.0.0.1 --port 19000
  PYTHONPATH=tools python3 tools/relay_cli.py --spawn-server

进入后输入 help 查看命令。
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Optional

import msgpack

from relay_proto import (
    MSG_CLIENT_CONTROL,
    MSG_CLIENT_DATA,
    MSG_CLIENT_HEARTBEAT,
    MSG_CLIENT_LOGIN,
    MSG_DELIVER,
    MSG_KICK,
    MSG_SERVER_REPLY,
    pack_frame_msgpack,
    recv_frame,
    recv_frame_msgpack,
    unpack_deliver_body,
)


def _now_ms() -> int:
    return int(time.time() * 1000)


def _fmt_bytes(n: int) -> str:
    if n < 1024:
        return f"{n}B"
    if n < 1024 * 1024:
        return f"{n/1024:.1f}KiB"
    if n < 1024 * 1024 * 1024:
        return f"{n/1024/1024:.1f}MiB"
    return f"{n/1024/1024/1024:.1f}GiB"


def _load_payload(spec: str) -> bytes:
    """
    payload 输入格式：
    - 普通字符串：原样 UTF-8 编码
    - @path：读取文件二进制
    - hex:...：十六进制字符串（可含空格）
    """
    if spec.startswith("@"):
        p = spec[1:]
        with open(p, "rb") as f:
            return f.read()
    if spec.startswith("hex:"):
        h = spec[4:].replace(" ", "").replace("\n", "").replace("\t", "")
        return bytes.fromhex(h)
    return spec.encode("utf-8", errors="strict")


@dataclass
class ConnStats:
    frames_in: int = 0
    bytes_in: int = 0
    deliver_in: int = 0
    replies_in: int = 0
    frames_out: int = 0
    bytes_out: int = 0
    errors: int = 0
    last_error: str = ""

    # 简单延迟：按 seq 记录发出时间，仅统计 SERVER_REPLY
    inflight: dict[int, int] = field(default_factory=dict)
    rtt_samples_ms: list[int] = field(default_factory=list)


class RelayConn:
    def __init__(self, name: str, host: str, port: int) -> None:
        self.name = name
        self.host = host
        self.port = port
        self.sock: Optional[socket.socket] = None
        self.user_id: int = 0
        self.username: str = ""
        self.peer_role: str = "user"
        self.conn_id: int = 0
        self.logged_in: bool = False
        # 每连接心跳间隔（秒）；None 表示不自动心跳
        self.hb_interval_s: Optional[float] = 5.0
        self._next_hb_ms: int = 0
        self._rx_th: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self.inbox: "queue.Queue[tuple[str, Any]]" = queue.Queue()
        self.stats = ConnStats()

    def connect(self, timeout: float = 10.0) -> None:
        if self.sock is not None:
            raise RuntimeError(f"{self.name} already connected")
        s = socket.create_connection((self.host, self.port), timeout=timeout)
        s.settimeout(1.0)
        self.sock = s
        self._stop.clear()
        self._rx_th = threading.Thread(target=self._rx_loop, name=f"rx-{self.name}", daemon=True)
        self._rx_th.start()

    def close(self) -> None:
        self._stop.set()
        s = self.sock
        self.sock = None
        self.logged_in = False
        self.stats.inflight.clear()
        if s is not None:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass
            try:
                s.close()
            except Exception:
                pass

    def is_connected(self) -> bool:
        return self.sock is not None

    def _send(self, frame: bytes, seq: int = 0) -> None:
        if self.sock is None:
            raise RuntimeError(f"{self.name} not connected")
        if seq:
            self.stats.inflight[seq] = _now_ms()
        self.sock.sendall(frame)
        self.stats.frames_out += 1
        self.stats.bytes_out += len(frame)

    def send_msgpack(self, obj: dict, *, seq: int = 0, msg_type: int = 0) -> None:
        frame = pack_frame_msgpack(obj, seq=seq, msg_type=msg_type)
        self._send(frame, seq=seq)

    def _rx_loop(self) -> None:
        assert self.sock is not None
        s = self.sock
        while not self._stop.is_set():
            try:
                h, body_raw = recv_frame(s)
            except socket.timeout:
                continue
            except Exception as e:
                self.stats.errors += 1
                self.stats.last_error = f"rx error: {e!r}"
                self.inbox.put(("closed", self.stats.last_error))
                # 确保 UI 状态从 UP 变为 DOWN，避免“假 UP”
                try:
                    self.close()
                except Exception:
                    pass
                break

            self.stats.frames_in += 1
            self.stats.bytes_in += 40 + (len(body_raw) if body_raw else 0)
            mt = int(h.get("msg_type", 0))
            if mt == MSG_DELIVER:
                self.stats.deliver_in += 1
                self.inbox.put(("deliver", (h, body_raw)))
            elif mt == MSG_KICK:
                try:
                    kobj = msgpack.unpackb(body_raw, raw=False) if body_raw else {}
                except Exception:
                    kobj = {}
                self.inbox.put(("kick", (h, kobj)))
            elif mt == MSG_SERVER_REPLY:
                self.stats.replies_in += 1
                try:
                    obj = msgpack.unpackb(body_raw, raw=False) if body_raw else None
                except Exception as e:
                    self.stats.errors += 1
                    self.stats.last_error = f"bad msgpack reply: {e!r}"
                    self.inbox.put(("reply", (h, {"ok": False, "error": "bad msgpack"})))
                    continue
                seq = int(h.get("seq", 0))
                if seq and seq in self.stats.inflight:
                    sent_ms = self.stats.inflight.pop(seq)
                    self.stats.rtt_samples_ms.append(_now_ms() - sent_ms)
                if isinstance(obj, dict) and obj.get("ok") is True and obj.get("op") == "LOGIN":
                    self.logged_in = True
                    self.user_id = int(obj.get("user_id", 0))
                    self.username = str(obj.get("username", ""))
                    self.peer_role = str(obj.get("peer_role", "user"))
                    self.conn_id = int(obj.get("conn_id", 0))
                self.inbox.put(("reply", (h, obj)))
            else:
                self.inbox.put(("frame", (h, body_raw)))


class RelayCLI:
    def __init__(self, host: str, port: int, admin_port: int = 0) -> None:
        self.host = host
        self.port = port
        self.admin_port = admin_port
        self.conns: dict[str, RelayConn] = {}
        self._seq = 1
        self._print_lock = threading.Lock()
        self._running = True
        # 默认自动心跳（每连接可覆盖/关闭）
        self._default_hb_interval_s: Optional[float] = 5.0
        self._hb_th = threading.Thread(target=self._hb_loop, name="hb", daemon=True)
        self._hb_th.start()

        self._pump_th = threading.Thread(target=self._pump_loop, name="pump", daemon=True)
        self._pump_th.start()

    def _p(self, s: str) -> None:
        with self._print_lock:
            print(s, flush=True)

    def _next_seq(self) -> int:
        self._seq = (self._seq + 1) & 0x7FFFFFFF
        if self._seq == 0:
            self._seq = 1
        return self._seq

    def _pump_loop(self) -> None:
        while self._running:
            any_msg = False
            for c in list(self.conns.values()):
                while True:
                    try:
                        typ, payload = c.inbox.get_nowait()
                    except queue.Empty:
                        break
                    any_msg = True
                    if typ == "deliver":
                        h, raw = payload
                        try:
                            pl, sc, dc, su, du = unpack_deliver_body(raw)
                            preview = pl[:120]
                            tail = "..." if len(pl) > len(preview) else ""
                            self._p(
                                f"[DELIVER] conn={c.name} src={h.get('src_user_id')} dst={h.get('dst_user_id')} "
                                f"src_conn={sc} dst_conn={dc} seq={h.get('seq')} plen={len(pl)} preview={preview!r}{tail}"
                                f" src_user={su!r} dst_user={du!r}"
                            )
                        except Exception as e:
                            self._p(f"[DELIVER] conn={c.name} decode err={e!r} raw_len={len(raw)}")
                    elif typ == "kick":
                        h, kobj = payload
                        self._p(f"[KICK]    conn={c.name} seq={h.get('seq')} body={kobj}")
                    elif typ == "reply":
                        h, obj = payload
                        # 降噪：自动心跳会频繁收到 ACK，默认不打印（避免刷屏影响交互输入）。
                        if isinstance(obj, dict) and obj.get("ok") is True and obj.get("op") == "HEARTBEAT":
                            continue
                        self._p(f"[REPLY]   conn={c.name} seq={h.get('seq')} body={obj}")
                    elif typ == "closed":
                        self._p(f"[CLOSED]  conn={c.name} {payload}")
                    else:
                        h, raw = payload
                        self._p(f"[FRAME]   conn={c.name} type={h.get('msg_type')} seq={h.get('seq')} len={len(raw)}")
            if not any_msg:
                time.sleep(0.03)

    def _hb_loop(self) -> None:
        while self._running:
            now = _now_ms()
            # 细粒度调度，避免所有连接同一时刻心跳
            time.sleep(0.2)
            if not self._running:
                break
            for c in list(self.conns.values()):
                if not c.is_connected() or not c.logged_in:
                    continue
                if c.hb_interval_s is None or c.hb_interval_s <= 0:
                    continue
                if c._next_hb_ms == 0:
                    c._next_hb_ms = now + int(c.hb_interval_s * 1000)
                    continue
                if now < c._next_hb_ms:
                    continue
                try:
                    seq = self._next_seq()
                    c.send_msgpack({}, seq=seq, msg_type=MSG_CLIENT_HEARTBEAT)
                except Exception:
                    # 不刷屏；错误会在 rx_loop/下一次操作时体现
                    pass
                finally:
                    c._next_hb_ms = now + int(c.hb_interval_s * 1000)

    def help(self) -> None:
        self._p(
            "\n".join(
                [
                    "命令：",
                    "  help",
                    "  quit / exit",
                    "",
                    "连接管理：",
                    "  connect <name> [hb_s]          # 建立 TCP 连接（未登录）；可选每连接心跳秒数",
                    "  close <name>                   # 关闭连接",
                    "  list                            # 列出连接及状态",
                    "",
                    "登录与心跳：",
                    "  login <name> <user|admin> <username> <password> [register] [hb_s]",
                    "    # 末尾 register 表示注册新账号；否则为登录",
                    "  hb <name>",
                    "  sethb <name> <hb_s|off>         # 动态修改自动心跳；off 表示关闭",
                    "",
                    "转发（payload 支持: 文本 / @文件 / hex:...）：",
                    "  send <from> unicast <dst_username> <dst_conn_id> <payload>   # dst_conn_id=0 自动选连接",
                    "  send <from> broadcast <dst_username> <payload>            # 发往目标用户全部连接",
                    "  send <from> round_robin <dst_username> <interval_ms> <payload>",
                    "",
                    "对象/结构体（msgpack）转发：",
                    "  sendmp <from> unicast <dst_username> <dst_conn_id> <json>",
                    "  sendmp <from> broadcast <dst_username> <json>",
                    "  sendmp <from> round_robin <dst_username> <interval_ms> <json>",
                    "    - json: 标准 JSON 字符串（会被解析为对象，再用 msgpack 打包成 payload(bin)）",
                    "",
                    "控制面（SERVER_REPLY）：",
                    "  ctl <from> list_users",
                    "  ctl <from> kick_user <target_user_id>",
                    "",
                    "观测与统计：",
                    "  stats [name]                    # 不带 name 则打印所有连接统计",
                    "  admin health|stats|events|users   # 需提供 --admin-port 或 --spawn-server",
                    "",
                    "压力/稳定性：",
                    "  flood <from> unicast <dst_username> <dst_conn_id> <count> <bytes> [window=64]",
                    "  flood <from> broadcast <dst_username> <count> <bytes> [window=64]",
                    "  flood <from> round_robin <dst_username> <interval_ms> <count> <bytes> [window=64]",
                    "    - bytes: 每条 payload 大小（随机填充）",
                    "    - window: 允许在途 seq 数（越大越压）",
                    "",
                    "辅助：",
                    "  wait <seconds>                 # 仅等待（便于观察异步打印）",
                ]
            )
        )

    def _get(self, name: str) -> RelayConn:
        if name not in self.conns:
            raise RuntimeError(f"unknown conn: {name}")
        return self.conns[name]

    def cmd_connect(self, name: str) -> None:
        # connect <name> [hb_s] 在 _dispatch 解析
        if name in self.conns and self.conns[name].is_connected():
            raise RuntimeError(f"{name} already connected")
        c = self.conns.get(name) or RelayConn(name, self.host, self.port)
        if c.hb_interval_s is None:
            c.hb_interval_s = self._default_hb_interval_s
        c.connect()
        self.conns[name] = c
        self._p(f"OK connected {name} -> {self.host}:{self.port}")

    def cmd_close(self, name: str) -> None:
        c = self._get(name)
        c.close()
        self._p(f"OK closed {name}")

    def cmd_list(self) -> None:
        if not self.conns:
            self._p("(no connections)")
            return
        for name, c in self.conns.items():
            st = "UP" if c.is_connected() else "DOWN"
            self._p(
                f"- {name}: {st} user_id={c.user_id} user={c.username!r} peer_role={c.peer_role} conn_id={c.conn_id} "
                f"in={c.stats.frames_in} out={c.stats.frames_out} err={c.stats.errors}"
            )

    def cmd_login(self, name: str, peer_role: str, username: str, password: str, register: bool) -> None:
        c = self._get(name)
        if peer_role not in ("user", "admin"):
            peer_role = "user"
        seq = self._next_seq()
        c.send_msgpack(
            {
                "username": username,
                "password": password,
                "peer_role": peer_role,
                "register": register,
            },
            seq=seq,
            msg_type=MSG_CLIENT_LOGIN,
        )
        self._p(f"OK sent LOGIN({'register' if register else 'login'}) on {name} seq={seq}")
    def cmd_sethb(self, name: str, hb: str) -> None:
        c = self._get(name)
        if hb.lower() == "off":
            c.hb_interval_s = None
            c._next_hb_ms = 0
            self._p(f"OK sethb {name}=off")
            return
        try:
            v = float(hb)
        except Exception:
            raise RuntimeError("sethb <name> <hb_s|off>")
        if v <= 0:
            c.hb_interval_s = None
            c._next_hb_ms = 0
            self._p(f"OK sethb {name}=off")
            return
        c.hb_interval_s = v
        c._next_hb_ms = 0
        self._p(f"OK sethb {name}={v}s")


    def cmd_hb(self, name: str) -> None:
        c = self._get(name)
        seq = self._next_seq()
        c.send_msgpack({}, seq=seq, msg_type=MSG_CLIENT_HEARTBEAT)
        self._p(f"OK sent HEARTBEAT on {name} seq={seq}")

    def cmd_send(self, from_name: str, mode: str, args: list[str]) -> None:
        c = self._get(from_name)
        seq = self._next_seq()
        if mode == "unicast":
            if len(args) < 3:
                raise RuntimeError("send unicast needs: <dst_username> <dst_conn_id> <payload>")
            dst = str(args[0])
            dst_conn = int(args[1])
            payload = _load_payload(args[2])
            c.send_msgpack(
                {"mode": "unicast", "dst_username": dst, "dst_conn_id": dst_conn, "payload": payload},
                seq=seq,
                msg_type=MSG_CLIENT_DATA,
            )
        elif mode == "broadcast":
            if len(args) < 2:
                raise RuntimeError("send broadcast needs: <dst_username> <payload>")
            dst = str(args[0])
            payload = _load_payload(args[1])
            c.send_msgpack(
                {"mode": "broadcast", "dst_username": dst, "payload": payload},
                seq=seq,
                msg_type=MSG_CLIENT_DATA,
            )
        elif mode in ("round_robin", "roundrobin"):
            if len(args) < 3:
                raise RuntimeError("send round_robin needs: <dst_username> <interval_ms> <payload>")
            dst = str(args[0])
            interval_ms = int(args[1])
            payload = _load_payload(args[2])
            c.send_msgpack(
                {"mode": "round_robin", "dst_username": dst, "interval_ms": interval_ms, "payload": payload},
                seq=seq,
                msg_type=MSG_CLIENT_DATA,
            )
        else:
            raise RuntimeError(f"unknown mode: {mode}")
        self._p(f"OK sent DATA({mode}) from {from_name} seq={seq}")

    def cmd_sendmp(self, from_name: str, mode: str, args: list[str]) -> None:
        """
        sendmp：把 JSON 解析为对象，再 msgpack 打包进 TRANSFER.payload（bin）。
        这是“方式 A”的直接操作入口：payload = msgpack(struct/object bytes)。
        """
        c = self._get(from_name)
        seq = self._next_seq()
        if mode == "unicast":
            if len(args) < 3:
                raise RuntimeError("sendmp unicast needs: <dst_username> <dst_conn_id> <json>")
            dst = str(args[0])
            dst_conn = int(args[1])
            obj = json.loads(args[2])
            payload = msgpack.packb(obj, use_bin_type=True)
            c.send_msgpack(
                {"mode": "unicast", "dst_username": dst, "dst_conn_id": dst_conn, "payload": payload},
                seq=seq,
                msg_type=MSG_CLIENT_DATA,
            )
        elif mode == "broadcast":
            if len(args) < 2:
                raise RuntimeError("sendmp broadcast needs: <dst_username> <json>")
            dst = str(args[0])
            obj = json.loads(args[1])
            payload = msgpack.packb(obj, use_bin_type=True)
            c.send_msgpack(
                {"mode": "broadcast", "dst_username": dst, "payload": payload},
                seq=seq,
                msg_type=MSG_CLIENT_DATA,
            )
        elif mode in ("round_robin", "roundrobin"):
            if len(args) < 3:
                raise RuntimeError("sendmp round_robin needs: <dst_username> <interval_ms> <json>")
            dst = str(args[0])
            interval_ms = int(args[1])
            obj = json.loads(args[2])
            payload = msgpack.packb(obj, use_bin_type=True)
            c.send_msgpack(
                {"mode": "round_robin", "dst_username": dst, "interval_ms": interval_ms, "payload": payload},
                seq=seq,
                msg_type=MSG_CLIENT_DATA,
            )
        else:
            raise RuntimeError(f"unknown mode: {mode}")
        self._p(f"OK sent DATA({mode}) msgpack(payload) from {from_name} seq={seq}")

    def cmd_ctl(self, from_name: str, action: str, args: list[str]) -> None:
        c = self._get(from_name)
        seq = self._next_seq()
        if action == "list_users":
            c.send_msgpack({"action": "list_users"}, seq=seq, msg_type=MSG_CLIENT_CONTROL)
        elif action == "kick_user":
            if not args:
                raise RuntimeError("kick_user needs: <target_user_id>")
            c.send_msgpack(
                {"action": "kick_user", "target_user_id": int(args[0])},
                seq=seq,
                msg_type=MSG_CLIENT_CONTROL,
            )
        else:
            raise RuntimeError(f"unknown action: {action}")
        self._p(f"OK sent CONTROL({action}) from {from_name} seq={seq}")

    def cmd_stats(self, name: str = "") -> None:
        targets = [self._get(name)] if name else list(self.conns.values())
        if not targets:
            self._p("(no connections)")
            return
        for c in targets:
            rtts = c.stats.rtt_samples_ms[-200:]
            if rtts:
                rtts_sorted = sorted(rtts)
                p50 = rtts_sorted[len(rtts_sorted) // 2]
                p95 = rtts_sorted[int(len(rtts_sorted) * 0.95) - 1]
                p99 = rtts_sorted[int(len(rtts_sorted) * 0.99) - 1]
                rtt_s = f"rtt_ms(p50/p95/p99)={p50}/{p95}/{p99} samples={len(rtts)}"
            else:
                rtt_s = "rtt_ms(n/a)"
            self._p(
                f"[{c.name}] up={c.is_connected()} user_id={c.user_id} user={c.username!r} peer_role={c.peer_role} conn_id={c.conn_id} "
                f"in={c.stats.frames_in}({_fmt_bytes(c.stats.bytes_in)}) "
                f"out={c.stats.frames_out}({_fmt_bytes(c.stats.bytes_out)}) "
                f"deliver={c.stats.deliver_in} replies={c.stats.replies_in} inflight={len(c.stats.inflight)} "
                f"err={c.stats.errors} {rtt_s}"
            )
            if c.stats.last_error:
                self._p(f"  last_error={c.stats.last_error}")

    def _admin_url(self, path: str) -> str:
        if not self.admin_port:
            raise RuntimeError("admin port unknown; start with --admin-port or --spawn-server")
        return f"http://127.0.0.1:{self.admin_port}{path}"

    def cmd_admin(self, what: str) -> None:
        if what == "health":
            url = self._admin_url("/api/health")
        elif what == "stats":
            url = self._admin_url("/api/stats")
        elif what == "events":
            url = self._admin_url("/api/events")
        elif what == "users":
            url = self._admin_url("/api/users")
        else:
            raise RuntimeError("admin: health|stats|events|users")
        with urllib.request.urlopen(url, timeout=3) as r:
            data = r.read().decode("utf-8", errors="replace")
        self._p(data)

    def cmd_flood(self, from_name: str, mode: str, args: list[str]) -> None:
        """
        轻量压测：持续发送 count 条 TRANSFER，并以 window 控制在途 seq 数；
        统计 ACK RTT 与吞吐（仅就客户端观测）。
        """
        c = self._get(from_name)
        if mode == "unicast":
            if len(args) < 4:
                raise RuntimeError("flood unicast needs: <dst_username> <dst_conn_id> <count> <bytes> [window]")
            dst = str(args[0])
            dst_conn = int(args[1])
            count = int(args[2])
            nbytes = int(args[3])
            window = int(args[4]) if len(args) >= 5 else 64
            mk_obj = lambda payload: {
                "mode": "unicast",
                "dst_username": dst,
                "dst_conn_id": dst_conn,
                "payload": payload,
            }
            mk_mt = MSG_CLIENT_DATA
        elif mode == "broadcast":
            if len(args) < 3:
                raise RuntimeError("flood broadcast needs: <dst_username> <count> <bytes> [window]")
            dst = str(args[0])
            count = int(args[1])
            nbytes = int(args[2])
            window = int(args[3]) if len(args) >= 4 else 64
            mk_obj = lambda payload: {"mode": "broadcast", "dst_username": dst, "payload": payload}
            mk_mt = MSG_CLIENT_DATA
        elif mode in ("round_robin", "roundrobin"):
            if len(args) < 4:
                raise RuntimeError("flood round_robin needs: <dst_username> <interval_ms> <count> <bytes> [window]")
            dst = str(args[0])
            interval_ms = int(args[1])
            count = int(args[2])
            nbytes = int(args[3])
            window = int(args[4]) if len(args) >= 5 else 64
            mk_obj = lambda payload: {
                "mode": "round_robin",
                "dst_username": dst,
                "interval_ms": interval_ms,
                "payload": payload,
            }
            mk_mt = MSG_CLIENT_DATA
        else:
            raise RuntimeError(f"unknown mode: {mode}")

        if count <= 0 or nbytes < 0:
            raise RuntimeError("count must be >0 and bytes >=0")
        if window <= 0:
            raise RuntimeError("window must be >0")

        # 记录开始前统计，结束后做差分
        base_out = c.stats.frames_out
        base_in = c.stats.replies_in
        base_rtt_len = len(c.stats.rtt_samples_ms)

        start = time.time()
        sent = 0
        payload = os.urandom(nbytes) if nbytes else b""

        self._p(f"flood start: from={from_name} mode={mode} count={count} bytes={nbytes} window={window}")
        while sent < count:
            # window 控制：在途过多则等待回复回收
            while len(c.stats.inflight) >= window:
                time.sleep(0.005)
            seq = self._next_seq()
            c.send_msgpack(mk_obj(payload), seq=seq, msg_type=mk_mt)
            sent += 1
            if sent % max(1, (count // 10)) == 0:
                elapsed = max(1e-6, time.time() - start)
                self._p(f"  progress: {sent}/{count} sent, inflight={len(c.stats.inflight)} t={elapsed:.2f}s")

        # 等待在途清空（给个上限）
        deadline = time.time() + 10.0
        while c.stats.inflight and time.time() < deadline:
            time.sleep(0.01)

        elapsed = max(1e-6, time.time() - start)
        out_frames = c.stats.frames_out - base_out
        in_replies = c.stats.replies_in - base_in
        new_rtts = c.stats.rtt_samples_ms[base_rtt_len:]
        if new_rtts:
            srt = sorted(new_rtts)
            p50 = srt[len(srt) // 2]
            p95 = srt[int(len(srt) * 0.95) - 1]
            p99 = srt[int(len(srt) * 0.99) - 1]
            rtt_s = f"rtt_ms(p50/p95/p99)={p50}/{p95}/{p99} samples={len(new_rtts)}"
        else:
            rtt_s = "rtt_ms(n/a)"
        self._p(
            f"flood done: sent={sent} acks={in_replies} elapsed={elapsed:.3f}s "
            f"send_rate={sent/elapsed:.1f}/s out_frames={out_frames} {rtt_s}"
        )

    def repl(self) -> None:
        self._p("relay_cli ready. 输入 help 查看命令。")
        while True:
            try:
                line = input("> ").strip()
            except (EOFError, KeyboardInterrupt):
                self._p("")
                break
            if not line:
                continue
            try:
                self._dispatch(line)
            except EOFError:
                # quit/exit
                break
            except Exception as e:
                self._p(f"ERR {e}")

        self._running = False
        for c in list(self.conns.values()):
            c.close()

    def _dispatch(self, line: str) -> None:
        # 支持脚本注释（行首 #）
        if line.lstrip().startswith("#"):
            return
        parts = shlex.split(line)
        if not parts:
            return
        cmd = parts[0].lower()
        args = parts[1:]
        if cmd in ("quit", "exit"):
            raise EOFError()
        if cmd == "help":
            return self.help()
        if cmd == "connect":
            if len(args) < 1 or len(args) > 2:
                raise RuntimeError("connect <name> [hb_s]")
            name = args[0]
            hb_s = args[1] if len(args) == 2 else None
            c = self.conns.get(name) or RelayConn(name, self.host, self.port)
            if hb_s is None:
                c.hb_interval_s = self._default_hb_interval_s
            elif hb_s.lower() == "off":
                c.hb_interval_s = None
            else:
                c.hb_interval_s = float(hb_s)
            self.conns[name] = c
            return self.cmd_connect(name)
        if cmd == "close":
            if len(args) != 1:
                raise RuntimeError("close <name>")
            return self.cmd_close(args[0])
        if cmd == "list":
            return self.cmd_list()
        if cmd == "login":
            if len(args) < 4:
                raise RuntimeError("login <name> <user|admin> <username> <password> [register] [hb_s]")
            name, peer_role, username, password = args[0], args[1], args[2], args[3]
            register = False
            rest = args[4:]
            if rest and rest[0].lower() == "register":
                register = True
                rest = rest[1:]
            if rest:
                hb_s = rest[0]
                c = self._get(name)
                if hb_s.lower() == "off":
                    c.hb_interval_s = None
                    c._next_hb_ms = 0
                else:
                    c.hb_interval_s = float(hb_s)
                    c._next_hb_ms = 0
            return self.cmd_login(name, peer_role, username, password, register)
        if cmd == "sethb":
            if len(args) != 2:
                raise RuntimeError("sethb <name> <hb_s|off>")
            return self.cmd_sethb(args[0], args[1])
        if cmd == "hb":
            if len(args) != 1:
                raise RuntimeError("hb <name>")
            return self.cmd_hb(args[0])
        if cmd == "send":
            if len(args) < 2:
                raise RuntimeError("send <from> <mode> ...")
            return self.cmd_send(args[0], args[1], args[2:])
        if cmd == "sendmp":
            if len(args) < 2:
                raise RuntimeError("sendmp <from> <mode> ...")
            return self.cmd_sendmp(args[0], args[1], args[2:])
        if cmd == "ctl":
            if len(args) < 2:
                raise RuntimeError("ctl <from> <action> ...")
            return self.cmd_ctl(args[0], args[1], args[2:])
        if cmd == "stats":
            return self.cmd_stats(args[0] if args else "")
        if cmd == "admin":
            if len(args) != 1:
                raise RuntimeError("admin health|stats|events|users")
            return self.cmd_admin(args[0])
        if cmd == "wait":
            if len(args) != 1:
                raise RuntimeError("wait <seconds>")
            time.sleep(float(args[0]))
            return
        if cmd == "flood":
            if len(args) < 2:
                raise RuntimeError("flood <from> <mode> ...")
            return self.cmd_flood(args[0], args[1], args[2:])
        raise RuntimeError(f"unknown cmd: {cmd}")


def _pick_free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    p = int(s.getsockname()[1])
    s.close()
    return p


def _spawn_server(root: str, base_cfg_path: str) -> tuple[subprocess.Popen[bytes], int, int, str]:
    exe = os.path.join(root, "build", "asio_forwarder")
    if not os.path.isfile(exe):
        raise RuntimeError("missing build/asio_forwarder; please build first (cmake --build build)")

    with open(base_cfg_path, encoding="utf-8") as f:
        cfg = json.load(f)
    client_port = _pick_free_port()
    admin_port = _pick_free_port()
    cfg["client"]["listen"]["host"] = "127.0.0.1"
    cfg["client"]["listen"]["port"] = client_port
    cfg["admin"]["listen"]["host"] = "127.0.0.1"
    cfg["admin"]["listen"]["port"] = admin_port

    tf = tempfile.NamedTemporaryFile("w", delete=False, encoding="utf-8", prefix="relay_cli_", suffix=".json")
    json.dump(cfg, tf, indent=2)
    tf.flush()
    tf.close()
    tmp_cfg = tf.name

    # 让子进程单独进程组，方便 Ctrl+C 清理
    p = subprocess.Popen(
        [exe, tmp_cfg],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        preexec_fn=os.setsid if hasattr(os, "setsid") else None,
    )

    # 等待端口可连
    deadline = time.time() + 5.0
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", client_port), timeout=0.2)
            s.close()
            break
        except Exception:
            time.sleep(0.05)

    return p, client_port, admin_port, tmp_cfg


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19000)
    ap.add_argument("--admin-port", type=int, default=0)
    ap.add_argument("--spawn-server", action="store_true", help="启动本地 build/asio_forwarder（自动挑端口）")
    ap.add_argument("--config", default="configs/dev/forwarder.json", help="spawn-server 用的基础配置文件")
    args = ap.parse_args()

    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    child: Optional[subprocess.Popen[bytes]] = None
    tmp_cfg = ""
    host, port, admin_port = args.host, args.port, args.admin_port

    if args.spawn_server:
        child, port, admin_port, tmp_cfg = _spawn_server(root, os.path.join(root, args.config))
        host = "127.0.0.1"
        print(f"[spawn] server started client=127.0.0.1:{port} admin=127.0.0.1:{admin_port} cfg={tmp_cfg}", flush=True)

        def _atexit(*_a: Any) -> None:
            nonlocal child, tmp_cfg
            if child is not None:
                try:
                    if hasattr(os, "killpg") and child.pid:
                        os.killpg(child.pid, signal.SIGTERM)
                    else:
                        child.terminate()
                except Exception:
                    pass
                try:
                    child.wait(timeout=1.0)
                except Exception:
                    pass
            if tmp_cfg:
                try:
                    os.unlink(tmp_cfg)
                except Exception:
                    pass

        signal.signal(signal.SIGINT, _atexit)
        signal.signal(signal.SIGTERM, _atexit)

    cli = RelayCLI(host=host, port=port, admin_port=admin_port)
    try:
        cli.repl()
    finally:
        if child is not None:
            try:
                if hasattr(os, "killpg") and child.pid:
                    os.killpg(child.pid, signal.SIGTERM)
                else:
                    child.terminate()
            except Exception:
                pass
            try:
                child.wait(timeout=1.0)
            except Exception:
                pass
        if tmp_cfg:
            try:
                os.unlink(tmp_cfg)
            except Exception:
                pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

