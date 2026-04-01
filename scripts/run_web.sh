#!/usr/bin/env bash
set -euo pipefail

# 本文件职责（启动 Web sidecar）：
# - 在 web/ 目录下安装依赖（如无 node_modules 则 npm install）
# - 启动 sidecar HTTP 服务（默认 8080，可通过 WEB_PORT 等环境变量覆盖）

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT/web"

if [ ! -d node_modules ]; then
  npm install
fi

exec npm start

