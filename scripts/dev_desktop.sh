#!/bin/bash
# ACECode Desktop 一键开发脚本 (macOS / Linux)
# 用法: ./scripts/dev_desktop.sh [选项]
# 详见 python scripts/dev_desktop.py --help

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 选择 python 解释器
if command -v python3 &>/dev/null; then
    PYTHON=python3
elif command -v python &>/dev/null; then
    PYTHON=python
else
    echo "[ERROR] 未找到 python3 或 python，请先安装 Python 3.8+"
    exit 1
fi

exec "$PYTHON" "$SCRIPT_DIR/dev_desktop.py" "$@"
