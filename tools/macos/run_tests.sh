#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required on macOS. Install Python 3.10+ and re-run this script." >&2
    exit 1
fi

python3 -m pip install --user capstone pytest pefile numpy
python3 -m pytest tools/ -q
