#!/usr/bin/env bash
# 端到端自测：临时空闲端口 + 临时配置启动 forwarder → downstream 先连 → upstream 发一帧 → 校验 v2 头。
# 由 `ctest` 或手动 `./scripts/test_e2e.sh` 调用；退出码 0 表示通过。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ ! -x "$ROOT/build/asio_forwarder" ]]; then
  echo "missing build/asio_forwarder; run ./scripts/build.sh first" >&2
  exit 1
fi

TMP_CFG=$(mktemp)
RECV_OUT=$(mktemp)
FWD_PID=""

cleanup() {
  if [[ -n "$FWD_PID" ]]; then
    kill "$FWD_PID" 2>/dev/null || true
    wait "$FWD_PID" 2>/dev/null || true
  fi
  rm -f "$TMP_CFG" "$RECV_OUT"
}
trap cleanup EXIT

read -r E2E_UP E2E_DOWN E2E_ADMIN < <(python3 -c "
import json, socket, sys

def pick():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p

used = set()
def unique():
    while True:
        p = pick()
        if p not in used:
            used.add(p)
            return p

up, down, adm = unique(), unique(), unique()
with open(sys.argv[1], encoding='utf-8') as f:
    c = json.load(f)
c['upstream']['listen']['host'] = '127.0.0.1'
c['upstream']['listen']['port'] = up
c['downstream']['listen']['host'] = '127.0.0.1'
c['downstream']['listen']['port'] = down
c['admin']['listen']['host'] = '127.0.0.1'
c['admin']['listen']['port'] = adm
c['timeouts']['idle_ms'] = 120000
with open(sys.argv[2], 'w', encoding='utf-8') as f:
    json.dump(c, f, indent=2)
print(up, down, adm)
" "$ROOT/configs/dev/forwarder.json" "$TMP_CFG")

"$ROOT/build/asio_forwarder" "$TMP_CFG" &
FWD_PID=$!

wait_tcp() {
  local host=$1 port=$2
  local i
  for i in $(seq 1 100); do
    if (echo >/dev/tcp/$host/$port) 2>/dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "timeout waiting for $host:$port (forwarder dead?)" >&2
  return 1
}

if ! wait_tcp 127.0.0.1 "$E2E_UP"; then
  exit 1
fi
wait_tcp 127.0.0.1 "$E2E_DOWN"

echo "---- e2e ports: upstream=$E2E_UP downstream=$E2E_DOWN admin=$E2E_ADMIN ----"
echo "---- C++ proto_send -> proto_recv (v2 header) ----"
"$ROOT/build/proto_recv" --host 127.0.0.1 --port "$E2E_DOWN" --count 1 --timeout 15 >"$RECV_OUT" 2>&1 &
RECV_PID=$!
sleep 0.2
"$ROOT/build/proto_send" --host 127.0.0.1 --port "$E2E_UP" --kind heartbeat --src-user 7 --dst-user 42 --node-id e2e-test
wait "$RECV_PID"
if ! grep -q 'src_user_id=7' "$RECV_OUT"; then
  echo "expected src_user_id=7 in proto_recv output:" >&2
  cat "$RECV_OUT" >&2
  exit 1
fi
if ! grep -q 'dst_user_id=42' "$RECV_OUT"; then
  echo "expected dst_user_id=42 in proto_recv output:" >&2
  cat "$RECV_OUT" >&2
  exit 1
fi

echo "---- Python upstream_send -> downstream_recv ----"
PYOUT=$(mktemp)
python3 "$ROOT/tools/downstream_recv.py" --host 127.0.0.1 --port "$E2E_DOWN" >"$PYOUT" 2>&1 &
PY_PID=$!
sleep 0.2
python3 "$ROOT/tools/upstream_send.py" --host 127.0.0.1 --port "$E2E_UP" --src-user 100 --dst-user 200 --text "e2e-python"
sleep 0.4
kill "$PY_PID" 2>/dev/null || true
wait "$PY_PID" 2>/dev/null || true
if ! grep -q 'src_user_id=100' "$PYOUT"; then
  echo "expected src_user_id=100 in downstream_recv output:" >&2
  cat "$PYOUT" >&2
  rm -f "$PYOUT"
  exit 1
fi
if ! grep -q 'dst_user_id=200' "$PYOUT"; then
  echo "expected dst_user_id=200 in downstream_recv output:" >&2
  cat "$PYOUT" >&2
  rm -f "$PYOUT"
  exit 1
fi
if ! grep -q 'e2e-python' "$PYOUT"; then
  echo "expected body preview in downstream_recv output:" >&2
  cat "$PYOUT" >&2
  rm -f "$PYOUT"
  exit 1
fi
rm -f "$PYOUT"

echo "---- admin /api/health ----"
python3 -c "import urllib.request; r=urllib.request.urlopen('http://127.0.0.1:'+str($E2E_ADMIN)+'/api/health'); assert b'ok' in r.read().lower()"

echo "---- OK: e2e passed ----"
exit 0
