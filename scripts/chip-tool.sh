#!/usr/bin/env bash
set -e
SMARTHUB_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SMARTHUB_CHIP_ROOT="$SMARTHUB_ROOT/.toolchains/esp-matter/connectedhomeip/connectedhomeip"
SMARTHUB_CHIP_TOOL="$SMARTHUB_CHIP_ROOT/out/host/chip-tool"
if [ ! -x "$SMARTHUB_CHIP_TOOL" ]; then
    echo "Mac CHIP Tool is not built yet." >&2
    exit 1
fi
mkdir -p "$SMARTHUB_ROOT/.matter-state"
chmod 700 "$SMARTHUB_ROOT/.matter-state"
exec "$SMARTHUB_CHIP_TOOL" "$@" --storage-directory "$SMARTHUB_ROOT/.matter-state" --paa-trust-store-path "$SMARTHUB_ROOT/paa-roots"
