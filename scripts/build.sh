#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$ROOT/build" -j

