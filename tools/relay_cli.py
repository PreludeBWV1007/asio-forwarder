#!/usr/bin/env python3
"""
交互式 Relay 测试控制台（对等中继协议）。

目标：
- 在一个终端里创建/关闭多个客户端连接
- 对每条连接执行 LOGIN/HEARTBEAT/TRANSFER/CONTROL
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

from relay_proto import MSG_DELIVER, MSG_SERVER_REPLY, pack_frame_msgpack, recv_frame, recv_frame_msgpack


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
        self.role: str = "data"
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

    def send_msgpack(self, obj: dict, *, seq: int = 0) -> None:
        frame = pack_frame_msgpack(obj, seq=seq)
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
                break

            self.stats.frames_in += 1
            self.stats.bytes_in += 40 + (len(body_raw) if body_raw else 0)
            mt = int(h.get("msg_type", 0))
            if mt == MSG_DELIVER:
                self.stats.deliver_in += 1
                self.inbox.put(("deliver", (h, body_raw)))
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
                        preview = raw[:120]
                        tail = "..." if len(raw) > len(preview) else ""
                        self._p(
                            f"[DELIVER] conn={c.name} src={h.get('src_user_id')} dst={h.get('dst_user_id')} seq={h.get('seq')} "
                            f"len={len(raw)} preview={preview!r}{tail}"
                        )
                    elif typ == "reply":
                        h, obj = payload
                        self._p(f"[REPLY]   conn={c.name} seq={h.get('seq')} body={obj}")
                    elif typ == "closed":
                        self._p(f"[CLOSED]  conn={c.name} {payload}")
                    else:
                        h, raw = payload
                        self._p(f"[FRAME]   conn={c.name} type={h.get('msg_type')} seq={h.get('seq')} len={len(raw)}")
            if not any_msg:
                time.sleep(0.03)

    def help(self) -> None:
        self._p(
            "\n".join(
                [
                    "命令：",
                    "  help",
                    "  quit / exit",
                    "",
                    "连接管理：",
                    "  connect <name>                 # 建立 TCP 连接（未登录）",
                    "  close <name>                   # 关闭连接",
                    "  list                            # 列出连接及状态",
                    "",
                    "登录与心跳：",
                    "  login <name> <user_id> [role=data|control]",
                    "  hb <name>",
                    "",
                    "转发（payload 支持: 文本 / @文件 / hex:...）：",
                    "  send <from> unicast <dst_user_id> <payload>",
                    "  send <from> broadcast <payload>",
                    "  send <from> round_robin <interval_ms> <payload>",
                    "",
                    "控制面（SERVER_REPLY）：",
                    "  ctl <from> list_users",
                    "  ctl <from> kick_user <target_user_id>",
                    "",
                    "观测与统计：",
                    "  stats [name]                    # 不带 name 则打印所有连接统计",
                    "  admin health|stats|events        # 需提供 --admin-port 或 --spawn-server",
                    "",
                    "压力/稳定性：",
                    "  flood <from> unicast <dst_user_id> <count> <bytes> [window=64]",
                    "  flood <from> broadcast <count> <bytes> [window=64]",
                    "  flood <from> round_robin <interval_ms> <count> <bytes> [window=64]",
                    "    - bytes: 每条 payload 大小（随机填充）",
                    "    - window: 允许在途 seq 数（越大越压）",
                ]
            )
        )

    def _get(self, name: str) -> RelayConn:
        if name not in self.conns:
            raise RuntimeError(f"unknown conn: {name}")
        return self.conns[name]

    def cmd_connect(self, name: str) -> None:
        if name in self.conns and self.conns[name].is_connected():
            raise RuntimeError(f"{name} already connected")
        c = self.conns.get(name) or RelayConn(name, self.host, self.port)
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
            self._p(f"- {name}: {st} user_id={c.user_id} role={c.role} in={c.stats.frames_in} out={c.stats.frames_out} err={c.stats.errors}")

    def cmd_login(self, name: str, user_id: int, role: str) -> None:
        c = self._get(name)
        c.user_id = int(user_id)
        c.role = role
        seq = self._next_seq()
        c.send_msgpack({"op": "LOGIN", "user_id": c.user_id, "role": role}, seq=seq)
        self._p(f"OK sent LOGIN on {name} seq={seq}")

    def cmd_hb(self, name: str) -> None:
        c = self._get(name)
        seq = self._next_seq()
        c.send_msgpack({"op": "HEARTBEAT"}, seq=seq)
        self._p(f"OK sent HEARTBEAT on {name} seq={seq}")

    def cmd_send(self, from_name: str, mode: str, args: list[str]) -> None:
        c = self._get(from_name)
        seq = self._next_seq()
        if mode == "unicast":
            if len(args) < 2:
                raise RuntimeError("send unicast needs: <dst_user_id> <payload>")
            dst = int(args[0])
            payload = _load_payload(args[1])
            c.send_msgpack({"op": "TRANSFER", "mode": "unicast", "dst_user_id": dst, "payload": payload}, seq=seq)
        elif mode == "broadcast":
            if len(args) < 1:
                raise RuntimeError("send broadcast needs: <payload>")
            payload = _load_payload(args[0])
            c.send_msgpack({"op": "TRANSFER", "mode": "broadcast", "payload": payload}, seq=seq)
        elif mode in ("round_robin", "roundrobin"):
            if len(args) < 2:
                raise RuntimeError("send round_robin needs: <interval_ms> <payload>")
            interval_ms = int(args[0])
            payload = _load_payload(args[1])
            c.send_msgpack({"op": "TRANSFER", "mode": "round_robin", "interval_ms": interval_ms, "payload": payload}, seq=seq)
        else:
            raise RuntimeError(f"unknown mode: {mode}")
        self._p(f"OK sent TRANSFER({mode}) from {from_name} seq={seq}")

    def cmd_ctl(self, from_name: str, action: str, args: list[str]) -> None:
        c = self._get(from_name)
        seq = self._next_seq()
        if action == "list_users":
            c.send_msgpack({"op": "CONTROL", "action": "list_users"}, seq=seq)
        elif action == "kick_user":
            if not args:
                raise RuntimeError("kick_user needs: <target_user_id>")
            c.send_msgpack({"op": "CONTROL", "action": "kick_user", "target_user_id": int(args[0])}, seq=seq)
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
                f"[{c.name}] up={c.is_connected()} user_id={c.user_id} role={c.role} "
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
        else:
            raise RuntimeError("admin: health|stats|events")
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
            if len(args) < 3:
                raise RuntimeError("flood unicast needs: <dst_user_id> <count> <bytes> [window]")
            dst = int(args[0])
            count = int(args[1])
            nbytes = int(args[2])
            window = int(args[3]) if len(args) >= 4 else 64
            mk_obj = lambda payload: {"op": "TRANSFER", "mode": "unicast", "dst_user_id": dst, "payload": payload}
        elif mode == "broadcast":
            if len(args) < 2:
                raise RuntimeError("flood broadcast needs: <count> <bytes> [window]")
            count = int(args[0])
            nbytes = int(args[1])
            window = int(args[2]) if len(args) >= 3 else 64
            mk_obj = lambda payload: {"op": "TRANSFER", "mode": "broadcast", "payload": payload}
        elif mode in ("round_robin", "roundrobin"):
            if len(args) < 3:
                raise RuntimeError("flood round_robin needs: <interval_ms> <count> <bytes> [window]")
            interval_ms = int(args[0])
            count = int(args[1])
            nbytes = int(args[2])
            window = int(args[3]) if len(args) >= 4 else 64
            mk_obj = lambda payload: {"op": "TRANSFER", "mode": "round_robin", "interval_ms": interval_ms, "payload": payload}
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
            c.send_msgpack(mk_obj(payload), seq=seq)
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
            except Exception as e:
                self._p(f"ERR {e}")

        self._running = False
        for c in list(self.conns.values()):
            c.close()

    def _dispatch(self, line: str) -> None:
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
            if len(args) != 1:
                raise RuntimeError("connect <name>")
            return self.cmd_connect(args[0])
        if cmd == "close":
            if len(args) != 1:
                raise RuntimeError("close <name>")
            return self.cmd_close(args[0])
        if cmd == "list":
            return self.cmd_list()
        if cmd == "login":
            if len(args) < 2:
                raise RuntimeError("login <name> <user_id> [role]")
            role = args[2] if len(args) >= 3 else "data"
            if role not in ("data", "control"):
                role = "data"
            return self.cmd_login(args[0], int(args[1]), role)
        if cmd == "hb":
            if len(args) != 1:
                raise RuntimeError("hb <name>")
            return self.cmd_hb(args[0])
        if cmd == "send":
            if len(args) < 2:
                raise RuntimeError("send <from> <mode> ...")
            return self.cmd_send(args[0], args[1], args[2:])
        if cmd == "ctl":
            if len(args) < 2:
                raise RuntimeError("ctl <from> <action> ...")
            return self.cmd_ctl(args[0], args[1], args[2:])
        if cmd == "stats":
            return self.cmd_stats(args[0] if args else "")
        if cmd == "admin":
            if len(args) != 1:
                raise RuntimeError("admin health|stats|events")
            return self.cmd_admin(args[0])
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

