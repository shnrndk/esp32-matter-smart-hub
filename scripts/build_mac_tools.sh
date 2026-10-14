#!/usr/bin/env bash
set -e
SMARTHUB_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$SMARTHUB_ROOT/.toolchains/esp-matter/connectedhomeip/connectedhomeip"
source .environment/activate.sh
gn gen out/host --args='chip_inet_config_enable_ipv4=false is_debug=false'
python "$SMARTHUB_ROOT/scripts/fast_logger.py"
ninja -C out/host -j 4 chip-tool
