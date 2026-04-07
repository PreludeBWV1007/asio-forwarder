#!/usr/bin/env bash
set -euo pipefail

# 根据 proto/*.proto 生成 Python 的 *_pb2.py 到 tools/generated/（供 upstream_send_proto.py 等使用）。

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tools/generated"
mkdir -p "$OUT"
protoc -I "$ROOT/proto" --python_out="$OUT" "$ROOT/proto"/*.proto
echo "OK: Python protobuf -> $OUT"
