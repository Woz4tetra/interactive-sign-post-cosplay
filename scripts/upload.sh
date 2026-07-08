#!/usr/bin/env bash
# Build and flash the firmware to the QT Py over USB.
# Extra args are passed through to `pio run` (e.g. --upload-port /dev/ttyACM0).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
VENV_DIR="$PROJECT_DIR/.venv"

cd "$PROJECT_DIR"

if [ ! -f "$VENV_DIR/bin/activate" ]; then
  echo "error: virtualenv not found. Run scripts/setup_venv.sh first." >&2
  exit 1
fi

# shellcheck source=/dev/null
source "$VENV_DIR/bin/activate"

echo ">> uploading"
pio run -t upload "$@"
