#!/bin/bash
# ACECode Web development launcher (macOS / Linux)
# Usage: ./scripts/dev_web.sh [Web daemon options]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if command -v python3 >/dev/null 2>&1; then
    PYTHON=python3
elif command -v python >/dev/null 2>&1; then
    PYTHON=python
else
    echo "[ERROR] python3 or python was not found. Install Python 3.8+ first." >&2
    exit 1
fi

exec "$PYTHON" "$SCRIPT_DIR/dev_environment.py" web "$@"
