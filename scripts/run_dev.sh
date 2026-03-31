#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$ROOT/build/asio_forwarder" "$ROOT/configs/dev/forwarder.json"

