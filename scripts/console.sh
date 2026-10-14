#!/usr/bin/env bash
set -e
SMARTHUB_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec "$SMARTHUB_ROOT/.venv/bin/python" -m serial.tools.miniterm --raw --eol CR --dtr 0 --rts 0 "${1:-/dev/cu.usbserial-0001}" 115200
