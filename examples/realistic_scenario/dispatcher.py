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
    ap.add_argument("--username", default="dispatcher")
    ap.add_argument("--password", default="dispatcher-pw")
    ap.add_argument("--register", action="store_true", help="首次运行用注册；已注册后用登录")
    ap.add_argument("--worker", default="worker1")
    args = ap.parse_args()

    c = RelayClient(args.host, args.port, hb_interval_s=5)
    c.connect()
    c.login(args.username, args.password, peer_role="user", register=args.register)

    task_id = 1
    print(f"[dispatcher] up. send tasks to {args.worker!r}")
    while True:
        task = {
            "type": "task",
            "task_id": task_id,
            "ts_ms": int(time.time() * 1000),
            "payload": {"op": "sum", "a": task_id, "b": task_id + 1},
        }
        c.send_unicast(args.worker, json.dumps(task, ensure_ascii=False).encode("utf-8"), dst_conn_id=0)
        print(f"[dispatcher] sent task_id={task_id}")
        task_id += 1

        # 收取 worker 的结果
        deadline = time.time() + 2.0
        while time.time() < deadline:
            try:
                d = c.inbox_deliver.get(timeout=0.2)
            except Exception:
                continue
            try:
                obj = json.loads(d.payload.decode("utf-8", errors="strict"))
            except Exception:
                print("[dispatcher] bad payload from", d.src_username, "len=", len(d.payload))
                continue
            if obj.get("type") == "result":
                print(f"[dispatcher] got result from {d.src_username}: {obj}")
        time.sleep(1.0)


if __name__ == "__main__":
    raise SystemExit(main())

