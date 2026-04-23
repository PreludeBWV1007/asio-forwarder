#!/usr/bin/env bash
set -euo pipefail

# 本文件职责（开发模式运行脚本）：
# - 直接运行 build/asio_forwarder，并指定开发配置 configs/dev/forwarder.json
# - 依赖你已先执行 ./scripts/build.sh 完成编译

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$ROOT/build/asio_forwarder" "$ROOT/deliver/server/forwarder.json"

