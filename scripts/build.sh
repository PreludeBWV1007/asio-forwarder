#!/usr/bin/env bash
set -euo pipefail

# 本文件职责（构建脚本）：
# - 生成/更新 CMake 构建目录 build/
# - 编译 forwarder 可执行文件到 build/asio_forwarder
# 适合开发机本地快速构建：./scripts/build.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$ROOT/build" -j

