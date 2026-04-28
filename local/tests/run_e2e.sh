#!/usr/bin/env bash
# 黑盒一体化：临时端口起 asio_forwarder → e2e_forwarder.py →（可选）first_use_client → admin /api/health
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
E2E_MYSQL_PASSWORD="${E2E_MYSQL_PASSWORD:-e2etest}"

cleanup() {
  if [[ -n "$FWD_PID" ]]; then
    kill "$FWD_PID" 2>/dev/null || true
    wait "$FWD_PID" 2>/dev/null || true
  fi
  rm -f "$TMP_CFG"
}
trap cleanup EXIT

read -r E2E_CLIENT E2E_ADMIN < <(python3 -c "
import json, os, socket, sys

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
if 'mysql' not in c:
    raise SystemExit('forwarder.json must contain mysql section')
c['mysql']['host'] = '127.0.0.1'
c['mysql']['port'] = 3306
c['mysql']['user'] = 'root'
c['mysql']['password'] = os.environ.get('E2E_MYSQL_PASSWORD', 'e2etest')
c['mysql']['database'] = 'forwarder_e2e'
with open(sys.argv[2], 'w', encoding='utf-8') as f:
    json.dump(c, f, indent=2)
print(cli, adm)
" "$ROOT/deliver/server/forwarder.json" "$TMP_CFG")

if command -v mysql >/dev/null 2>&1; then
  export MYSQL_PWD="${E2E_MYSQL_PASSWORD}"
  if mysql -h 127.0.0.1 -P 3306 -u root --protocol=tcp -e "SELECT 1" >/dev/null 2>&1; then
    mysql -h 127.0.0.1 -P 3306 -u root --protocol=tcp -e "CREATE DATABASE IF NOT EXISTS forwarder_e2e"
    mysql -h 127.0.0.1 -P 3306 -u root --protocol=tcp forwarder_e2e < "$ROOT/deliver/server/schema.sql"
    mysql -h 127.0.0.1 -P 3306 -u root --protocol=tcp forwarder_e2e < "$ROOT/local/tests/seed_e2e.sql"
  else
    echo "e2e: MySQL not reachable at 127.0.0.1:3306 (set root password E2E_MYSQL_PASSWORD=$E2E_MYSQL_PASSWORD)" >&2
    exit 1
  fi
else
  echo "e2e: mysql client not in PATH" >&2
  exit 1
fi

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
python3 "$ROOT/local/tests/e2e_forwarder.py" 127.0.0.1 "$E2E_CLIENT"

if [[ -x "$ROOT/build/first_use_client" ]]; then
  echo "---- first_use_client (C++ SDK 入门闭环) ----"
  "$ROOT/build/first_use_client" 127.0.0.1 "$E2E_CLIENT"
else
  echo "---- skip first_use_client (rebuild with ./scripts/build.sh) ----"
fi

echo "---- admin /api/health ----"
python3 -c "import urllib.request; r=urllib.request.urlopen('http://127.0.0.1:'+str($E2E_ADMIN)+'/api/health'); assert b'ok' in r.read().lower()"

echo "---- OK: e2e passed ----"
exit 0
