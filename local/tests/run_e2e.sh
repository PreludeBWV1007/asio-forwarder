#!/usr/bin/env bash
# 黑盒一体化：临时端口起 asio_forwarder → e2e_minimal → e2e_suite →（可选）forwarder_cpp_smoke → admin /api/health
# 本脚本位于 local/tests/，依赖 deliver/ 的已构建 asio_forwarder 与头文件；Python 依赖 local/tools（PYTHONPATH）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

if [[ ! -x "$ROOT/build/asio_forwarder" ]]; then
  echo "missing build/asio_forwarder; run ./scripts/build.sh first" >&2
  exit 1
fi

python3 -c "import msgpack" 2>/dev/null || {
  echo "installing msgpack for e2e: pip install -r local/tools/requirements-relay.txt" >&2
  pip install -q -r "$ROOT/local/tools/requirements-relay.txt"
}

TMP_CFG=$(mktemp)
FWD_PID=""

cleanup() {
  if [[ -n "$FWD_PID" ]]; then
    kill "$FWD_PID" 2>/dev/null || true
    wait "$FWD_PID" 2>/dev/null || true
  fi
  rm -f "$TMP_CFG"
}
trap cleanup EXIT

read -r E2E_CLIENT E2E_ADMIN < <(python3 -c "
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

cli, adm = unique(), unique()
with open(sys.argv[1], encoding='utf-8') as f:
    c = json.load(f)
c['client']['listen']['host'] = '127.0.0.1'
c['client']['listen']['port'] = cli
c['admin']['listen']['host'] = '127.0.0.1'
c['admin']['listen']['port'] = adm
c['timeouts']['idle_ms'] = 120000
with open(sys.argv[2], 'w', encoding='utf-8') as f:
    json.dump(c, f, indent=2)
print(cli, adm)
" "$ROOT/deliver/server/forwarder.json" "$TMP_CFG")

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
  echo "timeout waiting for $host:$port" >&2
  return 1
}

wait_tcp 127.0.0.1 "$E2E_CLIENT"
wait_tcp 127.0.0.1 "$E2E_ADMIN"

echo "---- e2e: client=$E2E_CLIENT admin=$E2E_ADMIN ----"
export PYTHONPATH="$ROOT/local/tools"
python3 "$ROOT/local/tests/e2e_minimal.py" 127.0.0.1 "$E2E_CLIENT"
python3 "$ROOT/local/tests/e2e_suite.py" 127.0.0.1 "$E2E_CLIENT"

if [[ -x "$ROOT/build/forwarder_cpp_smoke" ]]; then
  echo "---- forwarder_cpp_smoke (C++ SDK) ----"
  "$ROOT/build/forwarder_cpp_smoke" --connect 127.0.0.1 "$E2E_CLIENT" "$E2E_ADMIN"
else
  echo "---- skip forwarder_cpp_smoke (rebuild with ./scripts/build.sh) ----"
fi

echo "---- admin /api/health ----"
python3 -c "import urllib.request; r=urllib.request.urlopen('http://127.0.0.1:'+str($E2E_ADMIN)+'/api/health'); assert b'ok' in r.read().lower()"

echo "---- OK: e2e passed ----"
exit 0
