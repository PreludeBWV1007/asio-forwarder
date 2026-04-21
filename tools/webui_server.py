#!/usr/bin/env python3
"""
Web UI bridge: Browser <-> (WebSocket) <-> RelayClient (TCP).

目标：
- 提供一个前端交互界面展示：注册/登录、心跳、DATA(unicast/broadcast/round_robin)、CONTROL(list_users/kick_user)、KICK 原因
- 将中继服务当黑盒：这里仅做协议组装与展示，不侵入 C++ 服务端

运行：
  pip install -r tools/requirements-relay.txt -r tools/requirements-webui.txt
  PYTHONPATH=tools python3 tools/webui_server.py --relay-host 127.0.0.1 --relay-port 19000
然后打开浏览器： http://127.0.0.1:8080
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
import time
from dataclasses import asdict
from typing import Any, Optional

import msgpack
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse

from relay_client import RelayClient, Deliver, KickNotice, ServerReply


def _now_ms() -> int:
    return int(time.time() * 1000)


def _safe(obj: Any) -> Any:
    try:
        json.dumps(obj)
        return obj
    except Exception:
        return repr(obj)


def _read_index_html() -> str:
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "webui_static", "index.html")
    with open(path, encoding="utf-8") as f:
        html = f.read()
    # 轻量注入默认 relay（便于开箱即用，不做复杂模板）
    rh = os.environ.get("ASIOFWD_RELAY_HOST", "127.0.0.1")
    rp = os.environ.get("ASIOFWD_RELAY_PORT", "19000")
    return (
        html.replace('id="relayHost" value="127.0.0.1"', f'id="relayHost" value="{rh}"')
        .replace('id="relayPort" value="19000"', f'id="relayPort" value="{rp}"')
    )


app = FastAPI()


@app.get("/", response_class=HTMLResponse)
def index() -> str:
    return _read_index_html()


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket) -> None:
    await ws.accept()
    peer = "unknown"
    try:
        peer = str(ws.client)
    except Exception:
        pass
    print(f"[webui] ws accepted peer={peer}", flush=True)

    loop = asyncio.get_running_loop()
    q: "asyncio.Queue[dict[str, Any]]" = asyncio.Queue()

    # 一个 websocket 会话维护一条 RelayClient 连接
    rc: Optional[RelayClient] = None

    def emit(ev: dict[str, Any]) -> None:
        loop.call_soon_threadsafe(q.put_nowait, ev)

    def on_deliver(d: Deliver) -> None:
        dd = asdict(d)
        pl = dd.get("payload")
        if isinstance(pl, (bytes, bytearray)):
            b = bytes(pl)
            try:
                dd["payload_utf8"] = b.decode("utf-8")
            except Exception:
                dd["payload_utf8"] = None
            dd["payload_b64"] = base64.b64encode(b).decode("ascii")
            # Best-effort: decode poly envelope {kind,type,data}
            try:
                o = msgpack.unpackb(b, raw=False)
                if isinstance(o, dict) and "kind" in o and "type" in o and "data" in o:
                    dd["payload_poly"] = _safe(o)
            except Exception:
                pass
            # remove raw bytes to keep JSON-serializable
            dd["payload"] = None
            dd["payload_len"] = len(b)
        emit({"type": "deliver", "ts_ms": _now_ms(), "data": dd})

    def on_reply(r: ServerReply) -> None:
        emit({"type": "reply", "ts_ms": _now_ms(), "seq": r.seq, "body": _safe(r.body)})

    def on_kick(k: KickNotice) -> None:
        emit({"type": "kick", "ts_ms": _now_ms(), "seq": k.seq, "reason": k.reason})

    def on_closed(e: Exception) -> None:
        emit({"type": "closed", "ts_ms": _now_ms(), "error": repr(e)})

    async def sender_loop() -> None:
        while True:
            ev = await q.get()
            try:
                await ws.send_text(json.dumps(ev, ensure_ascii=False))
            except Exception as e:
                # If sending fails, stop the loop so the websocket can be torn down.
                print(f"[webui] ws send failed peer={peer} err={e!r}", flush=True)
                break

    sender_task = asyncio.create_task(sender_loop())

    try:
        while True:
            raw = await ws.receive_text()
            msg = json.loads(raw)
            op = str(msg.get("op", ""))
            print(f"[webui] ws recv peer={peer} op={op} msg={msg}", flush=True)

            if op == "connect":
                if rc is not None:
                    rc.close()
                    rc = None
                host = str(msg.get("host", "127.0.0.1"))
                port = int(msg.get("port", 19000))
                hb = msg.get("hb_interval_s", 5.0)
                hb_interval_s = None if hb in (None, "off", 0, "0") else float(hb)
                rc = RelayClient(host, port, hb_interval_s=hb_interval_s)
                rc.on_deliver = on_deliver
                rc.on_reply = on_reply
                rc.on_kick = on_kick
                rc.on_closed = on_closed
                rc.connect()
                emit({"type": "info", "ts_ms": _now_ms(), "msg": f"connected to {host}:{port}"})
                continue

            if rc is None:
                emit({"type": "error", "ts_ms": _now_ms(), "msg": "not connected; op=connect first"})
                continue

            if op == "close":
                rc.close()
                rc = None
                emit({"type": "info", "ts_ms": _now_ms(), "msg": "closed"})
                continue

            if op == "login":
                seq = rc.login(
                    str(msg.get("username", "")),
                    str(msg.get("password", "")),
                    peer_role=str(msg.get("peer_role", "user")),
                    register=bool(msg.get("register", False)),
                )
                emit({"type": "info", "ts_ms": _now_ms(), "msg": f"LOGIN sent seq={seq}"})
                continue

            if op == "heartbeat":
                seq = rc.heartbeat()
                emit({"type": "info", "ts_ms": _now_ms(), "msg": f"HEARTBEAT sent seq={seq}"})
                continue

            if op == "send":
                mode = str(msg.get("mode", "unicast"))
                payload = str(msg.get("payload", "")).encode("utf-8", errors="strict")
                if mode == "unicast":
                    seq = rc.send_unicast(str(msg.get("dst_username", "")), payload, dst_conn_id=int(msg.get("dst_conn_id", 0)))
                elif mode == "broadcast":
                    seq = rc.send_broadcast_to_user(str(msg.get("dst_username", "")), payload)
                elif mode in ("round_robin", "roundrobin"):
                    seq = rc.send_round_robin_to_user(str(msg.get("dst_username", "")), payload, interval_ms=int(msg.get("interval_ms", 0)))
                else:
                    emit({"type": "error", "ts_ms": _now_ms(), "msg": f"unknown mode: {mode}"})
                    continue
                emit({"type": "info", "ts_ms": _now_ms(), "msg": f"DATA({mode}) sent seq={seq}"})
                continue

            if op == "send_poly":
                mode = str(msg.get("mode", "unicast"))
                dst_username = str(msg.get("dst_username", ""))
                dst_conn_id = int(msg.get("dst_conn_id", 0))
                interval_ms = int(msg.get("interval_ms", 0))

                kind = int(msg.get("kind", 0))
                type_name = str(msg.get("type", "")) or f"Kind{kind}"
                data = msg.get("data", None)
                payload = msgpack.packb({"kind": kind, "type": type_name, "data": data}, use_bin_type=True)

                if mode == "unicast":
                    seq = rc.send_unicast(dst_username, payload, dst_conn_id=dst_conn_id)
                elif mode == "broadcast":
                    seq = rc.send_broadcast_to_user(dst_username, payload)
                elif mode in ("round_robin", "roundrobin"):
                    seq = rc.send_round_robin_to_user(dst_username, payload, interval_ms=interval_ms)
                else:
                    emit({"type": "error", "ts_ms": _now_ms(), "msg": f"unknown mode: {mode}"})
                    continue
                emit({"type": "info", "ts_ms": _now_ms(), "msg": f"DATA({mode}) poly(kind={kind} type={type_name}) sent seq={seq}"})
                continue

            if op == "control":
                action = str(msg.get("action", ""))
                if action == "list_users":
                    seq = rc.control_list_users()
                elif action == "kick_user":
                    seq = rc.control_kick_user(int(msg.get("target_user_id", 0)))
                else:
                    emit({"type": "error", "ts_ms": _now_ms(), "msg": f"unknown control action: {action}"})
                    continue
                emit({"type": "info", "ts_ms": _now_ms(), "msg": f"CONTROL({action}) sent seq={seq}"})
                continue

            emit({"type": "error", "ts_ms": _now_ms(), "msg": f"unknown op: {op}"})

    except WebSocketDisconnect:
        print(f"[webui] ws disconnect peer={peer}", flush=True)
        pass
    finally:
        try:
            if rc is not None:
                rc.close()
        except Exception:
            pass
        sender_task.cancel()
        print(f"[webui] ws closed peer={peer}", flush=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--relay-host", default="127.0.0.1")
    ap.add_argument("--relay-port", type=int, default=19000)
    args = ap.parse_args()

    # 将默认 relay 连接信息放到环境变量里，前端会读并填默认值（纯展示，无安全考虑）
    os.environ["ASIOFWD_RELAY_HOST"] = args.relay_host
    os.environ["ASIOFWD_RELAY_PORT"] = str(args.relay_port)

    import uvicorn

    uvicorn.run(app, host=args.host, port=args.port, log_level="info")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

