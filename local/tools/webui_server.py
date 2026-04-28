#!/usr/bin/env python3
"""
本机 Web 单页：左侧监控（转发 C++ 只读管理口 + 两张 MySQL 表），右侧经 WebSocket
连中继发 **二进制** 载荷（hex / Base64）。登录走服务端：白名单 + 库中鉴权，无账号则首登自动建表行。

  pip install -r local/tools/requirements-relay.txt -r local/tools/requirements-webui.txt
  pip install pymysql   # 改白名单/库内账号时
  PYTHONPATH=local/tools python3 local/tools/webui_server.py

浏览器： http://127.0.0.1:8080 ；/demo /ops 会重定向到 /
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
import re
import time
import urllib.error
import urllib.request
from dataclasses import asdict
from typing import Any, Optional

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, RedirectResponse
from pydantic import BaseModel, Field

from relay_client import RelayClient, Deliver, KickNotice, ServerReply

# 在 main 里按参数初始化（供 /api/ops 使用）
_ADMIN_HOST = "127.0.0.1"
_ADMIN_PORT = 19003
_MYSQL_CFG: Optional[dict] = None
_REPO_ROOT: str = ""


def _set_ops(*, admin_host: str, admin_port: int, mysql: Optional[dict], repo_root: str) -> None:
    global _ADMIN_HOST, _ADMIN_PORT, _MYSQL_CFG, _REPO_ROOT
    _ADMIN_HOST = admin_host
    _ADMIN_PORT = int(admin_port)
    _MYSQL_CFG = mysql
    _REPO_ROOT = repo_root


def _now_ms() -> int:
    return int(time.time() * 1000)


def _safe(obj: Any) -> Any:
    try:
        json.dumps(obj)
        return obj
    except Exception:
        return repr(obj)


def _read_static(name: str) -> str:
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "webui_static", name)
    with open(path, encoding="utf-8") as f:
        return f.read()


def _read_app_html() -> str:
    h = _read_static("app.html")
    h = h.replace("__INJECT_ADMIN__", f"{_ADMIN_HOST}:{_ADMIN_PORT}")
    rh = os.environ.get("ASIOFWD_RELAY_HOST", "127.0.0.1")
    rp = os.environ.get("ASIOFWD_RELAY_PORT", "19000")
    return (
        h.replace('id="sessHost" value="127.0.0.1"', f'id="sessHost" value="{rh}"')
        .replace('id="sessPort" type="number" value="19000"', f'id="sessPort" type="number" value="{rp}"')
    )


def _admin_get_json(path: str) -> Any:
    url = f"http://{_ADMIN_HOST}:{_ADMIN_PORT}{path}"
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=6) as r:
            return json.loads(r.read().decode("utf-8", errors="replace"))
    except urllib.error.HTTPError as e:
        try:
            detail = e.read().decode("utf-8", errors="replace")
        except Exception:
            detail = str(e)
        raise HTTPException(status_code=e.code, detail=detail) from e
    except Exception as e:
        raise HTTPException(status_code=502, detail=f"拉取 C++ 管理口失败: {e}") from e


def _mysql_connect():
    if not _MYSQL_CFG:
        raise HTTPException(503, "未配置 MySQL；请用 --forwarder-json 或环境变量 ASIOFWD_MYSQL_*")
    try:
        import pymysql
    except ImportError as e:
        raise HTTPException(503, "需要安装 pymysql：pip install pymysql") from e
    c = _MYSQL_CFG
    return pymysql.connect(
        host=c["host"],
        port=int(c["port"]),
        user=c["user"],
        password=c["password"],
        database=c["database"],
        charset="utf8mb4",
        cursorclass=pymysql.cursors.DictCursor,
    )


def _load_mysql_from_json(path: str) -> Optional[dict]:
    with open(path, encoding="utf-8") as f:
        j = json.load(f)
    m = j.get("mysql")
    if not m:
        return None
    return {
        "host": str(m.get("host", "127.0.0.1")),
        "port": int(m.get("port", 3306)),
        "user": str(m.get("user", "root")),
        "password": str(m.get("password", "")),
        "database": str(m.get("database", "forwarder")),
    }


def _mysql_from_env() -> Optional[dict]:
    if "ASIOFWD_MYSQL_HOST" not in os.environ:
        return None
    return {
        "host": os.environ.get("ASIOFWD_MYSQL_HOST", "127.0.0.1"),
        "port": int(os.environ.get("ASIOFWD_MYSQL_PORT", "3306")),
        "user": os.environ.get("ASIOFWD_MYSQL_USER", "root"),
        "password": os.environ.get("ASIOFWD_MYSQL_PASSWORD", ""),
        "database": os.environ.get("ASIOFWD_MYSQL_DATABASE", "forwarder"),
    }


app = FastAPI()


@app.get("/", response_class=HTMLResponse)
def page_app() -> str:
    return _read_app_html()


@app.get("/demo")
def page_demo_redirect() -> RedirectResponse:
    return RedirectResponse("/", status_code=302)


@app.get("/ops")
def page_ops_redirect() -> RedirectResponse:
    return RedirectResponse("/", status_code=302)


@app.get("/api/ops/relay/health")
def ops_relay_health() -> Any:
    return _admin_get_json("/api/health")


@app.get("/api/ops/relay/stats")
def ops_relay_stats() -> Any:
    return _admin_get_json("/api/stats")


@app.get("/api/ops/relay/users")
def ops_relay_users() -> Any:
    return _admin_get_json("/api/users")


@app.get("/api/ops/relay/events")
def ops_relay_events() -> Any:
    return _admin_get_json("/api/events")


@app.get("/api/ops/db_status")
def ops_db_status() -> dict[str, Any]:
    if not _MYSQL_CFG:
        return {"ok": False, "error": "未配置 MySQL"}
    try:
        conn = _mysql_connect()
        try:
            conn.ping()
        finally:
            conn.close()
    except HTTPException as e:
        return {"ok": False, "error": str(e.detail) if e.detail is not None else "失败"}
    except Exception as e:
        return {"ok": False, "error": str(e)}
    return {"ok": True}


@app.get("/api/ops/whitelist")
def wl_list() -> dict[str, Any]:
    conn = _mysql_connect()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT id, ip FROM ip_allowlist ORDER BY id ASC")
            rows = list(cur.fetchall() or [])
    finally:
        conn.close()
    return {"rows": rows}


class WlAdd(BaseModel):
    ip: str = Field(..., min_length=1)


_re_ip = re.compile(r"^[\d.a-fA-F:%\-_/]+$")


@app.post("/api/ops/whitelist")
def wl_add(x: WlAdd) -> dict[str, Any]:
    s = x.ip.strip()
    if not s or not _re_ip.match(s):
        raise HTTPException(400, "IP 不合法或为空")
    conn = _mysql_connect()
    try:
        with conn.cursor() as cur:
            cur.execute("INSERT INTO ip_allowlist (ip) VALUES (%s)", (s,))
        conn.commit()
    except Exception as e:
        if "Duplicate" in str(e) or "1062" in str(e):
            raise HTTPException(409, "该 IP 已存在") from e
        raise HTTPException(500, str(e)) from e
    finally:
        conn.close()
    return {"ok": True}


@app.delete("/api/ops/whitelist/{row_id}")
def wl_del(row_id: int) -> dict[str, bool]:
    conn = _mysql_connect()
    r = 0
    try:
        with conn.cursor() as cur:
            r = cur.execute("DELETE FROM ip_allowlist WHERE id = %s", (row_id,))
        conn.commit()
    finally:
        conn.close()
    if not r:
        raise HTTPException(404, "无此编号")
    return {"ok": True}


@app.get("/api/ops/accounts")
def acc_list() -> dict[str, Any]:
    conn = _mysql_connect()
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT id, username, is_admin FROM users ORDER BY id ASC")
            rows = list(cur.fetchall() or [])
    finally:
        conn.close()
    for r in rows:
        r["is_admin"] = int(r.get("is_admin") or 0)
    return {"rows": rows}


class AccNew(BaseModel):
    username: str = Field(..., min_length=1, max_length=255)
    password: str = Field(..., min_length=1, max_length=4000)
    is_admin: int = 0


@app.post("/api/ops/accounts")
def acc_add(x: AccNew) -> dict[str, bool]:
    conn = _mysql_connect()
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO users (username, password, is_admin) VALUES (%s, %s, %s)",
                (x.username.strip(), x.password, 1 if x.is_admin else 0),
            )
        conn.commit()
    except Exception as e:
        if "Duplicate" in str(e) or "1062" in str(e):
            raise HTTPException(409, "用户名已存在") from e
        raise HTTPException(500, str(e)) from e
    finally:
        conn.close()
    return {"ok": True}


class AccPut(BaseModel):
    username: str = Field(..., min_length=1, max_length=255)
    is_admin: int = 0
    password: str = ""


@app.put("/api/ops/accounts/{row_id}")
def acc_put(row_id: int, x: AccPut) -> dict[str, bool]:
    conn = _mysql_connect()
    r = 0
    try:
        with conn.cursor() as cur:
            if x.password.strip():
                r = cur.execute(
                    "UPDATE users SET username=%s, is_admin=%s, password=%s WHERE id=%s",
                    (x.username.strip(), 1 if x.is_admin else 0, x.password, row_id),
                )
            else:
                r = cur.execute(
                    "UPDATE users SET username=%s, is_admin=%s WHERE id=%s",
                    (x.username.strip(), 1 if x.is_admin else 0, row_id),
                )
        conn.commit()
    except Exception as e:
        if "Duplicate" in str(e) or "1062" in str(e):
            raise HTTPException(409, "用户名与已有记录冲突") from e
        raise HTTPException(500, str(e)) from e
    finally:
        conn.close()
    if not r:
        raise HTTPException(404, "无此账号编号")
    return {"ok": True}


@app.delete("/api/ops/accounts/{row_id}")
def acc_del(row_id: int) -> dict[str, bool]:
    conn = _mysql_connect()
    r = 0
    try:
        with conn.cursor() as cur:
            r = cur.execute("DELETE FROM users WHERE id = %s", (row_id,))
        conn.commit()
    finally:
        conn.close()
    if not r:
        raise HTTPException(404, "无此账号编号")
    return {"ok": True}


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
                    recv_mode=str(msg.get("recv_mode", "broadcast")),
                )
                emit({"type": "info", "ts_ms": _now_ms(), "msg": f"LOGIN sent seq={seq}"})
                continue

            if op == "heartbeat":
                seq = rc.heartbeat()
                emit({"type": "info", "ts_ms": _now_ms(), "msg": f"HEARTBEAT sent seq={seq}"})
                continue

            if op == "send":
                enc = str(msg.get("payload_encoding", "b64")).lower().strip()
                raw = msg.get("payload", "")
                try:
                    if enc == "hex":
                        hx = re.sub(r"[\s:]", "", str(raw))
                        if len(hx) % 2 != 0:
                            raise ValueError("十六进制长度须为偶数")
                        payload = bytes.fromhex(hx)
                    elif enc in ("b64", "base64"):
                        payload = base64.b64decode(str(raw), validate=False)
                    else:
                        emit({"type": "error", "ts_ms": _now_ms(), "msg": f"未知 payload_encoding: {enc}（用 hex 或 b64）"})
                        continue
                except Exception as e:
                    emit({"type": "error", "ts_ms": _now_ms(), "msg": f"载荷解码失败: {e!r}"})
                    continue
                seq = rc.send_data(str(msg.get("dst_username", "")), payload)
                emit({"type": "info", "ts_ms": _now_ms(), "msg": f"DATA sent seq={seq} bytes={len(payload)}"})
                continue

            if op == "send_poly":
                emit({"type": "error", "ts_ms": _now_ms(), "msg": "send_poly 已移除；请使用 op:send + payload_encoding hex|b64"})
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


def _bootstrap_config() -> None:
    """在 main 与 uvicorn 工作进程下尽量拿到默认 MySQL；若无配置则管理页中数据库区块不可用。"""
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.abspath(os.path.join(here, "..", ".."))
    fj = os.path.join(repo, "deliver", "server", "forwarder.json")
    mc: Optional[dict] = None
    if os.path.isfile(fj):
        try:
            mc = _load_mysql_from_json(fj)
        except OSError as e:
            print(f"[webui] warn: 无法读 {fj}: {e}", flush=True)
    if not mc:
        mc = _mysql_from_env()
    _set_ops(
        admin_host=os.environ.get("ASIOFWD_ADMIN_HOST", "127.0.0.1"),
        admin_port=int(os.environ.get("ASIOFWD_ADMIN_PORT", "19003")),
        mysql=mc,
        repo_root=repo,
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--relay-host", default="127.0.0.1")
    ap.add_argument("--relay-port", type=int, default=19000)
    ap.add_argument("--admin-host", default="127.0.0.1", help="C++ 只读管理口，用于 /ops 转发")
    ap.add_argument("--admin-port", type=int, default=19003)
    ap.add_argument(
        "--forwarder-json",
        default="",
        help="从中读取 MySQL 配置；可省略则尝试仓库内 deliver/server/forwarder.json 与 ASIOFWD_MYSQL_*",
    )
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.abspath(os.path.join(here, "..", ".."))
    mc: Optional[dict] = None
    if args.forwarder_json:
        mc = _load_mysql_from_json(os.path.abspath(args.forwarder_json))
    else:
        dft = os.path.join(repo, "deliver", "server", "forwarder.json")
        if os.path.isfile(dft):
            try:
                mc = _load_mysql_from_json(dft)
            except OSError as e:
                print(f"[webui] warn: 无法读 {dft}: {e}", flush=True)
    if not mc:
        mc = _mysql_from_env()

    _set_ops(
        admin_host=args.admin_host,
        admin_port=args.admin_port,
        mysql=mc,
        repo_root=repo,
    )

    os.environ["ASIOFWD_RELAY_HOST"] = args.relay_host
    os.environ["ASIOFWD_RELAY_PORT"] = str(args.relay_port)

    import uvicorn

    print(
        f"[webui] 打开 http://{args.host}:{args.port}  转发管理口 {args.admin_host}:{args.admin_port}  数据库: "
        f"{'已配置' if mc else '未配置（仅无 MySQL 功能）'}",
        flush=True,
    )
    uvicorn.run(app, host=args.host, port=args.port, log_level="info")
    return 0


_bootstrap_config()


if __name__ == "__main__":
    raise SystemExit(main())

