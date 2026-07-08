#!/usr/bin/env bash
# Create a Python virtualenv and install PlatformIO into it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
VENV_DIR="$PROJECT_DIR/.venv"
PYTHON_BIN="${PYTHON:-python3}"

cd "$PROJECT_DIR"

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
  echo "error: '$PYTHON_BIN' not found. Install Python 3.9+ or set PYTHON=<path>." >&2
  exit 1
fi

if [ ! -d "$VENV_DIR" ]; then
  echo ">> creating virtualenv at $VENV_DIR"
  "$PYTHON_BIN" -m venv "$VENV_DIR"
else
  echo ">> virtualenv already exists at $VENV_DIR"
fi

# shellcheck source=/dev/null
source "$VENV_DIR/bin/activate"

echo ">> upgrading pip"
python -m pip install --upgrade pip >/dev/null

echo ">> installing platformio"
pip install --upgrade platformio

echo ">> done: $(pio --version)"
echo "   activate later with: source .venv/bin/activate"
