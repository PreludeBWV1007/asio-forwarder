#!/usr/bin/env bash
# 与 `local/tests/run_e2e.sh` 相同，便于在仓库根从旧习惯路径调用
set -euo pipefail
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/local/tests/run_e2e.sh"
