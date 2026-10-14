#!/usr/bin/env bash
# Source from bash to enable the project-local SDK tools.
SMARTHUB_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export IDF_TOOLS_PATH="$SMARTHUB_ROOT/.toolchains/idf-tools"
export ESP_MATTER_PATH="$SMARTHUB_ROOT/.toolchains/esp-matter"
export PATH="/opt/homebrew/opt/python@3.11/libexec/bin:/opt/homebrew/bin:$PATH"
source "$SMARTHUB_ROOT/.toolchains/esp-idf/export.sh" || return 1
source "$ESP_MATTER_PATH/export.sh" || return 1
