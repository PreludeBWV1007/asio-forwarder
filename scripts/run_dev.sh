#!/usr/bin/env bash
set -euo pipefail

# 开发模式：运行 build/asio_forwarder，默认配置 deliver/server/forwarder.json。
# MySQL：
# - 若模板里 password 为空而本机 root 需要密码，可仅在本终端导出环境变量再运行（不写进仓库）：
#     export FORWARDER_MYSQL_PASSWORD='你的密码'
#     ./scripts/run_dev.sh
# - 或把密码写进你本机拷贝的 JSON / 改 deliver/server/forwarder.json（勿提交真实口令）。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CFG="${FORWARDER_CONFIG:-$ROOT/deliver/server/forwarder.json}"

if [[ ! -f "$CFG" ]]; then
  echo "fatal: config not found: $CFG" >&2
  exit 1
fi

if [[ -n "${FORWARDER_MYSQL_PASSWORD+x}" ]]; then
  TMP="$(mktemp)"
  # shellcheck disable=SC2064
  trap 'rm -f "$TMP"' EXIT
  FORWARDER_MYSQL_PASSWORD="${FORWARDER_MYSQL_PASSWORD}" python3 - "$CFG" "$TMP" <<'PY'
import json, os, sys
src, dst = sys.argv[1], sys.argv[2]
with open(src, encoding="utf-8") as f:
    c = json.load(f)
c.setdefault("mysql", {})["password"] = os.environ.get("FORWARDER_MYSQL_PASSWORD", "")
with open(dst, "w", encoding="utf-8") as f:
    json.dump(c, f, indent=2, ensure_ascii=False)
    f.write("\n")
PY
  CFG="$TMP"
fi

exec "$ROOT/build/asio_forwarder" "$CFG"
