#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import time

from relay_client import RelayClient


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19000)
    ap.add_argument("--username", default="worker1")
    ap.add_argument("--password", default="worker1-pw")
    ap.add_argument("--register", action="store_true")
    ap.add_argument("--dispatcher", default="dispatcher")
    args = ap.parse_args()

    c = RelayClient(args.host, args.port, hb_interval_s=5)
    c.connect()
    c.login(args.username, args.password, peer_role="user", register=args.register)

    print(f"[worker] up as {args.username!r}, waiting tasks")
    while True:
        # 如果被踢，打印原因并退出（真实系统可自动重连）
        try:
            k = c.inbox_kick.get_nowait()
            print("[worker] KICK:", k.reason)
            return 2
        except Exception:
            pass

        d = c.inbox_deliver.get()
        try:
            obj = json.loads(d.payload.decode("utf-8", errors="strict"))
        except Exception:
            continue
        if obj.get("type") != "task":
            continue
        payload = obj.get("payload") or {}
        a = int(payload.get("a", 0))
        b = int(payload.get("b", 0))
        # 模拟工作
        time.sleep(0.05)
        res = {
            "type": "result",
            "task_id": obj.get("task_id"),
            "ts_ms": int(time.time() * 1000),
            "ok": True,
            "value": a + b,
        }
        c.send_unicast(args.dispatcher, json.dumps(res, ensure_ascii=False).encode("utf-8"), dst_conn_id=0)


if __name__ == "__main__":
    raise SystemExit(main())

