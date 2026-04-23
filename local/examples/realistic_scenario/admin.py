#!/usr/bin/env python3
from __future__ import annotations

import argparse
import time

from relay_client import RelayClient


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19000)
    ap.add_argument("--username", default="admin")
    ap.add_argument("--password", default="admin-pw")
    ap.add_argument("--register", action="store_true")
    args = ap.parse_args()

    c = RelayClient(args.host, args.port, hb_interval_s=5)
    c.connect()
    c.login(args.username, args.password, peer_role="admin", register=args.register)

    print("[admin] up. polling list_users every 3s; kick is manual via relay_cli/webui.")
    while True:
        seq = c.control_list_users()
        try:
            r = c.wait_reply(seq, timeout_s=2.0)
            print("[admin] users:", r.body.get("users"))
        except Exception as e:
            print("[admin] list_users error:", repr(e))
        time.sleep(3.0)


if __name__ == "__main__":
    raise SystemExit(main())

