#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ $# -ne 0 ]]; then
    echo "Usage: ./scripts/update.sh" >&2
    echo "Scans every completed match in logs/tournament/runs and rewrites logs/data.js." >&2
    exit 2
fi

exec python3 "$ROOT_DIR/scripts/aggregate.py" --root "$ROOT_DIR"
